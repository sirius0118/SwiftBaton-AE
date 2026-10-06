#ifndef SB_CPU_LIMIT_H
#define SB_CPU_LIMIT_H

/* Optional, root-owned AE control. Apply before any CRIU worker is created,
 * including swrk processes started by Docker. Children inherit the mask;
 * the application still restores its own saved CPU affinity. */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SB_CPU_CONTROL "/run/swiftbaton-ae/criu-cpus"
#define SB_CPU_AUDIT "/run/swiftbaton-ae/criu-cpu-audit.jsonl"

static int sb_cpu_limit(void)
{
	char text[4096], record[4300], *p, *end;
	struct stat st;
	cpu_set_t wanted, actual;
	ssize_t n;
	int fd, count = 0, i, length;
	long cpu;

	fd = open(SB_CPU_CONTROL, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return errno == ENOENT ? 0 : -1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
	    (st.st_mode & 022) || st.st_size <= 0 || (size_t)st.st_size >= sizeof(text)) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	n = read(fd, text, sizeof(text) - 1);
	close(fd);
	if (n != st.st_size) {
		errno = EIO;
		return -1;
	}
	text[n] = 0;
	if (text[n - 1] == '\n') text[--n] = 0;
	CPU_ZERO(&wanted);
	p = text;
	do {
		if (*p < '0' || *p > '9') goto invalid;
		errno = 0;
		cpu = strtol(p, &end, 10);
		if (errno || cpu < 0 || cpu >= CPU_SETSIZE || CPU_ISSET(cpu, &wanted))
			goto invalid;
		CPU_SET(cpu, &wanted);
		count++;
		if (!*end) break;
		if (*end != ',' || !end[1]) goto invalid;
		p = end + 1;
	} while (1);
	if (sched_setaffinity(0, sizeof(wanted), &wanted) ||
	    sched_getaffinity(0, sizeof(actual), &actual)) return -1;
	for (i = 0; i < CPU_SETSIZE; i++)
		if (CPU_ISSET(i, &wanted) != CPU_ISSET(i, &actual)) goto invalid;
	fd = open(SB_CPU_AUDIT, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) return -1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022)) {
		close(fd);
		goto invalid;
	}
	length = snprintf(record, sizeof(record),
		"{\"pid\":%d,\"cpus\":\"%s\",\"cpu_count\":%d}\n", getpid(), text, count);
	n = write(fd, record, length);
	close(fd);
	if (n != length) { errno = EIO; return -1; }
	return 0;
invalid:
	errno = EINVAL;
	return -1;
}
#endif
