// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_perf_counter.h
 *
 * Header file for perf counter functionality. This allows allocating perf counters
 * specifying either a portable event name (like cycles, instructions) or a
 * platform specific event name that is specified in "pmu_events.c".
 *
 * Portable events are the following (see the perf_event_open syscall manpage for more info)
    | name | perf_event_open type | perf_event_open config |
    | ---- | -------------------- | ---------------------- |
    | cycles | PERF_TYPE_HARDWARE | PERF_COUNT_HW_CPU_CYCLES |
    | instructions | PERF_TYPE_HARDWARE | PERF_COUNT_HW_INSTRUCTIONS |
    | cache-references | PERF_TYPE_HARDWARE | PERF_COUNT_HW_CACHE_REFERENCES |
    | cache-misses | PERF_TYPE_HARDWARE | PERF_COUNT_HW_CACHE_MISSES |
    | branch-instructions | PERF_TYPE_HARDWARE | PERF_COUNT_HW_BRANCH_INSTRUCTIONS |
    | branch-misses | PERF_TYPE_HARDWARE | PERF_COUNT_HW_BRANCH_MISSES |
    | bus-cycles | PERF_TYPE_HARDWARE | PERF_COUNT_HW_BUS_CYCLES |
    | stalled-cycles-frontend | PERF_TYPE_HARDWARE | PERF_COUNT_HW_STALLED_CYCLES_FRONTEND |
    | stalled-cycles-backend | PERF_TYPE_HARDWARE | PERF_COUNT_HW_STALLED_CYCLES_BACKEND |
    | ref-cycles | PERF_TYPE_HARDWARE | PERF_COUNT_HW_REF_CPU_CYCLES |
    | cpu-clock | PERF_TYPE_SOFTWARE | PERF_COUNT_SW_CPU_CLOCK |
    | l1d-read | PERF_TYPE_HW_CACHE | PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16) |
    | l1d-read-miss | PERF_TYPE_HW_CACHE | PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) |
    | l1d-write | PERF_TYPE_HW_CACHE | PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16) |
    | l1d-write-miss | PERF_TYPE_HW_CACHE | PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) |
    | l1i-read | PERF_TYPE_HW_CACHE | PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16) |
    | l1i-read-miss | PERF_TYPE_HW_CACHE | PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) |
    | l1i-write | PERF_TYPE_HW_CACHE | PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16) |
    | l1i-write-miss | PERF_TYPE_HW_CACHE | PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) |
    | ll-read | PERF_TYPE_HW_CACHE | PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16) |
    | ll-read-miss | PERF_TYPE_HW_CACHE | PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) |
    | ll-write | PERF_TYPE_HW_CACHE | PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16) |
    | ll-write-miss | PERF_TYPE_HW_CACHE | PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) |
 *
 * For platform specific event names see "perf list --details" and the pmu_events.c file
 *
 * COPYRIGHT_PLACEHOLDER
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#ifndef _MEMUTIL_PERF_COUNTER_H
#define _MEMUTIL_PERF_COUNTER_H

#include <linux/perf_event.h>

/**
 * memutil_setup_events_map - Setup which pmu_events_map we use depending on the cpuid.
 *                            This map maps cpuid strings to a collection of platform
 *                            specific events for that cpuid.
 *
 *                            This function may sleep.
 *                            Returns 0 on success, otherwise an error code is returned.
 */
int memutil_setup_events_map(void);
/**
 * memutil_teardown_events_map - Teardown a setup events map
 */
void memutil_teardown_events_map(void);

/**
 * memutil_allocate_perf_counters_for_cpu - Allocate / create the given perf events.
 *                                          The events map has to be setup prior
 *                                          to calling this function.
 *
 *                                          This function may sleep.
 *                                          On success 0 is returned, otherwise an
 *                                          error code is returned.
 * @cpu: Cpu for which the perf events should be allocated.
 * @event_names: Array of names of the events to allocate. This is one of the event
 *               names described at the start of this header (e.g. cycles)
 * @events_array: Array into which the allocated events will be written
 * @event_count: Amount of events to allocate
 */
int memutil_allocate_perf_counters_for_cpu(unsigned int cpu, char **event_names, struct perf_event **events_array, int event_count);
/**
 * memutil_release_perf_events - Release previously allocated perf events.
 *
 *                               This function may sleep.
 * @events_array: Array of events to release.
 * @array_size: Size of the events array
 */
void memutil_release_perf_events(struct perf_event **events_array, int array_size);

#endif //_MEMUTIL_PERF_COUNTER_H
