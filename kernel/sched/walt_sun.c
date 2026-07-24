/*
 * Walt Governor Enhancement for REDMI K80 Pro (miro)
 *
 * Enhanced features:
 * - Foreground app frequency boosting
 * - Smarter frequency scaling for interactive workloads
 * - Better battery life optimization
 * - Cluster-aware frequency selection
 */

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/cpufreq.h>
#include <linux/unfair_sched.h>
#include <linux/sun_power.h>

#define WALT_SUN_VERSION	"v1.0.0-sun"

struct walt_sun_data {
	int			enabled;
	unsigned int		powersave_bias;
	unsigned int		performance_bias;
	int			fast_rise_enabled;
	int			slow_drop_enabled;

	unsigned int		foreground_boost_freq;
	unsigned int		topapp_boost_freq;
	int			foreground_boost_active;
	int			topapp_boost_active;

	unsigned int		down_differential;
	unsigned int		up_differential;
	unsigned int		fast_rise_threshold;
	unsigned int		slow_drop_threshold;

	ktime_t			last_foreground_boost_time;
	ktime_t			last_topapp_boost_time;
	spinlock_t		lock;
};

static struct walt_sun_data walt_sun = {
	.enabled = 1,
	.powersave_bias = 10,
	.fast_rise_enabled = 1,
	.slow_drop_enabled = 1,
	.foreground_boost_freq = 1800000,
	.topapp_boost_freq = 2400000,
	.foreground_boost_active = 0,
	.topapp_boost_active = 0,
	.down_differential = 20,
	.up_differential = 10,
	.fast_rise_threshold = 70,
	.slow_drop_threshold = 30,
};

static unsigned int walt_sun_adjust_target(unsigned int target_freq,
					   struct cpufreq_policy *policy,
					   unsigned int util)
{
	unsigned int adjusted = target_freq;
	int cluster_id;
	int boost = 0;

	if (!walt_sun.enabled || !policy)
		return target_freq;

	spin_lock(&walt_sun.lock);

	if (unfair_sched_data.enabled) {
		struct task_struct *p = current;
		boost = unfair_sched_boost(p);

		if (boost > 0) {
			unsigned int boost_freq;
			unsigned int boost_pct = boost * 10;

			boost_freq = target_freq +
				(target_freq * boost_pct / 100);
			adjusted = max(adjusted, boost_freq);
		}
	}

	if (walt_sun.topapp_boost_active)
		adjusted = max(adjusted, walt_sun.topapp_boost_freq);
	else if (walt_sun.foreground_boost_active)
		adjusted = max(adjusted, walt_sun.foreground_boost_freq);

	if (walt_sun.fast_rise_enabled && util > walt_sun.fast_rise_threshold) {
		unsigned int fast_rise_freq;

		fast_rise_freq = policy->max * util / 100;
		adjusted = max(adjusted, fast_rise_freq);
	}

	if (walt_sun.slow_drop_enabled && util < walt_sun.slow_drop_threshold) {
		unsigned int min_hold_freq;

		min_hold_freq = policy->cur * 90 / 100;
		if (adjusted < min_hold_freq)
			adjusted = min_hold_freq;
	}

	if (walt_sun.powersave_bias > 0 &&
	    !walt_sun.topapp_boost_active &&
	    !walt_sun.foreground_boost_active &&
	    boost == 0) {
		unsigned int reduce_pct = walt_sun.powersave_bias;
		adjusted = adjusted - (adjusted * reduce_pct / 100);
	}

	cluster_id = topology_physical_package_id(policy->cpu);

	spin_unlock(&walt_sun.lock);

	adjusted = max(adjusted, policy->min);
	adjusted = min(adjusted, policy->max);

	return adjusted;
}

static struct kobject *walt_sun_kobj;

#define show_walt_sun_one(name, format, ...)				\
static ssize_t name##_show(struct kobject *kobj,			\
			    struct kobj_attribute *attr, char *buf)	\
{									\
	return sprintf(buf, format "\n", ##__VA_ARGS__);		\
}

#define store_walt_sun_int(name, field, min_val, max_val)		\
static ssize_t name##_store(struct kobject *kobj,			\
			     struct kobj_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	int val;							\
	if (sscanf(buf, "%d", &val) != 1)				\
		return -EINVAL;						\
	if (val < min_val || val > max_val)				\
		return -EINVAL;						\
	spin_lock(&walt_sun.lock);					\
	walt_sun.field = val;						\
	spin_unlock(&walt_sun.lock);					\
	return count;							\
}

show_walt_sun_one(enabled, "%d", walt_sun.enabled)
store_walt_sun_int(enabled, enabled, 0, 1)
static struct kobj_attribute enabled_attr =
	__ATTR(enabled, 0644, enabled_show, enabled_store);

show_walt_sun_one(powersave_bias, "%u", walt_sun.powersave_bias)
store_walt_sun_int(powersave_bias, powersave_bias, 0, 50)
static struct kobj_attribute powersave_bias_attr =
	__ATTR(powersave_bias, 0644, powersave_bias_show, powersave_bias_store);

show_walt_sun_one(fast_rise_enabled, "%d", walt_sun.fast_rise_enabled)
store_walt_sun_int(fast_rise_enabled, fast_rise_enabled, 0, 1)
static struct kobj_attribute fast_rise_attr =
	__ATTR(fast_rise_enabled, 0644, fast_rise_enabled_show, fast_rise_enabled_store);

show_walt_sun_one(slow_drop_enabled, "%d", walt_sun.slow_drop_enabled)
store_walt_sun_int(slow_drop_enabled, slow_drop_enabled, 0, 1)
static struct kobj_attribute slow_drop_attr =
	__ATTR(slow_drop_enabled, 0644, slow_drop_enabled_show, slow_drop_enabled_store);

show_walt_sun_one(foreground_boost_freq, "%u", walt_sun.foreground_boost_freq)
store_walt_sun_int(foreground_boost_freq, foreground_boost_freq, 0, 4000000)
static struct kobj_attribute fg_boost_freq_attr =
	__ATTR(foreground_boost_freq, 0644,
	       foreground_boost_freq_show, foreground_boost_freq_store);

show_walt_sun_one(topapp_boost_freq, "%u", walt_sun.topapp_boost_freq)
store_walt_sun_int(topapp_boost_freq, topapp_boost_freq, 0, 5000000)
static struct kobj_attribute topapp_boost_freq_attr =
	__ATTR(topapp_boost_freq, 0644,
	       topapp_boost_freq_show, topapp_boost_freq_store);

show_walt_sun_one(fast_rise_threshold, "%u", walt_sun.fast_rise_threshold)
store_walt_sun_int(fast_rise_threshold, fast_rise_threshold, 0, 100)
static struct kobj_attribute fast_rise_thr_attr =
	__ATTR(fast_rise_threshold, 0644,
	       fast_rise_threshold_show, fast_rise_threshold_store);

show_walt_sun_one(slow_drop_threshold, "%u", walt_sun.slow_drop_threshold)
store_walt_sun_int(slow_drop_threshold, slow_drop_threshold, 0, 100)
static struct kobj_attribute slow_drop_thr_attr =
	__ATTR(slow_drop_threshold, 0644,
	       slow_drop_threshold_show, slow_drop_threshold_store);

static ssize_t version_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", WALT_SUN_VERSION);
}
static struct kobj_attribute version_attr =
	__ATTR(version, 0444, version_show, NULL);

static struct attribute *walt_sun_attrs[] = {
	&enabled_attr.attr,
	&powersave_bias_attr.attr,
	&fast_rise_attr.attr,
	&slow_drop_attr.attr,
	&fg_boost_freq_attr.attr,
	&topapp_boost_freq_attr.attr,
	&fast_rise_thr_attr.attr,
	&slow_drop_thr_attr.attr,
	&version_attr.attr,
	NULL,
};

static struct attribute_group walt_sun_attr_group = {
	.attrs = walt_sun_attrs,
};

static int __init walt_sun_init(void)
{
	spin_lock_init(&walt_sun.lock);

	walt_sun_kobj = kobject_create_and_add("walt_sun", kernel_kobj);
	if (!walt_sun_kobj) {
		pr_err("walt_sun: failed to create kobject\n");
		return -ENOMEM;
	}

	if (sysfs_create_group(walt_sun_kobj, &walt_sun_attr_group)) {
		pr_err("walt_sun: failed to create sysfs group\n");
		kobject_put(walt_sun_kobj);
		return -ENOMEM;
	}

	pr_info("Walt Sun Enhancement %s initialized\n", WALT_SUN_VERSION);
	return 0;
}
late_initcall(walt_sun_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Walt Governor Enhancement for miro (SM8750)");
MODULE_VERSION(WALT_SUN_VERSION);
