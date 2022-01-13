#include "memutil_debugfs.h"

#include <linux/debugfs.h>
#include <linux/fs.h>

#include "memutil_debugfs_log.h"
#include "memutil_debugfs_info.h"

static struct dentry *root_dir = NULL;

int memutil_debugfs_init(const struct memutil_infofile_data infofile_data)
{
	if (root_dir != NULL) {
		return 0;
	}
	root_dir = debugfs_create_dir("memutil", NULL);
	if (IS_ERR(root_dir)) {
		pr_warn("Memutil: Failed to initialize memutil debugfs root");
		goto rootdir_error;
	}
	if (memutil_debugfs_log_init(root_dir) != 0) {
		pr_warn("Memutil: Failed to initialize memutil debugfs log file");
		goto filedata_error;
	}
	if (memutil_debugfs_info_init(root_dir, infofile_data) != 0) {
		pr_warn("Memutil: Failed to initialize memutil debugfs info file");
		goto filedata_error;
	}
	pr_info("Memutil: Initialized memutil debugfs (/sys/kernel/debug)");
	return 0;

filedata_error:
	debugfs_remove_recursive(root_dir);
rootdir_error:
	root_dir = NULL;
	return -1;
}

int memutil_debugfs_exit(void)
{
	memutil_debugfs_log_exit(root_dir);
	memutil_debugfs_info_exit(root_dir);
	debugfs_remove_recursive(root_dir);
	root_dir = NULL;
	return 0;
}
