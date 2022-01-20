#include "memutil_debugfs_log.h"

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "memutil_log.h"

static struct dentry *log_file = NULL;

#define MAX_RINGBUFFER_COUNT 32
#define LOGFILE_CAPACITY 2000000 //2MB

struct memutil_registered_ringbuffers {
	struct memutil_ringbuffer *buffers[MAX_RINGBUFFER_COUNT];
	unsigned int count;
};

static struct memutil_registered_ringbuffers ringbuffers = {
	.count = 0
};

struct memutil_logfile_data {
	void *data;
	size_t size_used;
	size_t size_total;
};

static struct memutil_logfile_data *logfile_data = NULL;

static void clear_log(void)
{
	logfile_data->size_used = 0;
}

static ssize_t user_read_log(struct file *file, char __user *user_buf, size_t count, loff_t *ppos)
{
	ssize_t return_value;
	unsigned int i;
	if (ppos && *ppos == 0) {
		clear_log();
		for (i = 0; i < ringbuffers.count; ++i) {
			memutil_ringbuffer_append_to_logfile(ringbuffers.buffers[i]);
		}
	} else if (!ppos) {
		pr_warn("Memutil: No ppos in read log");
	}
	return_value = simple_read_from_buffer(user_buf, count, ppos, logfile_data->data, logfile_data->size_used);
	return return_value;
}

static const struct file_operations fops_memutil = {
	.owner = THIS_MODULE,
	.read = user_read_log,
	.open = simple_open,
	.llseek = default_llseek,
};

int memutil_debugfs_log_init(struct dentry *root_dir)
{
	logfile_data = kzalloc(sizeof(struct memutil_logfile_data), GFP_KERNEL);
	if (!logfile_data) {
		pr_warn("Memutil: Alloc logfile_data failed");
		goto filedata_error;
	}
	logfile_data->data = vmalloc(LOGFILE_CAPACITY);
	if (!logfile_data->data) {
		pr_warn("Memutil: Alloc logfile_data's buf failed");
		goto filedata_buffer_error;
	}
	logfile_data->size_used = 0;
	logfile_data->size_total = LOGFILE_CAPACITY;

	log_file = debugfs_create_file("log", S_IRUSR | S_IRGRP | S_IROTH, root_dir, logfile_data, &fops_memutil);
	if (IS_ERR(log_file)) {
		pr_warn("Memutil: Create file failed: %pe", log_file);
		goto file_error;
	}
	return 0;

file_error:
	log_file = NULL;
	vfree(logfile_data->data);
filedata_buffer_error:
	kfree(logfile_data);
	logfile_data = NULL;
filedata_error:
	return -1;
}

int memutil_debugfs_log_exit(struct dentry *root_dir)
{
	ringbuffers.count = 0;
	vfree(logfile_data->data);
	kfree(logfile_data);
	logfile_data = NULL;
	debugfs_remove(log_file);
	log_file = NULL;
	return 0;
}

int memutil_debugfs_register_ringbuffer(struct memutil_ringbuffer *buffer)
{
	pr_info("Memutil: Registering ringbuffer in DebugFS");
	if (unlikely(ringbuffers.count >= MAX_RINGBUFFER_COUNT)) {
		pr_warn("Memutil: Cannot register additional memutil ringbuffer");
		return -1;
	}
	ringbuffers.buffers[ringbuffers.count++] = buffer;
	return 0;
}

int memutil_debugfs_append_to_logfile(char *buffer, size_t buffer_size)
{
	if (logfile_data->size_used + buffer_size > logfile_data->size_total) {
		pr_warn("Memutil: logfile is getting to large. Force clear");
		clear_log();
	}
	if (logfile_data->size_total < buffer_size) {
		pr_warn("Memutil: cleared logfile is to small for message");
		return -1;
	}
	memcpy(logfile_data->data + logfile_data->size_used, buffer, buffer_size);
	logfile_data->size_used += buffer_size;
	return 0;
}
