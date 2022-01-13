#include <linux/types.h>

#include "pmu_cpuid_helper.h"
#include <asm/processor.h>
#include <linux/slab.h>

static int
__get_cpuid(char *buffer, size_t sz, const char *fmt)
{
	int nb;

	nb = scnprintf(buffer, sz, fmt, boot_cpu_data.x86_vendor_id, boot_cpu_data.x86, boot_cpu_data.x86_model, boot_cpu_data.x86_stepping);

	/* look for end marker to ensure the entire data fit */
	if (strchr(buffer, '$')) {
		buffer[nb-1] = '\0';
		return 0;
	}
	return ENOBUFS;
}

char *
memutil_get_cpuid_str(void)
{
	char *buf = kmalloc(128, GFP_KERNEL);

	if (buf && __get_cpuid(buf, 128, "%s-%u-%X-%X$") < 0) {
		kfree(buf);
		return NULL;
	}
	return buf;
}

/* Full CPUID format for x86 is vendor-family-model-stepping */
static bool is_full_cpuid(const char *id)
{
	const char *tmp = id;
	int count = 0;

	while ((tmp = strchr(tmp, '-')) != NULL) {
		count++;
		tmp++;
	}

	if (count == 3)
		return true;

	return false;
}

int memutil_strcmp_cpuid_str(const char *mapcpuid, const char *id)
{
	bool full_mapcpuid = is_full_cpuid(mapcpuid);
	bool full_cpuid = is_full_cpuid(id);

	/*
	 * Full CPUID format is required to identify a platform.
	 * Error out if the cpuid string is incomplete.
	 */
	if (full_mapcpuid && !full_cpuid) {
		pr_info("Invalid CPUID %s. Full CPUID is required, "
			"vendor-family-model-stepping\n", id);
		return 1;
	}
	return strncmp(mapcpuid, id, strlen(mapcpuid));
}
