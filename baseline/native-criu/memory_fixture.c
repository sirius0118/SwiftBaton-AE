// SPDX-License-Identifier: GPL-2.0
/* Standalone CRIU smoke fixture: preserve 64 MiB and a running counter. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const size_t bytes = 64UL << 20, page = 4096;
	uint8_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED)
		return 3;
	for (size_t i = 0; i < bytes; i += page)
		memory[i] = (uint8_t)((i / page) * 43 + 7);
	uint64_t tick = 0;
	for (;;) {
		for (size_t i = 0; i < bytes; i += page)
			if (memory[i] != (uint8_t)((i / page) * 43 + 7))
				return 4;
		char tmp[512];
		int n = snprintf(tmp, sizeof(tmp), "%s.tmp", argv[1]);
		if (n < 0 || n >= (int)sizeof(tmp))
			return 5;
		int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd < 0)
			return 6;
		n = dprintf(fd, "pid=%ld tick=%llu verified_pages=%zu\n",
			    (long)getpid(), (unsigned long long)tick++, bytes / page);
		close(fd);
		if (n < 0 || rename(tmp, argv[1]))
			return 7;
		struct timespec delay = {.tv_nsec = 100000000};
		nanosleep(&delay, NULL);
	}
}
