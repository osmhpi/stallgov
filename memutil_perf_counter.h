#ifndef _MEMUTIL_PERF_COUNTER_H
#define _MEMUTIL_PERF_COUNTER_H

#include <linux/perf_event.h>

int memutil_setup_events_map(void);
void memutil_teardown_events_map(void);

long memutil_allocate_perf_counters_for_cpu(unsigned int cpu, char **event_names, struct perf_event **events_array, int event_count);
void memutil_release_perf_events(struct perf_event **events_array, int array_size);

#endif
