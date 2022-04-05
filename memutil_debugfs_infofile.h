// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_debugfs_infofile.h
 *
 * Header file for the implementation of the debugfs infofile.
 *
 * The debugfs infofile provides some information about memutil in a text file.
 * The information contains: The amount of cores that are online,
 * the interval with which memutil does frequency updates, the size of the log
 * ringbuffers. The format is:
 * core_count=<core_count>
 * update_interval=<update_interval_milliseconds>
 * log_ringbuffer_size=<log_ringbuffer_size>
 *
 * Copyright (C) 2021-2022 Leon Matthes, Maximilian Stiede, Erik Griese
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#ifndef _MEMUTIL_DEBUGFS_INFOFILE_H
#define _MEMUTIL_DEBUGFS_INFOFILE_H

#include <linux/types.h>
#include <linux/fs.h>

/**
 * struct memutil_infofile_data - Data that the infofile provides to the userspace
 *
 * @core_count: Number of online cpus
 * @update_interval_ms: Interval with which memutil does frequency updates
 *                      (in milliseconds)
 * @log_ringbuffer_size: Size of the log ringbuffers
 */
struct memutil_infofile_data {
	unsigned int core_count;
	unsigned int update_interval_ms;
	unsigned int log_ringbuffer_size;
};

/**
 * memutil_debugfs_infofile_init - Initialize / create the memutil infofile under the
 *                                 <debugfs>/memutil folder
 *
 *                                 This function may sleep.
 *                                 If the function succeeds it returns 0, otherwise
 *                                 an error code is returned.
 * @root_dir: Directory in which the infofile should be created
 * @data: Data that the infofile will provide
 */
int memutil_debugfs_infofile_init(struct dentry *root_dir, struct memutil_infofile_data *data);
/**
 * memutil_debugfs_infofile_exit - Deinitialize / remove the infofile
 */
void memutil_debugfs_infofile_exit(void);

#endif //_MEMUTIL_DEBUGFS_INFOFILE_H
