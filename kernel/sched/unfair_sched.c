/*
 * Unfair Scheduler for Android
 * Inspired by vivo OriginOS Unfair Scheduling
 *
 * Prioritizes foreground and important tasks to improve
 * perceived smoothness and reduce jank.
 *
 * For REDMI K80 Pro (miro) - SM8750
 */

#include <linux/unfair_sched.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sysctl.h>
#include <linux/topology.h>
#include <linux/cpumask.h>
#include <linux/sched/cpufreq.h>

#define CREATE_TRACE_POINTS
#undef TRACE_SYSTEM
#define TRACE_SYSTEM unfair_sched

struct unfair_sched_data unfair_sched_data;
EXPORT_SYMBOL_GPL(unfair_sched_data);

static pid_t current_top_app_pid = -1;
static pid_t current_foreground_pid = -1;

static inline struct favored_task_info *find_favored_task_locked(pid_t pid)
{
	int i;

	for (i = 0; i < MAX_FAVORED_TASKS; i++) {
		if (unfair_sched_data.favored_tasks[i].is_active &&
		    unfair_sched_data.favored_tasks[i].pid == pid)
			return &unfair_sched_data.favored_tasks[i];
	}
	return NULL;
}

static inline struct favored_task_info *find_free_slot_locked(void)
{
	int i;

	for (i = 0; i < MAX_FAVORED_TASKS; i++) {
		if (!unfair_sched_data.favored_tasks[i].is_active)
			return &unfair_sched_data.favored_tasks[i];
	}
	return NULL;
}

int unfair_sched_add_task(pid_t pid, int boost_level)
{
	struct favored_task_info *info;
	unsigned long flags;

	if (pid <= 0 || boost_level < 0 || boost_level > FAVOR_BOOST_MAX)
		return -EINVAL;

	spin_lock_irqsave(&unfair_sched_data.lock, flags);

	info = find_favored_task_locked(pid);
	if (info) {
		info->boost_level = boost_level;
		info->last_used_time = jiffies;
		spin_unlock_irqrestore(&unfair_sched_data.lock, flags);
		return 0;
	}

	info = find_free_slot_locked();
	if (!info) {
		spin_unlock_irqrestore(&unfair_sched_data.lock, flags);
		return -ENOSPC;
	}

	memset(info, 0, sizeof(*info));
	info->pid = pid;
	info->boost_level = boost_level;
	info->prio_level = UNFAIR_PRIO_NORMAL;
	info->last_used_time = jiffies;
	info->is_active = 1;
	unfair_sched_data.favor_count++;

	spin_unlock_irqrestore(&unfair_sched_data.lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(unfair_sched_add_task);

int unfair_sched_remove_task(pid_t pid)
{
	struct favored_task_info *info;
	unsigned long flags;

	spin_lock_irqsave(&unfair_sched_data.lock, flags);

	info = find_favored_task_locked(pid);
	if (!info) {
		spin_unlock_irqrestore(&unfair_sched_data.lock, flags);
		return -ENOENT;
	}

	info->is_active = 0;
	info->pid = 0;
	unfair_sched_data.favor_count--;

	spin_unlock_irqrestore(&unfair_sched_data.lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(unfair_sched_remove_task);

int unfair_sched_set_boost(pid_t pid, int boost_level)
{
	struct favored_task_info *info;
	unsigned long flags;

	if (boost_level < 0 || boost_level > FAVOR_BOOST_MAX)
		return -EINVAL;

	spin_lock_irqsave(&unfair_sched_data.lock, flags);

	info = find_favored_task_locked(pid);
	if (!info) {
		spin_unlock_irqrestore(&unfair_sched_data.lock, flags);
		return -ENOENT;
	}

	info->boost_level = boost_level;

	spin_unlock_irqrestore(&unfair_sched_data.lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(unfair_sched_set_boost);

int unfair_sched_get_boost(pid_t pid)
{
	struct favored_task_info *info;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&unfair_sched_data.lock, flags);

	info = find_favored_task_locked(pid);
	if (info)
		ret = info->boost_level;

	spin_unlock_irqrestore(&unfair_sched_data.lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(unfair_sched_get_boost);

int unfair_sched_is_favored(struct task_struct *p)
{
	struct favored_task_info *info;
	unsigned long flags;
	int ret = 0;

	if (!unfair_sched_data.enabled)
		return 0;

	spin_lock_irqsave(&unfair_sched_data.lock, flags);

	info = find_favored_task_locked(p->pid);
	if (info) {
		ret = 1;
		info->last_used_time = jiffies;
	}

	spin_unlock_irqrestore(&unfair_sched_data.lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(unfair_sched_is_favored);

int unfair_sched_boost(struct task_struct *p)
{
	struct favored_task_info *info;
	unsigned long flags;
	int boost = 0;

	if (!unfair_sched_data.enabled)
		return 0;

	spin_lock_irqsave(&unfair_sched_data.lock, flags);

	info = find_favored_task_locked(p->pid);
	if (info) {
		boost = info->boost_level * unfair_sched_data.boost_scale;
		info->last_used_time = jiffies;
	}

	if (p->pid == current_top_app_pid)
		boost = max(boost, FAVOR_BOOST_MAX * unfair_sched_data.boost_scale);
	else if (p->pid == current_foreground_pid)
		boost = max(boost, (FAVOR_BOOST_MAX - 2) * unfair_sched_data.boost_scale);

	spin_unlock_irqrestore(&unfair_sched_data.lock, flags);
	return boost;
}
EXPORT_SYMBOL_GPL(unfair_sched_boost);

void unfair_sched_update_foreground(pid_t pid)
{
	unsigned long flags;

	spin_lock_irqsave(&unfair_sched_data.lock, flags);
	current_foreground_pid = pid;
	spin_unlock_irqrestore(&unfair_sched_data.lock, flags);
}
EXPORT_SYMBOL_GPL(unfair_sched_update_foreground);

void unfair_sched_update_top_app(pid_t pid)
{
	unsigned long flags;

	spin_lock_irqsave(&unfair_sched_data.lock, flags);
	current_top_app_pid = pid;
	spin_unlock_irqrestore(&unfair_sched_data.lock, flags);
}
EXPORT_SYMBOL_GPL(unfair_sched_update_top_app);

unsigned long unfair_sched_adjust_slice(struct task_struct *p,
					unsigned long base_slice)
{
	int boost = unfair_sched_boost(p);
	unsigned long adjusted_slice;

	if (!boost)
		return base_slice;

	adjusted_slice = base_slice + (base_slice * boost / 10);
	return adjusted_slice;
}
EXPORT_SYMBOL_GPL(unfair_sched_adjust_slice);

int unfair_sched_adjust_wakeup_gran(struct task_struct *p)
{
	int boost = unfair_sched_boost(p);

	if (!boost)
		return 0;

	return boost * 100000;
}
EXPORT_SYMBOL_GPL(unfair_sched_adjust_wakeup_gran);

int unfair_sched_should_preempt(struct task_struct *curr,
				struct task_struct *p)
{
	int curr_boost = unfair_sched_boost(curr);
	int p_boost = unfair_sched_boost(p);

	if (p_boost > curr_boost + 2)
		return 1;

	return 0;
}
EXPORT_SYMBOL_GPL(unfair_sched_should_preempt);

int unfair_sched_select_cpu(struct task_struct *p, int prev_cpu,
			    const struct cpumask *cpumask)
{
	int boost = unfair_sched_boost(p);
	int cpu = prev_cpu;

	if (!unfair_sched_data.enabled || !boost)
		return prev_cpu;

	if (unfair_sched_data.prefer_big_core_fg && boost >= FAVOR_BOOST_MAX / 2) {
		int big_cpu = cpumask_last(cpu_top_mask());
		if (cpumask_test_cpu(big_cpu, cpumask))
			cpu = big_cpu;
	}

	return cpu;
}
EXPORT_SYMBOL_GPL(unfair_sched_select_cpu);

unsigned long unfair_sched_adjust_load(struct task_struct *p,
				       unsigned long load)
{
	int boost = unfair_sched_boost(p);

	if (!boost)
		return load;

	return load + (load * boost / 5);
}
EXPORT_SYMBOL_GPL(unfair_sched_adjust_load);

#ifdef CONFIG_PROC_FS

static int unfair_sched_proc_show(struct seq_file *m, void *v)
{
	int i;
	unsigned long flags;

	seq_printf(m, "=== Unfair Scheduler Status ===\n");
	seq_printf(m, "Version: %s\n", UNFAIR_SCHED_VERSION);
	seq_printf(m, "Enabled: %d\n", unfair_sched_data.enabled);
	seq_printf(m, "Boost Scale: %d\n", unfair_sched_data.boost_scale);
	seq_printf(m, "Favored Tasks: %d / %d\n",
		   unfair_sched_data.favor_count, MAX_FAVORED_TASKS);
	seq_printf(m, "Top App PID: %d\n", current_top_app_pid);
	seq_printf(m, "Foreground PID: %d\n", current_foreground_pid);
	seq_printf(m, "Sched Latency ns: %lu\n", unfair_sched_data.sched_latency_ns);
	seq_printf(m, "Min Granularity ns: %lu\n", unfair_sched_data.min_granularity_ns);
	seq_printf(m, "Wakeup Granularity ns: %lu\n",
		   unfair_sched_data.wakeup_granularity_ns);
	seq_printf(m, "Top App CPU Boost: %d\n", unfair_sched_data.top_app_cpu_boost);
	seq_printf(m, "Foreground CPU Boost: %d\n",
		   unfair_sched_data.foreground_cpu_boost);
	seq_printf(m, "Prefer Big Core FG: %d\n",
		   unfair_sched_data.prefer_big_core_fg);
	seq_printf(m, "\n=== Favored Tasks ===\n");
	seq_printf(m, "%-8s %-8s %-8s %-12s %-16s\n",
		   "PID", "UID", "BOOST", "PRIO_LEVEL", "LAST_USED");

	spin_lock_irqsave(&unfair_sched_data.lock, flags);

	for (i = 0; i < MAX_FAVORED_TASKS; i++) {
		if (unfair_sched_data.favored_tasks[i].is_active) {
			seq_printf(m, "%-8d %-8d %-8d %-12d %-16lu\n",
				   unfair_sched_data.favored_tasks[i].pid,
				   unfair_sched_data.favored_tasks[i].uid,
				   unfair_sched_data.favored_tasks[i].boost_level,
				   unfair_sched_data.favored_tasks[i].prio_level,
				   unfair_sched_data.favored_tasks[i].last_used_time);
		}
	}

	spin_unlock_irqrestore(&unfair_sched_data.lock, flags);
	return 0;
}

static int unfair_sched_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, unfair_sched_proc_show, NULL);
}

static ssize_t unfair_sched_proc_write(struct file *file,
				       const char __user *buf,
			       size_t count, loff_t *ppos)
{
	char kbuf[64];
	int val;

	if (count >= sizeof(kbuf))
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (sscanf(kbuf, "enabled=%d", &val) == 1) {
		unfair_sched_data.enabled = val ? 1 : 0;
		pr_info("unfair_sched: enabled=%d\n", unfair_sched_data.enabled);
	} else if (sscanf(kbuf, "boost_scale=%d", &val) == 1) {
		if (val >= 1 && val <= 5) {
			unfair_sched_data.boost_scale = val;
			pr_info("unfair_sched: boost_scale=%d\n",
				unfair_sched_data.boost_scale);
		}
	} else if (sscanf(kbuf, "top_app=%d", &val) == 1) {
		unfair_sched_update_top_app(val);
		pr_info("unfair_sched: top_app=%d\n", val);
	} else if (sscanf(kbuf, "foreground=%d", &val) == 1) {
		unfair_sched_update_foreground(val);
		pr_info("unfair_sched: foreground=%d\n", val);
	} else if (sscanf(kbuf, "add %d %d", &val, &val) == 2) {
		int pid, boost;
		if (sscanf(kbuf, "add %d %d", &pid, &boost) == 2) {
			unfair_sched_add_task(pid, boost);
		}
	} else if (sscanf(kbuf, "remove %d", &val) == 1) {
		unfair_sched_remove_task(val);
	} else if (sscanf(kbuf, "prefer_big_core=%d", &val) == 1) {
		unfair_sched_data.prefer_big_core_fg = val ? true : false;
	}

	return count;
}

static const struct proc_ops unfair_sched_proc_ops = {
	.proc_open	= unfair_sched_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= unfair_sched_proc_write,
};

#endif

static enum hrtimer_restart idle_cull_timer_func(struct hrtimer *timer)
{
	int i;
	unsigned long flags;
	unsigned long threshold;

	threshold = msecs_to_jiffies(unfair_sched_data.idle_cull_threshold_ms);

	spin_lock_irqsave(&unfair_sched_data.lock, flags);

	for (i = 0; i < MAX_FAVORED_TASKS; i++) {
		if (unfair_sched_data.favored_tasks[i].is_active &&
		    time_after(jiffies, unfair_sched_data.favored_tasks[i].last_used_time + threshold)) {
			unfair_sched_data.favored_tasks[i].is_active = 0;
			unfair_sched_data.favored_tasks[i].pid = 0;
			unfair_sched_data.favor_count--;
		}
	}

	spin_unlock_irqrestore(&unfair_sched_data.lock, flags);

	hrtimer_forward_now(timer, ms_to_ktime(unfair_sched_data.idle_cull_threshold_ms));
	return HRTIMER_RESTART;
}

int __init unfair_sched_init(void)
{
	spin_lock_init(&unfair_sched_data.lock);
	unfair_sched_data.enabled = 1;
	unfair_sched_data.boost_scale = 2;
	unfair_sched_data.favor_count = 0;
	unfair_sched_data.sched_latency_ns = 12000000;
	unfair_sched_data.min_granularity_ns = 1500000;
	unfair_sched_data.wakeup_granularity_ns = 2000000;
	unfair_sched_data.migration_cost_ns = 500000;
	unfair_sched_data.top_app_cpu_boost = 1;
	unfair_sched_data.foreground_cpu_boost = 1;
	unfair_sched_data.idle_cull_threshold_ms = 30000;
	unfair_sched_data.prefer_big_core_fg = true;
	unfair_sched_data.wakeup_preempt_aggressive = true;

	memset(unfair_sched_data.favored_tasks, 0,
	       sizeof(unfair_sched_data.favored_tasks));

	hrtimer_init(&unfair_sched_data.idle_cull_timer,
		     CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	unfair_sched_data.idle_cull_timer.function = idle_cull_timer_func;
	hrtimer_start(&unfair_sched_data.idle_cull_timer,
		      ms_to_ktime(unfair_sched_data.idle_cull_threshold_ms),
		      HRTIMER_MODE_REL);

#ifdef CONFIG_PROC_FS
	proc_create("unfair_sched", 0644, NULL, &unfair_sched_proc_ops);
#endif

	pr_info("Unfair Scheduler %s initialized for miro (SM8750)\n",
		UNFAIR_SCHED_VERSION);

	return 0;
}
late_initcall(unfair_sched_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Unfair Scheduler for Android - Foreground Priority Boost");
MODULE_VERSION(UNFAIR_SCHED_VERSION);
