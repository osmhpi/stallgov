#include "linux/cpufreq.h"
#include "linux/percpu-defs.h"
#include "linux/printk.h"
#include "linux/rcupdate.h"
#include "linux/smp.h"
#include <linux/module.h> // included for all kernel modules
#include <linux/kernel.h> // included for KERN_INFO
#include <linux/init.h> // included for __init and __exit macros
#include <linux/sched/cpufreq.h>
#include <trace/events/power.h>

#include "memutil_log.h"
#include "memutil_debugfs.h"

#define LOGBUFFER_SIZE 2000

struct memutil_policy {
	struct cpufreq_policy *policy;

	u64		last_freq_update_time;
	s64		freq_update_delay_ns;
	struct memutil_ringbuffer *logbuffer;
};

struct memutil_cpu {
	struct update_util_data update_util;
	struct memutil_policy	*memutil_policy;
	unsigned int cpu;

	u64			last_update;
};

struct memutil_logfile_info {
	bool is_initialized;
	bool tried_init;
};

static struct memutil_logfile_info logfile_info = {
	.is_initialized = false,
	.tried_init = false
};

static DEFINE_PER_CPU(struct memutil_cpu, memutil_cpu_list);
static DEFINE_MUTEX(memutil_init_mutex);

static void memutil_log_data(u64 time, unsigned int frequency, unsigned int cpu, struct memutil_ringbuffer *logbuffer)
{
	struct memutil_perf_data data = {
		.timestamp = time,
		.frequency = frequency,
		.cpu = cpu
	};

	if (logbuffer) {
		memutil_write_ringbuffer(logbuffer, &data, 1);
	}
}

void memutil_set_frequency(struct cpufreq_policy *policy, u64 time)
{
	struct memutil_policy *memutil_policy = policy->governor_data;
	if (!policy_is_shared(policy) && policy->fast_switch_enabled) {
		memutil_log_data(time, policy->min, policy->cpu, memutil_policy->logbuffer);
		cpufreq_driver_fast_switch(policy, policy->min);
	}
	else {
		pr_err("Cannot set frequency");
	}
}

/********************** cpufreq governor interface *********************/

static struct memutil_policy *
memutil_policy_alloc(struct cpufreq_policy *policy)
{
	struct memutil_policy *memutil_policy;

	memutil_policy = kzalloc(sizeof(*memutil_policy), GFP_KERNEL);
	if (!memutil_policy) {
		return NULL;
	}

	memutil_policy->policy = policy;
	return memutil_policy;
}

static void memutil_policy_free(struct memutil_policy *memutil_policy)
{
	kfree(memutil_policy);
}

static int memutil_init(struct cpufreq_policy *policy)
{
	struct memutil_policy *memutil_policy;
	int return_value = 0;

	printk(KERN_INFO "Loading memutil module");

	/* State should be equivalent to EXIT */
	if (policy->governor_data) {
		return -EBUSY;
	}

	cpufreq_enable_fast_switch(policy);

	memutil_policy = memutil_policy_alloc(policy);
	if (!memutil_policy) {
		return_value = -ENOMEM;
		goto disable_fast_switch;
	}

	policy->governor_data = memutil_policy;
	return 0;

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);
	pr_err("initialization failed (error %d)\n", return_value);
	return return_value;
}

static void memutil_exit(struct cpufreq_policy *policy)
{
	struct memutil_policy *memutil_policy = policy->governor_data;

	printk(KERN_INFO "Exiting memutil module");
	pr_info("cpus: %px smp_processor_id: %d", policy->cpus->bits, smp_processor_id());

	policy->governor_data = NULL;

	memutil_policy_free(memutil_policy);
	cpufreq_disable_fast_switch(policy);
}

static bool memutil_this_cpu_can_update(struct cpufreq_policy *policy) {
	return cpumask_test_cpu(smp_processor_id(), policy->cpus);
	// TODO: Add policy->dvfs_possible_from_any_cpu, see: cpufreq_this_cpu_can_update
}

static bool memutil_should_update_frequency(struct memutil_policy *memutil_policy, u64 time)
{
	s64 delta_ns;

	/*
	 * Drivers cannot in general deal with cross-CPU
	 * requests, so switching frequencies may not work for the fast
	 * switching platforms.
	 *
	 * Hence stop here for remote requests if they aren't supported
	 * by the hardware, as calculating the frequency is pointless if
	 * we cannot in fact act on it.
	 *
	 * // vvvvvvvvvvvvvvvvvvv memutil TODO vvvvvvvvvvvvvvvvv
	 * This is needed on the slow switching platforms too to prevent CPUs
	 * going offline from leaving stale IRQ work items behind.
	 * // ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
	 */
	if (!memutil_this_cpu_can_update(memutil_policy->policy)) {
		return false;
	}

	delta_ns = time - memutil_policy->last_freq_update_time;

	return delta_ns >= memutil_policy->freq_update_delay_ns;
}

static void memutil_update_single_frequency(struct update_util_data *hook, u64 time,
				     unsigned int flags)
{
	struct memutil_cpu *memutil_cpu = container_of(hook, struct memutil_cpu, update_util);
	struct memutil_policy *memutil_policy = memutil_cpu->memutil_policy;

	if(!memutil_should_update_frequency(memutil_policy, time)) {
		return;
	}

	memutil_set_frequency(memutil_policy->policy, time);
	memutil_policy->last_freq_update_time = time;
}

static int memutil_start(struct cpufreq_policy *policy)
{
	struct memutil_policy *memutil_policy = policy->governor_data;
	unsigned int cpu;
	printk(KERN_INFO "Starting memutil governor");

	memutil_policy->last_freq_update_time	= 0;
	memutil_policy->freq_update_delay_ns	= NSEC_PER_USEC * cpufreq_policy_transition_delay_us(policy);

	mutex_lock(&memutil_init_mutex);
	if (!logfile_info.tried_init) {
		logfile_info.tried_init = true;
		logfile_info.is_initialized = memutil_debugfs_init() == 0;
		if (!logfile_info.is_initialized) {
			pr_warn("Failed to initialize memutil debugfs logfile");
		}
	}
	memutil_policy->logbuffer = memutil_open_ringbuffer(LOGBUFFER_SIZE);
	if (!memutil_policy->logbuffer) {
		pr_warn("Failed to create memutil logbuffer");
	} else if (logfile_info.is_initialized) {
		memutil_debugfs_register_ringbuffer(memutil_policy->logbuffer);
	}
	mutex_unlock(&memutil_init_mutex);

	for_each_cpu(cpu, policy->cpus) {
		struct memutil_cpu *mu_cpu = &per_cpu(memutil_cpu_list, cpu);

		memset(mu_cpu, 0, sizeof(*mu_cpu));
		mu_cpu->cpu 		= cpu;
		mu_cpu->memutil_policy	= memutil_policy;
	}

	for_each_cpu(cpu, policy->cpus) {
		struct memutil_cpu *mu_cpu = &per_cpu(memutil_cpu_list, cpu);
		cpufreq_add_update_util_hook(cpu, &mu_cpu->update_util, memutil_update_single_frequency);
	}

	return 0;
}

static void memutil_stop(struct cpufreq_policy *policy)
{
	unsigned int cpu;
	struct memutil_policy *memutil_policy = policy->governor_data;

	printk(KERN_INFO "Stopping memutil governor");
	// TODO
	//
	mutex_lock(&memutil_init_mutex);
	if (logfile_info.is_initialized) {
		memutil_debugfs_exit();
		logfile_info.is_initialized = false;
		logfile_info.tried_init = false;
	}
	mutex_unlock(&memutil_init_mutex);

	if (memutil_policy->logbuffer) {
		memutil_close_ringbuffer(memutil_policy->logbuffer);
	}
	for_each_cpu(cpu, policy->cpus) {
		cpufreq_remove_update_util_hook(cpu);
	}

	synchronize_rcu();
}

static void memutil_limits(struct cpufreq_policy *policy)
{
	printk(KERN_INFO "memutil limits changed");
	// TODO
}

struct cpufreq_governor memutil_gov = {
	.name = "memutil",
	.owner = THIS_MODULE,
	.flags = CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init = memutil_init,
	.exit = memutil_exit,
	.start = memutil_start,
	.stop = memutil_stop,
	.limits = memutil_limits,
};

MODULE_LICENSE(		"GPL");
MODULE_AUTHOR(		"Erik Griese <erik.griese@student.hpi.de>, "
			"Leon Matthes <leon.matthes@student.hpi.de>, "
			"Maximilian Stiede <maximilian.stiede@student.hpi.de>");
MODULE_DESCRIPTION(	"A CpuFreq governor based on Memory Access Patterns.");

cpufreq_governor_init(memutil_gov);
cpufreq_governor_exit(memutil_gov);
