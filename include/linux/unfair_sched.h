#ifndef _LINUX_UNFAIR_SCHED_H
#define _LINUX_UNFAIR_SCHED_H

#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>

#define UNFAIR_SCHED_VERSION		"v1.0.0-sun"
#define UNFAIR_SCHED_MAGIC		0x53554E55

#define MAX_FAVORED_TASKS		256
#define FAVOR_BOOST_DEFAULT		3
#define FAVOR_BOOST_MAX			10

enum unfair_priority_level {
	UNFAIR_PRIO_LOWEST = 0,
	UNFAIR_PRIO_BACKGROUND,
	UNFAIR_PRIO_NORMAL,
	UNFAIR_PRIO_FOREGROUND,
	UNFAIR_PRIO_TOP_APP,
	UNFAIR_PRIO_REALTIME,
	UNFAIR_PRIO_MAX
};

struct favored_task_info {
	pid_t				pid;
	uid_t				uid;
	int				boost_level;
	enum unfair_priority_level	prio_level;
	unsigned long			last_used_time;
	unsigned long			total_runtime_boosted;
	int				is_active;
};

struct unfair_sched_data {
	spinlock_t			lock;
	int				enabled;
	int				boost_scale;
	int				favor_count;
	struct favored_task_info	favored_tasks[MAX_FAVORED_TASKS];
	unsigned long			sched_latency_ns;
	unsigned long			min_granularity_ns;
	unsigned long			wakeup_granularity_ns;
	unsigned long			migration_cost_ns;
	int				top_app_cpu_boost;
	int				foreground_cpu_boost;
	int				idle_cull_threshold_ms;
	bool				prefer_big_core_fg;
	bool				wakeup_preempt_aggressive;
	struct hrtimer			idle_cull_timer;
};

#ifdef CONFIG_UNFAIR_SCHED

extern struct unfair_sched_data unfair_sched_data;

extern int unfair_sched_init(void);
extern int unfair_sched_add_task(pid_t pid, int boost_level);
extern int unfair_sched_remove_task(pid_t pid);
extern int unfair_sched_set_boost(pid_t pid, int boost_level);
extern int unfair_sched_get_boost(pid_t pid);
extern int unfair_sched_is_favored(struct task_struct *p);
extern int unfair_sched_boost(struct task_struct *p);
extern void unfair_sched_update_foreground(pid_t pid);
extern void unfair_sched_update_top_app(pid_t pid);
extern unsigned long unfair_sched_adjust_slice(struct task_struct *p,
					       unsigned long base_slice);
extern int unfair_sched_adjust_wakeup_gran(struct task_struct *p);
extern int unfair_sched_should_preempt(struct task_struct *curr,
				       struct task_struct *p);
extern int unfair_sched_select_cpu(struct task_struct *p, int prev_cpu,
				   const struct cpumask *cpumask);
extern unsigned long unfair_sched_adjust_load(struct task_struct *p,
					      unsigned long load);
#else

static inline int unfair_sched_init(void) { return 0; }
static inline int unfair_sched_add_task(pid_t pid, int boost_level) { return -ENODEV; }
static inline int unfair_sched_remove_task(pid_t pid) { return -ENODEV; }
static inline int unfair_sched_set_boost(pid_t pid, int boost_level) { return -ENODEV; }
static inline int unfair_sched_get_boost(pid_t pid) { return 0; }
static inline int unfair_sched_is_favored(struct task_struct *p) { return 0; }
static inline int unfair_sched_boost(struct task_struct *p) { return 0; }
static inline void unfair_sched_update_foreground(pid_t pid) { }
static inline void unfair_sched_update_top_app(pid_t pid) { }
static inline unsigned long unfair_sched_adjust_slice(struct task_struct *p,
						      unsigned long base_slice)
{ return base_slice; }
static inline int unfair_sched_adjust_wakeup_gran(struct task_struct *p)
{ return 0; }
static inline int unfair_sched_should_preempt(struct task_struct *curr,
					      struct task_struct *p)
{ return 0; }
static inline int unfair_sched_select_cpu(struct task_struct *p, int prev_cpu,
					  const struct cpumask *cpumask)
{ return prev_cpu; }
static inline unsigned long unfair_sched_adjust_load(struct task_struct *p,
					     unsigned long load)
{ return load; }

#endif

#endif
