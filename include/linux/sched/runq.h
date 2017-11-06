#include <linux/sched.h>

/*
 * Run queue structure.  Contains an array of run queues on which processes
 * are placed, and a structure to maintain the status of each queue.
 */
#define KTZ_HEADS_PER_RUNQ (MAX_KTZ_PRIO)
//#define KTZ_PRIO_PER_QUEUE (1)
//#define KTZ_RUNQ_BITMAP_SIZE KTZ_HEADS_PER_RUNQ / (sizeof(unsigned long) * 8)
#define KTZ_RUNQ_BITMAP_SIZE KTZ_HEADS_PER_RUNQ

struct runq {
	DECLARE_BITMAP(status, KTZ_HEADS_PER_RUNQ); 
	struct list_head queues[KTZ_HEADS_PER_RUNQ];
};

void runq_init(struct runq *q);
void runq_set_bit(struct runq *q, int pri);
void runq_clear_bit(struct runq *q, int pri);
void runq_add(struct runq * q, struct task_struct *p, int flags);
void runq_add_pri(struct runq * q, struct task_struct *p, int pri, int flags);
void runq_remove(struct runq *q, struct task_struct *p);
void runq_remove_idx(struct runq *q, struct task_struct *p, int *idx);
struct task_struct *runq_choose(struct runq *rq, struct task_struct *p);
struct task_struct *runq_choose_from(struct runq *rq, int idx, struct task_struct *p);
