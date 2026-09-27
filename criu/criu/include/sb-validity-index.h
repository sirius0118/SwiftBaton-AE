/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SB_VALIDITY_INDEX_H
#define SB_VALIDITY_INDEX_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sb-precopy.h"
struct sb_validity_group { uint32_t source; uint64_t begin, end; };
struct sb_validity_index {
    const struct sb_precopy_page *pages;
    uint64_t count, nonce;
    unsigned char *copied;
    struct sb_validity_group *groups;
    size_t group_count;
};
static inline void sb_validity_index_free(struct sb_validity_index *index)
{
    free(index->copied); free(index->groups); memset(index, 0, sizeof(*index));
}
/* Coordinator only, after sb_precopy_view validates the immutable descriptor
 * array. Pages remain mapped and immutable through final manifest acceptance. */
static inline int sb_validity_index_prepare(struct sb_validity_index *index,
                                          const struct sb_precopy_view *view)
{
    struct sb_validity_index next = { .pages = view->pages,
        .count = view->count, .nonce = view->nonce };
    if (index->copied && index->pages == view->pages &&
        index->count == view->count && index->nonce == view->nonce) return 0;
    if (view->count > SIZE_MAX - 7 || (view->count && !view->pages)) return -1;
    next.copied = calloc((view->count + 7) / 8 + 1, 1);
    next.groups = calloc(4096, sizeof(*next.groups));
    if (!next.copied || !next.groups) goto fail;
    for (uint64_t i = 0; i < view->count; i++) {
        if (!i || view->pages[i].pid != view->pages[i-1].pid) {
            if (next.group_count == 4096) goto fail;
            if (next.group_count) next.groups[next.group_count-1].end = i;
            next.groups[next.group_count++] = (struct sb_validity_group){
                .source = view->pages[i].pid, .begin = i, .end = view->count };
        }
        if (view->pages[i].copied) next.copied[i / 8] |= 1U << (i % 8);
    }
    sb_validity_index_free(index); *index = next; return 0;
fail:
    sb_validity_index_free(&next); return -1;
}
static inline int sb_validity_bits_any(const unsigned char *bits, uint64_t begin, uint64_t end)
{
    while (begin < end && (begin & 7)) {
        if (bits[begin / 8] & (1U << (begin % 8))) return 1;
        begin++;
    }
    while (end - begin >= 8) {
        if (bits[begin / 8]) return 1;
        begin += 8;
    }
    while (begin < end) {
        if (bits[begin / 8] & (1U << (begin % 8))) return 1;
        begin++;
    }
    return 0;
}
/* Final bitmap must still match nonce/count/size and a one-to-one positive PID
 * map (checked by the caller). Reject uncopied data and accepted missing PIDs
 * using the PS index, without rereading every 48-byte page descriptor in IS. */
static inline int sb_validity_index_check(const struct sb_validity_index *index,
                                         const struct sb_precopy_pid *pids, size_t count,
                                         const unsigned char *bitmap)
{
    for (uint64_t i = 0; i < (index->count + 7) / 8; i++)
        if (bitmap[i] & ~index->copied[i]) return -1;
    for (size_t g = 0; g < index->group_count; g++) {
        const struct sb_validity_group *group = &index->groups[g];
        size_t p;
        for (p = 0; p < count; p++)
            if (pids[p].source == (int32_t)group->source && pids[p].destination > 0) break;
        if (p == count && sb_validity_bits_any(bitmap, group->begin, group->end)) return -1;
    }
    return 0;
}
#endif
