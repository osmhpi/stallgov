// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_debugfs_logfile.h
 *
 * Header file for the memutil debugfs logfile functionality. The logfile
 * provides data that was logged to the user in the form of a text file. For
 * more information see the memutil architecture wiki page.
 *
 * COPYRIGHT_PLACEHOLDER
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#ifndef _MEMUTIL_DEBUGFS_LOGFILE_H
#define _MEMUTIL_DEBUGFS_LOGFILE_H

#include <linux/types.h>
#include <linux/fs.h>

struct memutil_ringbuffer;

/**
 * memutil_debugfs_logfile_init - Initialize / create the memutil logfile in the
 *                                "<debugfs>/memutil" folder.
 *
 *                                This function may sleep.
 *                                If this function succeeds it returns 0, otherwise
 *                                an error code is returned.
 * @root_dir: The folder in which the logfile should be created
 */
int memutil_debugfs_logfile_init(struct dentry *root_dir);
/**
 * memutil_debugfs_logfile_exit - Deinitialize / remove the logfile from the memutil
 *                                debugfs folder
 */
void memutil_debugfs_logfile_exit(void);
/**
 * memutil_debugfs_register_ringbuffer - Register the given ringbuffer as someone
 *                                       who wants to write log data to the logfile.
 *                                       Because the logging works in a way where
 *                                       the data is only written to the logfile
 *                                       when the user reads it, the ringbuffers
 *                                       have to register themself to be called
 *                                       when the user reads the log. Then the
 *                                       ringbuffers can append their content
 *                                       to the log before the user gets the data.
 *                                       See the memutil architecture wiki page.
 *
 *                                       On success 0 is returned, otherwise an
 *                                       error code is returned.
 * @buffer: The ringbuffer which is registered
 */
int memutil_debugfs_register_ringbuffer(struct memutil_ringbuffer *buffer);
/**
 * memutil_debugfs_append_to_logfile - Append the given text to the logfile.
 *
 *                                     Note that the text that is appended may not
 *                                     be larger than the logfile itself, otherwise
 *                                     this function fails. If the logfile is to full
 *                                     to hold the text, the logfile is cleared and
 *                                     the text appended.
 *
 *                                     On success this function returns 0, otherwise
 *                                     an error code is returned.
 * @buffer: Text that should be appended
 * @buffer_size: Size of the given buffer
 */
int memutil_debugfs_append_to_logfile(char *buffer, size_t buffer_size);

#endif //_MEMUTIL_DEBUGFS_LOGFILE_H
