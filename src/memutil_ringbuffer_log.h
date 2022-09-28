// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_ringbuffer_log.h
 *
 * Header file for memutil ringbuffer logging. See the memutil architecture wiki
 * page for more general information on the logging architecture.
 *
 * Copyright (C) 2021-2022 Leon Matthes, Maximilian Stiede, Erik Griese
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#ifndef _MEMUTIL_RINGBUFFER_LOG_H
#define _MEMUTIL_RINGBUFFER_LOG_H

#include <linux/types.h>
#include <linux/spinlock.h>

/**
 * struct memutil_log_entry - Structure for data entries that are logged with
 *                            every frequency update into a ringbuffer.
 *
 * @timestamp: Timestamp for the log entry
 * @perf_value1: First perf event value
 * @perf_value2: Second perf event value
 * @perf_value3: Third perf event value
 * @requested_freq: Frequency that was set / requested by memutil
 * @cpu: The cpu to which the perf values / frequency apply
 */
struct memutil_log_entry {
	u64 timestamp;
	u64 perf_value1;
	u64 perf_value2;
	u64 perf_value3;
	unsigned int requested_freq;
	unsigned int cpu;
};

/**
 * struct memutil_ringbuffer - Structure that defines a memutil ringbuffer.
 *                             This ringbuffer is used to log data with every
 *                             frequency update. The ringbuffer is intended to be
 *                             small and fast, to store the logged data only
 *                             for a couple of seconds until it is copied and
 *                             written to a more long-term log in the form of
 *                             actual formatted text.
 *
 * @lock: spinlock that protects access to this ringbuffer
 * @data: Array of stored log entries
 * @size: The total size of the buffer (in elements)
 * @insert_offset: Offset at which a new element to log should be placed. Used to
 *                 perform the ringbuffer functionality: If the offset reaches the
 *                 end it is set back to point to the start which causes new
 *                 elements to override the oldest ones
 * @had_wraparound: Tracks whether this buffer had at least one wraparound (i.e.
 *                  the insert offset reached the end and was reset to the start)
 */
struct memutil_ringbuffer {
	raw_spinlock_t lock;
	struct memutil_log_entry *data;
	u32 size;
	u32 insert_offset;
	bool had_wraparound;
};

/**
 * memutil_open_ringbuffer - Open a new ringbuffer for writing log data
 *                           Returns NULL on failure.
 *
 *                           Note that this function may sleep.
 * @buffer_size: The size of the buffer in elements. This should be small
 *               (not more than (4MB / sizeof(struct memutil_ringbuffer)). The
 *               buffer is intended to be fast and small.
 */
struct memutil_ringbuffer *memutil_open_ringbuffer(u32 buffer_size);
/**
 * memutil_close_ringbuffer - Close a previously opened ringbuffer
 *
 *                            Note: This function may sleep
 * @buffer: The buffer to close
 */
void memutil_close_ringbuffer(struct memutil_ringbuffer *buffer);
/**
 * memutil_write_ringbuffer - Write the given log entries into the given ringbuffer.
 *
 *                            Note: This function does not sleep.
 * @buffer: The buffer into which the data should be logged
 * @data: Array of log entries that should be written into the buffer
 * @count: Size of the log entry array
 */
void memutil_write_ringbuffer(struct memutil_ringbuffer *buffer, struct memutil_log_entry *data, u32 count);
/**
 * memutil_ringbuffer_append_to_logfile - Append the data of the given ringbuffer to
 *                                        the debugfs logfile and clear the ringbuffer.
 *
 *                                        NOTE again that this also clears the ringbuffer.
 *                                        This function may sleep.
 *
 *                                        This returns 0 on success, otherwise an error
 *                                        code.
 * @buffer: The ringbuffer whose content should be appended
 */
int memutil_ringbuffer_append_to_logfile(struct memutil_ringbuffer *buffer);

#endif //_MEMUTIL_RINGBUFFER_LOG_H
