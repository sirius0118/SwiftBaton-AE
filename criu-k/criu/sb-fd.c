/* Expanding a files_struct shared by dump threads can stall SCM_RIGHTS
 * reception. Allocate its capacity during PS, retaining no extra descriptor. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include "log.h"
#include "sb-fd.h"
#include "sb-fdpool.h"

int sb_eventfd_restore(uint64_t counter, bool semaphore)
{
    int fd;
    ssize_t ret;
    if (counter == UINT64_MAX) { errno = EINVAL; return -1; }
    fd = sb_fdpool_take(semaphore ? SB_FDP_SEMAPHORE : SB_FDP_EVENT);
    if (fd < 0) fd = eventfd(0, semaphore ? EFD_SEMAPHORE : 0);
    if (fd < 0 || !counter) return fd;
    do { ret = write(fd, &counter, sizeof(counter)); } while (ret < 0 && errno == EINTR);
    if (ret != sizeof(counter)) {
        int error = ret < 0 ? errno : EIO;
        close(fd); errno = error; return -1;
    }
    return fd;
}

static size_t count_fds(pid_t pid, int *highest)
{
    char path[64];
    struct dirent *entry;
    DIR *dir;
    size_t count = 0;
    snprintf(path, sizeof(path), "/proc/%d/fd", pid);
    dir = opendir(path);
    if (!dir) return 0; /* Racy PS hint; IS will check the actual process. */
    while ((entry = readdir(dir))) {
        char *end;
        long fd;
        if (entry->d_name[0] == '.') continue;
        fd = strtol(entry->d_name, &end, 10);
        if (*end || fd < 0 || fd > INT_MAX) continue;
        count++;
        if (highest && fd > *highest) *highest = fd;
    }
    closedir(dir);
    return count;
}

int sb_fdtable_prepare(const pid_t *pids, size_t count)
{
    uint64_t incoming = 0, wanted, capacity = 0;
    struct rlimit limit;
    int highest = 2, fd, reserved;
    char line[256];
    FILE *status;
    if ((!pids && count) || count > 4096 || getrlimit(RLIMIT_NOFILE, &limit)) return -1;
    for (size_t i = 0; i < count; i++) incoming += count_fds(pids[i], NULL);
    count_fds(getpid(), &highest);
    /* Allow simultaneous task drains and their image/RPC descriptors, with
     * spare slots for source FD churn after this observation. */
    wanted = (uint64_t)highest + 2 * incoming + 16 * count + 128;
    if (wanted < 512) wanted = 512;
    if (wanted > INT_MAX) wanted = INT_MAX;
    if (limit.rlim_cur != RLIM_INFINITY && wanted >= limit.rlim_cur) wanted = limit.rlim_cur;
    if (!wanted) return -1;
    status = fopen("/proc/self/status", "re");
    if (status) {
        while (fgets(line, sizeof(line), status)) {
            unsigned long long n;
            if (sscanf(line, "FDSize: %llu", &n) == 1) { capacity = n; break; }
        }
        fclose(status);
    }
    if (capacity >= wanted) {
        pr_info("SB_FD capacity_ready incoming=%llu capacity=%llu\n",
                (unsigned long long)incoming, (unsigned long long)capacity);
        return 0;
    }
    fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    reserved = fcntl(fd, F_DUPFD_CLOEXEC, (int)wanted - 1);
    close(fd);
    if (reserved < 0) { pr_perror("Prepare dump descriptor capacity"); return -1; }
    close(reserved);
    pr_info("SB_FD capacity_prepared incoming=%llu requested=%llu previous=%llu\n",
            (unsigned long long)incoming, (unsigned long long)wanted, (unsigned long long)capacity);
    return 0;
}
