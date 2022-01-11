#ifndef _MEMUTIL_DEBUGFS_INFO_H
#define _MEMUTIL_DEBUGFS_INFO_H

#include <linux/types.h>
#include <linux/fs.h>

struct memutil_infofile_data {
    unsigned int core_count;
    unsigned int update_interval; // in ms
};

int memutil_debugfs_info_init(struct dentry *root_dir, const struct memutil_infofile_data data);
int memutil_debugfs_info_exit(struct dentry *root_dir);

#endif
