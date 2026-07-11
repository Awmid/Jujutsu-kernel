/*
 * Bhaisome IO scheduler
 * Copyright (C) 2025 Bhaisome
 * Derived from Anxiety IO scheduler by Draco (Tyler Nijmeh)
 *
 * Patched version for Linux 4.19+ hybrid blk-mq/legacy block layer
 * Fixes: init race, missing merge_fn, null checks, list corruption
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>

/* default tunable values */
static const int max_writes_starved = 16; /* max amount of times reads can starve pending writes */

struct bhaisome_data {
	struct list_head queue[2];
	unsigned int writes_starved;

	/* tunables */
	unsigned int max_writes_starved;
};

/*
 * Merge callback: called when two requests are merged.
 * Bhaisome uses simple FIFO, so we just need to remove the 'next'
 * request from our internal queue if it was there.
 * Defensive: check if next is actually in a list before deleting.
 */
static void bhaisome_merged_requests(struct request_queue *q, struct request *rq,
				  struct request *next)
{
	/* Only clear if next was actually in our FIFO queue */
	if (!list_empty(&next->queuelist))
		list_del_init(&next->queuelist);
}

/*
 * Bio merge callback: called to check if a bio can merge with a request.
 * Bhaisome doesn't do position-based merging, so always allow.
 */
static int bhaisome_allow_merge(struct request_queue *q, struct request *rq,
				struct bio *bio)
{
	return 1;
}

/*
 * Position-based merge: called to find a merge candidate.
 * Bhaisome is FIFO, not position-based, so return NO_MERGE.
 */
static int bhaisome_merge(struct request_queue *q, struct request **req,
			 struct bio *bio)
{
	return ELEVATOR_NO_MERGE;
}

static __always_inline struct request *bhaisome_choose_request(struct bhaisome_data *mdata)
{
	/* prioritize reads unless writes are exceedingly starved */
	bool starved = mdata->writes_starved > mdata->max_writes_starved;

	/* read */
	if (!starved && !list_empty(&mdata->queue[READ])) {
		mdata->writes_starved++;
		return list_entry_rq(mdata->queue[READ].next);
	}

	/* write */
	if (!list_empty(&mdata->queue[WRITE])) {
		mdata->writes_starved = 0;
		return list_entry_rq(mdata->queue[WRITE].next);
	}

	/* all queues are empty, i.e. no pending requests */
	mdata->writes_starved = 0;
	return NULL;
}

static int bhaisome_dispatch(struct request_queue *q, int force)
{
	struct bhaisome_data *mdata;
	struct request *rq;

	/* Defensive: elevator might not be fully initialized yet */
	if (!q->elevator || !q->elevator->elevator_data)
		return 0;

	mdata = q->elevator->elevator_data;
	rq = bhaisome_choose_request(mdata);

	if (!rq)
		return 0;

	/* Remove from our internal queue and add to dispatch queue */
	list_del_init(&rq->queuelist);
	elv_dispatch_add_tail(rq->q, rq);

	return 1;
}

static void bhaisome_add_request(struct request_queue *q, struct request *rq)
{
	struct bhaisome_data *mdata;
	const int dir = rq_data_dir(rq);

	/* Defensive: elevator might not be fully initialized yet */
	if (!q->elevator || !q->elevator->elevator_data) {
		/* Fallback: add directly to dispatch queue */
		list_add_tail(&rq->queuelist, &q->queue_head);
		return;
	}

	mdata = q->elevator->elevator_data;
	list_add_tail(&rq->queuelist, &mdata->queue[dir]);
}

static struct request *bhaisome_former_request(struct request_queue *q, struct request *rq)
{
	struct bhaisome_data *mdata;
	const int dir = rq_data_dir(rq);

	if (!q->elevator || !q->elevator->elevator_data)
		return NULL;

	mdata = q->elevator->elevator_data;

	if (rq->queuelist.prev == &mdata->queue[dir])
		return NULL;

	return list_prev_entry(rq, queuelist);
}

static struct request *bhaisome_latter_request(struct request_queue *q, struct request *rq)
{
	struct bhaisome_data *mdata;
	const int dir = rq_data_dir(rq);

	if (!q->elevator || !q->elevator->elevator_data)
		return NULL;

	mdata = q->elevator->elevator_data;

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

	/* allocate data */
	data = kzalloc_node(sizeof(*data), GFP_KERNEL, q->node);
	if (!data) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = data;

	/* initialize data */
	INIT_LIST_HEAD(&data->queue[READ]);
	INIT_LIST_HEAD(&data->queue[WRITE]);
	data->writes_starved = 0;
	data->max_writes_starved = max_writes_starved;

	/*
	 * Set the elevator to us.
	 * NOTE: elevator_init() in elevator.c holds q->sysfs_lock,
	 * not q->queue_lock. We take queue_lock here for safety
	 * since other paths may check q->elevator without holding
	 * sysfs_lock.
	 */
	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	return 0;
}

static void bhaisome_exit_queue(struct elevator_queue *eq)
{
	struct bhaisome_data *mdata = eq->elevator_data;

	kfree(mdata);
}

/* sysfs tunables */
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

/*
 * CRITICAL: .uses_mq MUST be explicitly set to false for legacy
 * single-queue schedulers in hybrid 4.x kernels. If left unset,
 * the hybrid elevator_find() may fail to match this scheduler
 * when searching for non-mq schedulers.
 */
static struct elevator_type elevator_bhaisome = {
	.ops = {
		.sq = {
			.elevator_merge_fn		= bhaisome_merge,
			.elevator_merge_req_fn		= bhaisome_merged_requests,
			.elevator_allow_bio_merge_fn	= bhaisome_allow_merge,
			.elevator_dispatch_fn		= bhaisome_dispatch,
			.elevator_add_req_fn		= bhaisome_add_request,
			.elevator_former_req_fn		= bhaisome_former_request,
			.elevator_latter_req_fn		= bhaisome_latter_request,
			.elevator_init_fn		= bhaisome_init_queue,
			.elevator_exit_fn		= bhaisome_exit_queue,
		},
	},
	.uses_mq = false,
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
