#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sb-precopy.h"
#include "sb-stage.h"
#define P 4096
#define N 512
static int reservation_flags;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d %s errno=%d\n", __FILE__, __LINE__, #x, errno); exit(1); } } while (0)
static int eligible(pid_t pid, uint64_t address) { (void)pid; (void)address; return 1; }

static void *stage_address(void)
{
    char line[512];
    unsigned long start = 0, end = 0, a, b;
    FILE *f = fopen("/proc/self/smaps", "r");
    CHECK(f);
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%lx-%lx", &a, &b) == 2) { start = a; end = b; }
        if (!strncmp(line, "VmFlags:", 8) && strstr(line, " dc ") && end - start == N * P) {
            fclose(f); return (void *)start;
        }
    }
    fclose(f); CHECK(0); return NULL;
}

static void check_helper_excluded(void *address)
{
    unsigned char resident;
    int status;
    pid_t helper = fork(); CHECK(helper >= 0);
    if (!helper) {
        errno = 0;
        /* No allocations in this child before the probe can reuse the hole. */
        CHECK(mincore(address, P, &resident) == -1 && errno == ENOMEM);
        _exit(0);
    }
    CHECK(waitpid(helper, &status, 0) == helper && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void check_reacceptance_rejected(int directory, const char *name, int (*validate)(int))
{
    int status, manifest = openat(directory, name, O_RDWR);
    CHECK(manifest >= 0);
    off_t bitmap_start = lseek(manifest, 0, SEEK_END) - (N + 7) / 8;
    unsigned char original, forged;
    CHECK(pread(manifest, &original, 1, bitmap_start) == 1);
    CHECK(!(original & (1U << 5)));
    forged = original | (1U << 5);
    CHECK(pwrite(manifest, &forged, 1, bitmap_start) == 1);
    pid_t rejected = fork(); CHECK(rejected >= 0);
    if (!rejected) { CHECK(validate(directory) == -1); _exit(0); }
    CHECK(waitpid(rejected, &status, 0) == rejected && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(pwrite(manifest, &original, 1, bitmap_start) == 1);
    close(manifest);
}

static void check_child(uint64_t source_address)
{
    unsigned char residency[N];
    unsigned long bitmap[N / 64] = {0}, parent_bitmap[N / 64];
    memset(parent_bitmap, 0xff, sizeof(parent_bitmap));
    unsigned char *target = mmap(NULL, N * P, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(target != MAP_FAILED);
    CHECK(sb_stage_adopt(1, source_address, source_address + N * P, target,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN, PROT_READ | PROT_WRITE,
                        bitmap, parent_bitmap) == 0);
    CHECK(sb_stage_adopt(1, source_address, source_address + N * P, target,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | reservation_flags, PROT_READ | PROT_WRITE,
                        bitmap, parent_bitmap) == 1);
    CHECK(!mincore(target, N * P, residency));
    for (int i = 0; i < N; i++) {
        if (i == 5 || i == 6 || i == 7 || i >= 256) {
            CHECK(!(residency[i] & 1));
            CHECK(!(bitmap[i / 64] & (1UL << (i % 64))) && (parent_bitmap[i / 64] & (1UL << (i % 64))));
            CHECK(!sb_stage_was_adopted(i));
        } else {
            CHECK(residency[i] & 1);
            CHECK((bitmap[i / 64] & (1UL << (i % 64))) && !(parent_bitmap[i / 64] & (1UL << (i % 64))));
            CHECK(sb_stage_was_adopted(i));
            for (int j = 0; j < P; j++) CHECK(target[i * P + j] == (unsigned char)(i + 1));
        }
    }
    /* A file-backed snapshot would incorrectly reappear after DONTNEED. */
    CHECK(!madvise(target + P, P, MADV_DONTNEED));
    for (int j = 0; j < P; j++) CHECK(target[P + j] == 0);
    /* Adoption must remove the staging-only DONTFORK flag. */
    int status;
    pid_t app_child = fork(); CHECK(app_child >= 0);
    if (!app_child) {
        CHECK(target[0] == 1); target[0] = 0xab; _exit(0);
    }
    CHECK(waitpid(app_child, &status, 0) == app_child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(target[0] == 1);
    target[0] = 0xee;
    CHECK(!munmap(target, N * P));
    _exit(0);
}

int main(int argc, char **argv)
{
    (void)argv;
    reservation_flags = argc > 1 ? MAP_NORESERVE : 0;
    int sockets[2], status, directory, snapshot_fd;
    pid_t stage_parent;
    char flag;
    char path[] = "/tmp/sb-stage-test-XXXXXX";
    void *snapshot, *rx;
    uint64_t length;
    struct sb_precopy_page candidates[N];
    unsigned char *arena = mmap(NULL, (N + 2) * P, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | reservation_flags, -1, 0);
    CHECK(arena != MAP_FAILED && !mprotect(arena + P, N * P, PROT_READ | PROT_WRITE));
    unsigned char *source = arena + P;
    CHECK(!madvise(source, N * P, MADV_NOHUGEPAGE));
    for (int i = 0; i < N; i++) {
        memset(source + i * P, i + 1, P);
        candidates[i] = (struct sb_precopy_page){ .pid = getpid(), .address = (uint64_t)source + i * P };
    }
    CHECK(!sb_precopy_build(candidates, N, 8 << 20, 4, &snapshot, &length));
    rx = sb_stage_allocate(length, &snapshot_fd);
    CHECK(rx); memcpy(rx, snapshot, length);
    CHECK(mkdtemp(path)); directory = open(path, O_RDONLY | O_DIRECTORY); CHECK(directory >= 0);
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    stage_parent = fork(); CHECK(stage_parent >= 0);
    if (!stage_parent) {
        close(sockets[0]);
        sb_stage_set_max_mb(1);
        CHECK(!sb_stage_receive(sockets[1], 4));
        CHECK(write(sockets[1], "S", 1) == 1);
        CHECK(read(sockets[1], &flag, 1) == 1 && flag == 'P');
        CHECK(!sb_stage_prune(directory));
        CHECK(write(sockets[1], "P", 1) == 1);
        CHECK(read(sockets[1], &flag, 1) == 1 && flag == 'R');
        check_reacceptance_rejected(directory, "sb-precopy-ps.img", sb_stage_prune);
        CHECK(!sb_stage_prune(directory));
        CHECK(write(sockets[1], "R", 1) == 1);
        CHECK(read(sockets[1], &flag, 1) == 1 && flag == 'I');
        check_reacceptance_rejected(directory, "sb-precopy.img", sb_stage_finalize);
        CHECK(!sb_stage_finalize(directory));
        void *address = stage_address();
        check_helper_excluded(address);
        for (int i = 0; i < 2; i++) {
            CHECK(!sb_stage_inherit(true));
            pid_t child = fork(); CHECK(child >= 0);
            CHECK(!sb_stage_inherit(false));
            if (!child) {
                check_helper_excluded(address);
                /* Descendant restore tasks must still be able to inherit. */
                if (i == 1) {
                    CHECK(!sb_stage_inherit(true));
                    pid_t grandchild = fork(); CHECK(grandchild >= 0);
                    CHECK(!sb_stage_inherit(false));
                    if (!grandchild) check_child((uint64_t)source);
                    CHECK(waitpid(grandchild, &status, 0) == grandchild && WIFEXITED(status) && WEXITSTATUS(status) == 0);
                    _exit(0);
                }
                check_child((uint64_t)source);
            }
            CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
            check_helper_excluded(address);
        }
        _exit(0);
    }
    close(sockets[1]);
    CHECK(!sb_stage_send(sockets[0], snapshot_fd, rx, length));
    CHECK(read(sockets[0], &flag, 1) == 1 && flag == 'S');
    memset(source + 5 * P, 0xee, P);
    CHECK(!sb_precopy_prune(directory));
    CHECK(write(sockets[0], "P", 1) == 1);
    CHECK(read(sockets[0], &flag, 1) == 1 && flag == 'P');
    memset(source + 6 * P, 0xee, P);
    CHECK(!sb_precopy_prune(directory));
    CHECK(write(sockets[0], "R", 1) == 1);
    CHECK(read(sockets[0], &flag, 1) == 1 && flag == 'R');
    memset(source + 7 * P, 0xee, P);
    struct sb_precopy_pid pid = { .source = getpid(), .destination = 1 };
    CHECK(!sb_precopy_finalize(&pid, 1, directory, eligible));
    CHECK(write(sockets[0], "I", 1) == 1);
    CHECK(waitpid(stage_parent, &status, 0) == stage_parent && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("{\"snapshot_pages\":512,\"invalidated_pages\":3,\"adopted_per_child\":253,\"unselected_pages\":256,\"children\":2,\"byte_verification\":true,\"fork_cow_verified\":true,\"invalid_pages_nonresident\":true,\"discard_returns_zero\":true,\"unsupported_flags_fallback\":true,\"helpers_excluded\":true,\"descendant_inheritance\":true,\"adopted_application_fork\":true,\"ps_refresh_then_is_prune\":true,\"reject_reacceptance\":true}\n");
    unlinkat(directory, "sb-precopy.img", 0); unlinkat(directory, "sb-precopy-ps.img", 0);
    close(directory); rmdir(path);
    return 0;
}
