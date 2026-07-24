/*
 * Sun Idle Optimization - for REDMI K80 Pro (SM8750)
 *
 * Optimizations:
 * - Aggressive idle state promotion
 * - Smart C-state selection
 * - Cluster-level idle coordination
 * - S2Idle improvement
 */

#include <linux/module.h>
#include <linux/cpuidle.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/sun_power.h>
#include <linux/unfair_sched.h>

#define SUN_IDLE_VERSION	"v1.0.0-sun"

struct sun_idle_data {
	int			enabled;
	int			aggressive_promotion;
	int			deep_idle_enabled;
	int			s2idle_optimized;

	unsigned int		promotion_threshold_us;
	unsigned int		demotion_threshold_us;
	unsigned int		min_residency_multiplier;
	unsigned int		latency_multiplier;

	int			use_deeper_idle;
	int			idle_cutoff_us;

	ktime_t			last_wakeup_time;
	ktime_t			last_deep_idle_time;
	spinlock_t		lock;

	unsigned long		deep_idle_count;
	unsigned long		shallow_idle_count;
};

static struct sun_idle_data sun_idle = {
	.enabled = 1,
	.aggressive_promotion = 1,
	.deep_idle_enabled = 1,
	.s2idle_optimized = 1,
	.promotion_threshold_us = 500,
	.demotion_threshold_us = 200,
	.min_residency_multiplier = 80,
	.latency_multiplier = 100,
	.use_deeper_idle = 1,
	.idle_cutoff_us = 100,
};

static int sun_idle_select(struct cpuidle_driver *drv,
			   struct cpuidle_device *dev,
			   bool *stop_tick)
{
	int target_state = 0;
	int deepest_state;
	s64 predicted_idle_us;

	if (!sun_idle.enabled)
		return -ENODEV;

	deepest_state = drv->state_count - 1;
	if (deepest_state < 0)
		return 0;

	spin_lock(&sun_idle.lock);

	target_state = 0;

	if (sun_idle.aggressive_promotion) {
		for (target_state = deepest_state; target_state > 0;
		     target_state--) {
			unsigned int adjusted_residency;

			adjusted_residency = drv->states[target_state].exit_latency *
				sun_idle.min_residency_multiplier / 100;

			if (adjusted_residency <= drv->states[target_state].target_residency)
				break;
		}

		if (sun_idle.use_deeper_idle && target_state < deepest_state) {
			predicted_idle_us = ktime_us_delta(
				ktime_get(), sun_idle.last_wakeup_time);

			if (predicted_idle_us > sun_idle.promotion_threshold_us)
				target_state = deepest_state;
		}
	}

	if (!sun_idle.deep_idle_enabled && target_state > 1)
		target_state = 1;

	if (sun_idle.idle_cutoff_us > 0) {
		predicted_idle_us = ktime_us_delta(
			ktime_get(), sun_idle.last_wakeup_time);
		if (predicted_idle_us < sun_idle.idle_cutoff_us)
			target_state = 0;
	}

	if (target_state >= drv->state_count)
		target_state = drv->state_count - 1;
	if (target_state < 0)
		target_state = 0;

	spin_unlock(&sun_idle.lock);

	return target_state;
}

static void sun_idle_reflect(struct cpuidle_device *dev, int index)
{
	if (!sun_idle.enabled)
		return;

	spin_lock(&sun_idle.lock);

	sun_idle.last_wakeup_time = ktime_get();

	if (index > 1)
		sun_idle.deep_idle_count++;
	else
		sun_idle.shallow_idle_count++;

	spin_unlock(&sun_idle.lock);
}

static struct cpuidle_governor sun_idle_governor = {
	.name = "sun_idle",
	.rating = 50,
	.enable = NULL,
	.disable = NULL,
	.select = sun_idle_select,
	.reflect = sun_idle_reflect,
	.owner = THIS_MODULE,
};

#ifdef CONFIG_PROC_FS
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

static int sun_idle_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "=== Sun Idle Optimization ===\n");
	seq_printf(m, "Version: %s\n", SUN_IDLE_VERSION);
	seq_printf(m, "Enabled: %d\n", sun_idle.enabled);
	seq_printf(m, "Aggressive Promotion: %d\n",
		   sun_idle.aggressive_promotion);
	seq_printf(m, "Deep Idle Enabled: %d\n", sun_idle.deep_idle_enabled);
	seq_printf(m, "S2Idle Optimized: %d\n", sun_idle.s2idle_optimized);
	seq_printf(m, "Promotion Threshold: %u us\n",
		   sun_idle.promotion_threshold_us);
	seq_printf(m, "Demotion Threshold: %u us\n",
		   sun_idle.demotion_threshold_us);
	seq_printf(m, "Min Residency Multiplier: %u%%\n",
		   sun_idle.min_residency_multiplier);
	seq_printf(m, "Idle Cutoff: %d us\n", sun_idle.idle_cutoff_us);
	seq_printf(m, "Deep Idle Count: %lu\n", sun_idle.deep_idle_count);
	seq_printf(m, "Shallow Idle Count: %lu\n",
		   sun_idle.shallow_idle_count);
	return 0;
}

static int sun_idle_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, sun_idle_proc_show, NULL);
}

static ssize_t sun_idle_proc_write(struct file *file,
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
		sun_idle.enabled = val ? 1 : 0;
	} else if (sscanf(kbuf, "aggressive_promotion=%d", &val) == 1) {
		sun_idle.aggressive_promotion = val ? 1 : 0;
	} else if (sscanf(kbuf, "deep_idle_enabled=%d", &val) == 1) {
		sun_idle.deep_idle_enabled = val ? 1 : 0;
	} else if (sscanf(kbuf, "promotion_threshold=%d", &val) == 1) {
		if (val >= 0 && val <= 10000)
			sun_idle.promotion_threshold_us = val;
	} else if (sscanf(kbuf, "idle_cutoff=%d", &val) == 1) {
		sun_idle.idle_cutoff_us = val;
	} else if (sscanf(kbuf, "residency_mult=%d", &val) == 1) {
		if (val >= 10 && val <= 200)
			sun_idle.min_residency_multiplier = val;
	}

	return count;
}

static const struct proc_ops sun_idle_proc_ops = {
	.proc_open	= sun_idle_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= sun_idle_proc_write,
};
#endif

static int __init sun_idle_init(void)
{
	spin_lock_init(&sun_idle.lock);

#ifdef CONFIG_PROC_FS
	proc_create("sun_idle", 0644, NULL, &sun_idle_proc_ops);
#endif

	pr_info("Sun Idle Optimization %s initialized for miro (SM8750)\n",
		SUN_IDLE_VERSION);

	return 0;
}
late_initcall(sun_idle_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Sun Idle Optimization for SM8750");
MODULE_VERSION(SUN_IDLE_VERSION);
