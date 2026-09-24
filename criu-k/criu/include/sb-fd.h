#ifndef __SB_FD_H__
#define __SB_FD_H__
#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
/* Capacity hint only: never substitutes for the authoritative IS FD dump. */
int sb_fdtable_prepare(const pid_t *pids, size_t count);
/* Initialize through an eight-byte write: eventfd() takes only unsigned int. */
int sb_eventfd_restore(uint64_t counter, bool semaphore);
#endif
