#ifndef _linux_POSIX_TIMERS_H
#define _linux_POSIX_TIMERS_H

#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/timex.h>
#include <linux/alarmtimer.h>

struct siginfo;

struct cpu_timer_list {
	struct list_head entry;
	u64 expires, incr;
	struct task_struct *task;
	int firing;
};

/*
 * Bit fields within a clockid:
 *
 * The most significant 29 bits hold either a pid or a file descriptor.
 *
 * Bit 2 indicates whether a cpu clock refers to a thread or a process.
 *
 * Bits 1 and 0 give the type: PROF=0, VIRT=1, SCHED=2, or FD=3.
 *
 * A clockid is invalid if bits 2, 1, and 0 are all set.
 */
#define CPUCLOCK_PID(clock)		((pid_t) ~((clock) >> 3))
#define CPUCLOCK_PERTHREAD(clock) \
	(((clock) & (clockid_t) CPUCLOCK_PERTHREAD_MASK) != 0)

#define CPUCLOCK_PERTHREAD_MASK	4
#define CPUCLOCK_WHICH(clock)	((clock) & (clockid_t) CPUCLOCK_CLOCK_MASK)
#define CPUCLOCK_CLOCK_MASK	3
#define CPUCLOCK_PROF		0
#define CPUCLOCK_VIRT		1
#define CPUCLOCK_SCHED		2
#define CPUCLOCK_MAX		3
#define CLOCKFD			CPUCLOCK_MAX
#define CLOCKFD_MASK		(CPUCLOCK_PERTHREAD_MASK|CPUCLOCK_CLOCK_MASK)

#define MAKE_PROCESS_CPUCLOCK(pid, clock) \
	((~(clockid_t) (pid) << 3) | (clockid_t) (clock))
#define MAKE_THREAD_CPUCLOCK(tid, clock) \
	MAKE_PROCESS_CPUCLOCK((tid), (clock) | CPUCLOCK_PERTHREAD_MASK)

#define FD_TO_CLOCKID(fd)	((~(clockid_t) (fd) << 3) | CLOCKFD)
#define CLOCKID_TO_FD(clk)	((unsigned int) ~((clk) >> 3))

/* POSIX.1b interval timer structure. */
struct k_itimer {
	struct list_head list;		/* free/ allocate list */
	struct hlist_node t_hash;
	spinlock_t it_lock;
	clockid_t it_clock;		/* which timer type */
	timer_t it_id;			/* timer id */
	s64 it_overrun;			/* overrun on pending signal  */
	s64 it_overrun_last;		/* overrun on last delivered signal */
	int it_requeue_pending;		/* waiting to requeue this timer */
#define REQUEUE_PENDING 1
	int it_sigev_notify;		/* notify word of sigevent struct */
	struct signal_struct *it_signal;
	union {
		struct pid *it_pid;	/* pid of process to send signal to */
		struct task_struct *it_process;	/* for clock_nanosleep */
	};
	struct sigqueue *sigq;		/* signal queue entry. */
	union {
		struct {
			struct hrtimer timer;
			ktime_t interval;
		} real;
		struct cpu_timer_list cpu;
		struct {
			unsigned int clock;
			unsigned int node;
			unsigned long incr;
			unsigned long expires;
		} mmtimer;
		struct {
			struct alarm alarmtimer;
			ktime_t interval;
		} alarm;
		struct rcu_head rcu;
	} it;
};

void run_posix_cpu_timers(struct task_struct *task);
void posix_cpu_timers_exit(struct task_struct *task);
void posix_cpu_timers_exit_group(struct task_struct *task);
void set_process_cpu_timer(struct task_struct *task, unsigned int clock_idx,
			   u64 *newval, u64 *oldval);

long clock_nanosleep_restart(struct restart_block *restart_block);

void update_rlimit_cpu(struct task_struct *task, unsigned long rlim_new);

void do_schedule_next_timer(struct siginfo *info);

#endif
