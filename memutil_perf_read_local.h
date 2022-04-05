// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_perf_read_local.h
 *
 * Header file for own implementation of perf_read_local because that function
 * is not exported for kernel modules.
 *
 * Copyright (C) 2021-2022 Leon Matthes, Maximilian Stiede, Erik Griese
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#ifndef _MEMUTIL_PERF_READ_LOCAL_H
#define _MEMUTIL_PERF_READ_LOCAL_H

/**
 * memutil_perf_event_read_local - Own implementation of perf_event_read_local
 *                                 because the symbol is not exported for kernel
 *                                 modules. For documentation on the function see
 *                                 the documentation for perf_event_read_local
 */
int memutil_perf_event_read_local(struct perf_event *event, u64 *value,
			  u64 *enabled, u64 *running);

#endif //_MEMUTIL_PERF_READ_LOCAL_H
