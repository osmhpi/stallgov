#ifndef _MEMUTIL_DEBUGFS_LOG_H
#define _MEMUTIL_DEBUGFS_LOG_H

#include <linux/types.h>
#include <linux/fs.h>

struct memutil_ringbuffer;

int memutil_debugfs_log_init(struct dentry *root_dir);
int memutil_debugfs_log_exit(struct dentry *root_dir);
int memutil_debugfs_append_to_logfile(char *buffer, size_t buffer_size);
int memutil_debugfs_register_ringbuffer(struct memutil_ringbuffer *buffer);

#endif
