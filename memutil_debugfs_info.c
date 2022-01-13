#include "memutil_debugfs_info.h"

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/slab.h>

static struct dentry *info_file = NULL;

static char *infofile_data;
static struct debugfs_blob_wrapper *infofile_data_blob;

static char const *output_fmtstr =
    "core_count=%u\n"
    "update_interval=%u\n"
    "logbuffer_size=%u\n";

int init_infofile_data(
    struct memutil_infofile_data data
) {
    size_t nbytes = snprintf(
        NULL, 0,
        output_fmtstr,
        data.core_count, data.update_interval, data.logbuffer_size
    ) + 1; /* +1 for the '\0' */
    infofile_data = kmalloc(nbytes, GFP_KERNEL);
    if (!infofile_data) {
		return -1;
	}
    return scnprintf(
        infofile_data, nbytes,
        output_fmtstr,
        data.core_count, data.update_interval, data.logbuffer_size
    );
}

int memutil_debugfs_info_init(struct dentry *root_dir, const struct memutil_infofile_data data)
{
    int data_size = init_infofile_data(data);
    if(data_size <= 0) {
		pr_warn("Memutil: Alloc infofile_data failed");
        goto filedata_error;
    }

    infofile_data_blob = (struct debugfs_blob_wrapper *) kmalloc(sizeof(struct debugfs_blob_wrapper), GFP_KERNEL);
    if(infofile_data_blob == NULL) {
		pr_warn("Memutil: Alloc debugfs_blob_wrapper failed");
        goto filedata_blob_error;
    }
    infofile_data_blob->data = (void *) infofile_data;
    infofile_data_blob->size = (unsigned long) data_size;

	info_file = debugfs_create_blob("info", 0444, root_dir, infofile_data_blob);
	if (IS_ERR(info_file)) {
		pr_warn("Memutil: Create file failed: %pe", info_file);
		goto file_error;
	}
	return 0;

file_error:
	info_file = NULL;

    kfree(infofile_data_blob);
    infofile_data_blob = NULL;
filedata_blob_error:
	kfree(infofile_data);
	infofile_data = NULL;
filedata_error:
	return -1;
}

int memutil_debugfs_info_exit(struct dentry *root_dir)
{
    kfree(infofile_data_blob);
    infofile_data_blob = NULL;
	kfree(infofile_data);
	infofile_data = NULL;
	debugfs_remove(info_file);
	info_file = NULL;
	return 0;
}
