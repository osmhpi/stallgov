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
#include <linux/perf_event.h>
#include <linux/err.h>

struct memutil_policy {
	struct cpufreq_policy *policy;

	raw_spinlock_t		update_lock; /* For shared policies */
	u64			last_freq_update_time;
	s64			freq_update_delay_ns;

	struct perf_event	*perf_event;
};

struct memutil_cpu {
	struct update_util_data update_util;
	struct memutil_policy	*memutil_policy;
	unsigned int cpu;

	u64			last_update;
};

static DEFINE_PER_CPU(struct memutil_cpu, memutil_cpu_list);

void memutil_set_frequency(struct memutil_policy *memutil_policy)
{

	int			perf_result;
	u64			perf_value;
	u64			enabled_time;
	u64			running_time;
	struct cpufreq_policy 	*policy = memutil_policy->policy;

	if(memutil_policy->policy->cpu == 0) {
		perf_result = perf_event_read_local(
				memutil_policy->perf_event,
				&perf_value,
				&enabled_time,
				&running_time); 

		if(likely(perf_result == 0)) {
			pr_info_ratelimited("Perf value %llu", perf_value);
		} else {
			pr_info_ratelimited("Perf read failed: %d", perf_result);
		}
	}

	if (!policy_is_shared(policy) && policy->fast_switch_enabled) {
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
	raw_spin_lock_init(&memutil_policy->update_lock);
	return memutil_policy;
}

static void memutil_policy_free(struct memutil_policy *memutil_policy)
{
	kfree(memutil_policy);
}

static long __must_check memutil_allocate_perf_counter(struct memutil_policy *policy) 
{
	struct perf_event_attr perf_attr;
	struct perf_event *perf_event;

	memset(&perf_attr, 0, sizeof(perf_attr));

	perf_attr.type = PERF_TYPE_HARDWARE;
	perf_attr.size = sizeof(perf_attr);
	perf_attr.config = PERF_COUNT_HW_INSTRUCTIONS;
	perf_attr.disabled = 0; // enable the event by default
	perf_attr.exclude_kernel = 1;
	perf_attr.exclude_hv = 1;

	perf_event = perf_event_create_kernel_counter(
			&perf_attr,
			policy->policy->cpu,
			/*task*/ NULL, 
			/*overflow_handler*/ NULL,
			/*context*/ NULL);

	if(unlikely(IS_ERR(perf_event))) {
		return PTR_ERR(perf_event);
	}

	policy->perf_event = perf_event;
	return 0;
}

static void memutil_release_perf_event(struct memutil_policy *policy)
{
	if(unlikely(policy->perf_event == NULL)) {
		pr_warn("Tried to release perf_event which is NULL");
		return;
	}
	perf_event_release_kernel(policy->perf_event);
}

static int memutil_init(struct cpufreq_policy *policy)
{
	struct memutil_policy 	*memutil_policy;
	int 			return_value = 0;

	printk(KERN_INFO "Loading memutil module");

	/* State should be equivalent to EXIT */
	if (policy->governor_data) {
		return -EBUSY;
	}

	cpufreq_enable_fast_switch(policy);

	memutil_policy = memutil_policy_alloc(policy);
	if (!memutil_policy) {
		pr_err("Failed to allocate memutil policy!");
		return_value = -ENOMEM;
		goto disable_fast_switch;
	}

	policy->governor_data = memutil_policy;

	return_value = (int) memutil_allocate_perf_counter(memutil_policy);
	if (unlikely(return_value != 0)) {
		pr_err("Failed to acquire hardware performance counter");
		goto free_policy;
	}

	return 0;

free_policy:
	memutil_policy_free(memutil_policy);
	// fallthrough
disable_fast_switch:
	cpufreq_disable_fast_switch(policy);
	pr_err("memutil_init failed (error %d)\n", return_value);
	return return_value;
}

static void memutil_exit(struct cpufreq_policy *policy)
{
	struct memutil_policy *memutil_policy = policy->governor_data;

	printk(KERN_INFO "Exiting memutil module");
	pr_info("cpus: %px smp_processor_id: %d", policy->cpus->bits, smp_processor_id());

	policy->governor_data = NULL;

	memutil_release_perf_event(memutil_policy);
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

	memutil_set_frequency(memutil_policy);
	memutil_policy->last_freq_update_time = time;
}

static int memutil_start(struct cpufreq_policy *policy)
{
	struct memutil_policy *memutil_policy = policy->governor_data;
	unsigned int cpu;
	printk(KERN_INFO "Starting memutil governor");

	memutil_policy->last_freq_update_time	= 0;
	memutil_policy->freq_update_delay_ns	= NSEC_PER_USEC * cpufreq_policy_transition_delay_us(policy);

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

	printk(KERN_INFO "Stopping memutil governor");
	// TODO
	//
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
