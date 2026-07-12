/*
 * Bhaisome IO scheduler - Improved blk-mq version
 * Copyright (C) 2025 Nishan_Najmal (Bhaisome)
 * Derived from Anxiety + stability ideas from Deadline
 */

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>

/* default tunable */
static const int max_writes_starved = 16;

struct bhaisome_data {
	struct list_head queue[2];
	unsigned int writes_starved;
	unsigned int max_writes_starved;
};

static __always_inline struct request *bhaisome_choose_request(struct bhaisome_data *mdata)
{
	bool starved = mdata->writes_starved > mdata->max_writes_starved;

	/* Prioritize reads unless writes are starved */
	if (!starved && !list_empty(&mdata->queue[READ])) {
		mdata->writes_starved++;
		return list_entry_rq(mdata->queue[READ].next);
	}

	/* Serve writes */
	if (!list_empty(&mdata->queue[WRITE])) {
		mdata->writes_starved = 0;
		return list_entry_rq(mdata->queue[WRITE].next);
	}

	mdata->writes_starved = 0;
	return NULL;
}

static int bhaisome_merge(struct request_queue *q, struct request **req, struct bio *bio)
{
	return ELEVATOR_NO_MERGE;   /* Simple FIFO for now */
}

static void bhaisome_merged_requests(struct request_queue *q, struct request *req,
				     struct request *next)
{
	if (!list_empty(&next->queuelist))
		list_del_init(&next->queuelist);
}

static int bhaisome_dispatch(struct request_queue *q, int force)
{
	struct bhaisome_data *mdata;
	struct request *rq;

	if (!q->elevator || !q->elevator->elevator_data)
		return 0;

	mdata = q->elevator->elevator_data;
	rq = bhaisome_choose_request(mdata);

	if (!rq)
		return 0;

	list_del_init(&rq->queuelist);
	elv_dispatch_add_tail(q, rq);

	return 1;
}

static void bhaisome_add_request(struct request_queue *q, struct request *rq)
{
	struct bhaisome_data *mdata;
	const int dir = rq_data_dir(rq);

	if (!q->elevator || !q->elevator->elevator_data) {
		elv_dispatch_add_tail(q, rq);
		return;
	}

	mdata = q->elevator->elevator_data;
	list_add_tail(&rq->queuelist, &mdata->queue[dir]);
}

static struct request *bhaisome_former_request(struct request_queue *q, struct request *rq)
{
	return NULL;   /* Not needed for simple FIFO */
}

static struct request *bhaisome_latter_request(struct request_queue *q, struct request *rq)
{
	return NULL;
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

/* Sysfs tunable */
static ssize_t bhaisome_max_writes_starved_show(struct elevator_queue *e, char *page)
{
	struct bhaisome_data *ad = e->elevator_data;
	return snprintf(page, PAGE_SIZE, "%d\n", ad->max_writes_starved);
}

static ssize_t bhaisome_max_writes_starved_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct bhaisome_data *ad = e->elevator_data;
	int ret = kstrtouint(page, 0, &ad->max_writes_starved);
	return ret < 0 ? ret : count;
}

static struct elv_fs_entry bhaisome_attrs[] = {
	__ATTR(max_writes_starved, 0644, bhaisome_max_writes_starved_show, bhaisome_max_writes_starved_store),
	__ATTR_NULL
};

static struct elevator_type elevator_bhaisome = {
	.ops = {
		.sq = {
			.elevator_merge_fn		= bhaisome_merge,
			.elevator_merge_req_fn		= bhaisome_merged_requests,
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
MODULE_DESCRIPTION("Bhaisome IO scheduler");
