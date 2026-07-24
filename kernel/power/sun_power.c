/*
 * Sun Power Optimization - for REDMI K80 Pro (SM8750)
 *
 * Features:
 * - Aggressive CPU idle management
 * - Wakelock blocker
 * - Smart frequency scaling
 * - Input boost optimization
 * - Cluster-based power saving
 */

#include <linux/sun_power.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/hrtimer.h>
#include <linux/pm_wakeup.h>
#include <linux/suspend.h>
#include <linux/cpu.h>

struct sun_power_data sun_power_data;
EXPORT_SYMBOL_GPL(sun_power_data);

static struct hrtimer input_boost_timer;
static struct workqueue_struct *sun_power_wq;

static struct work_struct input_boost_end_work;
static struct work_struct wakelock_cleanup_work;

static int get_cluster_for_cpu(int cpu)
{
	int i;

	for (i = 0; i < sun_power_data.num_clusters; i++) {
		if (cpumask_test_cpu(cpu, sun_power_data.clusters[i].cpus))
			return i;
	}
	return -1;
}

static void init_cluster_info(void)
{
	int cpu;
	int cluster = 0;

	sun_power_data.num_clusters = 0;

	for_each_possible_cpu(cpu) {
		int cid = topology_physical_package_id(cpu);
		int found = 0;
		int i;

		for (i = 0; i < cluster; i++) {
			if (cid == i || cpumask_test_cpu(cpu, sun_power_data.clusters[i].cpus)) {
				cpumask_set_cpu(cpu, sun_power_data.clusters[i].cpus);
				found = 1;
				break;
			}
		}

		if (!found && cluster < MAX_CLUSTERS) {
			if (!zalloc_cpumask_var(&sun_power_data.clusters[cluster].cpus,
						GFP_KERNEL))
				continue;
			cpumask_set_cpu(cpu, sun_power_data.clusters[cluster].cpus);
			sun_power_data.clusters[cluster].cluster_id = cluster;
			sun_power_data.clusters[cluster].min_freq = 300000;
			sun_power_data.clusters[cluster].max_freq = 3000000;
			sun_power_data.clusters[cluster].suspend_freq = 300000;
			sun_power_data.clusters[cluster].idle_freq = 600000;
			cluster++;
		}
	}

	sun_power_data.num_clusters = cluster;
	pr_info("sun_power: detected %d CPU clusters\n", cluster);
}

static void input_boost_end_work_func(struct work_struct *work)
{
	int i;

	sun_power_data.input_boost_active = 0;

	for (i = 0; i < sun_power_data.num_clusters; i++)
		sun_power_data.clusters[i].boost_active = 0;
}

static enum hrtimer_restart input_boost_timer_func(struct hrtimer *timer)
{
	queue_work(sun_power_wq, &input_boost_end_work);
	return HRTIMER_NORESTART;
}

void sun_power_input_boost(void)
{
	int i;

	if (!sun_power_data.enabled)
		return;

	if (!sun_power_data.input_boost_active) {
		sun_power_data.input_boost_active = 1;

		for (i = 0; i < sun_power_data.num_clusters; i++)
			sun_power_data.clusters[i].boost_active = 1;
	}

	hrtimer_try_to_cancel(&input_boost_timer);
	hrtimer_start(&input_boost_timer,
		      ms_to_ktime(sun_power_data.cpu_boost_timeout_ms),
		      HRTIMER_MODE_REL);
}
EXPORT_SYMBOL_GPL(sun_power_input_boost);

int sun_power_adjust_cpufreq(struct cpufreq_policy *policy,
			     unsigned int *target_freq)
{
	int cluster_id;
	struct sun_cluster_info *ci;
	unsigned int adjusted_freq;

	if (!sun_power_data.enabled || !policy || !target_freq)
		return 0;

	cluster_id = get_cluster_for_cpu(policy->cpu);
	if (cluster_id < 0)
		return 0;

	ci = &sun_power_data.clusters[cluster_id];
	adjusted_freq = *target_freq;

	if (ci->boost_active) {
		adjusted_freq = max(adjusted_freq, sun_power_data.input_boost_freq);
		adjusted_freq = min(adjusted_freq, ci->max_freq);
	}

	if (sun_power_data.power_save_level >= 2 && !ci->boost_active) {
		unsigned int max_limit = ci->max_freq * 85 / 100;
		adjusted_freq = min(adjusted_freq, max_limit);
	}

	if (sun_power_data.power_save_level >= 1 && !ci->boost_active) {
		if (adjusted_freq > ci->idle_freq &&
		    adjusted_freq < ci->idle_freq * 2)
			adjusted_freq = ci->idle_freq;
	}

	adjusted_freq = max(adjusted_freq, ci->min_freq);
	adjusted_freq = min(adjusted_freq, ci->max_freq);

	ci->cur_freq = adjusted_freq;
	*target_freq = adjusted_freq;

	return 1;
}
EXPORT_SYMBOL_GPL(sun_power_adjust_cpufreq);

static int find_wakelock_locked(const char *name)
{
	int i;

	for (i = 0; i < MAX_BLOCKED_WAKELOCKS; i++) {
		if (sun_power_data.wakelocks[i].active &&
		    strncmp(sun_power_data.wakelocks[i].name, name,
			    WAKELOCK_NAME_MAX - 1) == 0)
			return i;
	}
	return -1;
}

static int find_free_wakelock_locked(void)
{
	int i;

	for (i = 0; i < MAX_BLOCKED_WAKELOCKS; i++) {
		if (!sun_power_data.wakelocks[i].active)
			return i;
	}
	return -1;
}

int sun_power_toggle_wakelock(const char *name, int block)
{
	int idx;
	unsigned long flags;

	if (!name || !*name)
		return -EINVAL;

	spin_lock_irqsave(&sun_power_data.wakelock_lock, flags);

	idx = find_wakelock_locked(name);

	if (block) {
		if (idx >= 0) {
			sun_power_data.wakelocks[idx].blocked = 1;
		} else {
			idx = find_free_wakelock_locked();
			if (idx < 0) {
				spin_unlock_irqrestore(&sun_power_data.wakelock_lock, flags);
				return -ENOSPC;
			}
			strncpy(sun_power_data.wakelocks[idx].name, name,
				WAKELOCK_NAME_MAX - 1);
			sun_power_data.wakelocks[idx].active = 1;
			sun_power_data.wakelocks[idx].blocked = 1;
			sun_power_data.wakelock_count++;
		}
	} else {
		if (idx >= 0) {
			sun_power_data.wakelocks[idx].active = 0;
			sun_power_data.wakelocks[idx].blocked = 0;
			sun_power_data.wakelock_count--;
		}
	}

	spin_unlock_irqrestore(&sun_power_data.wakelock_lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(sun_power_toggle_wakelock);

int sun_power_is_wakelock_allowed(const char *name)
{
	int idx;
	unsigned long flags;
	int allowed = 1;

	if (!sun_power_data.enabled || !sun_power_data.wakelock_block_enable)
		return 1;

	if (!name || !*name)
		return 1;

	spin_lock_irqsave(&sun_power_data.wakelock_lock, flags);

	idx = find_wakelock_locked(name);
	if (idx >= 0 && sun_power_data.wakelocks[idx].blocked)
		allowed = 0;

	spin_unlock_irqrestore(&sun_power_data.wakelock_lock, flags);

	return allowed;
}
EXPORT_SYMBOL_GPL(sun_power_is_wakelock_allowed);

int sun_power_set_power_save(int level)
{
	int i;

	if (level < 0 || level > 3)
		return -EINVAL;

	sun_power_data.power_save_level = level;

	for (i = 0; i < sun_power_data.num_clusters; i++) {
		struct sun_cluster_info *ci = &sun_power_data.clusters[i];

		switch (level) {
		case 0:
			ci->power_save_mode = 0;
			ci->suspend_freq = 300000;
			ci->idle_freq = 600000;
			break;
		case 1:
			ci->power_save_mode = 1;
			ci->suspend_freq = 300000;
			ci->idle_freq = 450000;
			break;
		case 2:
			ci->power_save_mode = 2;
			ci->suspend_freq = 300000;
			ci->idle_freq = 300000;
			break;
		case 3:
			ci->power_save_mode = 3;
			ci->suspend_freq = 300000;
			ci->idle_freq = 300000;
			break;
		}
	}

	pr_info("sun_power: power save level set to %d\n", level);
	return 0;
}
EXPORT_SYMBOL_GPL(sun_power_set_power_save);

unsigned int sun_power_get_suspend_freq(int cpu)
{
	int cluster_id = get_cluster_for_cpu(cpu);

	if (cluster_id < 0)
		return 300000;

	return sun_power_data.clusters[cluster_id].suspend_freq;
}
EXPORT_SYMBOL_GPL(sun_power_get_suspend_freq);

#ifdef CONFIG_PROC_FS

static int sun_power_proc_show(struct seq_file *m, void *v)
{
	int i;
	unsigned long flags;

	seq_printf(m, "=== Sun Power Optimization Status ===\n");
	seq_printf(m, "Version: %s\n", SUN_POWER_VERSION);
	seq_printf(m, "Enabled: %d\n", sun_power_data.enabled);
	seq_printf(m, "Power Save Level: %d\n", sun_power_data.power_save_level);
	seq_printf(m, "Aggressive Idle: %d\n", sun_power_data.aggressive_idle);
	seq_printf(m, "Wakelock Block Enable: %d\n",
		   sun_power_data.wakelock_block_enable);
	seq_printf(m, "Input Boost Active: %d\n",
		   sun_power_data.input_boost_active);
	seq_printf(m, "Input Boost Timeout: %u ms\n",
		   sun_power_data.cpu_boost_timeout_ms);
	seq_printf(m, "Input Boost Freq: %u kHz\n",
		   sun_power_data.input_boost_freq);
	seq_printf(m, "Number of Clusters: %d\n", sun_power_data.num_clusters);

	seq_printf(m, "\n=== Cluster Info ===\n");
	seq_printf(m, "%-5s %-20s %-10s %-10s %-10s %-10s\n",
		   "ID", "CPUs", "Min Freq", "Max Freq", "Idle Freq", "Boost");

	for (i = 0; i < sun_power_data.num_clusters; i++) {
		struct sun_cluster_info *ci = &sun_power_data.clusters[i];
		seq_printf(m, "%-5d %*pb %-10u %-10u %-10u %-10d\n",
			   ci->cluster_id,
			   cpumask_pr_args(ci->cpus),
			   ci->min_freq,
			   ci->max_freq,
			   ci->idle_freq,
			   ci->boost_active);
	}

	seq_printf(m, "\n=== Blocked Wakelocks (%d) ===\n",
		   sun_power_data.wakelock_count);

	spin_lock_irqsave(&sun_power_data.wakelock_lock, flags);
	for (i = 0; i < MAX_BLOCKED_WAKELOCKS; i++) {
		if (sun_power_data.wakelocks[i].active) {
			seq_printf(m, "  %s\n",
				   sun_power_data.wakelocks[i].name);
		}
	}
	spin_unlock_irqrestore(&sun_power_data.wakelock_lock, flags);

	return 0;
}

static int sun_power_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, sun_power_proc_show, NULL);
}

static ssize_t sun_power_proc_write(struct file *file,
				    const char __user *buf,
				    size_t count, loff_t *ppos)
{
	char kbuf[128];
	int val;
	char name_buf[WAKELOCK_NAME_MAX];

	if (count >= sizeof(kbuf))
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (sscanf(kbuf, "enabled=%d", &val) == 1) {
		sun_power_data.enabled = val ? 1 : 0;
		pr_info("sun_power: enabled=%d\n", sun_power_data.enabled);
	} else if (sscanf(kbuf, "power_save=%d", &val) == 1) {
		sun_power_set_power_save(val);
	} else if (sscanf(kbuf, "aggressive_idle=%d", &val) == 1) {
		sun_power_data.aggressive_idle = val ? 1 : 0;
	} else if (sscanf(kbuf, "wakelock_block=%d", &val) == 1) {
		sun_power_data.wakelock_block_enable = val ? 1 : 0;
	} else if (sscanf(kbuf, "block_wl %63s", name_buf) == 1) {
		sun_power_toggle_wakelock(name_buf, 1);
	} else if (sscanf(kbuf, "unblock_wl %63s", name_buf) == 1) {
		sun_power_toggle_wakelock(name_buf, 0);
	} else if (sscanf(kbuf, "boost_timeout=%d", &val) == 1) {
		if (val > 0 && val <= 5000)
			sun_power_data.cpu_boost_timeout_ms = val;
	} else if (sscanf(kbuf, "boost_freq=%d", &val) == 1) {
		if (val > 0)
			sun_power_data.input_boost_freq = val;
	} else if (strncmp(kbuf, "input_boost", 11) == 0) {
		sun_power_input_boost();
	}

	return count;
}

static const struct proc_ops sun_power_proc_ops = {
	.proc_open	= sun_power_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= sun_power_proc_write,
};

#endif

static int sun_power_input_notifier(struct notifier_block *nb,
				    unsigned long action, void *data)
{
	struct input_dev *dev = input_get_device((struct input_handle *)data);

	if (action == INPUT_OPEN || action == INPUT_EVOKE) {
		if (dev && test_bit(EV_KEY, dev->evbit))
			sun_power_input_boost();
	}

	return NOTIFY_OK;
}

static struct notifier_block sun_power_input_nb = {
	.notifier_call = sun_power_input_notifier,
};

int __init sun_power_init(void)
{
	spin_lock_init(&sun_power_data.wakelock_lock);

	sun_power_data.enabled = 1;
	sun_power_data.power_save_level = 1;
	sun_power_data.aggressive_idle = 1;
	sun_power_data.wakelock_block_enable = 0;
	sun_power_data.wakelock_count = 0;
	sun_power_data.cpu_boost_timeout_ms = 1500;
	sun_power_data.input_boost_freq = 1200000;
	sun_power_data.input_boost_active = 0;
	sun_power_data.s2idle_enabled = true;
	sun_power_data.deep_idle_enabled = true;
	sun_power_data.idle_cutoff_us = 500;

	memset(sun_power_data.wakelocks, 0, sizeof(sun_power_data.wakelocks));

	init_cluster_info();

	sun_power_wq = create_singlethread_workqueue("sun_power_wq");
	if (!sun_power_wq) {
		pr_err("sun_power: failed to create workqueue\n");
		return -ENOMEM;
	}

	INIT_WORK(&input_boost_end_work, input_boost_end_work_func);
	INIT_WORK(&wakelock_cleanup_work, NULL);

	hrtimer_init(&input_boost_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	input_boost_timer.function = input_boost_timer_func;

#ifdef CONFIG_PROC_FS
	proc_create("sun_power", 0644, NULL, &sun_power_proc_ops);
#endif

	pr_info("Sun Power Optimization %s initialized for miro (SM8750)\n",
		SUN_POWER_VERSION);

	return 0;
}
late_initcall(sun_power_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Sun Power Optimization for SM8750");
MODULE_VERSION(SUN_POWER_VERSION);
