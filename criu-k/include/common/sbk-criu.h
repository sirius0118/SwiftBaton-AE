/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SBK_CRIU_H
#define SBK_CRIU_H
#include "sbk_uapi.h"
#define SBK_EXPORT_BATCH 32
struct sbk_export_args {
    __u32 version, reserved;
    struct sbk_export_batch batch;
};
/* Stored in CRIU's relocatable RM_PRIVATE arena; no source/host pointers. */
struct sbk_restore_region {
    __u64 address, pages;
    __s32 fd;
    __u32 reserved;
};
#endif
