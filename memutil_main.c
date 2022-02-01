#include "linux/compiler.h"
#include "linux/cpufreq.h"
#include "linux/percpu-defs.h"
#include "linux/perf_event.h"
#include "linux/printk.h"
#include "linux/rcupdate.h"
#include "linux/smp.h"
#include "linux/types.h"
#include <linux/module.h> // included for all kernel modules
#include <linux/kernel.h> // included for KERN_INFO
#include <linux/init.h> // included for __init and __exit macros
#include <linux/sched/cpufreq.h>
#include <trace/events/power.h>
#include <linux/perf_event.h>
#include <linux/err.h>

#include "memutil_log.h"
#include "memutil_debug_log.h"
#include "memutil_debugfs.h"
#include "memutil_debugfs_log.h"
#include "memutil_debugfs_info.h"
#include "memutil_perf_read_local.h"
#include "memutil_perf_counter.h"

#define LOGBUFFER_SIZE 2000

#define AGGREGATE_LOG 0

#define PERF_EVENT_COUNT 3

struct memutil_policy {
	struct cpufreq_policy	*policy;

	u64			last_freq_update_time;
	s64			freq_update_delay_ns;

	struct perf_event	*events[PERF_EVENT_COUNT];
	u64			last_event_value[PERF_EVENT_COUNT];

	unsigned int		last_requested_freq;

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

static bool is_logfile_initialized = false;

static DEFINE_PER_CPU(struct memutil_cpu, memutil_cpu_list);
static DEFINE_MUTEX(memutil_init_mutex);

static char *event_name1 = "inst_retired.any";
static char *event_name2 = "cpu_clk_unhalted.thread";
static char *event_name3 = "cycle_activity.stalls_l2_miss";

module_param(event_name1, charp, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(event_name1, "First perf counter name");
module_param(event_name2, charp, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(event_name2, "Second perf counter name");
module_param(event_name3, charp, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(event_name3, "Third perf counter name");

static void memutil_log_data(u64 time, u64 values[PERF_EVENT_COUNT], unsigned int cpu, unsigned int requested_freq, struct memutil_ringbuffer *logbuffer)
{
	struct memutil_perf_data data = {
		.timestamp = time,
		.value1 = values[0],
		.value2 = values[1],
		.value3 = values[2],
		.requested_freq = requested_freq,
		.cpu = cpu
	};
	BUILD_BUG_ON_MSG(PERF_EVENT_COUNT != 3, "Function has to be adjusted for the PERF_EVENT_COUNT");

	if (logbuffer) {
		memutil_write_ringbuffer(logbuffer, &data, 1);
	}
}

static int memutil_read_perf_event(struct memutil_policy *policy, int event_index, u64* current_value)
{
	int perf_result;
	u64 absolute_value;
	u64 enabled_time;
	u64 running_time;

	perf_result = memutil_perf_event_read_local(
		policy->events[event_index],
		&absolute_value,
		&enabled_time,
		&running_time);

	if(unlikely(perf_result != 0)) {
		pr_warn_ratelimited("Memutil: Perf event %d read failed: %d", event_index, perf_result);
		*current_value = 0;
		return perf_result;
	}

	*current_value = absolute_value - policy->last_event_value[event_index];
	policy->last_event_value[event_index] = absolute_value;
	return 0;
}

int memutil_set_frequency_to(struct memutil_policy* memutil_policy, unsigned int value)
{
	struct cpufreq_policy	*policy = memutil_policy->policy;

	memutil_policy->last_requested_freq = value;
	
	if (!policy_is_shared(policy) && policy->fast_switch_enabled) {
		cpufreq_driver_fast_switch(policy, value);
		return 0;
	}
	else {
		pr_err_ratelimited("Memutil: Cannot set frequency");
		return 1;
	}

}

void memutil_update_frequency(struct memutil_policy *memutil_policy, u64 time)
{
	u64			event_values[PERF_EVENT_COUNT];
	s64			offcore_stalls;
	s64			cycles;
	
	s64			stalls_per_cycle;
	s64			max_stalls_perc;
	s64			min_stalls_perc;
	s64			stalls_perc_diff;
	s64			power_perc;
	unsigned int		new_frequency;

	int			i;

	struct cpufreq_policy 	*policy = memutil_policy->policy;

	for (i = 0; i < PERF_EVENT_COUNT; ++i) {
		if (unlikely(!memutil_policy->events[i])) {
			pr_err_ratelimited("Missing perf event %d", i);
			memutil_set_frequency_to(memutil_policy, policy->max);
			return;
		}
		if(unlikely(memutil_read_perf_event(memutil_policy, i, &event_values[i]) != 0)) {
			memutil_set_frequency_to(memutil_policy, policy->max);
			return;
		}
	}


	// this will cast the values into signed types which are easier to work with
	offcore_stalls = event_values[2];
	cycles = event_values[1];

	new_frequency = policy->max;
	if(unlikely(cycles == 0)) {
		new_frequency = max(policy->min, memutil_policy->last_requested_freq - (policy->max - policy->min) / 10);

	}
	else {
		stalls_per_cycle = (offcore_stalls * 100) / cycles;
		/* inst_per_cycle = (instructions * 100) / cycles; // IPC */

		// Do a linear interpolation:
		// 10% stalls/cycle = max cpu frequency, 80% stalls/cycle = min cpu frequency
		max_stalls_perc = 65;
		min_stalls_perc = 10;
		stalls_perc_diff = max_stalls_perc - min_stalls_perc;
		power_perc = max(min(((stalls_perc_diff) - stalls_per_cycle + min_stalls_perc) * 100 / stalls_perc_diff, 100LL), 0LL);
		new_frequency = power_perc * (policy->max - policy->min) / 100 + policy->min;
	}
	// We must always set the frequency, otherwise the cpufreq driver will
	// start chooseing a frequency for us.
	memutil_set_frequency_to(memutil_policy, new_frequency);

	memutil_log_data(time, event_values, policy->cpu, memutil_policy->last_requested_freq, memutil_policy->logbuffer);
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
	memutil_policy->last_requested_freq = policy->max;
	return memutil_policy;
}

static void memutil_policy_free(struct memutil_policy *memutil_policy)
{
	kfree(memutil_policy);
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

	return 0;

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

	memutil_update_frequency(memutil_policy, time);
	memutil_policy->last_freq_update_time = time;
}

static void print_start_info(struct memutil_policy *memutil_policy, struct memutil_infofile_data *infofile_data)
{
	pr_info("Memutil: Starting governor (core=%d)", memutil_policy->policy->cpu);
	if (memutil_policy->policy->cpu != 0) {
		return;
	}
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
	pr_info("Memutil: Update delay=%ums - Ringbuffer will be full after %ld seconds",
		infofile_data->update_interval_ms,
		LOGBUFFER_SIZE / (MSEC_PER_SEC / infofile_data->update_interval_ms));
}

static void start_logging(struct memutil_policy *memutil_policy, struct memutil_infofile_data *infofile_data)
{
	debug_info("Memutil: Entering start logging");

	mutex_lock(&memutil_init_mutex);
	if (memutil_policy->policy->cpu == 0) {
		infofile_data->core_count = num_online_cpus(); // cores available to scheduler
		infofile_data->logbuffer_size = LOGBUFFER_SIZE;

		is_logfile_initialized = memutil_debugfs_init(*infofile_data) == 0;
		if (!is_logfile_initialized) {
			pr_warn("Memutil: Failed to initialize memutil debugfs");
		}
	}
	memutil_policy->logbuffer = memutil_open_ringbuffer(LOGBUFFER_SIZE);
	if (!memutil_policy->logbuffer) {
		pr_warn("Memutil: Failed to create memutil logbuffer");
	} else if (is_logfile_initialized) {
		memutil_debugfs_register_ringbuffer(memutil_policy->logbuffer);
	}
	mutex_unlock(&memutil_init_mutex);
	debug_info("Memutil: Leaving start logging");
}

static void setup_per_cpu_data(struct cpufreq_policy *policy, struct memutil_policy *memutil_policy)
{
	unsigned int cpu;
	debug_info("Memutil: Setting up per CPU data");
	for_each_cpu(cpu, policy->cpus) {
		struct memutil_cpu *mu_cpu = &per_cpu(memutil_cpu_list, cpu);

		memset(mu_cpu, 0, sizeof(*mu_cpu));
		mu_cpu->cpu 		= cpu;
		mu_cpu->memutil_policy	= memutil_policy;
	}
	debug_info("Memutil: Finished setting up per CPU data");
}

static void install_update_hook(struct cpufreq_policy *policy)
{
	unsigned int cpu;
	debug_info("Memutil: Setting up CPU update hooks");
	for_each_cpu(cpu, policy->cpus) {
		struct memutil_cpu *mu_cpu = &per_cpu(memutil_cpu_list, cpu);
		cpufreq_add_update_util_hook(cpu, &mu_cpu->update_util, memutil_update_single_frequency);
	}
}

static int allocate_perf_counters(struct memutil_policy *policy)
{
	char *event_names[PERF_EVENT_COUNT] = {
		event_name1,
		event_name2,
		event_name3
	};

	return memutil_allocate_perf_counters_for_cpu(policy->policy->cpu, event_names, policy->events, PERF_EVENT_COUNT);
}

static int memutil_start(struct cpufreq_policy *policy)
{
	int return_value;
	struct memutil_infofile_data infofile_data;
	struct memutil_policy *memutil_policy = policy->governor_data;

	memutil_policy->last_freq_update_time	= 0;
	memutil_policy->freq_update_delay_ns	= max(NSEC_PER_USEC * cpufreq_policy_transition_delay_us(policy), 5 * NSEC_PER_MSEC);
	infofile_data.update_interval_ms = memutil_policy->freq_update_delay_ns / NSEC_PER_MSEC;

	print_start_info(memutil_policy, &infofile_data);

	start_logging(memutil_policy, &infofile_data);
	if (policy->cpu == 0) {
		if (memutil_setup_events_map() != 0) {
			return_value = -1;
			goto fail_events_map;
		}
	}

	return_value = allocate_perf_counters(memutil_policy);
	if (unlikely(return_value != 0)) {
		goto fail_allocate_perf_counters;
	}
	setup_per_cpu_data(policy, memutil_policy);
	install_update_hook(policy);

	pr_info("Memutil: Governor init done");
	return 0;

fail_allocate_perf_counters:
	if (memutil_policy->policy->cpu == 0) {
		memutil_teardown_events_map();
	}
fail_events_map:
	mutex_lock(&memutil_init_mutex);
	if (is_logfile_initialized) {
		memutil_debugfs_exit();
		is_logfile_initialized = false;
	}
	mutex_unlock(&memutil_init_mutex);

	if (memutil_policy->logbuffer) {
		memutil_close_ringbuffer(memutil_policy->logbuffer);
	}
	return return_value;
}

static void memutil_stop(struct cpufreq_policy *policy)
{
	unsigned int cpu;
	struct memutil_policy *memutil_policy = policy->governor_data;

	pr_info("Memutil: Stopping governor (core=%d)", policy->cpu);
	// TODO
	//
	memutil_release_perf_events(memutil_policy->events, PERF_EVENT_COUNT);
	mutex_lock(&memutil_init_mutex);
	if (is_logfile_initialized) {
		memutil_debugfs_exit();
		is_logfile_initialized = false;
	}
	if (policy->cpu == 0) {
		memutil_teardown_events_map();
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
