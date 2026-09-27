// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "plan-test.h"

#define PAGE 4096UL
#define PMD (512 * PAGE)
#define BASE ((unsigned char *)0x4000000000UL)
#define OTHER ((unsigned char *)0x5000000000UL)
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s errno=%d (%s)\n", __FILE__, __LINE__, #x, errno, strerror(errno)); exit(1); } } while (0)
static int monitor;
static uint64_t ns(void) {
	struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
	return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int device(void) { int fd = open("/dev/sbk_plan_test", O_RDWR | O_CLOEXEC); CHECK(fd >= 0); return fd; }
static struct plan_result stat_fd(int fd) {
	struct plan_result r; CHECK(!ioctl(fd, PLAN_STAT, &r)); return r;
}
static void drained(const char *name) {
	struct plan_result r = stat_fd(monitor);
	CHECK(r.created == r.released);
	printf("SBK_PLAN_PASS %s created=%llu released=%llu faults=%llu\n", name,
		(unsigned long long)r.created, (unsigned long long)r.released, (unsigned long long)r.faults);
}
static int prepare(unsigned char *addr, size_t n, unsigned mode) {
	int fd = device(); struct plan_request q = {(uintptr_t)addr, n, mode};
	CHECK(!ioctl(fd, PLAN_PREPARE, &q)); return fd;
}
static unsigned char *mapping(unsigned char *addr, size_t n, int flags) {
	void *v = mmap(addr, n * PAGE, PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | flags, -1, 0);
	CHECK(v == addr); CHECK(!madvise(v, n * PAGE, MADV_NOHUGEPAGE)); return v;
}
static void remove_map(unsigned char *addr, size_t n) { CHECK(!munmap(addr, n * PAGE)); }
static void check_pages(unsigned char *addr, size_t n, size_t first) {
	volatile unsigned char *v = addr;
	for (size_t i = 0; i < n; i++)
		for (size_t j = 0; j < PAGE; j++)
			CHECK(v[i * PAGE + j] == (unsigned char)((first + i) * 37 + 11));
}
static void wait_child(pid_t p) { int s; CHECK(waitpid(p, &s, 0) == p); CHECK(WIFEXITED(s) && !WEXITSTATUS(s)); }
static void error_call(int fd, unsigned long cmd, void *arg, int error) {
	errno = 0; CHECK(ioctl(fd, cmd, arg) == -1); CHECK(errno == error);
}
static void canceled_and_errors(void) {
	int fd = prepare(BASE, 1234, 0); uint64_t i = 0;
	struct plan_result r = stat_fd(fd); CHECK(r.created - r.released == 1234);
	error_call(fd, PLAN_POPULATE, &i, EAGAIN);
	CHECK(!close(fd)); drained("cancel-before-mm");
	for (unsigned mode = 1; mode <= 2; mode++) {
		fd = device(); struct plan_request q = {(uintptr_t)BASE, 16, mode};
		error_call(fd, PLAN_PREPARE, &q, mode == 1 ? EEXIST : ENOENT);
		CHECK(!close(fd)); drained(mode == 1 ? "duplicate-id" : "missing-id");
	}
	fd = device(); struct plan_request q = {(uintptr_t)BASE + 1, 16, 0};
	error_call(fd, PLAN_PREPARE, &q, EINVAL); CHECK(!close(fd)); drained("unaligned");
	fd = prepare(BASE, 8, 0); error_call(fd, PLAN_ARM, NULL, EINVAL);
	error_call(fd, PLAN_ARM, NULL, EALREADY); CHECK(!close(fd)); drained("no-vma-one-shot");
}
static void adopt_and_bench(size_t n, unsigned mode) {
	uint64_t begin = ns(); int fd = prepare(BASE, n, mode); uint64_t prepared = ns();
	mapping(BASE, n, MAP_PRIVATE); uint64_t armbegin = ns();
	CHECK(!ioctl(fd, PLAN_ARM)); uint64_t armed = ns();
	struct plan_result r = stat_fd(fd);
	if (!mode) { CHECK(r.published == n); CHECK(r.moved_tables == (n + 511) / 512); CHECK(!r.copied_pages); }
	printf("SBK_PLAN_BENCH mode=%u pages=%zu prepare_us=%.3f arm_us=%.3f moved=%llu copied=%llu\n",
		mode, n, (prepared - begin) / 1000.0, (armed - armbegin) / 1000.0,
		(unsigned long long)r.moved_tables, (unsigned long long)r.copied_pages);
	uint64_t index = n / 2; CHECK(!ioctl(fd, PLAN_POPULATE, &index));
	/* Every creator ref and plan may vanish before first user fault. */
	CHECK(!close(fd));
	if (n <= 16384) check_pages(BASE, n, 0);
	else { check_pages(BASE, 1, 0); check_pages(BASE + (n - 1) * PAGE, 1, n - 1); }
	remove_map(BASE, n); drained(mode ? "legacy-arm" : "adopt-prepared-close-before-fault");
}
static void existing_tables(void) {
	size_t n = 1024; int fd = prepare(BASE + PAGE, n, 0);
	mapping(BASE, n + 2, MAP_PRIVATE); BASE[0] = 0x63; BASE[(n + 1) * PAGE] = 0xe1;
	CHECK(!ioctl(fd, PLAN_ARM)); struct plan_result r = stat_fd(fd);
	CHECK(r.tables == 3 && r.moved_tables == 1 && r.copied_pages == 512 && r.published == n);
	CHECK(!close(fd)); check_pages(BASE + PAGE, n, 0);
	CHECK(BASE[0] == 0x63 && BASE[(n + 1) * PAGE] == 0xe1);
	remove_map(BASE, n + 2); drained("existing-boundary-tables-sentinels");
}
static void occupied_and_shared(void) {
	int fd = prepare(BASE, 1024, 0); mapping(BASE, 1024, MAP_PRIVATE);
	BASE[900 * PAGE] = 0xa7; error_call(fd, PLAN_ARM, NULL, EEXIST);
	CHECK(!stat_fd(fd).published && BASE[900 * PAGE] == 0xa7);
	CHECK(!close(fd)); CHECK(BASE[0] == 0); remove_map(BASE, 1024); drained("late-occupied-pte-preflight");
	fd = prepare(BASE, 8, 0); mapping(BASE, 8, MAP_SHARED);
	error_call(fd, PLAN_ARM, NULL, EINVAL); CHECK(!close(fd)); remove_map(BASE, 8); drained("reject-shared-vma");
	fd = prepare(BASE, 8, 0); mapping(BASE, 8, MAP_PRIVATE);
	CHECK(!mprotect(BASE + PAGE, PAGE, PROT_READ));
	error_call(fd, PLAN_ARM, NULL, EINVAL); CHECK(!close(fd)); remove_map(BASE, 8); drained("reject-split-vma");
}
static void partial_rollback(void) {
	for (int existing = 0; existing < 2; existing++) {
		unsigned char *start = BASE + (existing ? PAGE : 0);
		int fd = prepare(start, 1536, 0); mapping(BASE, 1538, MAP_PRIVATE);
		if (existing) BASE[0] = 0xc9;
		uint64_t after = existing ? 511 : 512;
		CHECK(!ioctl(fd, PLAN_INJECT, &after)); error_call(fd, PLAN_ARM, NULL, ENOMEM);
		struct plan_result r = stat_fd(fd); CHECK(r.published == after);
		CHECK(r.created - r.released == 1536 - after);
		CHECK(r.copied_pages == (existing ? 511 : 0));
		CHECK(r.moved_tables == (existing ? 0 : 1));
		CHECK(!close(fd));
		for (size_t i = 0; i < 1536; i++) CHECK(start[i * PAGE] == 0);
		if (existing) CHECK(BASE[0] == 0xc9);
		remove_map(BASE, 1538); drained(existing ? "partial-copy-rollback" : "partial-adopt-rollback");
	}
}
static void different_mm(void) {
	int fd = prepare(BASE, 2048, 0); pid_t p = fork(); CHECK(p >= 0);
	if (!p) { mapping(BASE, 2048, MAP_PRIVATE); CHECK(!ioctl(fd, PLAN_ARM));
		CHECK(!close(fd)); check_pages(BASE, 2048, 0); remove_map(BASE, 2048); _exit(0); }
	wait_child(p); CHECK(stat_fd(fd).published == 2048); CHECK(!close(fd)); drained("prepare-parent-attach-child-mm");
}
static void aliases(void) {
	int fd = prepare(BASE, 64, 0); mapping(BASE, 64, MAP_PRIVATE); CHECK(!ioctl(fd, PLAN_ARM));
	CHECK(!close(fd)); check_pages(BASE, 1, 0);
	pid_t p = fork(); CHECK(p >= 0);
	if (!p) { check_pages(BASE, 64, 0); memset(BASE, 0x79, 64 * PAGE); remove_map(BASE, 64); _exit(0); }
	wait_child(p); check_pages(BASE, 64, 0); remove_map(BASE, 64); drained("fork-unresolved-and-cow");
}
static void offsets_remap_discard(void) {
	int fd = prepare(BASE, 64, 0); mapping(OTHER, 64, MAP_PRIVATE);
	CHECK(mremap(OTHER, 64 * PAGE, 64 * PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, BASE) == BASE);
	CHECK(!ioctl(fd, PLAN_ARM));
	CHECK(mremap(BASE, 64 * PAGE, 64 * PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, OTHER) == OTHER);
	CHECK(!mprotect(OTHER + 16 * PAGE, 16 * PAGE, PROT_READ));
	uint64_t before = stat_fd(fd).faults;
	for (uint64_t i = 0; i < 64; i++) CHECK(!ioctl(fd, PLAN_POPULATE, &i));
	CHECK(stat_fd(fd).faults == before + 64);
	check_pages(OTHER, 64, 0); CHECK(!close(fd)); remove_map(OTHER, 64); drained("offset-base-remap-split-background-populate");
	fd = prepare(BASE, 64, 0); mapping(BASE, 64, MAP_PRIVATE); CHECK(!ioctl(fd, PLAN_ARM));
	CHECK(!close(fd)); CHECK(!madvise(BASE, 32 * PAGE, MADV_DONTNEED));
	for (size_t i = 0; i < 32 * PAGE; i++) CHECK(BASE[i] == 0);
	check_pages(BASE + 32 * PAGE, 32, 32); remove_map(BASE, 64); drained("discard-unresolved-half");
}
static void file_write(const char *path, const char *text) {
	int fd = open(path, O_WRONLY); CHECK(fd >= 0);
	CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text)); CHECK(!close(fd));
}
static uint64_t page_table_charge(void) {
	/* The periodic flush is deferrable while guest CPUs idle. Force the
	 * normal rstat flush through the isolated-only fixture; no timed wait
	 * or relaxed tolerance can substitute for exact accounting. */
	CHECK(!ioctl(monitor, PLAN_FLUSH));
	FILE *f = fopen("/sys/fs/cgroup/plan/memory.stat", "r"); CHECK(f);
	char name[128]; unsigned long long value; uint64_t result = UINT64_MAX;
	while (fscanf(f, "%127s %llu", name, &value) == 2)
		if (!strcmp(name, "pagetables")) result = value;
	CHECK(!fclose(f)); CHECK(result != UINT64_MAX); return result;
}
static void pipe_byte(int fd, int output) {
	char c = 'x'; CHECK((output ? write(fd, &c, 1) : read(fd, &c, 1)) == 1);
}
static void memcg_adoption(void) {
	CHECK(!mkdir("/sys/fs/cgroup/plan", 0755));
	const size_t n = 16384; int fd = prepare(BASE, n, 0), to[2], from[2];
	CHECK(!pipe(to) && !pipe(from)); pid_t child = fork(); CHECK(child >= 0);
	if (!child) {
		CHECK(!close(to[1]) && !close(from[0]));
		file_write("/sys/fs/cgroup/plan/cgroup.procs", "0\n");
		mapping(BASE, n, MAP_PRIVATE); pipe_byte(from[1], 1); pipe_byte(to[0], 0);
		CHECK(!ioctl(fd, PLAN_ARM)); pipe_byte(from[1], 1); pipe_byte(to[0], 0);
		CHECK(!close(fd)); remove_map(BASE, n); pipe_byte(from[1], 1); pipe_byte(to[0], 0);
		_exit(0);
	}
	CHECK(!close(to[0]) && !close(from[1])); pipe_byte(from[0], 0);
	uint64_t before = page_table_charge(); pipe_byte(to[1], 1); pipe_byte(from[0], 0);
	uint64_t armed = page_table_charge();
	printf("SBK_PLAN_MEMCG_CHECK before=%" PRIu64 " armed=%" PRIu64 "\n", before, armed);
	CHECK(armed >= before + (n / 512) * PAGE);
	pipe_byte(to[1], 1); pipe_byte(from[0], 0); uint64_t removed = page_table_charge();
	CHECK(armed >= removed + (n / 512) * PAGE);
	printf("SBK_PLAN_MEMCG before=%" PRIu64 " armed=%" PRIu64 " removed=%" PRIu64 "\n", before, armed, removed);
	pipe_byte(to[1], 1); wait_child(child);
	CHECK(!close(to[1]) && !close(from[0]) && !close(fd));
	CHECK(!rmdir("/sys/fs/cgroup/plan")); drained("charge-adopting-cgroup-uncharge-on-unmap");
}
int main(int argc, char **argv) {
	(void)argv; CHECK(argc == 1); setbuf(stdout, NULL); monitor = device();
	canceled_and_errors(); adopt_and_bench(16384, 0); existing_tables(); occupied_and_shared(); partial_rollback();
	different_mm(); aliases(); offsets_remap_discard(); memcg_adoption();
	adopt_and_bench(262144, 0); adopt_and_bench(262144, 3);
	drained("ALL"); CHECK(!close(monitor)); puts("SBK_PLAN_ALL_PASS"); return 0;
}
