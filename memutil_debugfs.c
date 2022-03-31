// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_debugfs.c
 *
 * Implementation file for general memutil debugfs functionality.
 *
 * COPYRIGHT_PLACEHOLDER
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#include <linux/debugfs.h>
#include <linux/fs.h>

#include "memutil_debugfs.h"
#include "memutil_debugfs_logfile.h"
#include "memutil_debugfs_infofile.h"

/** The root memutil debugfs directory */
static struct dentry *root_dir = NULL;

int memutil_debugfs_init(struct memutil_infofile_data *infofile_data)
{
	int return_value = 0;
	if (root_dir != NULL) {
		//already initialized
		return 0;
	}
	root_dir = debugfs_create_dir("memutil", NULL);
	if (IS_ERR(root_dir)) {
		pr_warn("Memutil: Failed to initialize memutil debugfs root");
		return_value = PTR_ERR(root_dir);
		goto rootdir_error;
	}
	return_value = memutil_debugfs_logfile_init(root_dir);
	if (return_value != 0) {
		pr_warn("Memutil: Failed to initialize memutil debugfs log file");
		goto logfile_error;
	}
	return_value = memutil_debugfs_infofile_init(root_dir, infofile_data);
	if (return_value != 0) {
		pr_warn("Memutil: Failed to initialize memutil debugfs info file");
		goto infofile_error;
	}
	pr_info("Memutil: Initialized memutil debugfs (<debugfs>/memutil)");
	return 0;

infofile_error:
	memutil_debugfs_logfile_exit();
logfile_error:
	debugfs_remove_recursive(root_dir);
rootdir_error:
	root_dir = NULL;
	return return_value;
}

void memutil_debugfs_exit(void)
{
	memutil_debugfs_logfile_exit();
	memutil_debugfs_infofile_exit();
	debugfs_remove_recursive(root_dir);
	root_dir = NULL;
}
