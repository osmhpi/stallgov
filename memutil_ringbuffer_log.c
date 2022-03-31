// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_ringbuffer_log.c
 *
 * Implementation for memutil ringbuffer logging. See the memutil architecture wiki
 * page for more general information on the logging architecture.
 *
 * COPYRIGHT_PLACEHOLDER
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#include <linux/types.h>
#include <linux/slab.h> //kmalloc
#include <linux/mm.h> //kvmalloc

#include "memutil_ringbuffer_log.h"
#include "memutil_debugfs_logfile.h"
#include "memutil_printk_helper.h"

/**
 * memutil_output_element - Format the given log entry as string and append that
 *                          string to the debugfs logfile
 * @element: log element that should be written
 */
static void memutil_output_element(struct memutil_log_entry *element)
{
	char text[130];
	size_t bytes_written;

	bytes_written = scnprintf(text, sizeof(text), "%u,%llu,%llu,%llu,%llu,%u\n", element->cpu,
		  element->timestamp,
		  element->perf_value1,
		  element->perf_value2,
		  element->perf_value3,
		  element->requested_freq);
	memutil_debugfs_append_to_logfile(text, bytes_written);
}

/**
 * memutil_output_data - Format the content of the given ringbuffer as string and
 *                       append it to the debugfs logfile
 * @buffer: Ringbuffer whose content should be written
 */
static void memutil_output_data(struct memutil_ringbuffer *buffer)
{
	int i, read_offset, valid_size;
	if (buffer->had_wraparound) {
		pr_warn_ratelimited("Memutil: Ringbuffer had wraparound! Loss of data!");
	}
	read_offset = buffer->had_wraparound ? buffer->insert_offset : 0;
	valid_size = buffer->had_wraparound ? buffer->size : buffer->insert_offset;

	for (i = 0; i < valid_size; ++i) {
		memutil_output_element(&buffer->data[read_offset]);
		read_offset = (read_offset + 1) % buffer->size;
	}
}

struct memutil_ringbuffer *memutil_open_ringbuffer(u32 buffer_size)
{
	struct memutil_ringbuffer* buffer;
	void* data;
	size_t alloc_size = sizeof(struct memutil_ringbuffer);
	
	debug_info("Memutil: Initializing ringbuffer");
	buffer = (struct memutil_ringbuffer *) kmalloc(alloc_size, GFP_KERNEL);
	if (!buffer) {
		pr_warn("Memutil: Failed to allocate buffer of size: %zu", alloc_size);
		return NULL;
	}
	alloc_size = sizeof(struct memutil_log_entry) * buffer_size;
	data = kvmalloc(alloc_size, GFP_KERNEL);
	if (!data) {
		pr_warn("Memutil: Failed to allocate data-buffer of size: %zu", alloc_size);
		kfree(buffer);
		return NULL;
	}
	raw_spin_lock_init(&buffer->lock);
	buffer->data = (struct memutil_log_entry *)data;
	buffer->size = buffer_size;
	buffer->insert_offset = 0;
	buffer->had_wraparound = 0;

	debug_info("Memutil: Ringbuffer ready");
	return buffer;
}

void memutil_close_ringbuffer(struct memutil_ringbuffer *buffer)
{
	kvfree(buffer->data);
	kfree(buffer);
}

/**
 * clear_buffer - Clear the given ringbuffer. This simply resets the buffer's
 *                insert_offset and had_wraparound member.
 * @buffer: Buffer to clear
 */
static void clear_buffer(struct memutil_ringbuffer *buffer)
{
	buffer->had_wraparound = false;
	buffer->insert_offset = 0;
}

void memutil_write_ringbuffer(struct memutil_ringbuffer *buffer, struct memutil_log_entry *data, u32 count)
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
}

int memutil_ringbuffer_append_to_logfile(struct memutil_ringbuffer *buffer)
{
	struct memutil_ringbuffer buffer_copy;
	size_t alloc_size;
	unsigned long irqflags;

	raw_spin_lock_init(&buffer_copy.lock);
	alloc_size = sizeof(struct memutil_log_entry) * buffer->size;
	buffer_copy.data = kvmalloc(alloc_size, GFP_KERNEL);
	if (!buffer_copy.data) {
		pr_warn("Memutil: Failed to allocate memory for ringbuffer copy");
		return -ENOMEM;
	}
	buffer_copy.size = buffer->size;

	//We not only have to acquire the lock but also need to disable interrupts.
	//Otherwise an interrupt could cause the update_frequency code of memutil
	//to run while we hold the lock. As the update_frequency code is run
	//in a context that is not interruptable, it would deadlock trying to acquire
	//the lock we hold while we do not get the possibility to release the lock.
	raw_spin_lock_irqsave(&buffer->lock, irqflags);
	memcpy(
		buffer_copy.data,
		buffer->data,
		buffer->had_wraparound ? alloc_size : (buffer->insert_offset*sizeof(struct memutil_log_entry))
	);
	buffer_copy.had_wraparound = buffer->had_wraparound;
	buffer_copy.insert_offset = buffer->insert_offset;
	clear_buffer(buffer);
	raw_spin_unlock_irqrestore(&buffer->lock, irqflags);

	memutil_output_data(&buffer_copy);
	kvfree(buffer_copy.data);
	return 0;
}
