/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SPDX-FileCopyrightText: Copyright (C) 2025 NVIDIA CORPORATION.  All rights reserved.
 */

#ifndef __BOOT_TIME_PROFILER_H
#define __BOOT_TIME_PROFILER_H

size_t add_boot_time_prof_entry(const char *buf);

int boot_time_prof_module_init(void);

#endif
