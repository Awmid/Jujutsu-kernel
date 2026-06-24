/*
 * Anxiety IO scheduler
 * Copyright (C) 2018 Draco (Tyler Nijmeh) <tylernij@gmail.com>
 * Fixed for 4.19 Backported SQ Layers (Redmi 9A)
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>

static const unsigned int max_writes_starved = 8; 

struct anxiety_data {
	struct list_head queue[2];
	unsigned int writes_starved;
	unsigned int max_writes_starved;
};

static void anxiety_merged_requests(struct request_queue *q, struct request *rq, struct request *next)
{
	/* Safely dequeue the merged request from the FIFO list */
	list_del_init(&next->queuelist);
}

static __always_inline struct request *anxiety_choose_request(struct anxiety_data *mdata)
{
	bool starved = mdata->writes_starved > mdata->max_writes_starved;

	/* read */
	if (!starved && !list_empty(&mdata->queue[READ])) {
		mdata->writes_starved++;
		return list_first_entry(&mdata->queue[READ], struct request, queuelist);
	}

	/* write */
	if (!list_empty(&mdata->queue[WRITE])) {
		mdata->writes_starved = 0;
		return list_first_entry(&mdata->queue[WRITE], struct request, queuelist);
	}

	mdata->writes_starved = 0;
	return NULL;
}

static int anxiety_dispatch(struct request_queue *q, int force)
{
	struct anxiety_data *mdata = q->elevator->elevator_data;
	struct request *rq = anxiety_choose_request(mdata);

	if (!rq)
		return 0;

	list_del_init(&rq->queuelist);
	elv_dispatch_add_tail(q, rq);

	return 1;
}

static void anxiety_add_request(struct request_queue *q, struct request *rq)
{
	const uint8_t dir = (rq_data_dir(rq) == WRITE) ? 1 : 0;
	struct anxiety_data *mdata = q->elevator->elevator_data;

	list_add_tail(&rq->queuelist, &mdata->queue[dir]);
}

static struct request *anxiety_former_request(struct request_queue *q, struct request *rq)
{
	const uint8_t dir = (rq_data_dir(rq) == WRITE) ? 1 : 0;
	struct anxiety_data *mdata = q->elevator->elevator_data;

	if (rq->queuelist.prev == &mdata->queue[dir])
		return NULL;

	return list_prev_entry(rq, queuelist);
}

static struct request *anxiety_latter_request(struct request_queue *q, struct request *rq)
{
	const uint8_t dir = (rq_data_dir(rq) == WRITE) ? 1 : 0;
	struct anxiety_data *mdata = q->elevator->elevator_data;

	if (rq->queuelist.next == &mdata->queue[dir])
		return NULL;

	return list_next_entry(rq, queuelist);
}

static int anxiety_init_queue(struct request_queue *q, struct elevator_type *elv)
{
	struct anxiety_data *data;
	struct elevator_queue *eq = elevator_alloc(q, elv);

	if (!eq)
		return -ENOMEM;

	data = kmalloc_node(sizeof(*data), GFP_KERNEL | __GFP_ZERO, q->node);
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

static void anxiety_exit_queue(struct elevator_queue *e)
{
	struct anxiety_data *data = e->elevator_data;
	kfree(data);
}

static ssize_t anxiety_max_writes_starved_show(struct elevator_queue *e, char *page)
{
	struct anxiety_data *ad = e->elevator_data;
	return snprintf(page, PAGE_SIZE, "%u\n", ad->max_writes_starved);
}

static ssize_t anxiety_max_writes_starved_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct anxiety_data *ad = e->elevator_data;
	unsigned int val;
	int ret = kstrtouint(page, 0, &val);
	if (ret < 0)
		return ret;

	ad->max_writes_starved = val;
	return count;
}

static struct elv_fs_entry anxiety_attrs[] = {
	__ATTR(max_writes_starved, 0644, anxiety_max_writes_starved_show, anxiety_max_writes_starved_store),
	__ATTR_NULL
};

static struct elevator_type elevator_anxiety = {
	.ops.sq = {
		.elevator_merge_req_fn	= anxiety_merged_requests,
		.elevator_dispatch_fn	= anxiety_dispatch,
		.elevator_add_req_fn	= anxiety_add_request,
		.elevator_former_req_fn	= anxiety_former_request,
		.elevator_latter_req_fn	= anxiety_latter_request,
		.elevator_init_fn		= anxiety_init_queue,
		.elevator_exit_fn		= anxiety_exit_queue,
	},
	.elevator_name = "anxiety",
	.elevator_attrs = anxiety_attrs,
	.elevator_owner = THIS_MODULE,
};

static int __init anxiety_init(void)
{
	return elv_register(&elevator_anxiety);
}

static void __exit anxiety_exit(void)
{
	elv_unregister(&elevator_anxiety);
}

module_init(anxiety_init);
module_exit(anxiety_exit);

MODULE_AUTHOR("Draco (Tyler Nijmeh)");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Fixed Official Anxiety IO scheduler");
