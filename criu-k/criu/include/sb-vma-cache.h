#ifndef SB_VMA_CACHE_H
#define SB_VMA_CACHE_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
int sb_vma_cache_start(const uint64_t *pids, size_t count);
void sb_vma_cache_refresh(void);
/* Call after application threads stop. -1 selects live smaps fallback. */
int sb_vma_cache_open(pid_t pid);
void sb_vma_cache_close(void);
#endif
