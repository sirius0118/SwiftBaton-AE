#ifndef __SB_FDPOOL_H__
#define __SB_FDPOOL_H__
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#define SB_FDPOOL_KINDS 7
#define SB_FDPOOL_BATCH 16
enum sb_fdpool_kind { SB_FDP_EVENT, SB_FDP_SEMAPHORE, SB_FDP_EPOLL, SB_FDP_TCP4, SB_FDP_UDP4, SB_FDP_TCP6, SB_FDP_UDP6 };
/* Source PS counts are bounded sizing hints, never restored object identity. */
int sb_fdpool_write_hint(int dirfd, const pid_t *pids, size_t count);
/* Called in the restore parent before forking the netns placeholder. */
int sb_fdpool_prepare(int dirfd);
/* The child has joined its final candidate netns; parent closes its writers. */
void sb_fdpool_populate(int context_permitted);
void sb_fdpool_parent_close_writers(void);
/* Called after service FD relocation, only around authoritative FD restore. */
void sb_fdpool_enter(int temporary_min);
void sb_fdpool_leave(void);
void sb_fdpool_close(void);
int sb_fdpool_take(enum sb_fdpool_kind kind);
/* Revalidate network context after setns, including transitions to other netns. */
void sb_fdpool_netns_changed(void);
int sb_fdpool_socket_kind(int domain, int type, int protocol);
#endif
