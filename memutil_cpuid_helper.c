// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_cpuid_helper.c
 *
 * Implementation file for helper functions that allow retrieving and matching
 * cpuid strings.
 *
 * COPYRIGHT_PLACEHOLDER
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/err.h>

#include "memutil_cpuid_helper.h"

char *memutil_get_cpuid_str(void)
{
	int num_bytes;
	size_t buffer_size = 128;
	char *buffer = kmalloc(buffer_size, GFP_KERNEL);
	if (!buffer) {
		return ERR_PTR(-ENOMEM);
	}
	num_bytes = snprintf(buffer, buffer_size, "%s-%u-%X-%X",
			      boot_cpu_data.x86_vendor_id, boot_cpu_data.x86,
			      boot_cpu_data.x86_model, boot_cpu_data.x86_stepping);

	/* look for end marker to ensure the entire data fit */
	if (num_bytes >= buffer_size) {
		pr_err("Memutil: Get cpuid string, format-string buffer to small, "
		       "needs %d bytes", num_bytes+1);
		kfree(buffer);
		return ERR_PTR(-ENOBUFS);
	}
	return buffer;
}

/**
 * is_full_cpuid - Check whether the given cpuid is a full cpuid, i.e all components
 *                 of the format vendor-family-model-stepping are present
 *
 *                 Returns true if the given id is a full cpuid, otherwise returns
 *                 false.
 * @id: Cpuid to check
 */
static bool is_full_cpuid(const char *id)
{
	int count = 0;

	while ((id = strchr(id, '-')) != NULL) {
		count++;
		id++;
	}

	return count == 3;
}

bool memutil_cpuid_matches(const char *cpuid_pattern, const char *cpuid_to_match)
{
	if (!is_full_cpuid(cpuid_to_match)) {
		pr_err("Memutil: Invalid CPUID %s. Full CPUID is required, "
			"vendor-family-model-stepping\n", cpuid_to_match);
		return false;
	}
	return !strncmp(cpuid_pattern, cpuid_to_match, strlen(cpuid_pattern));
}
