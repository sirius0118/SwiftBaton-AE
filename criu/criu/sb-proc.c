#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "include/sb-proc.h"

/* Borrowed descriptors are stable until this same thread explicitly clears
 * its cache. Neither another worker nor shared service-fd bookkeeping can
 * close/reassign them. Threads still share the fd table, but own distinct fds. */
static __thread struct {
    int active, root, self, target;
    pid_t pid, owner;
} cache = { .root = -1, .self = -1, .target = -1 };
static void close_one(int *fd)
{
    if (*fd >= 0) close(*fd);
    *fd = -1;
}
void sb_proc_thread_clear(int root)
{
    close_one(&cache.self);
    close_one(&cache.target);
    cache.pid = -2;
    if (root) close_one(&cache.root);
}
void sb_proc_thread_begin(void)
{
    sb_proc_thread_clear(1);
    cache.owner = getpid();
    cache.active = 1;
}
void sb_proc_thread_end(void)
{
    sb_proc_thread_clear(1);
    cache.active = 0;
}
int sb_proc_thread_active(void) { return cache.active; }
int sb_proc_thread_set_root(int fd)
{
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) return -1;
    sb_proc_thread_clear(1);
    cache.root = copy;
    return 0;
}
int sb_proc_thread_set_self(int fd)
{
    close_one(&cache.self);
    cache.self = fd;
    cache.owner = getpid();
    return fd;
}
int sb_proc_thread_open(pid_t pid)
{
    char path[32];
    int fd;
    if (pid < -1) { errno = EINVAL; return -1; }
    if (cache.owner != getpid()) {
        sb_proc_thread_clear(1);
        cache.owner = getpid();
    }
    if (cache.root < 0) {
        cache.root = open("/proc", O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (cache.root < 0) return -1;
    }
    if (pid == -1) return cache.root;
    if (!pid && cache.self >= 0) return cache.self;
    if (pid > 0 && cache.pid == pid && cache.target >= 0) return cache.target;
    if (!pid) snprintf(path, sizeof(path), "self");
    else snprintf(path, sizeof(path), "%d", pid);
    fd = openat(cache.root, path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (!pid) { close_one(&cache.self); cache.self = fd; }
    else { close_one(&cache.target); cache.target = fd; cache.pid = pid; }
    return fd;
}
