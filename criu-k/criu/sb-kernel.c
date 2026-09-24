/* SPDX-License-Identifier: GPL-2.0 */
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <compel/infect.h>
#include <compel/infect-rpc.h>
#include <compel/infect-util.h>
#include "sb-kernel.h"
#include "sb-kernel-catalog.h"
#include "types.h"
#include "images/core.pb-c.h"
#include "parasite.h"
#include "restorer.h"
#include "rst-malloc.h"

int sb_kernel_export_seized(struct parasite_ctl *ctl, int fd,
                           struct sbk_rdma_region *regions, unsigned int count,
                           unsigned int workers, unsigned int *peak)
{
    struct sbk_export_args *args;
    unsigned int done = 0, n;
    int ret;
    if (!ctl || fd < 0 || !regions || !count || count > SBK_MAX_REGIONS || !workers || workers > SBK_MAX_BATCH || !peak)
        return -EINVAL;
    *peak = 0;
    args = compel_parasite_args(ctl, struct sbk_export_args);
    while (done < count) {
        n = count - done;
        if (n > SBK_EXPORT_BATCH) n = SBK_EXPORT_BATCH;
        memset(args, 0, sizeof(*args));
        args->version = SBK_ABI_VERSION;
        args->batch.count = n;
        args->batch.workers = workers;
        memcpy(args->batch.regions, regions + done, n * sizeof(*regions));
        ret = compel_rpc_call(PARASITE_CMD_SBK_EXPORT, ctl);
        if (ret) return ret;
        ret = compel_util_send_fd(ctl, fd);
        if (ret) return ret;
        ret = compel_rpc_sync(PARASITE_CMD_SBK_EXPORT, ctl);
        if (ret) return ret;
        if (args->batch.completed != n || !args->batch.peak || args->batch.peak > workers) return -EIO;
        if (args->batch.peak > *peak) *peak = args->batch.peak;
        memcpy(regions + done, args->batch.regions, n * sizeof(*regions));
        done += n;
    }
    return 0;
}

int sb_kernel_prepare_restore(struct task_restore_args *ta,
                              const struct sbk_restore_region *regions, unsigned int count, int ready_fd)
{
    struct sbk_restore_region *table;
    unsigned long pos;
    unsigned int i, j;
    __u64 end = 0;
    if (ready_fd < 0 || !ta || !count || count > SBK_MAX_REGIONS || !regions || ta->sbk_regions_nr)
        return -EINVAL;
    for (i = 0; i < count; i++) {
        const struct sbk_restore_region *r = &regions[i];
        if (r->reserved || r->fd < 0 || r->fd == ready_fd || !r->pages || r->pages > (1ULL << 20) ||
            (r->address & 4095) || r->address < end ||
            r->address > ~0ULL - (r->pages << 12)) return -EINVAL;
        end = r->address + (r->pages << 12);
        for (j = 0; j < i; j++) if (regions[j].fd == r->fd) return -EINVAL;
    }
    pos = rst_mem_align_cpos(RM_PRIVATE);
    table = rst_mem_alloc(count * sizeof(*table), RM_PRIVATE);
    if (!table) return -ENOMEM;
    memcpy(table, regions, count * sizeof(*table));
    ta->sbk_regions = (void *)pos;
    ta->sbk_regions_nr = count;
    ta->sbk_ready_fd = ready_fd;
    return 0;
}

int sb_kernel_receive_restore(struct task_restore_args *ta, int socket_fd, unsigned int pid)
{
    struct sbk_restore_region *regions;
    unsigned int count, i;
    int ready, ret;
    ret = sbk_catalog_receive(socket_fd, pid, &regions, &count, &ready);
    if (ret) return ret;
    ret = sb_kernel_prepare_restore(ta, regions, count, ready);
    if (ret) {
        for (i = 0; i < count; i++) close(regions[i].fd);
        close(ready);
    }
    free(regions);
    return ret;
}
