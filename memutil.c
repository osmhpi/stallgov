#include <linux/module.h> // included for all kernel modules
#include <linux/kernel.h> // included for KERN_INFO
#include <linux/init.h> // included for __init and __exit macros
#include <linux/sched/cpufreq.h>
#include <trace/events/power.h>

struct memutil_policy {
	struct cpufreq_policy *policy;

	raw_spinlock_t update_lock; /* For shared policies */
};

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

static int memutil_init(struct cpufreq_policy *policy)
{
	printk(KERN_INFO "Loading memutil module");
	struct memutil_policy *memutil_policy;
	int return_value = 0;

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
	printk(KERN_INFO "Exiting memutil module");

	struct memutil_policy *memutil_policy = policy->governor_data;

	policy->governor_data = NULL;

	memutil_policy_free(memutil_policy);
	cpufreq_disable_fast_switch(policy);
}

static int memutil_start(struct cpufreq_policy *policy)
{
	printk(KERN_INFO "Starting memutil governor");

	struct memutil_policy *memutil_policy = policy->governor_data;

	if (policy_is_shared(policy) && policy->fast_switch_enabled &&
	    cpufreq_driver_has_adjust_perf()) {
		memutil_set_frequency();
	} else {
		pr_err("Can't set frequency");
	}

	return 0;
}

static void memutil_stop(struct cpufreq_policy *policy)
{
	printk(KERN_INFO "Stopping memutil governor");
	// TODO
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
