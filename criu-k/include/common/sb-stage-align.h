/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SB_STAGE_ALIGN_H
#define SB_STAGE_ALIGN_H
#include <stdint.h>
/* Congruent 2 MiB offsets let x86 move whole PTE tables during mremap.
 * This requests no huge pages and changes no application mapping flags. */
#ifdef __x86_64__
#define SB_STAGE_ALIGNMENT (2UL * 1024 * 1024)
#else
#define SB_STAGE_ALIGNMENT 4096UL
#endif
static inline uintptr_t sb_stage_alignment_padding(uintptr_t base, uintptr_t application)
{
    return (application - base) & (SB_STAGE_ALIGNMENT - 1);
}
#endif
