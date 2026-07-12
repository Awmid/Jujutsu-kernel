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

static int bhaisome_dispatch(struct request_queue *q, int force)
{
	struct bhaisome_data *mdata = q->elevator->elevator_data;
	struct request *rq;

	rq = bhaisome_choose_request(mdata);
	if (!rq)
		return 0;

	list_del_init(&rq->queuelist);
	elv_dispatch_add_tail(rq->q, rq);

	return 1;
}

static void bhaisome_add_request(struct request_queue *q, struct request *rq)
{
	struct bhaisome_data *mdata = q->elevator->elevator_data;
	const int dir = rq_data_dir(rq);

	list_add_tail(&rq->queuelist, &mdata->queue[dir]);
}

static struct request *bhaisome_former_request(struct request_queue *q, struct request *rq)
{
	struct bhaisome_data *mdata = q->elevator->elevator_data;
	const int dir = rq_data_dir(rq);

	if (rq->queuelist.prev == &mdata->queue[dir])
		return NULL;

	return list_prev_entry(rq, queuelist);
}

static struct request *bhaisome_latter_request(struct request_queue *q, struct request *rq)
{
	struct bhaisome_data *mdata = q->elevator->elevator_data;
	const int dir = rq_data_dir(rq);

	if (rq->queuelist.next == &mdata->queue[dir])
		return NULL;

	return list_next_entry(rq, queuelist);
}

static int bhaisome_init_queue(struct request_queue *q, struct elevator_type *elv)
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

static void bhaisome_exit_queue(struct elevator_queue *eq)
{
	kfree(eq->elevator_data);
}

static ssize_t bhaisome_max_writes_starved_show(struct elevator_queue *e, char *page)
{
	struct bhaisome_data *ad = e->elevator_data;
	return snprintf(page, PAGE_SIZE, "%d\n", ad->max_writes_starved);
}

static ssize_t bhaisome_max_writes_starved_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct bhaisome_data *ad = e->elevator_data;
	int ret;

	ret = kstrtouint(page, 0, &ad->max_writes_starved);
	if (ret < 0)
		return ret;

	return count;
}

static struct elv_fs_entry bhaisome_attrs[] = {
	__ATTR(max_writes_starved, 0644, bhaisome_max_writes_starved_show, bhaisome_max_writes_starved_store),
	__ATTR_NULL
};

static struct elevator_type elevator_bhaisome = {
	.ops = {
		.sq = {
			.elevator_dispatch_fn		= bhaisome_dispatch,
			.elevator_add_req_fn		= bhaisome_add_request,
			.elevator_former_req_fn		= bhaisome_former_request,
			.elevator_latter_req_fn		= bhaisome_latter_request,
			.elevator_init_fn		= bhaisome_init_queue,
			.elevator_exit_fn		= bhaisome_exit_queue,
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
