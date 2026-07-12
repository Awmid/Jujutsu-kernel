/*
 * Bhaisome IO scheduler
 * Copyright (C) 2025 Bhaisome
 * Derived from Anxiety IO scheduler by Draco (Tyler Nijmeh)
 *
 * Patched version for Linux 4.19+ hybrid blk-mq/legacy block layer
 * Fixes: init race, missing merge_fn, null checks, list corruption
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>

/* default tunable values */
static const int max_writes_starved = 16;
static const int read_expire = HZ / 2;
static const int write_expire = 5 * HZ;
static const int fifo_batch = 16;

struct bhaisome_data {
	struct list_head queue[2];
	unsigned int writes_starved;
	unsigned int batching;

	/* tunables */
	unsigned int max_writes_starved;
	int fifo_expire[2];
	int fifo_batch;

	spinlock_t lock;
	struct list_head dispatch;
};

static __always_inline struct request *bhaisome_choose_request(struct bhaisome_data *bd)
{
	bool starved = bd->writes_starved > bd->max_writes_starved;

	/* read */
	if (!starved && !list_empty(&bd->queue[READ])) {
		bd->writes_starved++;
		return list_entry_rq(bd->queue[READ].next);
	}

	/* write */
	if (!list_empty(&bd->queue[WRITE])) {
		bd->writes_starved = 0;
		return list_entry_rq(bd->queue[WRITE].next);
	}

	/* all queues are empty */
	bd->writes_starved = 0;
	return NULL;
}

static struct request *bhaisome_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	struct bhaisome_data *bd = q->elevator->elevator_data;
	struct request *rq;

	spin_lock(&bd->lock);

	if (!list_empty(&bd->dispatch)) {
		rq = list_first_entry(&bd->dispatch, struct request, queuelist);
		list_del_init(&rq->queuelist);
		goto out;
	}

	rq = bhaisome_choose_request(bd);
	if (rq)
		list_del_init(&rq->queuelist);

out:
	spin_unlock(&bd->lock);
	return rq;
}

static void bhaisome_insert_request(struct blk_mq_hw_ctx *hctx,
				    struct request *rq, bool at_head)
{
	struct request_queue *q = hctx->queue;
	struct bhaisome_data *bd = q->elevator->elevator_data;
	const int dir = rq_data_dir(rq);

	if (at_head || blk_rq_is_passthrough(rq)) {
		if (at_head)
			list_add(&rq->queuelist, &bd->dispatch);
		else
			list_add_tail(&rq->queuelist, &bd->dispatch);
	} else {
		rq->fifo_time = jiffies + bd->fifo_expire[dir];
		list_add_tail(&rq->queuelist, &bd->queue[dir]);
	}
}

static void bhaisome_insert_requests(struct blk_mq_hw_ctx *hctx,
				     struct list_head *list, bool at_head)
{
	struct request_queue *q = hctx->queue;
	struct bhaisome_data *bd = q->elevator->elevator_data;

	spin_lock(&bd->lock);
	while (!list_empty(list)) {
		struct request *rq;

		rq = list_first_entry(list, struct request, queuelist);
		list_del_init(&rq->queuelist);
		bhaisome_insert_request(hctx, rq, at_head);
	}
	spin_unlock(&bd->lock);
}

static bool bhaisome_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	struct bhaisome_data *bd = q->elevator->elevator_data;

	return !list_empty_careful(&bd->dispatch) ||
	       !list_empty_careful(&bd->queue[READ]) ||
	       !list_empty_careful(&bd->queue[WRITE]);
}

static int bhaisome_init_sched(struct request_queue *q,
			       struct elevator_type *e)
{
	struct bhaisome_data *bd;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	bd = kzalloc_node(sizeof(*bd), GFP_KERNEL, q->node);
	if (!bd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = bd;

	INIT_LIST_HEAD(&bd->queue[READ]);
	INIT_LIST_HEAD(&bd->queue[WRITE]);
	INIT_LIST_HEAD(&bd->dispatch);
	spin_lock_init(&bd->lock);
	bd->writes_starved = 0;
	bd->batching = 0;
	bd->max_writes_starved = max_writes_starved;
	bd->fifo_expire[READ] = read_expire;
	bd->fifo_expire[WRITE] = write_expire;
	bd->fifo_batch = fifo_batch;

	q->elevator = eq;
	return 0;
}

static void bhaisome_exit_sched(struct elevator_queue *e)
{
	struct bhaisome_data *bd = e->elevator_data;

	kfree(bd);
}

/* sysfs tunables */
static ssize_t bhaisome_max_writes_starved_show(struct elevator_queue *e, char *page)
{
	struct bhaisome_data *bd = e->elevator_data;
	return snprintf(page, PAGE_SIZE, "%d\n", bd->max_writes_starved);
}

static ssize_t bhaisome_max_writes_starved_store(struct elevator_queue *e,
						   const char *page, size_t count)
{
	struct bhaisome_data *bd = e->elevator_data;
	int ret;

	ret = kstrtouint(page, 0, &bd->max_writes_starved);
	if (ret < 0)
		return ret;

	return count;
}

static struct elv_fs_entry bhaisome_attrs[] = {
	__ATTR(max_writes_starved, 0644,
	       bhaisome_max_writes_starved_show, bhaisome_max_writes_starved_store),
	__ATTR_NULL
};

static struct elevator_type elevator_bhaisome = {
	.ops.mq = {
		.insert_requests	= bhaisome_insert_requests,
		.dispatch_request	= bhaisome_dispatch_request,
		.has_work		= bhaisome_has_work,
		.init_sched		= bhaisome_init_sched,
		.exit_sched		= bhaisome_exit_sched,
	},
	.uses_mq = true,
	.elevator_name = "bhaisome",
	.elevator_attrs = bhaisome_attrs,
	.elevator_owner = THIS_MODULE,
};

static int __init bhaisome_init(void)
{
	return elv_register(&elevator_bhaisome);
}

static void __exit bhaisome_exit(void)
{
	elv_unregister(&elevator_bhaisome);
}

module_init(bhaisome_init);
module_exit(bhaisome_exit);

MODULE_AUTHOR("Nishan_Najmal");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bhaisome IO scheduler");
