/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SBK_CRIU_PIE_H
#define SBK_CRIU_PIE_H
#include "sbk-criu.h"
/* Include after the raw syscall declarations. The validation fixture uses the
 * same code with wrappers returning -errno, never libc's ambiguous -1. */
#ifndef SBK_RAW_IOCTL
#define SBK_RAW_IOCTL sys_ioctl
#endif
#ifndef SBK_RAW_WRITE
#define SBK_RAW_WRITE sys_write
#endif
#ifndef SBK_RAW_CLOSE
#define SBK_RAW_CLOSE sys_close
#endif
static inline int sbk_pie_export(int fd, struct sbk_export_args *args)
{
    unsigned int i;
    int ret;
    args->batch.completed = 0;
    args->batch.peak = 0;
    if (args->version != SBK_ABI_VERSION || args->reserved ||
        !args->batch.count || args->batch.count > SBK_EXPORT_BATCH ||
        !args->batch.workers || args->batch.workers > SBK_MAX_BATCH)
        return -EINVAL;
    /* Reject malformed batches before registering any region. */
    for (i = 0; i < args->batch.count; i++) {
        struct sbk_rdma_region *r = &args->batch.regions[i];
        if (!r->pages || r->pages > (1ULL << 20) || (r->address & 4095) ||
            r->address > ~0ULL - (r->pages << 12) || r->id || r->rkey)
            return -EINVAL;
    }
    if (args->batch.workers > 1)
        return SBK_RAW_IOCTL(fd, SBK_IOC_EXPORT_BATCH, (unsigned long)&args->batch);
    args->batch.peak = 1;
    for (i = 0; i < args->batch.count; i++) {
        ret = SBK_RAW_IOCTL(fd, SBK_IOC_EXPORT_REGION,
                            (unsigned long)&args->batch.regions[i]);
        if (ret)
            return ret; /* Catalog retains completed MRs for abort cleanup. */
        args->batch.completed++;
    }
    return 0;
}
static inline int sbk_pie_arm(const struct sbk_restore_region *regions, unsigned int nr)
{
    unsigned int i;
    __u64 end = 0;
    int ret;
    if (nr > SBK_MAX_REGIONS || (nr && !regions))
        return -EINVAL;
    for (i = 0; i < nr; i++) {
        const struct sbk_restore_region *r = &regions[i];
        if (r->fd < 0 || r->reserved || !r->pages || r->pages > (1ULL << 20) ||
            (r->address & 4095) || r->address < end ||
            r->address > ~0ULL - (r->pages << 12))
            return -EINVAL;
        end = r->address + (r->pages << 12);
    }
    for (i = 0; i < nr; i++) {
        struct sbk_anon_arm a = { .address = regions[i].address };
        ret = SBK_RAW_IOCTL(regions[i].fd, SBK_IOC_ARM_ANON, (unsigned long)&a);
        if (ret)
            return ret; /* Caller must abort restore, never resume partial state. */
    }
    for (i = 0; i < nr; i++)
        SBK_RAW_CLOSE(regions[i].fd);
    return 0;
}
/* Signal only after every range is armed. The page client starts background
 * installation after observing this event; demand faults are already usable. */
static inline int sbk_pie_ready(int fd)
{
    __u64 ready = 1;
    long ret;
    if (fd < 0) return -EINVAL;
    do { ret = SBK_RAW_WRITE(fd, &ready, sizeof(ready)); } while (ret == -EINTR);
    if (ret != sizeof(ready)) return ret < 0 ? (int)ret : -EIO;
    SBK_RAW_CLOSE(fd);
    return 0;
}
#endif
