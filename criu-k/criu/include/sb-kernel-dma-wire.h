/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SB_KERNEL_DMA_WIRE_H
#define SB_KERNEL_DMA_WIRE_H
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include "common/sbk_uapi.h"

/* The endpoint's previously zero reserved byte negotiates address semantics
 * before the source stops. Ordinary peers continue to exchange all zeros. */
static inline bool sbk_dma_wire_mode_matches(bool enabled, const struct sbk_rdma_endpoint *peer)
{
    return peer->reserved[0] == (enabled ? 1 : 0) && !peer->reserved[1];
}
static inline unsigned sbk_dma_ps_version(bool enabled) { return enabled ? 5 : 4; }
static inline unsigned sbk_dma_final_version(bool enabled) { return enabled ? 3 : 2; }
static inline bool sbk_dma_ps_version_valid(bool enabled, unsigned version)
{
    return enabled ? version == 5 : version >= 2 && version <= 4;
}

/* Called after a record and its index lists, before the record can enter the
 * destination catalog. No kernel table is published after a partial receive.
 * The device DMA vector is never inferred from a CPU/virtual base address. */
static inline int sbk_dma_wire_transfer(int socket_fd, int session_fd,
        const struct sbk_rdma_region *region, bool sending, bool enabled,
        int (*transfer)(int, void *, size_t, int))
{
    if (!enabled) return 0;
    if (!region->pages || region->pages > (1ULL << 20) ||
        !region->id || region->id > SBK_MAX_REGIONS || (region->address & 4095) ||
        region->address > UINT64_MAX - region->pages * 4096) return -EPROTO;
    size_t bytes = region->pages * sizeof(uint64_t);
    uint64_t *addresses = malloc(bytes);
    if (!addresses) return -ENOMEM;
    struct sbk_dma_map map = {.region = *region, .addresses = (uintptr_t)addresses};
    int ret = 0;
    if (sending && ioctl(session_fd, SBK_IOC_DMA_EXPORT_MAP, &map)) ret = -errno;
    if (!ret && transfer(socket_fd, addresses, bytes, sending)) ret = -EIO;
    if (!ret && !sending && ioctl(session_fd, SBK_IOC_DMA_IMPORT_MAP, &map)) ret = -errno;
    free(addresses);
    return ret;
}
#endif
