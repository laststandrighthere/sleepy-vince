#include <linux/sched/runq.h>

void runq_init(struct runq *q)
{
	int i;
	for (i = 0; i < KTZ_HEADS_PER_RUNQ; i ++) {
		INIT_LIST_HEAD(&q->queues[i]);
	}
}

void runq_set_bit(struct runq *q, int pri)
{
	unsigned long *bm = q->status;
	bitmap_set(bm, pri, 1);
}

void runq_clear_bit(struct runq *q, int pri)
{
	unsigned long *bm = q->status;
	bitmap_clear(bm, pri, 1);
}

void runq_add(struct runq * q, struct task_struct *p, int flags)
{
	runq_add_pri(q, p, p->ktz_prio, flags);
}

void runq_add_pri(struct runq * q, struct task_struct *p, int pri, int flags)
{
	struct list_head *head;
	struct sched_ktz_entity *ke = &p->ktz_se;
	ke->curr_runq = q;

	pri = pri % KTZ_HEADS_PER_RUNQ;

	//ke->rqindex = (pri - MIN_KTZ_PRIO) / KTZ_PRIO_PER_QUEUE;
	ke->rqindex = pri;
	/* Sanity check. */
	//printk_deferred("runq_add_pri for task = %d, pri = %d", p->pid, pri);
	if (pri < 0 || KTZ_HEADS_PER_RUNQ <= pri)
		BUG();
	runq_set_bit(q, pri);
	head = &q->queues[pri];
	if (flags & KTZ_PREEMPTED) {
		list_add(&ke->runq, head);	
	}
	else {
		list_add_tail(&ke->runq, head);	
	}
}

/*
 * Remove the thread from the queue specified by its priority, and clear the
 * corresponding status bit if the queue becomes empty.
 * Caller must set state afterwards.
 */
void runq_remove(struct runq *q, struct task_struct *p)
{
	runq_remove_idx(q, p, NULL);
}

void runq_remove_idx(struct runq *q, struct task_struct *p, int *idx)
{
	int pri;
	struct list_head *head;
	struct sched_ktz_entity *ke = &p->ktz_se;

	pri = ke->rqindex;
	head = &q->queues[pri];
	list_del_init(&ke->runq);
	if (list_empty(head)) {
		runq_clear_bit(q, pri);
		if (idx != NULL && *idx == pri)
			*idx = (pri + 1) % KTZ_HEADS_PER_RUNQ;
	}
}

static int runq_findbit(struct runq *q)
{
	int f;
	f = find_first_bit(q->status, KTZ_HEADS_PER_RUNQ);
	return f == KTZ_HEADS_PER_RUNQ ? -1 : f;
}

static inline struct task_struct *ktz_task_of(struct sched_ktz_entity *ktz_se)
{
	return container_of(ktz_se, struct task_struct, ktz_se);
}

/*
 * Find the highest priority process on the run queue.
 */
struct task_struct *runq_choose(struct runq *rq, struct task_struct *except)
{
	return runq_choose_from(rq, 0, except);
}

static int runq_findbit_from(struct runq *q, int idx)
{
	int f;
	f = find_next_bit(q->status, KTZ_HEADS_PER_RUNQ, idx);
	if (f == KTZ_HEADS_PER_RUNQ) {
		if (idx == 0)
			return -1;
		else
			return runq_findbit(q);
	}
	else {
		return f;
	}
}

struct task_struct *runq_choose_from(struct runq *rq, int idx, struct task_struct *except)
{
	struct list_head *rqh;
	struct sched_ktz_entity  *tmp;
	struct sched_ktz_entity *first;
	int pri;

retry:
	if ((pri = runq_findbit_from(rq, idx)) != -1) {
		rqh = &rq->queues[pri];
		if (!except) {
			first = list_first_entry(rqh, struct sched_ktz_entity, runq);
			return ktz_task_of(first);
		}
		else {
			list_for_each_entry(tmp, rqh, runq) {
				struct task_struct *t = ktz_task_of(tmp);
				if (t != except)
					return t;
			}
			idx ++;
			if (idx >= KTZ_RUNQ_BITMAP_SIZE)
				return NULL;
			goto retry;
		}
	}

	return (NULL);
}

