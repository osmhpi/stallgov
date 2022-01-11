#include "linux/compiler.h"
#include "linux/cpufreq.h"
#include "linux/percpu-defs.h"
#include "linux/perf_event.h"
#include "linux/printk.h"
#include "linux/rcupdate.h"
#include "linux/smp.h"
#include <linux/module.h> // included for all kernel modules
#include <linux/kernel.h> // included for KERN_INFO
#include <linux/init.h> // included for __init and __exit macros
#include <linux/sched/cpufreq.h>
#include <trace/events/power.h>
#include <linux/perf_event.h>
#include <linux/err.h>

#include "memutil_log.h"
#include "memutil_debugfs.h"
#include "memutil_debugfs_log.h"
#include "memutil_debugfs_info.h"

#define LOGBUFFER_SIZE 2000

#define AGGREGATE_LOG 0

struct memutil_policy {
	struct cpufreq_policy *policy;

	u64			last_freq_update_time;
	s64			freq_update_delay_ns;

	struct perf_event	*perf_cache_references_event;
	struct perf_event	*perf_cache_misses_event;

	u64			last_cache_misses_value;
	u64			last_cache_references_value;

	struct memutil_ringbuffer *logbuffer;
#if AGGREGATE_LOG
	unsigned int log_counter;
#endif
};

struct memutil_cpu {
	struct update_util_data	update_util;
	struct memutil_policy	*memutil_policy;
	unsigned int		cpu;

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

static void memutil_log_data(u64 time, u64 cache_references, u64 cache_misses, unsigned int cpu, struct memutil_ringbuffer *logbuffer)
{
	struct memutil_perf_data data = {
		.timestamp = time,
		.cache_misses = cache_misses,
		.cache_references = cache_references,
		.cpu = cpu
	};

	if (logbuffer) {
		memutil_write_ringbuffer(logbuffer, &data, 1);
	}
}

void memutil_log_perf_data(struct memutil_policy *memutil_policy, u64 time)
{
	u64			cache_references_value;
	u64			cache_references_diff;

	u64			cache_misses_value;
	u64			cache_misses_diff;

	int			perf_result;
	u64			enabled_time;
	u64			running_time;
	struct cpufreq_policy 	*policy = memutil_policy->policy;

#if AGGREGATE_LOG
	memutil_policy->log_counter++;
	if (memutil_policy->log_counter <= 25) {
		return;
	}
	memutil_policy->log_counter = 0;
#endif

	perf_result = perf_event_read_local(
			memutil_policy->perf_cache_references_event,
			&cache_references_value,
			&enabled_time,
			&running_time); 

	if(unlikely(perf_result != 0)) {
		pr_info_ratelimited("Memutil: Perf cache references read failed: %d", perf_result);
		return;
	}

	cache_references_diff = cache_references_value - memutil_policy->last_cache_references_value;
	memutil_policy->last_cache_references_value = cache_references_value;

	perf_result = perf_event_read_local(
			memutil_policy->perf_cache_misses_event,
			&cache_misses_value,
			&enabled_time,
			&running_time);

	if(unlikely(perf_result != 0)) {
		pr_info_ratelimited("Memutil: Perf cache misses read failed: %d", perf_result);
	}

	cache_misses_diff = cache_misses_value - memutil_policy->last_cache_misses_value;
	memutil_policy->last_cache_misses_value = cache_misses_value;

	memutil_log_data(time, cache_references_diff, cache_misses_diff, policy->cpu, memutil_policy->logbuffer);
}

void memutil_set_frequency(struct memutil_policy *memutil_policy, u64 time)
{
	struct cpufreq_policy 	*policy = memutil_policy->policy;

	memutil_log_perf_data(memutil_policy, time);

	if (!policy_is_shared(policy) && policy->fast_switch_enabled) {
		cpufreq_driver_fast_switch(policy, policy->max);
	}
	else {
		pr_err_ratelimited("Memutil: Cannot set frequency");
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

static struct perf_event * __must_check memutil_allocate_perf_counter_for(struct memutil_policy *policy, u32 perf_event_type, u64 perf_event_config)
{
	struct perf_event_attr perf_attr;
	struct perf_event *perf_event;

	memset(&perf_attr, 0, sizeof(perf_attr));

	perf_attr.type = perf_event_type;
	perf_attr.size = sizeof(perf_attr);
	perf_attr.config = perf_event_config;
	perf_attr.disabled = 0; // enable the event by default
	perf_attr.exclude_kernel = 1;
	perf_attr.exclude_hv = 1;

	perf_event = perf_event_create_kernel_counter(
			&perf_attr,
			policy->policy->cpu,
			/*task*/ NULL, 
			/*overflow_handler*/ NULL,
			/*context*/ NULL);

	return perf_event;
}

static long __must_check memutil_allocate_perf_counters(struct memutil_policy *policy) 
{
	struct perf_event *perf_event_cache_references;
	struct perf_event *perf_event_cache_misses;

	perf_event_cache_references = memutil_allocate_perf_counter_for(
			policy,
			PERF_TYPE_HARDWARE,
			PERF_COUNT_HW_CACHE_REFERENCES);

	if(unlikely(IS_ERR(perf_event_cache_references))) {
		return PTR_ERR(perf_event_cache_references);
	}

	perf_event_cache_misses = memutil_allocate_perf_counter_for(
			policy,
			PERF_TYPE_HARDWARE,
			PERF_COUNT_HW_CACHE_MISSES);

	if(unlikely(IS_ERR(perf_event_cache_misses))) {
		perf_event_release_kernel(perf_event_cache_references);
		return PTR_ERR(perf_event_cache_misses);
	}

	policy->perf_cache_references_event = perf_event_cache_references;
	policy->perf_cache_misses_event = perf_event_cache_misses;
	return 0;
}

static void memutil_release_perf_events(struct memutil_policy *policy)
{
	if(unlikely(policy->perf_cache_references_event == NULL)) {
		pr_warn("Memutil: Tried to release perf_cache_references_event which is NULL");
	}
	else {
		perf_event_release_kernel(policy->perf_cache_references_event);
	}

	if(unlikely(policy->perf_cache_misses_event == NULL)) {
		pr_warn("Memutil: Tried to release perf_cache_misses_event which is NULL");
	}
	else {
		perf_event_release_kernel(policy->perf_cache_misses_event);
	}
}

static int memutil_init(struct cpufreq_policy *policy)
{
	struct memutil_policy 	*memutil_policy;
	int 			return_value = 0;

	pr_info("Memutil: Loading module (core=%d)", policy->cpu);

	/* State should be equivalent to EXIT */
	if (policy->governor_data) {
		return -EBUSY;
	}

	cpufreq_enable_fast_switch(policy);

	memutil_policy = memutil_policy_alloc(policy);
	if (!memutil_policy) {
		pr_err("Memutil: Failed to allocate memutil policy!");
		return_value = -ENOMEM;
		goto disable_fast_switch;
	}

	policy->governor_data = memutil_policy;

	return_value = (int) memutil_allocate_perf_counters(memutil_policy);
	if (unlikely(return_value != 0)) {
		pr_err("Memutil: Failed to acquire hardware performance counter");
		goto free_policy;
	}

	return 0;

free_policy:
	memutil_policy_free(memutil_policy);
	// fallthrough
disable_fast_switch:
	cpufreq_disable_fast_switch(policy);
	pr_err("Memutil: init failed (error %d)\n", return_value);
	return return_value;
}

static void memutil_exit(struct cpufreq_policy *policy)
{
	struct memutil_policy *memutil_policy = policy->governor_data;

	pr_info("Memutil: Exiting module (core=%d)", policy->cpu);

	policy->governor_data = NULL;

	memutil_release_perf_events(memutil_policy);
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

static void memutil_update_single_frequency(
	struct update_util_data *hook, 
	u64 time,
	unsigned int flags
) {
	struct memutil_cpu *memutil_cpu = container_of(hook, struct memutil_cpu, update_util);
	struct memutil_policy *memutil_policy = memutil_cpu->memutil_policy;

	if(!memutil_should_update_frequency(memutil_policy, time)) {
		return;
	}

	memutil_set_frequency(memutil_policy, time);
	memutil_policy->last_freq_update_time = time;
}

static int memutil_start(struct cpufreq_policy *policy)
{
	struct memutil_policy *memutil_policy = policy->governor_data;
	struct memutil_infofile_data infofile_data;
	unsigned int cpu;

	memutil_policy->last_freq_update_time	= 0;
	memutil_policy->freq_update_delay_ns	= NSEC_PER_USEC * cpufreq_policy_transition_delay_us(policy);
	infofile_data.update_interval = memutil_policy->freq_update_delay_ns / 1000;
	if (policy->cpu == 0) {
		pr_info("Memutil: Info\n"
			"Populatable CPUs=%d\n"
			"Populated CPUs=%d\n"
			"CPUs available to scheduler=%d\n"
			"CPUs available to migration=%d\n",
			num_possible_cpus(),
			num_present_cpus(),
			num_online_cpus(),
			num_active_cpus()
		);
		pr_info("Memutil: Update delay=%uus - Ringbuffer will be full after %lld seconds",
			infofile_data.update_interval,
			LOGBUFFER_SIZE / (NSEC_PER_SEC / memutil_policy->freq_update_delay_ns));
	}

	pr_info("Memutil: Starting governor (core=%d)", policy->cpu);

	mutex_lock(&memutil_init_mutex);
	if (!logfile_info.tried_init) {
		logfile_info.tried_init = true;
		
		logfile_info.is_initialized = memutil_debugfs_init(infofile_data) == 0;
		if (!logfile_info.is_initialized) {
			pr_warn("Memutil: Failed to initialize memutil debugfs");
		}
	}
	memutil_policy->logbuffer = memutil_open_ringbuffer(LOGBUFFER_SIZE);
	if (!memutil_policy->logbuffer) {
		pr_warn("Memutil: Failed to create memutil logbuffer");
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

	pr_info("Memutil: Stopping governor (core=%d)", policy->cpu);
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
	pr_info("Memutil: Limits changed (core=%d)", policy->cpu);
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
