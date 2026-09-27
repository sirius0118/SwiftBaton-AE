/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SB_KERNEL_LAYOUT_WIRE_H
#define SB_KERNEL_LAYOUT_WIRE_H
#include "sb-kernel-dma-wire.h"
#include "sb-kernel-catalog.h"
#include <string.h>
/* Negotiate before source freeze. New PS versions add advisory layout records;
 * final records retain their existing authoritative MR/dirty semantics. */
static inline bool sbk_layout_mode_matches(bool dma, bool prepared,
                                           const struct sbk_rdma_endpoint *peer)
{
    return peer->reserved[0] == (unsigned)dma && peer->reserved[1] == (unsigned)prepared;
}
static inline unsigned sbk_layout_ps_version(bool dma, bool prepared)
{ return prepared ? (dma ? 7 : 6) : sbk_dma_ps_version(dma); }
static inline bool sbk_layout_ps_version_valid(bool dma, bool prepared, unsigned version)
{ return prepared ? version == sbk_layout_ps_version(dma, true) : sbk_dma_ps_version_valid(dma, version); }
/* Receiver owns the returned list only after complete bounded transfer. */
static inline int sbk_layout_wire_transfer(int fd, bool sending,
        struct sbk_catalog_layout **layout, unsigned *count,
        int (*transfer)(int, void *, size_t, int))
{
    struct { uint32_t count, reserved; } header = {sending ? *count : 0, 0};
    if (sending && (header.count > SBK_MAX_REGIONS / 2 || (header.count && !*layout))) return -EINVAL;
    if (transfer(fd, &header, sizeof(header), sending)) return -EIO;
    if (header.reserved || header.count > SBK_MAX_REGIONS / 2) return -EPROTO;
    size_t bytes = header.count * sizeof(**layout);
    struct sbk_catalog_layout *p = sending ? *layout : bytes ? malloc(bytes) : NULL;
    if (bytes && !p) return -ENOMEM;
    int ret = bytes && transfer(fd, p, bytes, sending) ? -EIO : 0;
    if (!sending) {
        if (ret) free(p);
        else { *layout = p; *count = header.count; }
    }
    return ret;
}
#endif
