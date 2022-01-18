#ifndef _MEMUTIL_PERF_READ_LOCAL_H
#define _MEMUTIL_PERF_READ_LOCAL_H

int memutil_perf_event_read_local(struct perf_event *event, u64 *value,
			  u64 *enabled, u64 *running);

#endif
