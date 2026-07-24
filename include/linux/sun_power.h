#ifndef _LINUX_SUN_POWER_H
#define _LINUX_SUN_POWER_H

#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/pm_qos.h>

#define SUN_POWER_VERSION		"v1.0.0-sun"

#define MAX_CLUSTERS			3
#define MAX_CPUS_PER_CLUSTER		8

#define WAKELOCK_NAME_MAX		64
#define MAX_BLOCKED_WAKELOCKS		128

struct sun_cluster_info {
	int			cluster_id;
	cpumask_var_t		cpus;
	unsigned int		min_freq;
	unsigned int		max_freq;
	unsigned int		cur_freq;
	unsigned int		suspend_freq;
	unsigned int		idle_freq;
	int			boost_active;
	int			power_save_mode;
};

struct sun_wakelock_entry {
	char			name[WAKELOCK_NAME_MAX];
	int			active;
	unsigned long		hold_time;
	unsigned long		max_hold_time;
	int			blocked;
	int			allow_in_doze;
};

struct sun_power_data {
	int			enabled;
	int			power_save_level;
	int			aggressive_idle;
	int			wakelock_block_enable;

	struct sun_cluster_info	clusters[MAX_CLUSTERS];
	int			num_clusters;

	struct sun_wakelock_entry wakelocks[MAX_BLOCKED_WAKELOCKS];
	int			wakelock_count;
	spinlock_t		wakelock_lock;

	unsigned int		cpu_boost_timeout_ms;
	unsigned int		input_boost_freq;
	int			input_boost_active;

	bool			s2idle_enabled;
	bool			deep_idle_enabled;
	int			idle_cutoff_us;
};

#ifdef CONFIG_SUN_POWER_OPTIMIZATION

extern struct sun_power_data sun_power_data;

extern int sun_power_init(void);
extern int sun_power_set_power_save(int level);
extern int sun_power_toggle_wakelock(const char *name, int block);
extern void sun_power_input_boost(void);
extern int sun_power_adjust_cpufreq(struct cpufreq_policy *policy,
				    unsigned int *target_freq);
extern int sun_power_is_wakelock_allowed(const char *name);
extern unsigned int sun_power_get_suspend_freq(int cpu);

#else

static inline int sun_power_init(void) { return 0; }
static inline int sun_power_set_power_save(int level) { return -ENODEV; }
static inline int sun_power_toggle_wakelock(const char *name, int block) { return -ENODEV; }
static inline void sun_power_input_boost(void) { }
static inline int sun_power_adjust_cpufreq(struct cpufreq_policy *policy,
					   unsigned int *target_freq) { return 0; }
static inline int sun_power_is_wakelock_allowed(const char *name) { return 1; }
static inline unsigned int sun_power_get_suspend_freq(int cpu) { return 0; }

#endif

#endif
