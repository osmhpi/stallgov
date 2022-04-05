// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_debugfs_infofile.c
 *
 * Implementation file for the implementation of the debugfs infofile.
 *
 * Copyright (C) 2021-2022 Leon Matthes, Maximilian Stiede, Erik Griese
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include "memutil_debugfs_infofile.h"

/** The infofile filesystem entry */
static struct dentry *info_file = NULL;

/** The textual data of the infofile */
static char *infofile_text_data;
/** The blob wrapper for the infofile text data */
static struct debugfs_blob_wrapper *infofile_data_blob;

/** The formatstring for the infofile */
static char const *output_fmtstr =
	"core_count=%u\n"
	"update_interval=%u\n"
	"log_ringbuffer_size=%u\n";

/**
 * init_infofile_text_data - Initialize the infofile text data from the given infofile
 *                           data structure.
 *
 *                           Returns the size of the infofile text data
 *                           (without nullbyte) on success. On failure a negative
 *                           error code is returned.
 * @data: The data for which the textual representation should be created.
 */
int init_infofile_text_data(struct memutil_infofile_data *data)
{
	size_t nbytes = snprintf(NULL, 0, output_fmtstr, data->core_count,
				 data->update_interval_ms, data->log_ringbuffer_size
				 ) + 1; // +1 for the nullbyte

	infofile_text_data = kmalloc(nbytes, GFP_KERNEL);
	if (!infofile_text_data) {
		return -ENOMEM;
	}
	return scnprintf(infofile_text_data, nbytes, output_fmtstr, data->core_count,
			 data->update_interval_ms, data->log_ringbuffer_size);
}

int memutil_debugfs_infofile_init(struct dentry *root_dir, struct memutil_infofile_data *data)
{
	int data_size;
	int return_value = init_infofile_text_data(data);
	if(return_value <= 0) {
		pr_warn("Memutil: Alloc infofile_data failed");
		goto filedata_error;
	}
	data_size = return_value;

	infofile_data_blob = (struct debugfs_blob_wrapper *) kmalloc(sizeof(struct debugfs_blob_wrapper), GFP_KERNEL);
	if(!infofile_data_blob) {
		pr_warn("Memutil: Alloc debugfs_blob_wrapper failed");
		return_value = -ENOMEM;
		goto filedata_blob_error;
	}
	infofile_data_blob->data = (void *) infofile_text_data;
	infofile_data_blob->size = (unsigned long) data_size;

	info_file = debugfs_create_blob("info", 0444, root_dir, infofile_data_blob);
	if (IS_ERR(info_file)) {
		pr_warn("Memutil: Create file failed: %pe", info_file);
		return_value = PTR_ERR(info_file);
		goto file_error;
	}
	return 0;

file_error:
	info_file = NULL;

	kfree(infofile_data_blob);
	infofile_data_blob = NULL;
filedata_blob_error:
	kfree(infofile_text_data);
	infofile_text_data = NULL;
filedata_error:
	return return_value;
}

void memutil_debugfs_infofile_exit(void)
{
	kfree(infofile_data_blob);
	infofile_data_blob = NULL;
	kfree(infofile_text_data);
	infofile_text_data = NULL;
	debugfs_remove(info_file);
	info_file = NULL;
}
