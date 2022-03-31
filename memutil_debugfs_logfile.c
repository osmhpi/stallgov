// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_debugfs_logfile.c
 *
 * Implementation file for the memutil debugfs logfile functionality. The logfile
 * provides data that was logged to the user in the form of a text file. For
 * more information see the memutil architecture wiki page.
 *
 *
 * COPYRIGHT_PLACEHOLDER
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "memutil_debugfs_logfile.h"
#include "memutil_ringbuffer_log.h"
#include "memutil_printk_helper.h"

/** The filesystem entry for the logfile */
static struct dentry *log_file = NULL;

/** Maximum amount of ringbuffers that may register to write to the logfile */
#define MAX_RINGBUFFER_COUNT 32

/*
 * The upper bound for the capcity can be calculated as ringbuffer_size * text_bytes_per_entry * ringbuffer_count
 * The text_bytes_per_entry specifies how many bytes are needed for one ringbuffer entry when it is formatted as text and appended
 * to the log.
 *
 * An example for a computer with 8 virtual cores would be 2000*130*8=2'080'000 ~ 2MB
 * Care has to be taken choosing the capcity as we do not want to waste memory or
 * run into performance issues but we also need enough room for all of our logging.
 * Especially with increasing core count the limit could become to small.
 * However as the logging is deterministic with consistent behaviour of how much
 * data is written in a given timeframe, this limit can be easily tuned experimentally.
 * In case not enough space is availabe a warning is printed to the kernel log.
 *
 * We did all of our tests with 2MB as the logfile capacity
 */
#define LOGFILE_CAPACITY 2000000 //2MB

/**
 * struct memutil_ringbuffer_registry - Structure for tracking which ringbuffers
 *                                      are registered to write to the logfile
 * @buffers: Array of the registered buffers
 * @count: Count of registered buffers
 */
struct memutil_ringbuffer_registry {
	struct memutil_ringbuffer *buffers[MAX_RINGBUFFER_COUNT];
	unsigned int count;
};

/** Global variable to store the registered ringbuffers for the logfile */
static struct memutil_ringbuffer_registry ringbuffers = {
	.count = 0
};

/**
 * struct memutil_logfile_data - structure to track information about the logfile
 *
 * @data: The actual data contained in the file
 * @size_used: The size (in bytes) of the logfile that is currently filled with
 *             valid data
 * @size_total: The total size (in bytes) of the logfile (valid + free data)
 */
struct memutil_logfile_data {
	void *data;
	size_t size_used;
	size_t size_total;
};

/** Global variable to track information about the logfile */
static struct memutil_logfile_data *logfile_data = NULL;

/**
 * clear_log - Clear the log.
 */
static void clear_log(void)
{
	logfile_data->size_used = 0;
}

/**
 * user_read_log - Function that is called when the logfile is read from userspace.
 *                 All ringbuffers and the logfile are cleared after one complete
 *                 read of the logfile.
 * @file: The file that is read
 * @user_buf: Userspace buffer into which the content of the file should be written
 */
static ssize_t user_read_log(struct file *file, char __user *user_buf, size_t count, loff_t *ppos)
{
	ssize_t return_value;
	unsigned int i;
	if (ppos && *ppos == 0) {
		//This function can be called multiple times to read the logfile
		//entirely, hence only clear the logfile before a new read when
		//we start at the beginning.
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

/**
 * file operations for the logfile
 */
static const struct file_operations fops_memutil = {
	.owner = THIS_MODULE,
	.read = user_read_log,
	.open = simple_open,
	.llseek = default_llseek,
};

int memutil_debugfs_logfile_init(struct dentry *root_dir)
{
	int return_value = 0;
	logfile_data = kzalloc(sizeof(struct memutil_logfile_data), GFP_KERNEL);
	if (!logfile_data) {
		pr_warn("Memutil: Alloc logfile_data failed");
		return_value = -ENOMEM;
		goto filedata_error;
	}
	logfile_data->data = vmalloc(LOGFILE_CAPACITY);
	if (!logfile_data->data) {
		pr_warn("Memutil: Alloc logfile_data's buf failed");
		return_value = -ENOMEM;
		goto filedata_buffer_error;
	}
	logfile_data->size_used = 0;
	logfile_data->size_total = LOGFILE_CAPACITY;

	log_file = debugfs_create_file("log", S_IRUSR | S_IRGRP | S_IROTH, root_dir, logfile_data, &fops_memutil);
	if (IS_ERR(log_file)) {
		pr_warn("Memutil: Create file failed: %pe", log_file);
		return_value = PTR_ERR(log_file);
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
	return return_value;
}

void memutil_debugfs_logfile_exit(void)
{
	ringbuffers.count = 0;
	vfree(logfile_data->data);
	kfree(logfile_data);
	logfile_data = NULL;
	debugfs_remove(log_file);
	log_file = NULL;
}

int memutil_debugfs_register_ringbuffer(struct memutil_ringbuffer *buffer)
{
	debug_info("Memutil: Registering ringbuffer for logfile");
	if (ringbuffers.count >= MAX_RINGBUFFER_COUNT) {
		pr_warn("Memutil: Cannot register additional memutil ringbuffer");
		return -EINVAL;
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
		return -EINVAL;
	}
	memcpy(logfile_data->data + logfile_data->size_used, buffer, buffer_size);
	logfile_data->size_used += buffer_size;
	return 0;
}
