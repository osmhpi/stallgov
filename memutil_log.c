#include "memutil_log.h"

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/atomic.h>

static int memutil_get_insert_offset(struct memutil_ringbuffer *buffer, int count)
{
	int start_offset, new_offset;
	do {
		start_offset = atomic_read_acquire(&buffer->insert_offset);
		new_offset = (offset + count) % buffer->size;
	} while(atomic_cmpxchg(&buffer->insert_offset, start_offset, new_offset) != start_offset);
	return start_offset;
}

static void memutil_output_element(struct memutil_perf_data *element)
{

}

static void memutil_output_data(struct memutil_ringbuffer *buffer)
{
	int i;
	smp_rmb();
	for (i = 0; i < buffer->size; ++i) {
		memutil_output_element(buffer->data[i]);
	}
}

struct memutil_ringbuffer *memutil_open_ringbuffer(u32 buffer_size)
{
	struct memutil_ringbuffer* buffer;
	void* data;
	buffer = (struct memutil_ringbuffer *) kmalloc(sizeof(struct memutil_ringbuffer), GFP_KERNEL);
	if (!buffer) {
		return NULL;
	}
	data = kzalloc(sizeof(struct memutil_perf_data) * buffer_size, GFP_KERNEL);
	if (!data) {
		kfree(buffer);
		return NULL;
	}
	buffer->data = (struct memutil_perf_data *)data;
	buffer->size = buffer_size;
	atomic_set_release(&buffer->insert_offset, 0);

	return buffer;
}

void memutil_close_ringbuffer(struct memutil_ringbuffer *buffer)
{
	memutil_output_data(buffer);
	kfree(buffer->data);
	kfree(buffer);
}

int memutil_write_ringbuffer(struct memutil_ringbuffer *buffer, struct memutil_perf_data *data, int count)
{
	int i;
	int offset;

	offset = memutil_get_insert_offset(buffer, count);
	for (i = 0; i < count; ++i) {
		buffer->data[offset % buffer->count] = data[count];
		++offset;
	}
}