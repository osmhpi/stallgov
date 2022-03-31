// SPDX-License-Identifier: GPL-2.0-only
/*
 * pmu_events.h
 *
 * Header file for defining platform specific events. Taken from
 * linux/tools/perf/perf/pmu-events/pmu-events.h
 *
 * COPYRIGHT_PLACEHOLDER
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#ifndef MEMUTIL_PMU_EVENTS_H
#define MEMUTIL_PMU_EVENTS_H

/*
 * Describe each PMU event. Each CPU has a table of PMU events.
 */
struct pmu_event {
	const char *name;
	const char *compat;
	const char *event;
	const char *desc;
	const char *topic;
	const char *long_desc;
	const char *pmu;
	const char *unit;
	const char *perpkg;
	const char *aggr_mode;
	const char *metric_expr;
	const char *metric_name;
	const char *metric_group;
	const char *deprecated;
	const char *metric_constraint;
};

/*
 *
 * Map a CPU to its table of PMU events. The CPU is identified by the
 * cpuid field, which is an arch-specific identifier for the CPU.
 * The identifier specified must match the get_cpuid_str()
 * in memutil_cpuid_helper.c
 *
 * The  cpuid can contain any character other than the comma.
 */
struct pmu_events_map {
	const char *cpuid;
	const char *version;
	const char *type;		/* core, uncore etc */
	struct pmu_event *table;
};

/*
 * Global table mapping each known CPU for the architecture to its
 * table of PMU events.
 */
extern struct pmu_events_map memutil_pmu_events_map[];

#endif //MEMUTIL_PMU_EVENTS_H
