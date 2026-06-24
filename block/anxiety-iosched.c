#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>

struct anxiety_data {
	struct list_head fifo_list[2];
	unsigned int batch_count;
	unsigned int write_count;
};

static void anxiety_merged_requests(struct request_queue *q, struct request *rq,
				    struct request *next)
{
	list_del_init(&next->queuelist);
}

static int anxiety_dispatch_batch(struct request_queue *q)
{
	struct anxiety_data *adata = q->elevator->elevator_data;
	struct request *rq;
	int dispatched = 0;
	int idx;

	if (!list_empty(&adata->fifo_list[READ])) {
		idx = READ;
	} else if (!list_empty(&adata->fifo_list[WRITE])) {
		idx = WRITE;
		adata->write_count = 0;
	} else {
		return 0;
	}

	while (!list_empty(&adata->fifo_list[idx])) {
		rq = list_first_entry(&adata->fifo_list[idx], struct request, queuelist);
		list_del_init(&rq->queuelist);
		elv_dispatch_sort(q, rq);
		dispatched = 1;

		if (idx == WRITE) {
			adata->write_count++;
			if (adata->write_count >= adata->batch_count)
				break;
		}

		if (dispatched)
			break;
	}

	return dispatched;
}

static int anxiety_dispatch_drain(struct request_queue *q)
{
	struct anxiety_data *adata = q->elevator->elevator_data;
	struct request *rq;
	int dispatched = 0;
	int i;

	for (i = 0; i < 2; i++) {
		while (!list_empty(&adata->fifo_list[i])) {
			rq = list_first_entry(&adata->fifo_list[i], struct request, queuelist);
			list_del_init(&rq->queuelist);
			elv_dispatch_sort(q, rq);
			dispatched = 1;
		}
	}

	return dispatched;
}

static int anxiety_dispatch(struct request_queue *q, int force)
{
	if (force)
		return anxiety_dispatch_drain(q);

	return anxiety_dispatch_batch(q);
}

static void anxiety_add_request(struct request_queue *q, struct request *rq)
{
	struct anxiety_data *adata = q->elevator->elevator_data;
	int idx = rq_data_dir(rq);

	list_add_tail(&rq->queuelist, &adata->fifo_list[idx]);
}

static int anxiety_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct anxiety_data *adata;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	adata = kmalloc_node(sizeof(*adata), GFP_KERNEL | __GFP_ZERO, q->node);
	if (!adata) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}

	eq->elevator_data = adata;
	INIT_LIST_HEAD(&adata->fifo_list[READ]);
	INIT_LIST_HEAD(&adata->fifo_list[WRITE]);
	
	adata->batch_count = 4;
	adata->write_count = 0;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	return 0;
}

static void anxiety_exit_queue(struct elevator_queue *e)
{
	struct anxiety_data *adata = e->elevator_data;
	kfree(adata);
}

static ssize_t anxiety_batch_show(struct elevator_queue *e, char *page)
{
	struct anxiety_data *adata = e->elevator_data;
	return sprintf(page, "%u\n", adata->batch_count);
}

static ssize_t anxiety_batch_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct anxiety_data *adata = e->elevator_data;
	unsigned int val;
	int ret = kstrtouint(page, 10, &val);
	if (ret)
		return ret;
	adata->batch_count = val;
	return count;
}

static struct elv_fs_entry anxiety_attrs[] = {
	__ATTR(batch_count, 0644, anxiety_batch_show, anxiety_batch_store),
	__ATTR_NULL
};

static struct elevator_type elevator_anxiety = {
	.ops.sq = {
		.elevator_merge_req_fn	= anxiety_merged_requests,
		.elevator_dispatch_fn	= anxiety_dispatch,
		.elevator_add_req_fn	= anxiety_add_request,
		.elevator_former_req_fn	= elv_rb_former_request,
		.elevator_latter_req_fn	= elv_rb_latter_request,
		.elevator_init_fn	= anxiety_init_queue,
		.elevator_exit_fn	= anxiety_exit_queue,
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

MODULE_AUTHOR("tytydraco");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Anxiety IO Scheduler for legacy 4.19 block layer");
