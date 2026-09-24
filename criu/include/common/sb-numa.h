#ifndef __SB_NUMA_H__
#define __SB_NUMA_H__
#include <errno.h>
#include <limits.h>
#include <linux/mempolicy.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* An explicit experimental policy, never a host-wide default. */
static inline int sb_numa_parse_node(const char *text, int *node)
{
	char *end;
	long value;
	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || end == text || end[strspn(end, " \t\r\n")] ||
	    value < -1 || value >= (long)(sizeof(unsigned long) * CHAR_BIT)) {
		errno = EINVAL;
		return -1;
	}
	*node = value;
	return 0;
}

/* Call before touching anonymous staging pages. Existing pages are not moved. */
static inline int sb_numa_bind(void *address, size_t bytes, int node)
{
	unsigned long mask;
	if (node == -1) return 0;
	if (node < 0 || node >= (int)(sizeof(mask) * CHAR_BIT)) {
		errno = EINVAL;
		return -1;
	}
	mask = 1UL << node;
	return syscall(SYS_mbind, address, bytes, MPOL_BIND, &mask, sizeof(mask) * CHAR_BIT, 0);
}
#endif
