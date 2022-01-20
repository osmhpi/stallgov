#include "memutil_log.h"

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>

#include "memutil_debugfs_log.h"

static void memutil_output_element(struct memutil_perf_data *element, bool write_logfile)
{
	char text[120];
	size_t bytes_written;

	if (!write_logfile) {
		pr_info("Memutil: CPU[%u]: at=%llu value1=%llu, value2=%llu, value3=%llu",
			element->cpu, element->timestamp, element->value1,
			element->value2, element->value3);
		return;
	}
	bytes_written = scnprintf(text, sizeof(text), "%u,%llu,%llu,%llu,%llu\n", element->cpu,
		  element->timestamp, element->value1, element->value2,
		  element->value3);
	memutil_debugfs_append_to_logfile(text, bytes_written);
}

static void memutil_output_data(struct memutil_ringbuffer *buffer, bool write_logfile)
{
	int i, read_offset, valid_size;
	if (buffer->had_wraparound) {
		pr_warn_ratelimited("Memutil: Ringbuffer had wraparound!");
	}
	read_offset = buffer->had_wraparound ? buffer->insert_offset : 0;
	valid_size = buffer->had_wraparound ? buffer->size : buffer->insert_offset;

	for (i = 0; i < valid_size; ++i) {
		memutil_output_element(&buffer->data[read_offset], write_logfile);
		read_offset = (read_offset + 1) % buffer->size;
	}
}

struct memutil_ringbuffer *memutil_open_ringbuffer(u32 buffer_size)
{
	struct memutil_ringbuffer* buffer;
	void* data;
	size_t alloc_size = sizeof(struct memutil_ringbuffer);
	
	pr_info("Memutil: Initializing ringbuffer");
	buffer = (struct memutil_ringbuffer *) kmalloc(alloc_size, GFP_KERNEL | GFP_NOWAIT);
	if (!buffer) {
		pr_warn("Memutil: Failed to allocate buffer of size: %zu", alloc_size);
		return NULL;
	}
	alloc_size = sizeof(struct memutil_perf_data) * buffer_size;
	data = kvmalloc(alloc_size, GFP_KERNEL);
	if (!data) {
		pr_warn("Memutil: Failed to allocate data-buffer of size: %zu", alloc_size);
		kfree(buffer);
		return NULL;
	}
	raw_spin_lock_init(&buffer->lock);
	buffer->data = (struct memutil_perf_data *)data;
	buffer->size = buffer_size;
	buffer->insert_offset = 0;
	buffer->had_wraparound = 0;

	pr_info("Memutil: Ringbuffer ready");
	return buffer;
}

void memutil_close_ringbuffer(struct memutil_ringbuffer *buffer)
{
	//smp_mb();
	//memutil_output_data(buffer, false);
	kvfree(buffer->data);
	kfree(buffer);
}

static void clear_buffer(struct memutil_ringbuffer *buffer)
{
	buffer->had_wraparound = false;
	buffer->insert_offset = 0;
}

int memutil_write_ringbuffer(struct memutil_ringbuffer *buffer, struct memutil_perf_data *data, u32 count)
{
	int i;
	raw_spin_lock(&buffer->lock);
	if (buffer->insert_offset + count >= buffer->size) {
		buffer->had_wraparound = true;
	}
	for (i = 0; i < count; ++i) {
		buffer->data[buffer->insert_offset] = data[i];
		buffer->insert_offset = (buffer->insert_offset + 1) % buffer->size;
	}
	raw_spin_unlock(&buffer->lock);
	return 0;
}

int memutil_ringbuffer_append_to_logfile(struct memutil_ringbuffer *buffer)
{
	struct memutil_ringbuffer buffer_copy;
	size_t alloc_size;

	raw_spin_lock_init(&buffer_copy.lock);
	alloc_size = sizeof(struct memutil_perf_data) * buffer->size;
	buffer_copy.data = kvmalloc(alloc_size, GFP_KERNEL);
	if (!buffer_copy.data) {
		pr_warn("Memutil: Failed to allocate memory for ringbuffer copy");
		return -1;
	}
	buffer_copy.size = buffer->size;

	raw_spin_lock(&buffer->lock);
	memcpy(
		buffer_copy.data,
		buffer->data,
		buffer->had_wraparound ? alloc_size : (buffer->insert_offset*sizeof(struct memutil_perf_data))
	);
	buffer_copy.had_wraparound = buffer->had_wraparound;
	buffer_copy.insert_offset = buffer->insert_offset;
	clear_buffer(buffer);
	raw_spin_unlock(&buffer->lock);

	memutil_output_data(&buffer_copy, true);
	kvfree(buffer_copy.data);
	return 0;
}
