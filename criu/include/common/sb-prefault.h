#ifndef __SB_PREFAULT_H__
#define __SB_PREFAULT_H__
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

struct sb_prefault_job {
	volatile unsigned char *memory;
	size_t first, last;
};
static void *sb_prefault_worker(void *argument)
{
	struct sb_prefault_job *job = argument;
	for (size_t page = job->first; page < job->last; page++)
		job->memory[page * 4096] = 0;
	return NULL;
}

/* Only for a NEW zero-backed writable mapping, before exposing it to DMA or
 * another reader. This populates physical pages; it does not clear arbitrary
 * existing memory. Each worker owns a disjoint range, all join before return. */
static inline int sb_prefault_zero_pages(void *memory, size_t bytes, unsigned workers)
{
	struct sb_prefault_job jobs[32];
	pthread_t threads[31];
	unsigned started = 0;
	size_t pages = bytes / 4096;
	int error = 0;
	if (!memory || ((uintptr_t)memory & 4095) || !bytes || (bytes & 4095) ||
	    !workers || workers > 32) { errno = EINVAL; return -1; }
	if (workers > pages) workers = pages;
	for (unsigned i = 0; i < workers; i++)
		jobs[i] = (struct sb_prefault_job){ memory, pages * i / workers, pages * (i + 1) / workers };
	for (unsigned i = 1; i < workers; i++) {
		error = pthread_create(&threads[started], NULL, sb_prefault_worker, &jobs[i]);
		if (error) break;
		started++;
	}
	if (!error) sb_prefault_worker(&jobs[0]);
	for (unsigned i = 0; i < started; i++) {
		int rc = pthread_join(threads[i], NULL);
		if (rc && !error) error = rc;
	}
	if (error) { errno = error; return -1; }
	return 0;
}
#endif
