// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_main.c
 *
 * Main memutil governor code and governor callbacks
 *
 * Note: Frequency values are always in KHz if not otherwise specified.
 *
 * COPYRIGHT_PLACEHOLDER
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#include <linux/compiler.h> // included for likely, unlikely etc.
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/init.h> // included for __init and __exit macros
#include <linux/err.h>
#include <linux/module.h> // included for all kernel modules
#include <linux/percpu-defs.h>
#include <linux/perf_event.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/sched/cpufreq.h>
#include <uapi/linux/sched/types.h>
#include <trace/events/power.h>

#include "memutil_ringbuffer_log.h"
#include "memutil_printk_helper.h"
#include "memutil_debugfs.h"
#include "memutil_debugfs_logfile.h"
#include "memutil_debugfs_infofile.h"
#include "memutil_perf_read_local.h"
#include "memutil_perf_counter.h"

#define HEURISTIC_IPC 1
#define HEURISTIC_OFFCORE_STALLS 2

/*
 * Size for the ringbuffers (one per cpu) into which logging information
 * will be written with each frequency update.
 */
#define LOG_RINGBUFFER_SIZE 2000
/*
 * The amount of perf events we measure. Adjusting this requires adjusting the
 * rest of this file as e.g. the heuristics assume that their events are available.
 * Also the logging would need to be adjusted as currently 3 event values are
 * logged.
 */
#define PERF_EVENT_COUNT 3

/*
 * Switch to toggle whether code for deferred frequency switching (no fast switch)
 * should be compiled. If possible keep this enabled. Only disable if your kernel
 * does not export the needed functions and you have fast switch capability.
 */
#define WITH_DEFFERED_FREQ_SWITCH 1

/*
 * Heuristic to use. See the wiki page for Memutil Heuristics and Porting
 * for more information.
 */
#define HEURISTIC HEURISTIC_OFFCORE_STALLS

#if HEURISTIC != HEURISTIC_IPC && HEURISTIC != HEURISTIC_OFFCORE_STALLS
#error "Unknown heuristic choosen"
#endif

/**********copied from kernel/sched/sched.h ***********************************/
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
/****** end copied from kernel/sched/sched.h **********************************/

/**
 * struct memutil_policy - The memutil data for a cpufreq policy that uses the
 *                         memutil governor
 *
 * @policy: The cpufreq policy that is the parent of this data
 * @last_freq_update_time_ns: Timestamp (nanoseconds) of when the last frequency update was made
 * @freq_update_delay_ns: How much time (in nanoseconds) should occur between consecutive frequency updates
 * @events: The perf events that are measured
 * @last_event_value: The last value each event had the last time they were read
 * @last_requested_freq: The frequency (in kHz) that was last requested during a frequency update
 * @logbuffer: The log - ringbuffer that logs the frequency update data
 * @update_lock: Lock to synchronize updates to this structure. Only needed when
 *               we use an extra thread for frequency updates.
 * @irq_work: Used to issue a frequency update via an interrupt
 * @kthread_work: One item of work (one frequency update) that is queued onto
 *                the kthread to be processed
 * @kthread_worker: kthread-worker that processes enqueued frequency update work
 * @kthread: The thread itself that is used to process frequency updates
 * @freq_update_in_progress: Boolean that stores whether a deferred frequency
 *                           update is currently being carried out
 */
struct memutil_policy {
	struct cpufreq_policy	*policy;

	u64			last_freq_update_time_ns;
	s64			freq_update_delay_ns;

	struct perf_event	*events[PERF_EVENT_COUNT];
	u64			last_event_value[PERF_EVENT_COUNT];

	unsigned int		last_requested_freq;

	struct memutil_ringbuffer *logbuffer;

	/* The next fields are only needed if fast switch cannot be used: */
#if WITH_DEFFERED_FREQ_SWITCH
	raw_spinlock_t          update_lock;
	/*
	 * The deferred frequency update works by issuing an interrupt via irq_work that than queues up
	 * a frequency update on a kernel thread for which kthread_work is used
	 */
	struct			irq_work irq_work;
	struct			kthread_work kthread_work;
	struct			kthread_worker kthread_worker;
	struct task_struct	*kthread;
	bool			freq_update_in_progress;
#endif
};

/**
 * struct memutil_cpu - The memutil data for a cpu that uses the
 *                      memutil governor. This might not be necessarily
 *                      needed because we have exactly one policy per cpu, so
 *                      the policy could also store this data. However it is still
 *                      useful to separate data that is always needed per cpu
 *                      from the policy in case support for shared policies will
 *                      be added.
 * @update_util: Update util hook for this cpu
 * @memutil_policy: The assigned memutil policy for that cpu
 * @cpu: The cpu this struct belongs to
 */
struct memutil_cpu {
	struct update_util_data	update_util;
	struct memutil_policy	*memutil_policy;
	unsigned int		cpu;
};

/* Boolean tracking whether the logfile is initialized */
static bool is_logfile_initialized = false;

/* List of per cpu memutil data */
static DEFINE_PER_CPU(struct memutil_cpu, memutil_cpu_list);
/* Mutex for doing some init / deinit work on just one cpu */
static DEFINE_MUTEX(memutil_init_mutex);

#if HEURISTIC == HEURISTIC_IPC

/* names of the perf counter events we measure */
static char *event_name1 = "instructions";
static char *event_name2 = "cycles"; //use same event twice to hopefully not use so many resources
static char *event_name3 = "cycles";

/* Max ipc value (in percent) (see wiki heursitics and porting page) */
static int max_ipc = 45;
/* Min ipc value (in percent) (see wiki heursitics and porting page) */
static int min_ipc = 10;

module_param(max_ipc, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(max_ipc, "max (IPC*100) value");
module_param(min_ipc, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(min_ipc, "min (IPC*100) value");

#elif HEURISTIC == HEURISTIC_OFFCORE_STALLS

/* names of the perf counter events we measure */
static char *event_name1 = "cpu_clk_unhalted.thread"; //use same event twice to hopefully not use so many resources
static char *event_name2 = "cpu_clk_unhalted.thread";
static char *event_name3 = "cycle_activity.stalls_l2_miss";

/* Max stalls per cycle value (in percent) (see wiki heursitics and porting page) */
static int max_stalls_per_cycle = 65;
/* Min stalls per cycle value (in percent) (see wiki heursitics and porting page) */
static int min_stalls_per_cycle = 10;

module_param(max_stalls_per_cycle, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(max_stalls_per_cycle, "max (stalls_per_cycle*100) value");
module_param(min_stalls_per_cycle, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(min_stalls_per_cycle, "min (stalls_per_cycle*100) value");

#endif

module_param(event_name1, charp, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(event_name1, "First perf counter name");
module_param(event_name2, charp, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(event_name2, "Second perf counter name");
module_param(event_name3, charp, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(event_name3, "Third perf counter name");

/**
 * memutil_log_data - Log key values for the given timestamp into the log-ringbuffer
 *
 * @time: Timestamp (nanosecond resolution) for the values
 * @values: perf counter values
 * @cpu: The cpu the data (frequency, perf counters) belongs to
 * @requested_freq: The frequency that was requested / set by memutil
 * @logbuffer: The buffer into which the data should be logged
 */
static void memutil_log_data(u64 time, u64 values[PERF_EVENT_COUNT], unsigned int cpu, unsigned int requested_freq, struct memutil_ringbuffer *logbuffer)
{
	struct memutil_log_entry data = {
		.timestamp = time,
		.perf_value1 = values[0],
		.perf_value2 = values[1],
		.perf_value3 = values[2],
		.requested_freq = requested_freq,
		.cpu = cpu
	};
	BUILD_BUG_ON_MSG(PERF_EVENT_COUNT != 3, "Function has to be adjusted for the PERF_EVENT_COUNT");

	if (logbuffer) { //if initializing logging failed, this is null
		memutil_write_ringbuffer(logbuffer, &data, 1);
	}
}

/**
 * memutil_read_perf_event - Read the current perf event value for the given event
 *                           Instead of simply providing the absolute value, this
 *                           function provides the event value difference to the
 *                           last time the value was read.
 *
 * @policy: The policy to which the perf events are associated. The current event
 *          absolute value is written to the member last_event_value
 * @event_index: The index (in the events array) of the perf event to read
 * @current_value: Pointer to a variable to which the event value should be written
 */
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

#if WITH_DEFFERED_FREQ_SWITCH
/**
 * memutil_deferred_set_frequency - Queue up a deferred frequency change.
 *                                  This method issues an irq which then queues
 *                                  up the update on the for this purpose created
 *                                  kernel thread
 * @memutil_policy: Policy for which the frequency update should be made
 */
static void memutil_deferred_set_frequency(struct memutil_policy* memutil_policy)
{
	//lock to prevent missing queuing new frequency update (see worker fn)
	raw_spin_lock(&memutil_policy->update_lock);
	if (!memutil_policy->freq_update_in_progress) {
		memutil_policy->freq_update_in_progress = true;
		irq_work_queue(&memutil_policy->irq_work);
	}
	raw_spin_unlock(&memutil_policy->update_lock);
}
#endif

/**
 * memutil_set_frequency_to - Set the frequency for the given policy to the given
 *                            value. This uses either a fast_switch if possible,
 *                            or queues up a deferred update if fast_switch is not
 *                            possible. If the module was build without deferred
 *                            frequency update support, an error is caused if fast_switch
 *                            is not possible.
 *                            Also this function only handles non-shared cpufreq
 *                            policies. If the policy is shared, an error is caused.
 *
 * @memutil_policy: Policy for which the frequency update is made
 * @freq: The frequency (in KHz) that should be set
 * @time: The timestamp (nanosecond resolution) that is associated with this frequency
 *        update
 */
int memutil_set_frequency_to(struct memutil_policy* memutil_policy, unsigned int freq, u64 time)
{
	struct cpufreq_policy	*policy = memutil_policy->policy;

	memutil_policy->last_requested_freq = freq;
	memutil_policy->last_freq_update_time_ns = time;

	if (policy_is_shared(policy)) {
		pr_err_ratelimited("Memutil: Cannot set frequency for shared policy");
		return -EINVAL;
	}

	if (policy->fast_switch_enabled) {
		cpufreq_driver_fast_switch(policy, freq);
	} else {
#if WITH_DEFFERED_FREQ_SWITCH
		memutil_deferred_set_frequency(memutil_policy);
#else
		pr_err_ratelimited("Memutil: Cannot set frequency because fast switch is disabled");
		return -EINVAL;
#endif
	}
	return 0;
}

#if HEURISTIC == HEURISTIC_IPC
/**
 * calculate_frequency_heuristic_ipc - Calculate the frequency to use based on the
 *                                     IPC heuristic (see the wiki page on heuristics)
 * @instructions: Instructions perf event value
 * @cycles: Cycles perf event value
 * @max_freq: Maximum choosable frequency (in KHz)
 * @min_freq: Minimum choosable frequency (in KHz)
 */
unsigned int calculate_frequency_heuristic_ipc(s64 instructions, s64 cycles, int max_freq, int min_freq)
{
	/**
	 * We cannot use floating point arithmetic, so instead we use fixed point arithmetic,
	 * treating values as per-cent by multiplying with 100
	 */
	s64			instructions_per_cycle;
	s64                     interpolation_range;
	s64                     frequency_factor;

	instructions_per_cycle = (instructions * 100) / cycles;

	// Do a linear interpolation:
	interpolation_range = max_ipc - min_ipc;
	frequency_factor = clamp(((instructions_per_cycle - min_ipc) * 100) / interpolation_range, 0LL, 100LL);
	return frequency_factor * (max_freq - min_freq) / 100 + min_freq;
}

#elif HEURISTIC == HEURISTIC_OFFCORE_STALLS

/**
 * calculate_frequency_heuristic_stalls - Calculate the frequency to use based on the
 *                                        offcore stalls heuristic
 *                                        (see the wiki page on heuristics)
 * @stalls: L2 Stalls perf event value
 * @cycles: Cycles perf event value
 * @max_freq: Maximum choosable frequency (in KHz)
 * @min_freq: Minimum choosable frequency (in KHz)
 */
unsigned int calculate_frequency_heuristic_stalls(s64 stalls, s64 cycles, int max_freq, int min_freq)
{
	/**
	 * We cannot use floating point arithmetic, so instead we use fixed point arithmetic,
	 * treating values as per-cent by multiplying with 100
	 */
	s64			stalls_per_cycle;
	s64                     interpolation_range;
	s64                     frequency_factor;

	stalls_per_cycle = (stalls * 100) / cycles;

	// Do a linear interpolation:
	interpolation_range = max_stalls_per_cycle - min_stalls_per_cycle;
	frequency_factor = 100LL - clamp(((stalls_per_cycle - min_stalls_per_cycle) * 100) / interpolation_range, 0LL, 100LL);
	return frequency_factor * (max_freq - min_freq) / 100 + min_freq;
}
#endif

/**
 * memutil_update_frequency - Calculate the frequency which should be used and
 *                            set it for the given policy.
 * @memutil_policy: Policy for which the update is made
 * @time: Timestamp (nanosecond resolution) at which this update is made
 */
void memutil_update_frequency(struct memutil_policy *memutil_policy, u64 time)
{
	u64			event_values[PERF_EVENT_COUNT];
	s64			cycles;
	s64 __maybe_unused	instructions;
	s64 __maybe_unused	offcore_stalls;

	unsigned int		new_frequency;
	int                     max_freq, min_freq, last_freq;

	int			i;

	struct cpufreq_policy 	*policy = memutil_policy->policy;

	//Using unsigned integer math can lead to unwanted underflows, so cast to int as we don't need values >~2'000'000'000
	max_freq = policy->max;
	min_freq = policy->min;
	last_freq = memutil_policy->last_requested_freq;

	/**************************
	 * Read perf event values *
	 **************************/
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
		new_frequency = last_freq;
		//we could assume that a cycles == 0 value means we have a lot of idling
		//in which case reducing the frequency would be good. However we did
		//not test this assumption so we are conservative. Otherwise a line
		//like the following could be used to decrease the frequency step
		//by step
		//new_frequency = max(min_freq, last_freq - (max_freq - min_freq) / 10);
	}
	else {
#if HEURISTIC == HEURISTIC_IPC
		new_frequency = calculate_frequency_heuristic_ipc(instructions, cycles, max_freq, min_freq);
#elif HEURISTIC == HEURISTIC_OFFCORE_STALLS
		new_frequency = calculate_frequency_heuristic_stalls(offcore_stalls, cycles, max_freq, min_freq);
#endif
	}
	// We always set the frequency, see the wiki memutil architecture page
	memutil_set_frequency_to(memutil_policy, new_frequency, time);

	memutil_log_data(time, event_values, policy->cpu, memutil_policy->last_requested_freq, memutil_policy->logbuffer);
}

/********************** cpufreq governor interface *********************/

/**
 * memutil_policy_alloc - allocate and initialize the memutil policy for the given
 *                        cpufreq policy
 * @policy: The cpufreq policy for which a memutil policy should be allocated
 */
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
#if WITH_DEFFERED_FREQ_SWITCH
	raw_spin_lock_init(&memutil_policy->update_lock);
#endif
	return memutil_policy;
}

/**
 * memutil_policy_free - deinitialize and free a previously allocated memutil
 *                       policy
 * @memutil_policy: The policy to free
 */
static void memutil_policy_free(struct memutil_policy *memutil_policy)
{
	kfree(memutil_policy);
}

#if WITH_DEFFERED_FREQ_SWITCH
/**
 * memutil_work - Main function that is executed by the kernel thread to perform
 *                a queued up frequency change
 * @work: Structure for the work to do
 */
static void memutil_work(struct kthread_work *work)
{
	struct memutil_policy *memutil_policy = container_of(work, struct memutil_policy, kthread_work);
	unsigned int frequency;
	unsigned long irq_flags;

	/*
	 * Hold memutil_policy->update_lock shortly to handle the case where:
	 * memutil_policy->next_freq is read here, and then updated by
	 * memutil_deferred_set_frequency() just before freq_update_in_progress is set to false
	 * here, we may miss queueing the new update.
	 * It wouldn't be that dramatic since we do an update periodically but
	 * still it is better to not miss one.
	 *
	 * Note: If a work was queued after the update_lock is released,
	 * memutil_work() will just be called again by kthread_work code; and the
	 * request will be processed before the thread sleeps.
	 */
	raw_spin_lock_irqsave(&memutil_policy->update_lock, irq_flags);
	frequency = memutil_policy->last_requested_freq;
	memutil_policy->freq_update_in_progress = false;
	raw_spin_unlock_irqrestore(&memutil_policy->update_lock, irq_flags);

	__cpufreq_driver_target(memutil_policy->policy, frequency, CPUFREQ_RELATION_L);
}

/**
 * memutil_irq_work - Work function for the queued up frequency change interrupts.
 *                    Simply queues up a freq change on the kernel thread
 * @irq_work: Work item for work that should be done.
 */
static void memutil_irq_work(struct irq_work *irq_work)
{
	struct memutil_policy *memutil_policy;

	memutil_policy = container_of(irq_work, struct memutil_policy, irq_work);
	kthread_queue_work(&memutil_policy->kthread_worker, &memutil_policy->kthread_work);
}

/**
 * memutil_create_worker_thread - Create the kernel thread that will execute queued
 *                                up frequency changes
 * @memutil_policy: Policy for which the thread is created. The thread will be bound
 *                  to the cpu the policy belongs to.
 */
static int memutil_create_worker_thread(struct memutil_policy* memutil_policy)
{
	struct task_struct *thread;
	/*
	 * The scheduling attributes are copied from the schedutil governor because
	 * we want the same behaviour.
	 */
	struct sched_attr attr = {
		.size		= sizeof(struct sched_attr),
		.sched_policy	= SCHED_DEADLINE,
		.sched_flags	= SCHED_FLAG_SUGOV, //we reuse this flag to have same scheduling behaviour as schedutil
		.sched_nice	= 0,
		.sched_priority	= 0,
		/*
		 * Copied comment from schedutil:
		 * Fake (unused) bandwidth; workaround to "fix"
		 * priority inheritance.
		 */
		.sched_runtime	=  1000000,
		.sched_deadline = 10000000,
		.sched_period	= 10000000,
	};
	struct cpufreq_policy *policy = memutil_policy->policy;
	int return_value;

	kthread_init_work(&memutil_policy->kthread_work, memutil_work);
	kthread_init_worker(&memutil_policy->kthread_worker);
	thread = kthread_create(kthread_worker_fn, &memutil_policy->kthread_worker,
				"memutil:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("Memutil: Failed to create kernel thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	return_value = sched_setattr_nocheck(thread, &attr);
	if (return_value) {
		kthread_stop(thread);
		pr_warn("Memutil: Failed to set SCHED_DEADLINE for kernel thread\n");
		return return_value;
	}

	memutil_policy->kthread = thread;
	kthread_bind(thread, policy->cpu);
	init_irq_work(&memutil_policy->irq_work, memutil_irq_work);

	wake_up_process(thread);

	return 0;
}

/**
 * memutil_stop_worker_thread - Stop a previously created kernel thread
 *                              (created by memutil_create_worker_thread)
 * @memutil_policy: Policy for which the worker thread should be stopped
 */
static void memutil_stop_worker_thread(struct memutil_policy *memutil_policy)
{
	kthread_flush_worker(&memutil_policy->kthread_worker);
	kthread_stop(memutil_policy->kthread);
}
#endif

/**
 * memutil_init - Governor init function, see wiki page on memutil architecture.
 * @policy: The cpufreq policy for which memutil is initialized
 */
static int memutil_init(struct cpufreq_policy *policy)
{
	struct memutil_policy 	*memutil_policy;
	int 			return_value = 0;

	pr_info("Memutil: Init module (core=%d)", policy->cpu);

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

	/* create kthread for slow path */
	if (!policy->fast_switch_enabled) {
#if WITH_DEFFERED_FREQ_SWITCH
		return_value = memutil_create_worker_thread(memutil_policy);
		if (return_value) {
			goto free_policy;
		}
#else
		pr_err("Memutil: Fast switching is disabled and this module is build without support for the slow path");
		return_value = -ECANCELED;
		goto free_policy;
#endif
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

/**
 * memutil_exit - Governor exit function, see wiki page on memutil architecture.
 * @policy: The cpufreq policy for which memutil exits
 */
static void memutil_exit(struct cpufreq_policy *policy)
{
	struct memutil_policy *memutil_policy = policy->governor_data;

	pr_info("Memutil: Exiting module (core=%d)", policy->cpu);

	policy->governor_data = NULL;

	/* stop kthread for slow path */
	if (!memutil_policy->policy->fast_switch_enabled) {
#if WITH_DEFFERED_FREQ_SWITCH
		memutil_stop_worker_thread(memutil_policy);
#endif
	}

	memutil_policy_free(memutil_policy);
	cpufreq_disable_fast_switch(policy);
}

/**
 * memutil_this_cpu_can_update - Check whether the current cpu can perform a
 *                               frequency change for the given policy.
 *
 *                               This is the case if either this cpu is the cpu
 *                               the policy is assigned to, i.e. the current cpu
 *                               can update its own frequency.
 *                               The other case is, if a frequency change is possible
 *                               from any cpu, and this cpu does not go offline.
 *                               However to simplify code we simply don't do remote
 *                               frequency updates even if it would be possible.
 * @policy: Policy for which the update should be made
 */
static bool memutil_this_cpu_can_update(struct cpufreq_policy *policy) {
	return cpumask_test_cpu(smp_processor_id(), policy->cpus);
}

/**
 * memutil_should_update_frequency - Check whether a frequency update is needed.
 *                                   As we do one always periodically we simply
 *                                   check whether enough time has passed to do
 *                                   the next update.
 * @memutil_policy: Policy for which to check whether we should update the frequency.
 * @time: Current timestamp (nanosecond resolution)
 */
static bool memutil_should_update_frequency(struct memutil_policy *memutil_policy, u64 time)
{
	s64 delta_ns;

	/*
	 * Stop here for remote requests as calculating the frequency is
	 * pointless if we do not in fact act on it.
	 */
	if (!memutil_this_cpu_can_update(memutil_policy->policy)) {
		return false;
	}

	delta_ns = time - memutil_policy->last_freq_update_time_ns;

	return delta_ns >= memutil_policy->freq_update_delay_ns;
}

/**
 * memutil_update_frequency_hook - Update hook that is called by scheduler. Here we check
 *                            if a frequency update is needed and perform one if
 *                            it is needed.
 * @hook: The data associated with this update hook.
 * @time: Timestamp (nanosecond resolution) for this update call
 */
static void memutil_update_frequency_hook(
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

/**
 * print_start_info - Function to print some information to the kernel log
 *                    when memutil is started
 * @memutil_policy: Policy for which the info should be printed
 * @infofile_data: Data of the infofile. The infofile is accessible in debugfs but
 *                 we want to directly print some information of it here.
 */
static void print_start_info(struct memutil_policy *memutil_policy, struct memutil_infofile_data *infofile_data)
{
	pr_info("Memutil: Starting governor (core=%d)", memutil_policy->policy->cpu);
	if (memutil_policy->policy->cpu != cpumask_first(cpu_online_mask)) {
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
		LOG_RINGBUFFER_SIZE / (MSEC_PER_SEC / infofile_data->update_interval_ms));
}

/**
 * init_logging_once - Do log initialization that should only be performed once
 * @infofile_data: Data for the debugfs infofile
 */
static void init_logging_once(struct memutil_policy *memutil_policy, struct memutil_infofile_data *infofile_data)
{
	if (memutil_policy->policy->cpu != cpumask_first(cpu_online_mask)) {
		return;
	}
	infofile_data->core_count = num_online_cpus(); // cores available to scheduler
	infofile_data->log_ringbuffer_size = LOG_RINGBUFFER_SIZE;

	is_logfile_initialized = memutil_debugfs_init(infofile_data) == 0;
	if (!is_logfile_initialized) {
		pr_warn("Memutil: Failed to initialize memutil debugfs");
	}
}

/**
 * init_logging - Initialize the logging functionality, i.e. create the ringbuffer
 *                and the log- and info-file in the debugfs. See the memutil
 *                architecture wiki page for more information.
 * @memutil_policy: Policy for which logging should be initialized. A ringbuffer
 *                  is created for each cpu, while the debugfs initialization is
 *                  just done once.
 * @infofile_data: Data for the infofile that will be created
 */
static void init_logging(struct memutil_policy *memutil_policy, struct memutil_infofile_data *infofile_data)
{
	debug_info("Memutil: Entering init logging");

	mutex_lock(&memutil_init_mutex);
	init_logging_once(memutil_policy, infofile_data);
	memutil_policy->logbuffer = memutil_open_ringbuffer(LOG_RINGBUFFER_SIZE);
	if (!memutil_policy->logbuffer) {
		pr_warn("Memutil: Failed to create memutil logbuffer");
	} else if (is_logfile_initialized) {
		memutil_debugfs_register_ringbuffer(memutil_policy->logbuffer);
	}
	mutex_unlock(&memutil_init_mutex);
	debug_info("Memutil: Leaving init logging");
}

/**
 * setup_per_cpu_data - Setup memutil data that is needed per cpu
 * @memutil_policy: Policy the cpus are assigned to.
 */
static void setup_per_cpu_data(struct memutil_policy *memutil_policy)
{
	unsigned int cpu;
	debug_info("Memutil: Setting up per CPU data");
	for_each_cpu(cpu, memutil_policy->policy->cpus) {
		struct memutil_cpu *mu_cpu = &per_cpu(memutil_cpu_list, cpu);

		memset(mu_cpu, 0, sizeof(*mu_cpu));
		mu_cpu->cpu 		= cpu;
		mu_cpu->memutil_policy	= memutil_policy;
	}
	debug_info("Memutil: Finished setting up per CPU data");
}

/**
 * install_update_hook - Install the scheduler update hook that will periodically
 *                       call this governor to perform frequency updates.
 * @policy: The policy for which the update hook should perform an update
 */
static void install_update_hook(struct cpufreq_policy *policy)
{
	unsigned int cpu;
	debug_info("Memutil: Setting up CPU update hooks");
	for_each_cpu(cpu, policy->cpus) {
		struct memutil_cpu *mu_cpu = &per_cpu(memutil_cpu_list, cpu);
		cpufreq_add_update_util_hook(cpu, &mu_cpu->update_util, memutil_update_frequency_hook);
	}
}

/**
 * allocate_perf_counters - Allocate the performance counters for that will be used
 *                          by the given policy to calculate the next frequency
 * @policy: Policy for which the counters should be allocated
 */
static int allocate_perf_counters(struct memutil_policy *policy)
{
	char *event_names[PERF_EVENT_COUNT] = {
		event_name1,
		event_name2,
		event_name3
	};

	return memutil_allocate_perf_counters_for_cpu(policy->policy->cpu, event_names, policy->events, PERF_EVENT_COUNT);
}

/**
 * memutil_start - Governor start method (see memutil wiki architecture page)
 * @policy: Policy for which the start is done
 */
 //TODO: Improve error handling where the error occurs on just one cpu / just once during startup
//       (handling of data / functionality that is just initialized once for all cpus / policies
//       does not properly account for that scenario)
static int memutil_start(struct cpufreq_policy *policy)
{
	int return_value;
	struct memutil_infofile_data infofile_data;
	struct memutil_policy *memutil_policy = policy->governor_data;

	memutil_policy->last_freq_update_time_ns	= 0;
	memutil_policy->freq_update_delay_ns	= max(NSEC_PER_USEC * cpufreq_policy_transition_delay_us(policy), 5 * NSEC_PER_MSEC);
#if WITH_DEFFERED_FREQ_SWITCH
	memutil_policy->freq_update_in_progress        = false;
#endif
	infofile_data.update_interval_ms = memutil_policy->freq_update_delay_ns / NSEC_PER_MSEC;

	print_start_info(memutil_policy, &infofile_data);

	init_logging(memutil_policy, &infofile_data);
	if (policy->cpu == cpumask_first(cpu_online_mask)) {
		if (memutil_setup_events_map() != 0) {
			return_value = -1;
			goto fail_events_map;
		}
	}

	return_value = allocate_perf_counters(memutil_policy);
	if (return_value != 0) {
		goto fail_allocate_perf_counters;
	}
	setup_per_cpu_data(memutil_policy);
	install_update_hook(policy);

	return 0;

fail_allocate_perf_counters:
	if (memutil_policy->policy->cpu == cpumask_first(cpu_online_mask)) {
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

/**
 * memutil_stop - Governor stop method (see memutil architecture wiki page)
 * @policy: The policy for which the stop is done
 */
static void memutil_stop(struct cpufreq_policy *policy)
{
	unsigned int cpu;
	struct memutil_policy *memutil_policy = policy->governor_data;

	pr_info("Memutil: Stopping governor (core=%d)", policy->cpu);

	for_each_cpu(cpu, policy->cpus) {
		cpufreq_remove_update_util_hook(cpu);
	}

	synchronize_rcu();

#if WITH_DEFFERED_FREQ_SWITCH
	if (!policy->fast_switch_enabled) {
		irq_work_sync(&memutil_policy->irq_work);
		kthread_cancel_work_sync(&memutil_policy->kthread_work);
	}
#endif

	memutil_release_perf_events(memutil_policy->events, PERF_EVENT_COUNT);
	mutex_lock(&memutil_init_mutex);
	if (is_logfile_initialized) {
		memutil_debugfs_exit();
		is_logfile_initialized = false;
	}
	if (policy->cpu == cpumask_first(cpu_online_mask)) {
		memutil_teardown_events_map();
	}
	mutex_unlock(&memutil_init_mutex);

	if (memutil_policy->logbuffer) {
		memutil_close_ringbuffer(memutil_policy->logbuffer);
	}
}

/**
 * memutil_limits - Governor limits change code (see memutil architecture wiki)
 * @policy: Policy for which the limits changed
 */
static void memutil_limits(struct cpufreq_policy *policy)
{
	pr_info("Memutil: Limits changed (core=%d)", policy->cpu);
}

/**
 * memutil governor data
 */
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
