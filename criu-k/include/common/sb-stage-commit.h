/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SB_STAGE_COMMIT_H
#define SB_STAGE_COMMIT_H
#include <stdint.h>

/* Publish only after every corresponding page bitmap has been updated.
 * Regions may share a committed byte, so publication must remain an atomic OR.
 * Delaying publication up to the next byte is safe: ACK readers may see fewer
 * ready pages temporarily, but cannot observe a ready page before adoption.
 */
struct sb_stage_commit_batch {
    unsigned char *base;
    uint64_t byte;
    unsigned char mask;
};
static inline void sb_stage_commit_flush(struct sb_stage_commit_batch *b)
{
    if (b->mask) {
        __atomic_fetch_or(b->base + b->byte, b->mask, __ATOMIC_RELEASE);
        b->mask = 0;
    }
}
static inline void sb_stage_commit_add(struct sb_stage_commit_batch *b, uint64_t page)
{
    uint64_t byte = page / 8;
    if (b->mask && b->byte != byte)
        sb_stage_commit_flush(b);
    b->byte = byte;
    b->mask |= (unsigned char)(1U << (page % 8));
}
#endif
