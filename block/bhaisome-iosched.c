/*
 * Bhaisome IO scheduler - blk-mq version
 * Copyright (C) 2025 Bhaisome
 * Derived from Anxiety IO scheduler by Draco (Tyler Nijmeh)
 *
 * blk-mq version for Linux 4.19+ / MTK Helio G25 / eMMC 5.1
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>

static const int max_writes_starved = 16;

struct bhaisome_data {
	struct list_head queue[2];
	unsigned int writes_starved;
	unsigned int max_writes_starved;
};

static __always_inline struct request *bhaisome_choose_request(struct bhaisome_data *mdata)
{
	bool starved = mdata->writes_starved > mdata->max_writes_starved;

	if (!starved && !list_empty(&mdata->queue[READ])) {
		mdata->writes_starved++;
		return list_entry_rq(mdata->queue[READ].next);
	}

	if (!list_empty(&mdata->queue[WRITE])) {
		mdata->writes_starved = 0;
		return list_entry_rq(mdata->queue[WRITE].next);
	}

	mdata->writes_starved = 0;
	return NULL;
}

static void bhaisome_insert_requests(struct blk_mq_hw_ctx *hctx,
				     struct list_head *list, bool at_head)
{
	struct request_queue *q = hctx->queue;
	struct bhaisome_data *mdata = q->elevator->elevator_data;
	struct request *rq;

	list_for_each_entry(rq, list, queuelist) {
		const int dir = rq_data_dir(rq);
		list_add_tail(&rq->queuelist, &mdata->queue[dir]);
	}
}

static struct request *bhaisome_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	struct bhaisome_data *mdata = q->elevator->elevator_data;
	struct request *rq;

	rq = bhaisome_choose_request(mdata);
	if (rq)
		list_del_init(&rq->queuelist);

	return rq;
}

static bool bhaisome_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	struct bhaisome_data *mdata = q->elevator->elevator_data;

	return !list_empty(&mdata->queue[READ]) ||
	       !list_empty(&mdata->queue[WRITE]);
}

static int bhaisome_init_sched(struct request_queue *q,
			       struct elevator_type *elv)
{
	struct bhaisome_data *data;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, elv);
	if (!eq)
		return -ENOMEM;

	data = kzalloc_node(sizeof(*data), GFP_KERNEL, q->node);
	if (!data) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = data;

	INIT_LIST_HEAD(&data->queue[READ]);
	INIT_LIST_HEAD(&data->queue[WRITE]);
	data->writes_starved = 0;
	data->max_writes_starved = max_writes_starved;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	return 0;
}

static void bhaisome_exit_sched(struct elevator_queue *eq)
{
	kfree(eq->elevator_data);
}

static ssize_t bhaisome_max_writes_starved_show(struct elevator_queue *e, char *page)
{
	struct bhaisome_data *ad = e->elevator_data;
	return snprintf(page, PAGE_SIZE, "%d\n", ad->max_writes_starved);
}

static ssize_t bhaisome_max_writes_starved_store(struct elevator_queue *e,
						 const char *page, size_t count)
{
	struct bhaisome_data *ad = e->elevator_data;
	int ret;

	ret = kstrtouint(page, 0, &ad->max_writes_starved);
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
	.ops = {
		.mq = {
			.insert_requests	= bhaisome_insert_requests,
			.dispatch_request	= bhaisome_dispatch_request,
			.has_work		= bhaisome_has_work,
			.init_sched		= bhaisome_init_sched,
			.exit_sched		= bhaisome_exit_sched,
		},
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
MODULE_DESCRIPTION("Bhaisome IO scheduler - blk-mq");
