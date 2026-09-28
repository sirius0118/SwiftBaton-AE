/* Checkerboard validity: half a million surviving pre-copy fragments. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "sb-sched.h"

static int descriptors(void)
{
	DIR *directory = opendir("/proc/self/fd");
	struct dirent *entry;
	int count = 0;
	if (!directory) return -1;
	while ((entry = readdir(directory)))
		if (entry->d_name[0] != '.') count++;
	closedir(directory);
	return count;
}

static double seconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec + now.tv_nsec / 1e9;
}

int main(void)
{
	const uint64_t pages = 1U << 20;
	const size_t words = pages / (8 * sizeof(unsigned long));
	const size_t bytes = sb_sched_size(pages, 1024);
	unsigned long *bitmap = calloc(words, sizeof(*bitmap));
	void *memory = aligned_alloc(64, (bytes + 63) & ~(size_t)63);
	struct sb_sched *sched;
	struct sb_sched_stats stats;
	uint64_t seeded = 0;
	int before = descriptors(), after;
	double start = seconds();
	if (!bitmap || !memory || before < 0) return 1;
	for (size_t word = 0; word < words; word++)
		bitmap[word] = 0x5555555555555555UL;
	sched = sb_sched_init(memory, bytes, pages, 1024);
	if (!sched || sb_sched_seed_bitmap(sched, 0, pages, bitmap, &seeded) ||
	    seeded != pages / 2) return 2;
	for (uint64_t page = 1; page < pages; page += 2) {
		struct sb_job job;
		if (sb_sched_request(sched, page, SB_BACKGROUND) != 1 ||
		    sb_sched_claim(sched, SB_LANE_MASK(SB_BACKGROUND), &job) != 1 ||
		    job.page != page || sb_sched_commit(sched, &job)) return 3;
	}
	for (uint64_t page = 0; page < pages; page += 2)
		if (sb_sched_seed_commit(sched, page)) return 4;
	sb_sched_stats(sched, &stats);
	after = descriptors();
	if (stats.committed != pages || stats.failed || before != after) return 5;
	printf("{\"pages\":%" PRIu64 ",\"fragments\":%" PRIu64
	       ",\"precopy\":%" PRIu64 ",\"background\":%" PRIu64
	       ",\"fds_before\":%d,\"fds_after\":%d,\"index_bytes\":%zu,\"seconds\":%.6f}\n",
	       pages, pages / 2, seeded, stats.claimed[SB_BACKGROUND],
	       before, after, bytes, seconds() - start);
	free(memory);
	free(bitmap);
	return 0;
}
