// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_printk_helper.h
 *
 * Header file defining a macro that allows adding verbose debug output depending
 * on a preprocessor define.
 *
 * Copyright (C) 2021-2022 Leon Matthes, Maximilian Stiede, Erik Griese
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#define DO_DEBUG_OUTPUT 0

#ifndef debug_info
#if DO_DEBUG_OUTPUT
#define debug_info(fmt, ...) \
	pr_info(fmt, ##__VA_ARGS__)
#else
#define debug_info(fmt, ...)
#endif
#endif
