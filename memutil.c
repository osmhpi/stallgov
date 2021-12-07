#include <linux/module.h> // included for all kernel modules
#include <linux/kernel.h> // included for KERN_INFO
#include <linux/init.h> // included for __init and __exit macros
#include <linux/sched/cpufreq.h>
#include <trace/events/power.h>

static int memutil_init(struct cpufreq_policy *policy)
{
	printk(KERN_INFO "Loading memutil module");
	// TODO
	return 0;
}

static void memutil_exit(struct cpufreq_policy *policy)
{
	printk(KERN_INFO "Exiting memutil module");
	// TODO
}

static int memutil_start(struct cpufreq_policy *policy)
{
	printk(KERN_INFO "Starting memutil governor");
	// TODO
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
	.name	= "memutil",
	.owner	= THIS_MODULE,
	.flags	= CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init	= memutil_init,
	.exit	= memutil_exit,
	.start	= memutil_start,
	.stop	= memutil_stop,
	.limits = memutil_limits,
};

MODULE_LICENSE(		"GPL");
MODULE_AUTHOR(		"Erik Griese <erik.griese@student.hpi.de>, "
			"Leon Matthes <leon.matthes@student.hpi.de>, "
			"Maximilian Stiede <maximilian.stiede@student.hpi.de>");
MODULE_DESCRIPTION(	"A CpuFreq governor based on Memory Access Patterns.");

cpufreq_governor_init(memutil_gov);
cpufreq_governor_exit(memutil_gov);
