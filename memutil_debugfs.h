#ifndef _MEMUTIL_DEBUGFS_H
#define _MEMUTIL_DEBUGFS_H

#include <linux/types.h>

#include "memutil_debugfs_info.h"

int memutil_debugfs_init(const struct memutil_infofile_data infofile_data);
int memutil_debugfs_exit(void);

#endif
