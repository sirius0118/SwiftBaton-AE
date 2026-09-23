#ifndef __SB_FORK_H__
#define __SB_FORK_H__
#include <stddef.h>

/* Temporary private anonymous restore mappings only. The caller must prove
 * that no restore child needs their contents (including CRIU's COW chains).
 * WIPEONFORK retains the address reservation in children: DONTFORK would let
 * later mmap reuse a hole inside the old premap area before its munmap. */
int sb_fork_wipe_private(void *address, size_t length);
/* Call in both branches before resuming application code. This also clears
 * WIPEONFORK inherited by a restore child, without changing either payload. */
int sb_fork_restore_inheritance(void);
#endif
