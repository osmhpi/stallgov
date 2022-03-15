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
#include <uapi/linux/sched/types.h>
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

#define HEURISTIC_IPC 1
#define HEURISTIC_OFFCORE_STALLS 2

#define LOGBUFFER_SIZE 2000
#define AGGREGATE_LOG 0
#define PERF_EVENT_COUNT 3
#define HEURISTIC HEURISTIC_OFFCORE_STALLS

#if HEURISTIC != HEURISTIC_IPC && HEURISTIC != HEURISTIC_OFFCORE_STALLS
#error "Unknown heuristic choosen"
#endif

//copied from kernel/sched/sched.h
/*
 * !! For sched_setattr_nocheck() (kernel) only !!
 *
 * This is actually gross. :(
 *
 * It is used to make schedutil kworker(s) higher priority than SCHED_DEADLINE
 * tasks, but still be able to sleep. We need this on platforms that cannot
 * atomically change clock frequency. Remove once fast switching will be
 * available on such platforms.
 *
 * SUGOV stands for SchedUtil GOVernor.
 */
#define SCHED_FLAG_SUGOV	0x10000000

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
	/* The next fields are only needed if fast switch cannot be used: */
	raw_spinlock_t          update_lock;
	struct			irq_work irq_work;
	struct			kthread_work work;
	struct			mutex work_lock;
	struct			kthread_worker worker;
	struct task_struct	*thread;
	bool			work_in_progress;
};

struct memutil_cpu {
	struct update_util_data	update_util;
	struct memutil_policy	*memutil_policy;
	unsigned int		cpu;
};

static bool is_logfile_initialized = false;

static DEFINE_PER_CPU(struct memutil_cpu, memutil_cpu_list);
static DEFINE_MUTEX(memutil_init_mutex);

#if HEURISTIC == HEURISTIC_IPC

static char *event_name1 = "instructions";
static char *event_name2 = "cycles";
static char *event_name3 = "cycles";

static int max_freq_ipc = 45;
static int min_freq_ipc = 10;

module_param(max_freq_ipc, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(max_freq_ipc, "(IPC*100) value at which the max frequency should be used");
module_param(min_freq_ipc, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(min_freq_ipc, "(IPC*100) value at which the min frequency should be used");

#elif HEURISTIC == HEURISTIC_OFFCORE_STALLS

static char *event_name1 = "inst_retired.any";
static char *event_name2 = "cpu_clk_unhalted.thread";
static char *event_name3 = "cycle_activity.stalls_l2_miss";

static int max_freq_stalls_per_cycle = 65;
static int min_freq_stalls_per_cycle = 10;

module_param(max_freq_stalls_per_cycle, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(max_freq_stalls_per_cycle, "(stalls_per_cycle*100) value at which the max frequency should be used");
module_param(min_freq_stalls_per_cycle, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(min_freq_stalls_per_cycle, "(stalls_per_cycle*100) value at which the min frequency should be used");

#endif

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

static void memutil_deferred_set_frequency(struct memutil_policy* memutil_policy)
{
	//lock to prevent missing queuing new frequency update (see worker fn)
	raw_spin_lock(&memutil_policy->update_lock);
	if (!memutil_policy->work_in_progress) {
		memutil_policy->work_in_progress = true;
		irq_work_queue(&memutil_policy->irq_work);
	}
	raw_spin_unlock(&memutil_policy->update_lock);
}

int memutil_set_frequency_to(struct memutil_policy* memutil_policy, unsigned int value, u64 time)
{
	struct cpufreq_policy	*policy = memutil_policy->policy;

	memutil_policy->last_requested_freq = value;
	memutil_policy->last_freq_update_time = time;

	if (policy_is_shared(policy)) {
		pr_err_ratelimited("Memutil: Cannot set frequency for shared policy");
		return 1;
	}

	if (policy->fast_switch_enabled) {
		cpufreq_driver_fast_switch(policy, value);
	} else {
		memutil_deferred_set_frequency(memutil_policy);
	}
	return 0;
}

void memutil_update_frequency(struct memutil_policy *memutil_policy, u64 time)
{
	u64			event_values[PERF_EVENT_COUNT];
	s64			cycles;
#if HEURISTIC == HEURISTIC_IPC
	s64			instructions;
	s64			instructions_per_cycle;
#elif HEURISTIC == HEURISTIC_OFFCORE_STALLS
	s64			offcore_stalls;
	s64			stalls_per_cycle;
#endif
	s64                     interpolation_range;
	s64                     frequency_factor;
	unsigned int		new_frequency;
	int                     max_freq, min_freq, last_freq;

	int			i;

	struct cpufreq_policy 	*policy = memutil_policy->policy;

	//Using unsigned integer math can lead to unwanted underflows, so cast to int as we don't need values >~2'000'000'000
	max_freq = policy->max;
	min_freq = policy->min;
	last_freq = memutil_policy->last_requested_freq;

	// We must always set the frequency, otherwise the cpufreq driver will
	// start chooseing a frequency for us.
	for (i = 0; i < PERF_EVENT_COUNT; ++i) {
		if (unlikely(!memutil_policy->events[i])) {
			pr_err_ratelimited("Missing perf event %d", i);
			memutil_set_frequency_to(memutil_policy, policy->max, time);
			return;
		}
		if(unlikely(memutil_read_perf_event(memutil_policy, i, &event_values[i]) != 0)) {
			memutil_set_frequency_to(memutil_policy, policy->max, time);
			return;
		}
	}

	// this will cast the values into signed types which are easier to work with
#if HEURISTIC == HEURISTIC_IPC
	instructions = event_values[0];
#elif HEURISTIC == HEURISTIC_OFFCORE_STALLS
	offcore_stalls = event_values[2];
#endif
	cycles = event_values[1];

	new_frequency = policy->max;
	if(unlikely(cycles == 0)) {
		new_frequency = max(min_freq, last_freq - (max_freq - min_freq) / 10);
	}
	else {
#if HEURISTIC == HEURISTIC_IPC
		instructions_per_cycle = (instructions * 100) / cycles;

		// Do a linear interpolation:
		interpolation_range = max_freq_ipc - min_freq_ipc;
		frequency_factor = clamp(((instructions_per_cycle - min_freq_ipc) * 100) / interpolation_range, 0LL, 100LL);
#elif HEURISTIC == HEURISTIC_OFFCORE_STALLS
		stalls_per_cycle = (offcore_stalls * 100) / cycles;

		// Do a linear interpolation:
		interpolation_range = max_freq_stalls_per_cycle - min_freq_stalls_per_cycle;
		frequency_factor = clamp(((stalls_per_cycle - min_freq_stalls_per_cycle) * 100) / interpolation_range, 0LL, 100LL);
#endif
		new_frequency = frequency_factor * (max_freq - min_freq) / 100 + min_freq;
	}
	// We must always set the frequency, otherwise the cpufreq driver will
	// start chooseing a frequency for us.
	memutil_set_frequency_to(memutil_policy, new_frequency, time);

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
	raw_spin_lock_init(&memutil_policy->update_lock);
	return memutil_policy;
}

static void memutil_policy_free(struct memutil_policy *memutil_policy)
{
	kfree(memutil_policy);
}

static void memutil_work(struct kthread_work *work)
{
	struct memutil_policy *memutil_policy = container_of(work, struct memutil_policy, work);
	unsigned int frequency;
	unsigned long irq_flags;

	/*
	 * Hold sg_policy->update_lock shortly to handle the case where:
	 * memutil_policy->next_freq is read here, and then updated by
	 * memutil_deferred_set_frequency() just before work_in_progress is set to false
	 * here, we may miss queueing the new update.
	 *
	 * Note: If a work was queued after the update_lock is released,
	 * memutil_work() will just be called again by kthread_work code; and the
	 * request will be proceed before the memutil thread sleeps.
	 */
	raw_spin_lock_irqsave(&memutil_policy->update_lock, irq_flags);
	frequency = memutil_policy->last_requested_freq;
	memutil_policy->work_in_progress = false;
	raw_spin_unlock_irqrestore(&memutil_policy->update_lock, irq_flags);

	mutex_lock(&memutil_policy->work_lock);
	__cpufreq_driver_target(memutil_policy->policy, frequency, CPUFREQ_RELATION_L);
	mutex_unlock(&memutil_policy->work_lock);
}

static void memutil_irq_work(struct irq_work *irq_work)
{
	struct memutil_policy *memutil_policy;

	memutil_policy = container_of(irq_work, struct memutil_policy, irq_work);
	kthread_queue_work(&memutil_policy->worker, &memutil_policy->work);
}

static int memutil_create_worker_thread(struct memutil_policy* memutil_policy)
{
	struct task_struct *thread;
	struct sched_attr attr = {
		.size		= sizeof(struct sched_attr),
		.sched_policy	= SCHED_DEADLINE,
		.sched_flags	= SCHED_FLAG_SUGOV, //we reuse this flag to have same scheduling behaviour as schedutil
		.sched_nice	= 0,
		.sched_priority	= 0,
		/*
		 * Fake (unused) bandwidth; workaround to "fix"
		 * priority inheritance.
		 */
		.sched_runtime	=  1000000,
		.sched_deadline = 10000000,
		.sched_period	= 10000000,
	};
	struct cpufreq_policy *policy = memutil_policy->policy;
	int return_value;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled) {
		return 0;
	}

	kthread_init_work(&memutil_policy->work, memutil_work);
	kthread_init_worker(&memutil_policy->worker);
	thread = kthread_create(kthread_worker_fn, &memutil_policy->worker,
				"memutil:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create memutil thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	return_value = sched_setattr_nocheck(thread, &attr);
	if (return_value) {
		kthread_stop(thread);
		pr_warn("Memutil: %s: failed to set SCHED_DEADLINE\n", __func__);
		return return_value;
	}

	memutil_policy->thread = thread;
	kthread_bind(thread, policy->cpu);
	init_irq_work(&memutil_policy->irq_work, memutil_irq_work);
	mutex_init(&memutil_policy->work_lock);

	wake_up_process(thread);

	return 0;
}

static void memutil_stop_worker_thread(struct memutil_policy *memutil_policy)
{
	/* kthread only required for slow path */
	if (memutil_policy->policy->fast_switch_enabled) {
		return;
	}

	kthread_flush_worker(&memutil_policy->worker);
	kthread_stop(memutil_policy->thread);
	mutex_destroy(&memutil_policy->work_lock);
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
	return_value = memutil_create_worker_thread(memutil_policy);
	if (return_value) {
		goto free_policy;
	}

	policy->governor_data = memutil_policy;

	return 0;

free_policy:
	memutil_policy_free(memutil_policy);

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

	memutil_stop_worker_thread(memutil_policy);
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
	unsigned int flags)
{
	struct memutil_cpu *memutil_cpu = container_of(hook, struct memutil_cpu, update_util);
	struct memutil_policy *memutil_policy = memutil_cpu->memutil_policy;

	if(!memutil_should_update_frequency(memutil_policy, time)) {
		return;
	}

	memutil_update_frequency(memutil_policy, time);
}

static void print_start_info(struct memutil_policy *memutil_policy, struct memutil_infofile_data *infofile_data)
{
	pr_info("Memutil: Starting governor (core=%d)", memutil_policy->policy->cpu);
	if (memutil_policy->policy->cpu != 0) {
		return;
	}
	pr_info("Memutil: Fastswitch is %s", memutil_policy->policy->fast_switch_enabled ? "enabled" : "disabled");
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
	memutil_policy->work_in_progress        = false;
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

	for_each_cpu(cpu, policy->cpus) {
		cpufreq_remove_update_util_hook(cpu);
	}

	synchronize_rcu();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&memutil_policy->irq_work);
		kthread_cancel_work_sync(&memutil_policy->work);
	}

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
