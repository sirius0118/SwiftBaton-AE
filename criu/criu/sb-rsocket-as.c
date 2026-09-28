/* Baseline AS transport: independent rsocket direct-write lanes. This does
 * not alter the sealed SwiftBaton-U/K profiles unless --rsocket-as is set. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <rdma/rsocket.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "RDMA.h"
#include "log.h"
#include "sb-rsocket-as.h"

#define AS_MAGIC UINT32_C(0x53424153)
#define AS_CHUNK (256U * 1024U)
#define AS_LANES 3
#define AS_TIMEOUT_NS UINT64_C(15000000000)
enum { AS_DATA = 1, AS_ACK = 2 };

struct as_map { uint32_t magic, lane; uint64_t offset, length; };
struct as_message { uint32_t magic, type; uint64_t sequence; };
struct as_lane {
    int fd, listener;
    off_t remote_offset;
    uint64_t remote_length, local_length;
    pthread_t receiver;
    pthread_mutex_t send_lock, write_lock;
    uint64_t sent, acknowledged, received, bytes, writes;
    int error;
};
static struct as_lane lanes[AS_LANES];
static int ready;

static uint64_t as_now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int as_send(int fd, const void *data, size_t length)
{
    const char *at = data;
    while (length) {
        ssize_t n = rsend(fd, at, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        at += n; length -= n;
    }
    return 0;
}
static int as_recv(int fd, void *data, size_t length)
{
    char *at = data;
    while (length) {
        ssize_t n = rrecv(fd, at, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        at += n; length -= n;
    }
    return 0;
}
static int as_send_message(struct as_lane *lane, uint32_t type, uint64_t sequence)
{
    struct as_message message = {AS_MAGIC, type, sequence};
    int result;
    pthread_mutex_lock(&lane->send_lock);
    result = as_send(lane->fd, &message, sizeof(message));
    pthread_mutex_unlock(&lane->send_lock);
    return result;
}
static void *as_receive(void *argument)
{
    struct as_lane *lane = argument;
    for (;;) {
        struct as_message message;
        if (as_recv(lane->fd, &message, sizeof(message)) || message.magic != AS_MAGIC)
            break;
        if (message.type == AS_DATA) {
            if (message.sequence != lane->received + 1) break;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            lane->received = message.sequence;
            if (as_send_message(lane, AS_ACK, message.sequence)) break;
        } else if (message.type == AS_ACK) {
            if (message.sequence != __atomic_load_n(&lane->acknowledged, __ATOMIC_RELAXED) + 1)
                break;
            __atomic_store_n(&lane->acknowledged, message.sequence, __ATOMIC_RELEASE);
        } else break;
    }
    __atomic_store_n(&lane->error, 1, __ATOMIC_RELEASE);
    return NULL;
}
static int as_configure(int fd)
{
    int mappings = 1, buffers = 8 * 1024 * 1024, queue = 512;
    return rsetsockopt(fd, SOL_RDMA, RDMA_IOMAPSIZE, &mappings, sizeof(mappings)) ||
           rsetsockopt(fd, SOL_RDMA, RDMA_RQSIZE, &queue, sizeof(queue)) ||
           rsetsockopt(fd, SOL_RDMA, RDMA_SQSIZE, &queue, sizeof(queue)) ||
           rsetsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffers, sizeof(buffers)) ||
           rsetsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffers, sizeof(buffers));
}
static int as_open(struct as_lane *lane, int source, struct sockaddr_in *address)
{
    int fd = rsocket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || as_configure(fd)) return -1;
    if (source) {
        if (rconnect(fd, (struct sockaddr *)address, sizeof(*address))) return -1;
    } else {
        lane->listener = fd;
        if (rbind(fd, (struct sockaddr *)address, sizeof(*address)) ||
            rlisten(fd, 1)) return -1;
        fd = raccept(fd, NULL, NULL);
        if (fd < 0) return -1;
    }
    lane->fd = fd;
    return 0;
}
int sb_rsocket_as_start(int control_socket, int source, int port)
{
    struct sockaddr_in address = {0};
    socklen_t address_length = sizeof(address);
    void *buffers[AS_LANES] = {PF_res.buf, FT_res.buf, TS_res.buf};
    uint64_t lengths[AS_LANES] = {
        source ? sizeof(struct page_request_set_t) : sizeof(struct page_data_set_t),
        sizeof(struct prefetch_t_buffer),
        source ? 2 * sizeof(int) : 2 * sizeof(int) + TRANSFER_REGION_SIZE * TRANSFER_BUFFER_SIZE,
    };
    if (ready || port < 1 || port + 11 > 65535) return -1;
    if ((source ? getpeername(control_socket, (struct sockaddr *)&address, &address_length) :
                  getsockname(control_socket, (struct sockaddr *)&address, &address_length)) ||
        address.sin_family != AF_INET) return -1;
    for (int i = 0; i < AS_LANES; i++) {
        struct as_lane *lane = &lanes[i];
        struct as_map own, peer;
        off_t offset;
        if (!buffers[i] || !lengths[i]) return -1;
        lane->listener = -1;
        lane->fd = -1;
        lane->local_length = lengths[i];
        address.sin_port = htons(port + 9 + i);
        if (pthread_mutex_init(&lane->send_lock, NULL) ||
            pthread_mutex_init(&lane->write_lock, NULL) ||
            as_open(lane, source, &address)) return -1;
        offset = riomap(lane->fd, buffers[i], lengths[i], PROT_WRITE, 0, -1);
        if (offset == -1) return -1;
        own = (struct as_map){AS_MAGIC, i, (uint64_t)offset, lengths[i]};
        /* The map is advertised by the first stream send after riomap. */
        if (source) {
            if (as_send(lane->fd, &own, sizeof(own)) || as_recv(lane->fd, &peer, sizeof(peer))) return -1;
        } else {
            if (as_recv(lane->fd, &peer, sizeof(peer)) || as_send(lane->fd, &own, sizeof(own))) return -1;
        }
        if (peer.magic != AS_MAGIC || peer.lane != (uint32_t)i ||
            peer.offset > INT64_MAX || !peer.length || peer.length > INT64_MAX - peer.offset)
            return -1;
        lane->remote_offset = (off_t)peer.offset;
        lane->remote_length = peer.length;
        if (pthread_create(&lane->receiver, NULL, as_receive, lane)) return -1;
        pthread_detach(lane->receiver);
        pr_info("SB_AS_RSOCKET connected lane=%d role=%s local_bytes=%llu remote_bytes=%llu\n",
                i, source ? "source" : "target", (unsigned long long)lengths[i],
                (unsigned long long)peer.length);
    }
    ready = 1;
    return 0;
}
int sb_rsocket_as_putv(int index, const struct write_part *parts, unsigned count)
{
    struct as_lane *lane;
    uint64_t sequence, begun, payload = 0;
    int result = -1;
    if (!ready || index < 0 || index >= AS_LANES || !parts || !count || count > SB_RDMA_WRITE_MAX)
        return -1;
    lane = &lanes[index];
    pthread_mutex_lock(&lane->write_lock);
    if (__atomic_load_n(&lane->error, __ATOMIC_ACQUIRE)) goto out;
    for (unsigned i = 0; i < count; i++) {
        const struct write_part *part = &parts[i];
        size_t done = 0;
        if (!part->address || !part->length || part->offset > lane->remote_length ||
            part->length > lane->remote_length - part->offset) goto out;
        while (done < part->length) {
            size_t chunk = part->length - done;
            if (chunk > AS_CHUNK) chunk = AS_CHUNK;
            size_t n = riowrite(lane->fd, (const char *)part->address + done, chunk,
                                lane->remote_offset + part->offset + done, 0);
            if (n == (size_t)-1 || !n) goto out;
            done += n;
        }
        payload += part->length;
    }
    sequence = ++lane->sent;
    if (as_send_message(lane, AS_DATA, sequence)) goto out;
    begun = as_now_ns();
    while (__atomic_load_n(&lane->acknowledged, __ATOMIC_ACQUIRE) < sequence) {
        if (__atomic_load_n(&lane->error, __ATOMIC_ACQUIRE) ||
            as_now_ns() - begun > AS_TIMEOUT_NS) goto out;
        sched_yield();
    }
    lane->bytes += payload;
    lane->writes++;
    result = 0;
out:
    pthread_mutex_unlock(&lane->write_lock);
    return result;
}
void sb_rsocket_as_report(const char *side)
{
    if (!ready) return;
    for (int i = 0; i < AS_LANES; i++) {
        struct as_lane *lane = &lanes[i];
        pr_info("SB_AS_RSOCKET_SUMMARY side=%s lane=%d writes=%llu bytes=%llu received=%llu acknowledged=%llu error=%d\n",
                side, i, (unsigned long long)lane->writes,
                (unsigned long long)lane->bytes, (unsigned long long)lane->received,
                (unsigned long long)__atomic_load_n(&lane->acknowledged, __ATOMIC_ACQUIRE),
                __atomic_load_n(&lane->error, __ATOMIC_ACQUIRE));
    }
}
