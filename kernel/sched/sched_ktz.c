#include "sched.h"

/*
 * Changes:
 * 	_ DROP everything related to fork.
 * 	_ DROP burrowing etc...
 * 	_ Simplify preemption.
 */

/* Macros and defines. */
/* Timeshare range = Whole range of this scheduler. */
#define	PRI_TIMESHARE_RANGE	(PRI_MAX_TIMESHARE - PRI_MIN_TIMESHARE + 1)
//#define PRI_MIN_TIMESHARE	MIN_KTZ_PRIO
//#define PRI_MAX_TIMESHARE	MAX_KTZ_PRIO
#define PRI_MIN_TIMESHARE	100
#define PRI_MAX_TIMESHARE	139

/* Interactive range. */
#define	PRI_INTERACT_RANGE	12
#define	PRI_MIN_INTERACT	PRI_MIN_TIMESHARE
#define	PRI_MAX_INTERACT	(PRI_MIN_TIMESHARE + PRI_INTERACT_RANGE - 1)

/* Batch range. */
#define	PRI_BATCH_RANGE		(PRI_TIMESHARE_RANGE - PRI_INTERACT_RANGE)
#define	PRI_MIN_BATCH		(PRI_MIN_TIMESHARE + PRI_INTERACT_RANGE)
#define	PRI_MAX_BATCH		PRI_MAX_TIMESHARE

/* Batch range separation. */ /* TODO : Hardcoded */
#define	SCHED_PRI_NRESV		(PRIO_MAX - PRIO_MIN)
#define	SCHED_PRI_NHALF		(SCHED_PRI_NRESV / 2)
#define	SCHED_PRI_MIN		(PRI_MIN_BATCH + 7)
#define	SCHED_PRI_MAX		(SCHED_PRI_MIN + 12)
#define	SCHED_PRI_RANGE		(SCHED_PRI_MAX - SCHED_PRI_MIN + 1)

/* Macros/defines used for stat computation. */
#define	SCHED_TICK_SHIFT	10	/* Used to avoid rounding errors. */
#define	SCHED_TICK_SECS		10	/* Number of secs for cpu stats. */
#define	SCHED_TICK_TARG		(HZ * SCHED_TICK_SECS)	/* 10s in ticks. */
#define	SCHED_TICK_MAX		(SCHED_TICK_TARG + HZ)
#define	SCHED_SLP_RUN_MAX	((HZ * 5) << SCHED_TICK_SHIFT)
#define	SCHED_INTERACT_MAX	(100)
#define	SCHED_INTERACT_HALF	(SCHED_INTERACT_MAX / 2)
#define	SCHED_INTERACT_THRESH	(30)

#define roundup(x, y) 		((((x)+((y)-1))/(y))*(y))
#define	SCHED_TICK_HZ(ts)	((ts)->ticks >> SCHED_TICK_SHIFT)
#define	SCHED_TICK_TOTAL(ts)	(max((ts)->ltick - (ts)->ftick, HZ))
#define	SCHED_PRI_TICKS(ts)						\
    (SCHED_TICK_HZ((ts)) /						\
    (roundup(SCHED_TICK_TOTAL((ts)), SCHED_PRI_RANGE) / SCHED_PRI_RANGE))
#define	SCHED_SLP_RUN_FORK	((HZ / 2) << SCHED_TICK_SHIFT)

/*
 * These parameters determine the slice behavior for batch work.
 */
#define	SCHED_SLICE_DEFAULT_DIVISOR	10	/* ~94 ms, 12 stathz ticks. */
#define	SCHED_SLICE_MIN_DIVISOR		6	/* DEFAULT/MIN = ~16 ms. */
#define	TDF_SLICEEND	0	/* TODO : find linux counterpart. */
#define TD_IS_IDLETHREAD(task)	false /* TODO : needed ? */

/*
 * Task states.
 */
#define TDS_INACTIVE	(2<<1)
#define TDS_INHIBITED	(2<<2)
#define TDS_CAN_RUN	(2<<3)
#define TDS_RUNQ	(2<<4)
#define TDS_RUNNING	(2<<5)

/* Flags from FreeBSD. */
#define SRQ_PREEMPTED 	(2<<1)

/* Locking stuff. */
#define TDQ_LOCK_ASSERT(tdq, flag)
#define THREAD_LOCK_ASSERT(td, flags)

/* Load balancing stuff */
#define THREAD_CAN_MIGRATE(td)	false /*TODO*/

/* Globals */
static int tickincr = 1 << SCHED_TICK_SHIFT;	/* 1 Should be correct. */
static int sched_interact = SCHED_INTERACT_THRESH;
static int sched_slice = 10;	/* reset during boot. */
static int sched_slice_min = 1;	/* reset during boot. */

unsigned int sysctl_ktz_enabled = 1; /* Enabled by default */

/* Helper macros / defines. */
#define LOG(...) 	printk_deferred(__VA_ARGS__)
#define KTZ_SE(p)	(&(p)->ktz_se)
#define PRINT(name)	printk_deferred(#name "\t\t = %d", name)
#define TDQ(rq)		(&(rq)->ktz)
#define RQ(tdq)		(container_of(tdq, struct rq, ktz))

//#define LOG(...) 	do{}while(0)

void init_ktz_tdq(struct ktz_tdq *ktz_tdq)
{
	INIT_LIST_HEAD(&ktz_tdq->queue);

	/* Init runqueues. */
	runq_init(&ktz_tdq->realtime);
	runq_init(&ktz_tdq->timeshare);
	runq_init(&ktz_tdq->idle);
	
	/* Print config. */
	PRINT(tickincr);
	PRINT(PRI_MIN_TIMESHARE);
	PRINT(PRI_MAX_TIMESHARE);
	PRINT(PRI_INTERACT_RANGE);
	PRINT(PRI_MIN_INTERACT);
	PRINT(PRI_MAX_INTERACT);
	PRINT(PRI_BATCH_RANGE);
	PRINT(PRI_MIN_BATCH);
	PRINT(PRI_MAX_BATCH);
	PRINT(SCHED_PRI_NRESV);
	PRINT(SCHED_PRI_NHALF);
	PRINT(SCHED_PRI_MIN);
	PRINT(SCHED_PRI_MAX);
	PRINT(SCHED_PRI_RANGE);
}

static void pctcpu_update(struct sched_ktz_entity *ts, bool run)
{
	int t = jiffies;

	if ((uint)(t - ts->ltick) >= SCHED_TICK_TARG) {
		ts->ticks = 0;
		ts->ftick = t - SCHED_TICK_TARG;
	}
	else if (t - ts->ftick >= SCHED_TICK_MAX) {
		ts->ticks = (ts->ticks / (ts->ltick - ts->ftick)) *
		    (ts->ltick - (t - SCHED_TICK_TARG));
		ts->ftick = t - SCHED_TICK_TARG;
	}
	if (run)
		ts->ticks += (t - ts->ltick) << SCHED_TICK_SHIFT;
	ts->ltick = t;
}

/*
 * This routine enforces a maximum limit on the amount of scheduling history
 * kept.  It is called after either the slptime or runtime is adjusted.  This
 * function is ugly due to integer math.
 */
static void interact_update(struct task_struct *p)
{
	u_int sum;
	struct sched_ktz_entity *ke_se = KTZ_SE(p);

	sum = ke_se->runtime + ke_se->slptime;
	if (sum < SCHED_SLP_RUN_MAX)
		return;
	/*
	 * This only happens from two places:
	 * 1) We have added an unusual amount of run time from fork_exit.
	 * 2) We have added an unusual amount of sleep time from sched_sleep().
	 */
	if (sum > SCHED_SLP_RUN_MAX * 2) {
		if (ke_se->runtime > ke_se->slptime) {
			ke_se->runtime = SCHED_SLP_RUN_MAX;
			ke_se->slptime = 1;
		} else {
			ke_se->slptime = SCHED_SLP_RUN_MAX;
			ke_se->runtime = 1;
		}
		return;
	}
	/*
	 * If we have exceeded by more than 1/5th then the algorithm below
	 * will not bring us back into range.  Dividing by two here forces
	 * us into the range of [4/5 * SCHED_INTERACT_MAX, SCHED_INTERACT_MAX]
	 */
	if (sum > (SCHED_SLP_RUN_MAX / 5) * 6) {
		ke_se->runtime /= 2;
		ke_se->slptime /= 2;
		return;
	}
	ke_se->runtime = (ke_se->runtime / 5) * 4;
	ke_se->slptime = (ke_se->slptime / 5) * 4;
}

static int interact_score(struct task_struct *p)
{
	int div;
	struct sched_ktz_entity *ktz_se = KTZ_SE(p);

	/*
	 * The score is only needed if this is likely to be an interactive
	 * task.  Don't go through the expense of computing it if there's
	 * no chance.
	 */
	if (sched_interact <= SCHED_INTERACT_HALF &&
		ktz_se->runtime >= ktz_se->slptime)
			return (SCHED_INTERACT_HALF);

	if (ktz_se->runtime > ktz_se->slptime) {
		div = max(1, ktz_se->runtime / SCHED_INTERACT_HALF);
		return (SCHED_INTERACT_HALF +
		    (SCHED_INTERACT_HALF - (ktz_se->slptime / div)));
	}
	if (ktz_se->slptime > ktz_se->runtime) {
		div = max(1, ktz_se->slptime / SCHED_INTERACT_HALF);
		return (ktz_se->runtime / div);
	}
	/* runtime == slptime */
	if (ktz_se->runtime)
		return (SCHED_INTERACT_HALF);

	/*
	 * This can happen if slptime and runtime are 0.
	 */
	return (0);

}

/*
 * Load is maintained for all threads RUNNING and ON_RUNQ.  Add the load
 * for this thread to the referenced thread queue.
 */
static void tdq_load_add(struct ktz_tdq *tdq, struct task_struct *p)
{
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	THREAD_LOCK_ASSERT(p, MA_OWNED);

	tdq->load++;
	//if ((td->td_flags & TDF_NOLOAD) == 0) /* We probably dont care. */
	tdq->sysload++;
}

/*
 * Remove the load from a thread that is transitioning to a sleep state or
 * exiting.
 */
static void
tdq_load_rem(struct ktz_tdq *tdq, struct task_struct *p)
{
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	THREAD_LOCK_ASSERT(p, MA_OWNED);

	tdq->load--;
	//if ((td->td_flags & TDF_NOLOAD) == 0) /* We probably dont care. */
	tdq->sysload--;
}

/*
 * Bound timeshare latency by decreasing slice size as load increases.  We
 * consider the maximum latency as the sum of the threads waiting to run
 * aside from curthread and target no more than sched_slice latency but
 * no less than sched_slice_min runtime.
 */
static inline int compute_slice(struct ktz_tdq *tdq) 
{
	int load = tdq->sysload - 1;
	if (load >= SCHED_SLICE_MIN_DIVISOR)
		return (sched_slice_min);
	if (load <= 1)
		return (sched_slice);
	return (sched_slice / load);
}

/*
 * Scale the scheduling priority according to the "interactivity" of this
 * process.
 */
static void compute_priority(struct task_struct *p)
{
	int score;
	int pri;
	struct sched_ktz_entity *ktz_se = KTZ_SE(p);

	/*
	 * If the score is interactive we place the thread in the realtime
	 * queue with a priority that is less than kernel and interrupt
	 * priorities.  These threads are not subject to nice restrictions.
	 *
	 * Scores greater than this are placed on the normal timeshare queue
	 * where the priority is partially decided by the most recent cpu
	 * utilization and the rest is decided by nice value.
	 *
	 * The nice value of the process has a linear effect on the calculated
	 * score.  Negative nice values make it easier for a thread to be
	 * considered interactive.
	 */
	score = max(0, interact_score(p) + task_nice(p));
	if (score < sched_interact) {
		pri = PRI_MIN_INTERACT;
		pri += ((PRI_MAX_INTERACT - PRI_MIN_INTERACT + 1) / sched_interact) * score;
	} else {
		pri = SCHED_PRI_MIN;
		if (ktz_se->ticks) {
			int d;
			d = min(SCHED_PRI_TICKS(ktz_se), SCHED_PRI_RANGE - 1);
			if (d < 0) {
				BUG();
			}
			pri += d;
		}
		pri += (int)((40 / 120) * task_nice(p));
	}

	/* Test : */
	p->ktz_prio = pri;
	ktz_se->base_user_pri = pri;
	if (ktz_se->lend_user_pri <= pri)
		return;
	ktz_se->user_pri = pri;
}

/*
 * Add a thread to the actual run-queue.  Keeps transferable counts up to
 * date with what is actually on the run-queue.  Selects the correct
 * queue position for timeshare threads.
 */
static inline void tdq_runq_add(struct ktz_tdq *tdq, struct task_struct *td, int flags)
{
	struct sched_ktz_entity *ts =  KTZ_SE(td);
	struct runq *dest;
	u_char pri;

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	THREAD_LOCK_ASSERT(td, MA_OWNED);

	pri = td->ktz_prio;
	ts->state = TDS_RUNQ;	
	if (THREAD_CAN_MIGRATE(td)) {
		tdq->transferable++;
		// TODO
		//ts->flags |= TSF_XFERABLE;
	}
	if (pri < PRI_MIN_BATCH) {
		dest = &tdq->realtime;
	}
	else if (pri <= PRI_MAX_BATCH) {
		dest = &tdq->timeshare;
		if ((flags & SRQ_PREEMPTED) == 0) {
			pri = KTZ_HEADS_PER_RUNQ * (pri - PRI_MIN_BATCH) / PRI_BATCH_RANGE;
			pri = (pri + tdq->idx) % KTZ_HEADS_PER_RUNQ;
			if (tdq->ridx != tdq->idx && pri == tdq->ridx)
				pri = (unsigned char)(pri - 1) % KTZ_HEADS_PER_RUNQ;
		}
		else {
			pri = tdq->ridx;
		}
		runq_add_pri(dest, td, pri, flags);
		return;
	}
	else {
		/* Should never happen. */
		dest = &tdq->idle;
	}
	runq_add(dest, td, flags);
}

static void tdq_add(struct ktz_tdq *tdq, struct task_struct *p, int flags)
{
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	if (p->ktz_prio < tdq->lowpri)
		tdq->lowpri = p->ktz_prio;
	tdq_runq_add(tdq, p, flags);
	tdq_load_add(tdq, p);
}

/* 
 * Remove a thread from a run-queue.  This typically happens when a thread
 * is selected to run.  Running threads are not on the queue and the
 * transferable count does not reflect them.
 */
static inline void tdq_runq_rem(struct ktz_tdq *tdq, struct task_struct *td)
{
	struct sched_ktz_entity *ts = KTZ_SE(td);
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	// TODO
	/*if (ts->ts_flags & TSF_XFERABLE) {
		tdq->tdq_transferable--;
		ts->ts_flags &= ~TSF_XFERABLE;
	}*/
	if (ts->curr_runq == &tdq->timeshare) {
		if (tdq->idx != tdq->ridx)
			runq_remove_idx(ts->curr_runq, td, &tdq->ridx);
		else
			runq_remove_idx(ts->curr_runq, td, NULL);
	} 
	else {
		runq_remove(ts->curr_runq, td);
	}
}

/*
 * Pick the highest priority task we have and return it.
 */
static struct task_struct *tdq_choose(struct ktz_tdq *tdq)
{
	struct task_struct *td;

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	td = runq_choose(&tdq->realtime);
	if (td != NULL) {
		return (td);
	}
	td = runq_choose_from(&tdq->timeshare, tdq->ridx);
	if (td != NULL) {
		return td;
	}
	td = runq_choose(&tdq->idle);
	if (td != NULL) {
		return td;
	}
	return NULL;
}

struct task_struct *sched_choose(struct rq *rq)
{
	struct task_struct *td;
	struct ktz_tdq *tdq = TDQ(rq);

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	td = tdq_choose(tdq);
	if (td) {
		tdq_runq_rem(tdq, td);
		tdq->lowpri = td->ktz_prio;
		return td;
	}
	//tdq->lowpri = PRI_MAX_IDLE;
	tdq->lowpri = MAX_KTZ_PRIO;
	return NULL;
}

void sched_fork_thread(struct task_struct *td, struct task_struct *child)
{
	struct sched_ktz_entity *ts;
	struct sched_ktz_entity *ts2;
	struct ktz_tdq *tdq;

	tdq = &this_rq()->ktz;
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	ts = KTZ_SE(td);
	ts2 = KTZ_SE(child);
	/*child->td_oncpu = NOCPU;
	child->td_lastcpu = NOCPU;
	child->td_lock = TDQ_LOCKPTR(tdq);
	child->td_cpuset = cpuset_ref(td->td_cpuset);
	ts2->ts_cpu = ts->ts_cpu;
	ts2->ts_flags = 0;*/
	/*
	 * Grab our parents cpu estimation information.
	 */
	ts2->ticks = ts->ticks;
	ts2->ltick = ts->ltick;
	ts2->ftick = ts->ftick;
	/*
	 * Do not inherit any borrowed priority from the parent.
	 */
	//child->td_priority = child->td_base_pri;
	/*
	 * And update interactivity score.
	 */
	ts2->slptime = ts->slptime;
	ts2->runtime = ts->runtime;
	/* Attempt to quickly learn interactivity. */
	ts2->slice = compute_slice(tdq) - sched_slice_min;
}

static void sched_interact_fork(struct task_struct *td)
{
	struct sched_ktz_entity *ts;
	int ratio;
	int sum;

	ts = KTZ_SE(td);
	sum = ts->runtime + ts->slptime;
	if (sum > SCHED_SLP_RUN_FORK) {
		ratio = sum / SCHED_SLP_RUN_FORK;
		ts->runtime /= ratio;
		ts->slptime /= ratio;
	}
}
 
static inline bool is_enqueued(struct task_struct *p)
{
	return KTZ_SE(p)->curr_runq;
}

/*
 * Set lowpri to its exact value by searching the run-queue and
 * evaluating curthread.  curthread may be passed as an optimization.
 */
static void tdq_setlowpri(struct ktz_tdq *tdq, struct task_struct *ctd)
{
	struct task_struct *td;
	struct rq *rq = RQ(tdq);

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	if (ctd == NULL)
		ctd = rq->curr;
	td = tdq_choose(tdq);
	if (td == NULL || td->ktz_prio > ctd->ktz_prio)
		tdq->lowpri = ctd->ktz_prio;
	else
		tdq->lowpri = td->ktz_prio;
}

static void sched_thread_priority(struct ktz_tdq *tdq, struct task_struct *td, int prio)
{
	struct rq *rq = RQ(tdq);
	struct sched_ktz_entity *ts = KTZ_SE(td);
	int oldpri;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	if (td->ktz_prio == prio)
		return;
	/*
	 * If the priority has been elevated due to priority
	 * propagation, we may have to move ourselves to a new
	 * queue.  This could be optimized to not re-add in some
	 * cases.
	 */
	if (is_enqueued(td) && prio < td->ktz_prio) {
		tdq_runq_rem(tdq, td);
		td->ktz_prio = prio;
		tdq_add(tdq, td, 0);
		return;
	}
	/*
	 * If the thread is currently running we may have to adjust the lowpri
	 * information so other cpus are aware of our current priority.
	 */
	if (task_curr(td)) {
		//tdq = TDQ_CPU(ts->ts_cpu); TODO : CAREFUL WITH SMP !
		oldpri = td->ktz_prio;
		td->ktz_prio = prio;
		if (prio < tdq->lowpri)
			tdq->lowpri = prio;
		else if (tdq->lowpri == oldpri)
			tdq_setlowpri(tdq, td);
		return;
	}
	td->ktz_prio = prio;
}

static inline struct task_struct *ktz_task_of(struct sched_ktz_entity *ktz_se)
{
	return container_of(ktz_se, struct task_struct, ktz_se);
}

static inline void print_stats(struct task_struct *p)
{
	struct sched_ktz_entity *kse = KTZ_SE(p);
	unsigned long long st = kse->slptime >> SCHED_TICK_SHIFT;
	unsigned long long rt = kse->runtime >> SCHED_TICK_SHIFT;
	int interact = interact_score(p);
	LOG("Task %d : ", p->pid);
	LOG("\t| slptime\t\t= %llu ms", st);
	LOG("\t| runtime\t\t= %llu ms", rt);
	LOG("\t| interact\t\t= %d", interact);
	LOG("\t| ticks\t\t= %d", kse->ticks);
	LOG("\t| lticks\t\t= %d", kse->ltick);
	LOG("\t| fticks\t\t= %d", kse->ftick);
}


static inline void runq_print(struct runq *q)
{
	int i;
	struct sched_ktz_entity *pos;
	struct task_struct *t;

	for (i = 0; i < KTZ_HEADS_PER_RUNQ; ++i) {
		if (!list_empty(&q->queues[i])) {
			list_for_each_entry(pos, &q->queues[i], runq) {
				t = ktz_task_of(pos);
				LOG("\t_ %d", t->pid);
			}
		}
	}
}

static inline void print_tdq(struct ktz_tdq *tdq)
{
	LOG("##################\n");
	LOG("tdq %p\n", tdq);
	LOG("idx : %d", tdq->idx);
	LOG("ridx : %d", tdq->ridx);
	LOG("Realtime runq :\n");
	runq_print(&tdq->realtime);
	LOG("Timeshare runq :\n");
	runq_print(&tdq->timeshare);
	LOG("Idle runq :\n");
	runq_print(&tdq->idle);
	LOG("##################\n");
}	

static inline void print_children(struct task_struct *p)
{
	struct task_struct *pos;
	struct list_head *head;
	head = &(p->children);

	LOG("Children of %d\n", p->pid); 
	if (list_empty(head)) {
		LOG("\tnone");
	}
	else {
		list_for_each_entry(pos, head, sibling) {
			LOG("\t%d\n", pos->pid);
		}
	}
}

static void enqueue_task_ktz(struct rq *rq, struct task_struct *p, int flags)
{
	struct ktz_tdq *tdq = TDQ(rq);
	struct sched_ktz_entity *ktz_se = KTZ_SE(p);
	struct list_head *queue = &rq->ktz.queue;

	add_nr_running(rq,1);
	if (p->ktz_prio == 0)
		p->ktz_prio = p->prio;
	if (flags & ENQUEUE_WAKEUP) {
		/* Count sleeping ticks. */
		ktz_se->slptime += (jiffies - ktz_se->slptick) << SCHED_TICK_SHIFT;
		ktz_se->slptick = 0;
		interact_update(p);
		pctcpu_update(ktz_se, false);
	}
	ktz_se->slice = 0;
	tdq_add(tdq, p, 0);
}

static void dequeue_task_ktz(struct rq *rq, struct task_struct *p, int flags)
{
	struct ktz_tdq *tdq = TDQ(rq);
	struct sched_ktz_entity *ktz_se = KTZ_SE(p);

	sub_nr_running(rq,1);
	if (flags & DEQUEUE_SLEEP) {
		ktz_se->slptick = jiffies;
	}
	//list_del_init(&ktz_se->run_list);
	tdq_runq_rem(tdq, p);
	ktz_se->curr_runq = NULL;
	tdq_load_rem(tdq, p);
	if (p->ktz_prio == tdq->lowpri)
		tdq_setlowpri(tdq, NULL);
}

static void yield_task_ktz(struct rq *rq)
{
	/* No neeed to renqueue here as we will do it in put_prev_task. */
}

/*
 * Very simplified version.
 */
static void check_preempt_curr_ktz(struct rq *rq, struct task_struct *p, int flags)
{
	int pri = p->ktz_prio;
	int cpri = rq->curr->ktz_prio;

	if (pri < cpri)
		resched_curr(rq);

	// TODO : Add when adding SMP support. ?
	/*if (remote && pri <= PRI_MAX_INTERACT && cpri > PRI_MAX_INTERACT)
		return (1);*/
}

static struct task_struct *pick_next_task_ktz(struct rq *rq, struct task_struct* prev, struct pin_cookie cookie)
{
	struct ktz_tdq *tdq = TDQ(rq);
	struct sched_ktz_entity *next;
	struct task_struct *next_task;
	struct task_struct *next_ule;

	put_prev_task(rq, prev);
	next_task = tdq_choose(tdq);
	return next_task;
}

static void put_prev_task_ktz(struct rq *rq, struct task_struct *prev)
{
	struct ktz_tdq *tdq = TDQ(rq);

	if (is_enqueued(prev)) {
		tdq_runq_rem(tdq, prev);
		tdq_runq_add(tdq, prev, 0);
	}
}

static void set_curr_task_ktz(struct rq *rq)
{
}

static void task_tick_ktz(struct rq *rq, struct task_struct *curr, int queued)
{
	struct ktz_tdq *tdq = TDQ(rq);
	struct sched_ktz_entity *ktz_se = KTZ_SE(curr);

	tdq->oldswitchcnt = tdq->switchcnt;
	tdq->switchcnt = tdq->load;

	/*
	 * Advance the insert index once for each tick to ensure that all
	 * threads get a chance to run.
	 */
	if (tdq->idx == tdq->ridx) {
		tdq->idx = (tdq->idx + 1) % KTZ_HEADS_PER_RUNQ;
		if (list_empty(&tdq->timeshare.queues[tdq->ridx]))
			tdq->ridx = tdq->idx;
	}

	/* Update CPU stats. */
	pctcpu_update(ktz_se, true);

	/* Account runtime. */
	ktz_se->runtime += tickincr;
	interact_update(curr);
	compute_priority(curr);

	if (!TD_IS_IDLETHREAD(curr) && ++ktz_se->slice >= compute_slice(tdq)) {
		ktz_se->slice = 0;
		//ktz_se->flags |= TDF_SLICEEND;
		resched_curr(rq);
	}
}

static void task_fork_ktz(struct task_struct *p)
{
	struct task_struct *child = p;
	struct task_struct *parent = p->parent;
	struct sched_ktz_entity *cktz_se = KTZ_SE(child);
	struct sched_ktz_entity *pktz_se = KTZ_SE(parent);

	/* Update parent stats. */
	pctcpu_update(pktz_se, task_curr(parent));	
	sched_fork_thread(parent, child);

	sched_interact_fork(child);
	compute_priority(child);

	pktz_se->runtime += tickincr;
	interact_update(parent);
	compute_priority(parent);
}

static void task_dead_ktz(struct task_struct *p)
{
}

static void switched_from_ktz(struct rq *rq, struct task_struct *p)
{
}

static void switched_to_ktz(struct rq *rq, struct task_struct *p)
{
}

static void prio_changed_ktz(struct rq*rq, struct task_struct *p, int oldprio)
{
	sched_thread_priority(TDQ(rq), p, p->prio);
}

static unsigned int get_rr_interval_ktz(struct rq* rq, struct task_struct *p)
{
	return 0;
}
#ifdef CONFIG_SMP
static inline int select_task_rq_ktz(struct task_struct *p, int cpu, int sd_flags, int wake_flags)
{
	int new_cpu = smp_processor_id();
	
	return new_cpu; //set assigned CPU to zero
}


static void set_cpus_allowed_ktz(struct task_struct *p,  const struct cpumask *new_mask)
{
}
#endif

static void update_curr_ktz(struct rq*rq)
{
}

const struct sched_class ktz_sched_class = {
	.next			= &idle_sched_class,
	.enqueue_task		= enqueue_task_ktz,
	.dequeue_task		= dequeue_task_ktz,
	.yield_task		= yield_task_ktz,

	.check_preempt_curr	= check_preempt_curr_ktz,
	
	.pick_next_task		= pick_next_task_ktz,
	.put_prev_task		= put_prev_task_ktz,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_ktz,
	.set_cpus_allowed	= set_cpus_allowed_ktz,
#endif

	.set_curr_task		= set_curr_task_ktz,
	.task_tick		= task_tick_ktz,
	.task_fork		= task_fork_ktz,
	.task_dead		= task_dead_ktz,

	.switched_from		= switched_from_ktz,
	.switched_to		= switched_to_ktz,
	.prio_changed		= prio_changed_ktz,

	.get_rr_interval	= get_rr_interval_ktz,
	.update_curr		= update_curr_ktz,
};


