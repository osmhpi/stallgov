#ifndef _MEMUTIL_LOG_H
#define _MEMUTIL_LOG_H

#include <linux/types.h>
#include <linux/spinlock.h>

struct memutil_perf_data;

struct memutil_ringbuffer {
	struct memutil_perf_data *data;
	int size;
	atomic_t insert_offset;

	raw_spinlock_t update_lock;
};

struct memutil_ringbuffer *memutil_open_ringbuffer(int buffer_size);
void memutil_close_ringbuffer(struct memutil_ringbuffer *buffer);
int memutil_write_ringbuffer(struct memutil_ringbuffer *buffer, struct memutil_perf_data *data, int count);

#endif
