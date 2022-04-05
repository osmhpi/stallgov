// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_debugfs.h
 *
 * Header file for general memutil debugfs functionality.
 *
 * Copyright (C) 2021-2022 Leon Matthes, Maximilian Stiede, Erik Griese
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#ifndef _MEMUTIL_DEBUGFS_H
#define _MEMUTIL_DEBUGFS_H

#include <linux/types.h>

#include "memutil_debugfs_infofile.h"

/**
 * memutil_debugfs_init - Initialize the memutil debugfs directory.
 *                        This will create a folder
 *                        <debugfs>/memutil that contains a logfile called "log"
 *                        and an infofile called "info".
 *                        This function may sleep.
 *                        If the function succeeds it returns 0, otherwise an
 *                        error code is returned.
 * @infofile_data: Data that the infofile should contain
 */
int memutil_debugfs_init(struct memutil_infofile_data *infofile_data);
/**
 * memutil_debugfs_exit - Deinitialize the memutil debugfs directory. This removes
 *                        The directory in the debugfs.
 *
 *                        This function may sleep.
 */
void memutil_debugfs_exit(void);

#endif //_MEMUTIL_DEBUGFS_H
