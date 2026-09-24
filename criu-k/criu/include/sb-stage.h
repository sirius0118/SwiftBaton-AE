#ifndef __SB_STAGE_H__
#define __SB_STAGE_H__
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

void *sb_stage_allocate(uint64_t length, int *fd);
int sb_stage_send(int socket, int snapshot_fd, void *buffer, uint64_t length);
int sb_stage_receive(int socket, unsigned workers);
/* -1 preserves the current policy; otherwise bind each stage before copying. */
void sb_stage_set_numa_node(int node);
int sb_stage_prune(int image_dir_fd);
int sb_stage_finalize(int image_dir_fd);
/* Enable only around restoration-tree forks; utility children need no pages. */
int sb_stage_inherit(bool enable);
/* Final IS process tree decides which not-yet-adopted stages a child needs. */
typedef bool (*sb_stage_needed_fn)(pid_t pid, void *opaque);
int sb_stage_prepare_fork(sb_stage_needed_fn needed, void *opaque);
/* Must run immediately in the child, before helpers or adoption. */
int sb_stage_enter_child(void);
/* 1=adopted, 0=use ordinary UFFD fallback, -1=fatal after a mapping changed. */
int sb_stage_adopt(pid_t pid, uint64_t begin, uint64_t end, void *target, int flags, int prot,
                   unsigned long *page_bitmap, unsigned long *parent_bitmap);
int sb_stage_was_adopted(uint64_t index);
#endif
