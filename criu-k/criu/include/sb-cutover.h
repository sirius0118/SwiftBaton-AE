/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SB_CUTOVER_H
#define SB_CUTOVER_H
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
/* The image directory is private to this migration and owned by the runtime.
 * The cooperating helper authenticates both hosts before creating the ACK.
 * Existing CRIU source/target network isolation is never bypassed. */
static inline int sb_cutover_marker(int dir, const char *name)
{
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    return close(fd);
}
static inline int sb_cutover_wait_named(int dir, const char *name)
{
    struct timespec start, now, delay = { .tv_nsec = 100000 };
    struct stat st;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return -1;
    for (;;) {
        if (!fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW))
            return S_ISREG(st.st_mode) && st.st_uid == geteuid() ? 0 : -1;
        if (errno != ENOENT || clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
        if (now.tv_sec - start.tv_sec >= 15) { errno = ETIMEDOUT; return -1; }
        nanosleep(&delay, NULL);
    }
}
static inline int sb_cutover_wait_closed(int dir)
{
    return sb_cutover_marker(dir, "sb-gate-request") ||
           sb_cutover_wait_named(dir, "sb-gate-closed") ? -1 : 0;
}
#endif
