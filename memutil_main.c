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
#include "memutil_debug_log.h"
#include "memutil_debugfs.h"
#include "pmu_cpuid_helper.h"
#include "pmu_events.h"
#include "memutil_debugfs_log.h"
#include "memutil_debugfs_info.h"
#include "memutil_perf_read_local.h"

#define LOGBUFFER_SIZE 2000

#define AGGREGATE_LOG 0

#define PARSE_EVENT_MAX_PAIRS 6
#define PERF_EVENT_COUNT 3

typedef char* key_value_pair_t[2];

struct memutil_policy {
	struct cpufreq_policy *policy;

	u64			last_freq_update_time;
	s64			freq_update_delay_ns;

	struct perf_event	*events[PERF_EVENT_COUNT];
	u64			last_event_value[PERF_EVENT_COUNT];

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
static struct pmu_events_map *events_map = NULL;

static char *event_name1 = "mem_inst_retired.all_loads";
static char *event_name2 = "mem_inst_retired.all_stores";
static char *event_name3 = "inst_retired.any";

module_param(event_name1, charp, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(event_name1, "First perf counter name");
module_param(event_name2, charp, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(event_name2, "Second perf counter name");
module_param(event_name3, charp, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(event_name3, "Third perf counter name");

static void memutil_log_data(u64 time, u64 values[PERF_EVENT_COUNT], unsigned int cpu, struct memutil_ringbuffer *logbuffer)
{
	struct memutil_perf_data data = {
		.timestamp = time,
		.value1 = values[0],
		.value2 = values[1],
		.value3 = values[2],
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

static void memutil_log_perf_data(struct memutil_policy *memutil_policy, u64 time)
{
	u64			event_values[PERF_EVENT_COUNT];
	struct cpufreq_policy 	*policy = memutil_policy->policy;
	int i;

#if AGGREGATE_LOG
	memutil_policy->log_counter++;
	if (memutil_policy->log_counter <= 5) {
		return;
	}
	memutil_policy->log_counter = 0;
#endif
	for (i = 0; i < PERF_EVENT_COUNT; ++i) {
		if (!memutil_policy->events[i]) {
			return;
		}
		memutil_read_perf_event(memutil_policy, i, &event_values[i]);
	}

	memutil_log_data(time, event_values, policy->cpu, memutil_policy->logbuffer);
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

static int setup_events_map(void)
{
	int i, return_value;
	char* cpuid = memutil_get_cpuid_str();
	debug_info("Memutil: Setting up events map");
	if (!cpuid) {
		pr_warn("Memutil: Failed to read CPUID");
		return -1;
	}
	i = 0;
	for (;;) {
		events_map = &memutil_pmu_events_map[i++];
		if (!events_map->table) {
			pr_warn("Memutil: Did not find pmu events map for CPUID=\"%s\"", cpuid);
			events_map = NULL;
			return_value = -2;
			break;
		}

		if (!memutil_strcmp_cpuid_str(events_map->cpuid, cpuid)) {
			debug_info("Memutil: Found table %s for CPUID=\"%s\"", events_map->cpuid, cpuid);
			return_value = 0;
			break;
		}
	}
	kfree(cpuid);
	return return_value;
}

static void teardown_events_map(void)
{
	events_map = NULL;
}

struct pmu_event *find_event(const char *event_name)
{
	struct pmu_event *event = NULL;
	int i = 0;
	if(!events_map)
		return NULL;
	for (;;) {
		event = &events_map->table[i++];
		if (!event->name && !event->event && !event->desc) {
			event = NULL;
			break;
		}
		if (event->name && !strcmp(event->name, event_name)) {
			break;
		}
	}
	return event;
}

static int parse_event_pair(char** pair, u64 *config, u64 *period)
{
	unsigned long long value;
	if (kstrtoull(pair[1], 0, &value)) {
		pr_err("Memutil: parse_event_pair: Converting number string failed. Str=%s", pair[1]);
		return -1;
	}

	//See arch/x86/events/perf_event.h struct x86_pmu_config for what bits are what or Intel Volume 3B documentation
	if (!strcmp("event", pair[0])) {
		*config |= (value & 0xFF);
		return 0;
	} else if (!strcmp("umask", pair[0])) {
		*config |= (value & 0xFF) << 8;
	} else if (!strcmp("cmask", pair[0])) {
		*config |= (value & 0xFF) << 24;
	} else if (!strcmp("edge", pair[0])) {
		*config |= (value & 1) << 18;
	} else if (!strcmp("any", pair[0])) {
		pr_warn("Memutil: parse_event_pair: Any config value is used");
		*config |= (value & 1) << 21;
	} else if (!strcmp("period", pair[0])) {
		*period = value;
	} else {
		pr_err("Memutil: parse_event_pair: Unknown key: %s", pair[0]);
	}
	return 0;
}

static int parse_event(struct pmu_event *event, u64 *config, u64 *period)
{
	key_value_pair_t pairs[PARSE_EVENT_MAX_PAIRS];
	size_t event_string_size;
	char *event_string;
	int pair_count;
	int return_value;
	bool is_reading_key = true;
	int pair_index = 0;
	int current_index = 0;

	*config = 0;

	event_string_size = strlen(event->event)+1;
	event_string = kmalloc(event_string_size, GFP_KERNEL);
	if (!event_string) {
		pr_err("Memutil: Failed to allocate memory in parse_event");
		return -1;
	}
	strncpy(event_string, event->event, event_string_size);

	pairs[0][0] = event_string;
	for (; event_string[current_index] != 0; ++current_index) {
		if (is_reading_key && event_string[current_index] == '=') {
			event_string[current_index] = 0;
			++current_index;
			pairs[pair_index][1] = event_string + current_index;
			is_reading_key = false;
		} else if (!is_reading_key && event_string[current_index] == ',') {
			event_string[current_index] = 0;
			++current_index;
			++pair_index;
			if (pair_index >= PARSE_EVENT_MAX_PAIRS) {
				pr_err("Memutil: parse event: more pairs than expected: Expected max %d, event string is %s",
				       PARSE_EVENT_MAX_PAIRS, event_string);
				goto err;
			}
			pairs[pair_index][0] = event_string + current_index;
			is_reading_key = true;
		}
	}
	pair_count = pair_index + 1;

	for (pair_index = 0; pair_index < pair_count; ++pair_index) {
		if (parse_event_pair(pairs[pair_index], config, period)) {
			goto err;
		}
	}

	return_value = 0;
	goto out;
err:
	return_value = -1;
out:
	kfree(event_string);
	return return_value;
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

	debug_info("Memutil: Perf create kernel counter");
	perf_event = perf_event_create_kernel_counter(
		&perf_attr,
		policy->policy->cpu,
		/*task*/ NULL,
		/*overflow_handler*/ NULL,
		/*context*/ NULL);

	return perf_event;
}


/**
 * @brief Allocate a named counter that is available in the pmu_events table.
 *        This currently only works for Intel CPUs
 * @param policy
 * @param counter_name
 * @return
 */
static struct perf_event *memutil_allocate_named_perf_counter(struct memutil_policy *policy, const char *counter_name)
{
	u32 perf_event_type;
	u64 perf_event_config;
	u64 perf_event_period;
	struct pmu_event *event;

	perf_event_type = 4; //hardcoded type that is usually cpu pmu
	perf_event_config = 0;
	perf_event_period = 0;

	debug_info("Memutil: Perf counter searching %s", counter_name);
	event = find_event(counter_name);
	if (!event) {
		pr_warn("Memutil: Failed to find event for given counter name %s", counter_name);
		return ERR_PTR(-1);
	}
	debug_info("Memutil: Perf counter parsing %s", counter_name);
	if (parse_event(event, &perf_event_config, &perf_event_period)) {
		pr_warn("Memutil: Failed to parse event for given counter name %s", counter_name);
		return ERR_PTR(-1);
	}
	debug_info("Memutil: Perf counter allocating %s", counter_name);
	return memutil_allocate_perf_counter_for(policy, perf_event_type, perf_event_config);
}

static long __must_check memutil_allocate_perf_counters(struct memutil_policy *policy)
{
	int i;
	struct perf_event *perf_event;
	char *event_names[PERF_EVENT_COUNT] = {
		event_name1,
		event_name2,
		event_name3
	};

	debug_info("Memutil: Allocating perf counters");
	for (i = 0; i < PERF_EVENT_COUNT; ++i) {
		debug_info("Memutil: Allocate perf counter %d", i);
		perf_event = memutil_allocate_named_perf_counter(
			policy,
			event_names[i]);
		debug_info("Memutil: Allocated perf counter %d", i);
		if(unlikely(IS_ERR(perf_event))) {
			pr_err("Memutil: Failed to allocate perf event %s: %pe", event_names[i], perf_event);
			goto cleanup;
		}
		policy->events[i] = perf_event;
	}
	debug_info("Memutil: Allocated perf counters");
	return 0;

cleanup:
	for (--i; i >= 0; --i) { //counter should start with i - 1 --> --i as initializer
		perf_event_release_kernel(policy->events[i]);
		policy->events[i] = NULL;
	}
	return PTR_ERR(perf_event);
}

static void memutil_release_perf_events(struct memutil_policy *policy)
{
	int i;
	for (i = 0; i < PERF_EVENT_COUNT; ++i) {
		if (unlikely(!policy->events[i])) {
			pr_warn("Memutil: Tried to release event %d which is NULL", i);
		} else {
			perf_event_release_kernel(policy->events[i]);
		}
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
		if (setup_events_map() != 0) {
			return_value = -1;
			goto fail_events_map;
		}
	}

	return_value = (int) memutil_allocate_perf_counters(memutil_policy);
	if (unlikely(return_value != 0)) {
		goto fail_allocate_perf_counters;
	}
	setup_per_cpu_data(policy, memutil_policy);
	install_update_hook(policy);

	pr_info("Memutil: Governor init done");
	return 0;

fail_allocate_perf_counters:
	if (memutil_policy->policy->cpu == 0) {
		teardown_events_map();
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
	mutex_lock(&memutil_init_mutex);
	if (is_logfile_initialized) {
		memutil_debugfs_exit();
		is_logfile_initialized = false;
	}
	if (policy->cpu == 0) {
		teardown_events_map();
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
