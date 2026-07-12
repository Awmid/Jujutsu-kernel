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
#include <linux/rbtree.h>

/* default tunable values */
static const int max_writes_starved = 16;
static const int read_expire = HZ / 2;
static const int write_expire = 5 * HZ;
static const int fifo_batch = 16;

struct bhaisome_data {
	struct list_head queue[2];
	struct rb_root sort_list[2];
	struct request *next_rq[2];
	unsigned int writes_starved;
	unsigned int batching;

	/* tunables */
	unsigned int max_writes_starved;
	int fifo_expire[2];
	int fifo_batch;
};

static inline struct rb_root *
bhaisome_rb_root(struct bhaisome_data *bd, struct request *rq)
{
	return &bd->sort_list[rq_data_dir(rq)];
}

static inline struct request *
bhaisome_latter_request(struct request *rq)
{
	struct rb_node *node = rb_next(&rq->rb_node);

	if (node)
		return rb_entry_rq(node);

	return NULL;
}

static void
bhaisome_add_rq_rb(struct bhaisome_data *bd, struct request *rq)
{
	elv_rb_add(bhaisome_rb_root(bd, rq), rq);
}

static inline void
bhaisome_del_rq_rb(struct bhaisome_data *bd, struct request *rq)
{
	const int data_dir = rq_data_dir(rq);

	if (bd->next_rq[data_dir] == rq)
		bd->next_rq[data_dir] = bhaisome_latter_request(rq);

	elv_rb_del(bhaisome_rb_root(bd, rq), rq);
}

static void bhaisome_merged_requests(struct request_queue *q, struct request *rq,
				  struct request *next)
{
	if (!list_empty(&next->queuelist)) {
		list_del_init(&next->queuelist);
		if (time_before((unsigned long)next->fifo_time,
				(unsigned long)rq->fifo_time)) {
			list_move(&rq->queuelist, &next->queuelist);
			rq->fifo_time = next->fifo_time;
		}
	}

	bhaisome_del_rq_rb(q->elevator->elevator_data, next);
}

static int bhaisome_allow_merge(struct request_queue *q, struct request *rq,
				struct bio *bio)
{
	return 1;
}

static int bhaisome_merge(struct request_queue *q, struct request **req,
			 struct bio *bio)
{
	struct bhaisome_data *bd = q->elevator->elevator_data;
	sector_t sector = bio_end_sector(bio);
	struct request *__rq;

	__rq = elv_rb_find(&bd->sort_list[bio_data_dir(bio)], sector);
	if (__rq) {
		BUG_ON(sector != blk_rq_pos(__rq));
		if (elv_bio_merge_ok(__rq, bio)) {
			*req = __rq;
			return ELEVATOR_FRONT_MERGE;
		}
	}

	return ELEVATOR_NO_MERGE;
}

static void bhaisome_merged_request(struct request_queue *q,
				    struct request *req, enum elv_merge type)
{
	struct bhaisome_data *bd = q->elevator->elevator_data;

	if (type == ELEVATOR_FRONT_MERGE) {
		elv_rb_del(bhaisome_rb_root(bd, req), req);
		bhaisome_add_rq_rb(bd, req);
	}
}

static __always_inline struct request *bhaisome_choose_request(struct bhaisome_data *bd)
{
	bool starved = bd->writes_starved > bd->max_writes_starved;

	if (!starved && !list_empty(&bd->queue[READ])) {
		bd->writes_starved++;
		return list_entry_rq(bd->queue[READ].next);
	}

	if (!list_empty(&bd->queue[WRITE])) {
		bd->writes_starved = 0;
		return list_entry_rq(bd->queue[WRITE].next);
	}

	bd->writes_starved = 0;
	return NULL;
}

static inline int bhaisome_check_fifo(struct bhaisome_data *bd, int ddir)
{
	struct request *rq = rq_entry_fifo(bd->queue[ddir].next);

	if (time_after_eq(jiffies, (unsigned long)rq->fifo_time))
		return 1;

	return 0;
}

static struct request *
bhaisome_fifo_request(struct bhaisome_data *bd, int data_dir)
{
	if (list_empty(&bd->queue[data_dir]))
		return NULL;

	return rq_entry_fifo(bd->queue[data_dir].next);
}

static struct request *
bhaisome_next_request(struct bhaisome_data *bd, int data_dir)
{
	return bd->next_rq[data_dir];
}

static void
bhaisome_move_to_dispatch(struct bhaisome_data *bd, struct request *rq)
{
	struct request_queue *q = rq->q;

	rq_fifo_clear(rq);
	bhaisome_del_rq_rb(bd, rq);
	elv_dispatch_add_tail(q, rq);
}

static void
bhaisome_move_request(struct bhaisome_data *bd, struct request *rq)
{
	const int data_dir = rq_data_dir(rq);

	bd->next_rq[READ] = NULL;
	bd->next_rq[WRITE] = NULL;
	bd->next_rq[data_dir] = bhaisome_latter_request(rq);

	bhaisome_move_to_dispatch(bd, rq);
}

static int bhaisome_dispatch(struct request_queue *q, int force)
{
	struct bhaisome_data *bd = q->elevator->elevator_data;
	const int reads = !list_empty(&bd->queue[READ]);
	const int writes = !list_empty(&bd->queue[WRITE]);
	struct request *rq, *next_rq;
	int data_dir;

	rq = bhaisome_next_request(bd, WRITE);
	if (!rq)
		rq = bhaisome_next_request(bd, READ);

	if (rq && bd->batching < bd->fifo_batch)
		goto dispatch_request;

	if (reads) {
		if (bhaisome_fifo_request(bd, WRITE) &&
		    (bd->writes_starved++ >= bd->max_writes_starved))
			goto dispatch_writes;

		data_dir = READ;
		goto dispatch_find_request;
	}

	if (writes) {
dispatch_writes:
		bd->writes_starved = 0;
		data_dir = WRITE;
		goto dispatch_find_request;
	}

	return 0;

dispatch_find_request:
	next_rq = bhaisome_next_request(bd, data_dir);
	if (bhaisome_check_fifo(bd, data_dir) || !next_rq) {
		rq = bhaisome_fifo_request(bd, data_dir);
	} else {
		rq = next_rq;
	}

	if (!rq)
		return 0;

	bd->batching = 0;

dispatch_request:
	bd->batching++;
	bhaisome_move_request(bd, rq);

	return 1;
}

static void bhaisome_add_request(struct request_queue *q, struct request *rq)
{
	struct bhaisome_data *bd = q->elevator->elevator_data;
	const int dir = rq_data_dir(rq);

	bhaisome_add_rq_rb(bd, rq);

	rq->fifo_time = jiffies + bd->fifo_expire[dir];
	list_add_tail(&rq->queuelist, &bd->queue[dir]);
}

static void bhaisome_remove_request(struct request_queue *q, struct request *rq)
{
	struct bhaisome_data *bd = q->elevator->elevator_data;

	rq_fifo_clear(rq);
	bhaisome_del_rq_rb(bd, rq);
}

static struct request *bhaisome_former_request(struct request_queue *q, struct request *rq)
{
	struct bhaisome_data *bd = q->elevator->elevator_data;

	return elv_rb_former_request(q, rq);
}

static struct request *bhaisome_latter_request(struct request_queue *q, struct request *rq)
{
	struct bhaisome_data *bd = q->elevator->elevator_data;

	return elv_rb_latter_request(q, rq);
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
	data->sort_list[READ] = RB_ROOT;
	data->sort_list[WRITE] = RB_ROOT;
	data->writes_starved = 0;
	data->batching = 0;
	data->max_writes_starved = max_writes_starved;
	data->fifo_expire[READ] = read_expire;
	data->fifo_expire[WRITE] = write_expire;
	data->fifo_batch = fifo_batch;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	return 0;
}

static void bhaisome_exit_queue(struct elevator_queue *eq)
{
	struct bhaisome_data *bd = eq->elevator_data;

	kfree(bd);
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
			.elevator_merge_fn		= bhaisome_merge,
			.elevator_merged_fn		= bhaisome_merged_request,
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
