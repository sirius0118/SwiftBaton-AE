/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CR_SB_KERNEL_HOT_WIRE_H
#define CR_SB_KERNEL_HOT_WIRE_H
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Final catalog flag, independent of the kernel UAPI. The existing protocol
 * uses native integers between matching x86 hosts. Preserve the entire hint
 * sequence, narrowing only its on-wire representation. Unknown flags fail
 * closed at the catalog header; a receiver also accepts the old wide format. */
#define SBK_WIRE_COMPACT_HOT 1U
static inline size_t sbk_hot_wire_width(uint64_t pages)
{
    return pages <= (UINT64_C(1) << 16) ? sizeof(uint16_t) : sizeof(uint32_t);
}
static inline int sbk_hot_wire_transfer(int fd, uint64_t *indices, uint64_t count,
                                       uint64_t pages, int sending,
                                       int (*transfer)(int, void *, size_t, int))
{
    if (!pages || pages > (UINT64_C(1) << 20) || count > pages ||
        (count && !indices) || !transfer)
        return -EINVAL;
    if (!count) return 0;
    size_t width = sbk_hot_wire_width(pages);
    if (count > SIZE_MAX / width) return -EOVERFLOW;
    void *buffer = malloc((size_t)count * width);
    if (!buffer) return -ENOMEM;
    int ret = 0;
    if (sending) {
        for (uint64_t i = 0; i < count; i++) {
            if (indices[i] >= pages) { ret = -ERANGE; goto out; }
            if (width == sizeof(uint16_t)) ((uint16_t *)buffer)[i] = indices[i];
            else ((uint32_t *)buffer)[i] = indices[i];
        }
    }
    if (transfer(fd, buffer, (size_t)count * width, sending)) {
        ret = -EIO;
        goto out;
    }
    if (!sending) {
        for (uint64_t i = 0; i < count; i++) {
            uint64_t index = width == sizeof(uint16_t) ?
                ((uint16_t *)buffer)[i] : ((uint32_t *)buffer)[i];
            if (index >= pages) { ret = -ERANGE; goto out; }
            indices[i] = index;
        }
    }
out:
    free(buffer);
    return ret;
}
#endif
