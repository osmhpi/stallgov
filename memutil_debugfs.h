#ifndef _MEMUTIL_DEBUGFS_H
#define _MEMUTIL_DEBUGFS_H

#include <linux/types.h>

struct memutil_ringbuffer;

int memutil_debugfs_init(void);
int memutil_debugfs_exit(void);
int memutil_debugfs_append_to_logfile(char *buffer, size_t buffer_size);
int memutil_debugfs_register_ringbuffer(struct memutil_ringbuffer *buffer);

#endif
