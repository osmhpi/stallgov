#ifndef _MEMUTIL_LOG_H
#define _MEMUTIL_LOG_H

#include <linux/types.h>
#include <linux/spinlock.h>

struct memutil_perf_data {
	u64 timestamp;
	u64 cache_misses;
	u64 cache_references;
	unsigned int cpu;
};

struct memutil_ringbuffer {
	raw_spinlock_t lock;
	struct memutil_perf_data *data;
	u32 size;
	u32 insert_offset;
	bool had_wraparound;
};

struct memutil_ringbuffer *memutil_open_ringbuffer(u32 buffer_size);
void memutil_close_ringbuffer(struct memutil_ringbuffer *buffer);
int memutil_write_ringbuffer(struct memutil_ringbuffer *buffer, struct memutil_perf_data *data, u32 count);
int memutil_ringbuffer_append_to_logfile(struct memutil_ringbuffer *buffer);

#endif
