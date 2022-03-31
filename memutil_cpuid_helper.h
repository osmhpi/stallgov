// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_cpuid_helper.h
 *
 * Header file for helper functions that allow retrieving and matching
 * cpuid strings.
 *
 * COPYRIGHT_PLACEHOLDER
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#ifndef MEMUTIL_CPUID_HELPER_H
#define MEMUTIL_CPUID_HELPER_H

/**
 * memutil_get_cpuid_str - Get the cpuidstr for the current cpu. The format is
 *                         vendor-family-model-stepping
 *
 *                         This function allocates the buffer for the string using
 *                         kmalloc. The caller is expected to free the memory using
 *                         kfree (if the function call was successful).
 *                         The function returns the cpuid string on success. On
 *                         failure an error pointer (see IS_ERR etc.) is returned.
 */
char *memutil_get_cpuid_str(void);
/**
 * memutil_cpuid_matches - Check whether the given cpuid matches a given cpuid pattern.
 *
 *                         Pattern here simply means that the final parts of the
 *                         cpuid can be missing. The format of a cpuid in context
 *                         of this function is vendor-family-model-stepping
 *                         The pattern always has to start with the start of the
 *                         vendor but can stop and omit at any point afterwards.
 *                         E.g. vendor-family is a valid cpuid_pattern. Also
 *                         vendor with just the first letter of the vendor would
 *                         be a valid pattern. The cpuid to match always has to be
 *                         a full cpuid.
 *
 *                         If the cpuid matches the pattern true is returned.
 *                         Otherwise false is returned. In case of invalid arguments
 *                         it is undefined whether true or false is returned.
 * @cpuid_pattern: Pattern that should be matched
 * @cpuid_to_match: Full cpuid for which is checked whether it matches the pattern
 */
bool memutil_cpuid_matches(const char *cpuid_pattern, const char *cpuid_to_match);

#endif //MEMUTIL_CPUID_HELPER_H
