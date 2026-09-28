/* SPDX-License-Identifier: GPL-2.0 */
/* Demand-only K baseline. Source reads its frozen process, then uses the same
 * userspace librdmacm rsocket direct-write transport as the U baselines. The
 * destination kernel still owns PTE interception, install and retirement. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sb-kernel-rsocket-proxy.h"
#include "log.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <rdma/rsocket.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define PROXY_MAGIC UINT32_C(0x53425058)
#define PROXY_PAGE 4096U
#define PROXY_MAX_WORKERS 16U
struct proxy_request {
    uint32_t magic, source_pid;
    uint64_t source_address, pages, index, sequence;
};
struct proxy_reply {
    uint32_t magic;
    int32_t status;
    uint64_t sequence;
};
struct source_peer {
    pthread_t thread;
    int fd;
    uint64_t reads, bytes;
    int error;
};
struct source_session {
    pthread_t accept_thread;
    struct source_peer peers[PROXY_MAX_WORKERS];
    struct sbk_catalog_record *records;
    size_t count;
    unsigned workers, accepted;
    int listener;
    atomic_bool stop;
    bool started;
};
struct target_peer {
    pthread_t thread;
    int fd;
    uint64_t faults, bytes;
    int error;
};
struct target_session {
    struct target_peer peers[PROXY_MAX_WORKERS];
    struct sbk_proxy_region *regions;
    size_t count;
    unsigned workers, launched;
    struct sockaddr_in source;
    atomic_bool stop;
    atomic_int error;
    pthread_mutex_t startup_lock;
    pthread_cond_t startup_wait;
    unsigned ready;
    bool started;
};
static struct source_session src = {.listener = -1};
static struct target_session dst;

static int full_send(int fd, const void *data, size_t size)
{
    const char *p = data;
    while (size) {
        ssize_t n = rsend(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? -errno : -EPIPE;
        p += n; size -= n;
    }
    return 0;
}
static int full_recv(int fd, void *data, size_t size)
{
    char *p = data;
    while (size) {
        ssize_t n = rrecv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? -errno : -EPIPE;
        p += n; size -= n;
    }
    return 0;
}
static int configure_socket(int fd, bool map)
{
    int maps = 1, queue = 512, bytes = 8 * 1024 * 1024;
    if ((map && rsetsockopt(fd, SOL_RDMA, RDMA_IOMAPSIZE, &maps, sizeof(maps))) ||
        rsetsockopt(fd, SOL_RDMA, RDMA_RQSIZE, &queue, sizeof(queue)) ||
        rsetsockopt(fd, SOL_RDMA, RDMA_SQSIZE, &queue, sizeof(queue)) ||
        rsetsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) ||
        rsetsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)))
        return -errno;
    return 0;
}
static int proxy_address(int control_fd, int source, int port,
                         struct sockaddr_in *address)
{
    socklen_t size = sizeof(*address);
    if (port < 1 || port > 65535 ||
        (source ? getsockname(control_fd, (struct sockaddr *)address, &size) :
                  getpeername(control_fd, (struct sockaddr *)address, &size)) ||
        address->sin_family != AF_INET)
        return -EINVAL;
    address->sin_port = htons(port);
    return 0;
}
static int allowed(const struct proxy_request *request)
{
    if (request->magic != PROXY_MAGIC || request->index >= request->pages ||
        request->source_address > UINT64_MAX - request->index * PROXY_PAGE)
        return -EPROTO;
    for (size_t i = 0; i < src.count; i++) {
        const struct sbk_catalog_record *r = &src.records[i];
        if (r->source_pid == request->source_pid &&
            r->remote.address == request->source_address &&
            r->remote.pages == request->pages)
            return 0;
    }
    return -EACCES;
}
static void *source_worker(void *argument)
{
    struct source_peer *peer = argument;
    void *page = NULL;
    uint64_t remote_offset;
    int ret = posix_memalign(&page, PROXY_PAGE, PROXY_PAGE);
    if (ret) { peer->error = -ret; goto out; }
    ret = full_recv(peer->fd, &remote_offset, sizeof(remote_offset));
    if (ret || remote_offset > INT64_MAX - PROXY_PAGE) {
        peer->error = ret ? ret : -EPROTO; goto out;
    }
    while (!atomic_load(&src.stop)) {
        struct proxy_request request;
        struct proxy_reply reply = {.magic = PROXY_MAGIC};
        struct iovec local = {.iov_base = page, .iov_len = PROXY_PAGE};
        struct iovec remote;
        ret = full_recv(peer->fd, &request, sizeof(request));
        if (ret) {
            if (ret != -EPIPE && !atomic_load(&src.stop)) peer->error = ret;
            break;
        }
        reply.sequence = request.sequence;
        reply.status = allowed(&request);
        if (!reply.status) {
            remote.iov_base = (void *)(uintptr_t)
                (request.source_address + request.index * PROXY_PAGE);
            remote.iov_len = PROXY_PAGE;
            ssize_t n = process_vm_readv(request.source_pid, &local, 1, &remote, 1, 0);
            if (n != PROXY_PAGE)
                reply.status = n < 0 ? -errno : -EIO;
        }
        if (!reply.status) {
            size_t done = 0;
            while (done < PROXY_PAGE) {
                size_t n = riowrite(peer->fd, (char *)page + done, PROXY_PAGE - done,
                                    (off_t)remote_offset + done, 0);
                if (n == (size_t)-1 || !n) {
                    reply.status = n == (size_t)-1 ? -errno : -EIO;
                    break;
                }
                done += n;
            }
            if (!reply.status) { peer->reads++; peer->bytes += PROXY_PAGE; }
        }
        if (full_send(peer->fd, &reply, sizeof(reply))) {
            peer->error = -EIO; break;
        }
        if (reply.status) { peer->error = reply.status; break; }
    }
out:
    free(page);
    return NULL;
}
static void *source_accept(void *argument)
{
    struct source_session *s = argument;
    while (!atomic_load(&s->stop) && s->accepted < s->workers) {
        struct pollfd p = {.fd = s->listener, .events = POLLIN};
        int n = rpoll(&p, 1, 100);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { s->peers[0].error = -errno; break; }
        if (!n) continue;
        int fd = raccept(s->listener, NULL, NULL);
        if (fd < 0) { s->peers[0].error = -errno; break; }
        struct source_peer *peer = &s->peers[s->accepted];
        peer->fd = fd;
        if (pthread_create(&peer->thread, NULL, source_worker, peer)) {
            peer->error = -errno; rclose(fd); peer->fd = -1; break;
        }
        s->accepted++;
    }
    return NULL;
}
int sbk_rsocket_source_start(int control_fd, int port,
                            const struct sbk_catalog_final *records, size_t count,
                            unsigned workers)
{
    struct sockaddr_in address = {};
    int ret;
    if (src.started || !records || !count || !workers || workers > PROXY_MAX_WORKERS ||
        (ret = proxy_address(control_fd, 1, port, &address)))
        return -EINVAL;
    src.records = calloc(count, sizeof(*src.records));
    if (!src.records) return -ENOMEM;
    src.count = count; src.workers = workers;
    for (size_t i = 0; i < count; i++) src.records[i] = records[i].record;
    for (unsigned i = 0; i < workers; i++) src.peers[i].fd = -1;
    atomic_store(&src.stop, false);
    src.listener = rsocket(AF_INET, SOCK_STREAM, 0);
    if (src.listener < 0) { ret = -errno; goto fail; }
    ret = configure_socket(src.listener, true);
    if (ret || rbind(src.listener, (struct sockaddr *)&address, sizeof(address)) ||
        rlisten(src.listener, workers)) { if (!ret) ret = -errno; goto fail; }
    if (pthread_create(&src.accept_thread, NULL, source_accept, &src)) {
        ret = -errno; goto fail;
    }
    src.started = true;
    pr_info("SBK_RSOCKET_SOURCE_READY regions=%zu workers=%u port=%d\n",
            count, workers, port);
    return 0;
fail:
    if (src.listener >= 0) rclose(src.listener);
    src.listener = -1;
    free(src.records); src.records = NULL; src.count = 0;
    return ret;
}
void sbk_rsocket_source_stop(void)
{
    if (!src.started) return;
    atomic_store(&src.stop, true);
    pthread_join(src.accept_thread, NULL);
    for (unsigned i = 0; i < src.accepted; i++)
        if (src.peers[i].fd >= 0) rshutdown(src.peers[i].fd, SHUT_RDWR);
    uint64_t reads = 0, bytes = 0;
    unsigned errors = 0;
    for (unsigned i = 0; i < src.accepted; i++) {
        pthread_join(src.peers[i].thread, NULL);
        reads += src.peers[i].reads; bytes += src.peers[i].bytes;
        errors += !!src.peers[i].error;
        rclose(src.peers[i].fd);
    }
    rclose(src.listener);
    pr_info("SBK_RSOCKET_SOURCE_SUMMARY connections=%u reads=%llu bytes=%llu errors=%u\n",
            src.accepted, (unsigned long long)reads, (unsigned long long)bytes, errors);
    free(src.records);
    memset(&src, 0, sizeof(src)); src.listener = -1;
}

static void target_fail(int error)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&dst.error, &expected, error)) {
        atomic_store(&dst.stop, true);
        for (size_t i = 0; i < dst.count; i++)
            ioctl(dst.regions[i].fd, SBK_IOC_CANCEL);
    }
}
static void target_ready(int error)
{
    pthread_mutex_lock(&dst.startup_lock);
    dst.ready++;
    if (error) target_fail(error);
    pthread_cond_broadcast(&dst.startup_wait);
    pthread_mutex_unlock(&dst.startup_lock);
}
static void *target_worker(void *argument)
{
    struct target_peer *peer = argument;
    struct pollfd *fds = NULL;
    void *page = NULL;
    uint64_t sequence = 0;
    bool mapped = false;
    int ret = 0;
    peer->fd = rsocket(AF_INET, SOCK_STREAM, 0);
    if (peer->fd < 0 || (ret = configure_socket(peer->fd, true)) ||
        rconnect(peer->fd, (struct sockaddr *)&dst.source, sizeof(dst.source))) {
        if (!ret) ret = -errno;
        goto startup_failed;
    }
    ret = posix_memalign(&page, PROXY_PAGE, PROXY_PAGE);
    if (ret) { ret = -ret; goto startup_failed; }
    off_t offset = riomap(peer->fd, page, PROXY_PAGE, PROT_WRITE, 0, -1);
    if (offset == (off_t)-1) { ret = -errno; goto startup_failed; }
    mapped = true;
    uint64_t wire_offset = (uint64_t)offset;
    ret = full_send(peer->fd, &wire_offset, sizeof(wire_offset));
    if (ret) goto startup_failed;
    fds = calloc(dst.count, sizeof(*fds));
    if (!fds) { ret = -ENOMEM; goto startup_failed; }
    for (size_t i = 0; i < dst.count; i++) {
        fds[i].fd = dst.regions[i].fd;
        fds[i].events = POLLIN;
    }
    target_ready(0);
    while (!atomic_load(&dst.stop)) {
        int n = poll(fds, dst.count, 100);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ret = -errno; break; }
        if (!n) continue;
        for (size_t i = 0; i < dst.count && !atomic_load(&dst.stop); i++) {
            struct sbk_proxy_request kernel = {};
            struct sbk_proxy_completion completion = {};
            struct proxy_request request;
            struct proxy_reply reply;
            if (!(fds[i].revents & POLLIN)) continue;
            if (ioctl(fds[i].fd, SBK_IOC_PROXY_NEXT, &kernel)) {
                if (errno == EAGAIN) continue;
                ret = -errno; break;
            }
            completion.id = kernel.id;
            if (kernel.index >= dst.regions[i].record.remote.pages ||
                kernel.lane != SBK_DEMAND) {
                completion.status = -EPROTO;
                ioctl(fds[i].fd, SBK_IOC_PROXY_COMPLETE, &completion);
                ret = -EPROTO; break;
            }
            request = (struct proxy_request){.magic = PROXY_MAGIC,
                .source_pid = dst.regions[i].record.source_pid,
                .source_address = dst.regions[i].record.remote.address,
                .pages = dst.regions[i].record.remote.pages,
                .index = kernel.index, .sequence = ++sequence};
            ret = full_send(peer->fd, &request, sizeof(request));
            if (!ret) ret = full_recv(peer->fd, &reply, sizeof(reply));
            if (!ret && (reply.magic != PROXY_MAGIC || reply.sequence != sequence ||
                         reply.status > 0)) ret = -EPROTO;
            completion.status = ret ? ret : reply.status;
            completion.data = (uintptr_t)page;
            if (ioctl(fds[i].fd, SBK_IOC_PROXY_COMPLETE, &completion) && errno != ENOENT)
                ret = -errno;
            if (!ret && completion.status) ret = completion.status;
            if (ret) break;
            peer->faults++; peer->bytes += PROXY_PAGE;
        }
        if (ret) break;
    }
    if (ret && !atomic_load(&dst.stop)) target_fail(ret);
    peer->error = ret;
    free(fds);
    if (peer->fd >= 0 && mapped && !ret) {
        /* librdmacm's implicit unmap in rclose can spin indefinitely after
         * the peer closes. Explicit unmap while it is still serving was
         * verified by the two-host 20k-page transport test. */
        if (riounmap(peer->fd, page, PROXY_PAGE)) peer->error = -errno;
        else mapped = false;
    }
    if (!mapped) {
        if (peer->fd >= 0) { rshutdown(peer->fd, SHUT_RDWR); rclose(peer->fd); peer->fd = -1; }
        free(page);
    } /* Failure path keeps registration until the page-client process exits. */
    return NULL;
startup_failed:
    peer->error = ret;
    target_ready(ret);
    if (!mapped) {
        if (peer->fd >= 0) { rclose(peer->fd); peer->fd = -1; }
        free(page);
    }
    return NULL;
}
int sbk_rsocket_target_start(int control_fd, int port,
                            const struct sbk_proxy_region *regions, size_t count,
                            unsigned workers)
{
    int ret;
    if (dst.started || !regions || !count || !workers || workers > PROXY_MAX_WORKERS ||
        (ret = proxy_address(control_fd, 0, port, &dst.source)))
        return -EINVAL;
    dst.regions = calloc(count, sizeof(*dst.regions));
    if (!dst.regions) return -ENOMEM;
    memcpy(dst.regions, regions, count * sizeof(*regions));
    dst.count = count; dst.workers = workers;
    atomic_store(&dst.stop, false); atomic_store(&dst.error, 0);
    pthread_mutex_init(&dst.startup_lock, NULL);
    pthread_cond_init(&dst.startup_wait, NULL);
    for (size_t i = 0; i < count; i++) {
        int flags = fcntl(regions[i].fd, F_GETFL);
        if (flags < 0 || fcntl(regions[i].fd, F_SETFL, flags | O_NONBLOCK)) {
            ret = -errno; goto fail;
        }
    }
    dst.started = true;
    for (unsigned i = 0; i < workers; i++) {
        dst.peers[i].fd = -1;
        ret = pthread_create(&dst.peers[i].thread, NULL, target_worker, &dst.peers[i]);
        if (ret) { ret = -ret; goto fail; }
        dst.launched++;
    }
    pthread_mutex_lock(&dst.startup_lock);
    while (dst.ready < dst.launched && !atomic_load(&dst.error))
        pthread_cond_wait(&dst.startup_wait, &dst.startup_lock);
    pthread_mutex_unlock(&dst.startup_lock);
    ret = atomic_load(&dst.error);
    if (ret) goto fail;
    pr_info("SBK_RSOCKET_TARGET_READY regions=%zu workers=%u port=%d\n",
            count, workers, port);
    return 0;
fail:
    if (dst.started) sbk_rsocket_target_stop();
    else {
        pthread_cond_destroy(&dst.startup_wait);
        pthread_mutex_destroy(&dst.startup_lock);
        free(dst.regions); dst.regions = NULL;
    }
    return ret;
}
void sbk_rsocket_target_stop(void)
{
    if (!dst.started) return;
    atomic_store(&dst.stop, true);
    uint64_t faults = 0, bytes = 0;
    unsigned errors = 0;
    for (unsigned i = 0; i < dst.launched; i++) {
        pthread_join(dst.peers[i].thread, NULL);
        faults += dst.peers[i].faults; bytes += dst.peers[i].bytes;
        errors += !!dst.peers[i].error;
    }
    pr_info("SBK_RSOCKET_TARGET_SUMMARY connections=%u faults=%llu bytes=%llu errors=%u\n",
            dst.launched, (unsigned long long)faults, (unsigned long long)bytes, errors);
    pthread_cond_destroy(&dst.startup_wait);
    pthread_mutex_destroy(&dst.startup_lock);
    free(dst.regions);
    memset(&dst, 0, sizeof(dst));
}
