/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SBK_PLAN_TEST_H
#define SBK_PLAN_TEST_H
#include <linux/types.h>
#include <linux/ioctl.h>
struct plan_request { __u64 address, pages, mode; };
struct plan_result {
	__u64 created, released, faults;
	__u64 pages, tables, published, moved_tables, copied_pages;
};
#define PLAN_PREPARE _IOW('Q', 1, struct plan_request)
#define PLAN_ARM _IO('Q', 2)
#define PLAN_STAT _IOR('Q', 3, struct plan_result)
#define PLAN_POPULATE _IOW('Q', 4, __u64)
#define PLAN_INJECT _IOW('Q', 5, __u64)
#define PLAN_FLUSH _IO('Q', 6)
#endif
