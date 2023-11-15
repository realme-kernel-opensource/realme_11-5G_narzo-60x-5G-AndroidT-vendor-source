/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */

#ifndef __MEMFREE_MM_RECLAIM_H__
#define __MEMFREE_MM_RECLAIM_H__

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/pagewalk.h>

#define DUMP_MEMFREE_STACK_ON_ERR 0

enum {
	MF_LOG_ERR = 0,
	MF_LOG_WARN,
	MF_LOG_INFO,
	MF_LOG_DEBUG,
	MF_MAX
};

#define pt_memfree(l, f, ...)	pr_err(" <%d>[%s:%d] [%s:%d]: "f, l, \
			       current->comm, current->tgid, \
			       __func__, __LINE__, ##__VA_ARGS__)

static inline void memfree_pr_none(void) {}
#define memfree_reclaim_log(l, f, ...) do {\
	(l <= memfree_loglevel()) ? pt_memfree(l, f, ##__VA_ARGS__) : memfree_pr_none();\
	if (DUMP_MEMFREE_STACK_ON_ERR && l == MF_LOG_ERR) dump_stack();\
} while (0)

#define memfree_log_err(f, ...)	memfree_reclaim_log(MF_LOG_ERR, f, ##__VA_ARGS__)
#define memfree_log_warn(f, ...) memfree_reclaim_log(MF_LOG_WARN, f, ##__VA_ARGS__)
#define memfree_log_info(f, ...) memfree_reclaim_log(MF_LOG_INFO, f, ##__VA_ARGS__)
#define memfree_log_dbg(f, ...) memfree_reclaim_log(MF_LOG_DEBUG, f, ##__VA_ARGS__)

#endif /* __MEMFREE_MM_RECLAIM_H__ */
