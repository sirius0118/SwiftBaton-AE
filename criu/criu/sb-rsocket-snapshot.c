/* PCLive baseline: bounded, parallel memory reads over librdmacm rsocket.
 * The source serves its existing snapshot mapping; the destination writes
 * directly into its existing resident memfd mapping. No TCP data fallback. */
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
#include <sys/socket.h>
#include <unistd.h>
#include "log.h"
#include "sb-rsocket-snapshot.h"

#define SB_RS_MAGIC UINT32_C(0x53425231)
#define SB_RS_CHUNK (64U * 1024U)
#define SB_RS_REQUEST (16U * 1024U * 1024U)
#define SB_RS_SESSION (64ULL * 1024U * 1024U * 1024U)
#define SB_RS_WORKERS 16U

struct sb_rs_request { uint32_t magic, length; uint64_t offset; };
struct sb_rs_source { const unsigned char *bytes; uint64_t length; int listener; };
struct sb_rs_session { struct sb_rs_source *source; int socket; };
struct sb_rs_client {
    unsigned char *destination;
    uint64_t length;
    const char *source_ip;
    int port;
    const struct sb_rsocket_range *ranges;
    size_t count, next;
    uint64_t bytes;
    int error;
    pthread_mutex_t lock;
};
struct sb_rs_job { struct sb_rs_client *client; unsigned slot; };
/* The pageclient reads the two PS rounds sequentially. Keep each lane open
 * across the round boundary: closing a busy rsocket can block rclose and
 * prevent the next raccept from progressing on this librdmacm build. */
static int reusable_fds[SB_RS_WORKERS] = {[0 ... SB_RS_WORKERS - 1] = -1};
static char reusable_ip[INET_ADDRSTRLEN];
static int reusable_port;

static int configure_socket(int fd)
{
    int buffer = 8 * 1024 * 1024, queue = 512;
    if (rsetsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer)) ||
        rsetsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) ||
        rsetsockopt(fd, SOL_RDMA, RDMA_RQSIZE, &queue, sizeof(queue)) ||
        rsetsockopt(fd, SOL_RDMA, RDMA_SQSIZE, &queue, sizeof(queue))) return -1;
    return 0;
}

static int send_all(int fd, const void *bytes, uint64_t length)
{
    const unsigned char *at = bytes;
    while (length) {
        size_t chunk = length > SB_RS_CHUNK ? SB_RS_CHUNK : length;
        ssize_t done = rsend(fd, at, chunk, MSG_NOSIGNAL);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return -1;
        at += done;
        length -= done;
    }
    return 0;
}

static int receive_all(int fd, void *bytes, uint64_t length, int eof_allowed)
{
    unsigned char *at = bytes;
    uint64_t initial = length;
    while (length) {
        size_t chunk = length > SB_RS_CHUNK ? SB_RS_CHUNK : length;
        ssize_t done = rrecv(fd, at, chunk, 0);
        if (done < 0 && errno == EINTR) continue;
        if (done < 0) return -1;
        if (!done && eof_allowed && length == initial) return 0;
        if (!done) { errno = EPIPE; return -1; }
        at += done;
        length -= done;
    }
    return 1;
}

static void *serve_session(void *argument)
{
    struct sb_rs_session *session = argument;
    struct sb_rs_source *source = session->source;
    struct sb_rs_request request;
    uint64_t served = 0, last_reported = 0;
    unsigned requests = 0;
    int fd = session->socket, result, success = 0;
    free(session);
    while ((result = receive_all(fd, &request, sizeof(request), 1)) > 0) {
        if (request.magic != SB_RS_MAGIC) break;
        if (!request.length) {
            if (!send_all(fd, "OK", 2)) success = 1;
            break;
        }
        if (!requests)
            pr_info("SB_RSOCKET source_begin fd=%d offset=%llu length=%u\n",
                    fd, (unsigned long long)request.offset, request.length);
        if (
            request.length > SB_RS_REQUEST || request.offset > source->length ||
            request.length > source->length - request.offset ||
            request.length > SB_RS_SESSION - served ||
            send_all(fd, source->bytes + request.offset, request.length)) break;
        served += request.length;
        requests++;
        if (served - last_reported >= 256ULL * 1024 * 1024) {
            pr_info("SB_RSOCKET source_progress bytes=%llu requests=%u\n",
                    (unsigned long long)served, requests);
            last_reported = served;
        }
    }
    pr_info("SB_RSOCKET source_session bytes=%llu requests=%u ack=%d\n",
            (unsigned long long)served, requests, success);
    rclose(fd);
    pr_info("SB_RSOCKET source_closed bytes=%llu\n", (unsigned long long)served);
    return NULL;
}

static void *accept_sessions(void *argument)
{
    struct sb_rs_source *source = argument;
    for (;;) {
        struct sb_rs_session *session;
        pthread_t thread;
        int fd = raccept(source->listener, NULL, NULL);
        if (fd < 0 && errno == EINTR) continue;
        if (fd < 0) break;
        session = malloc(sizeof(*session));
        if (!session) { rclose(fd); break; }
        *session = (struct sb_rs_session){source, fd};
        if (pthread_create(&thread, NULL, serve_session, session)) {
            rclose(fd); free(session); break;
        }
        pthread_detach(thread);
    }
    rclose(source->listener);
    free(source);
    return NULL;
}

int sb_rsocket_snapshot_serve(const void *source, uint64_t length,
                             const char *bind_ip, int port)
{
    struct sockaddr_in address = {.sin_family = AF_INET};
    struct sb_rs_source *server;
    pthread_t thread;
    if (!source || !length || !bind_ip || port < 1 || port > 65535) return -1;
    server = calloc(1, sizeof(*server));
    if (!server) return -1;
    server->bytes = source;
    server->length = length;
    server->listener = rsocket(AF_INET, SOCK_STREAM, 0);
    if (server->listener < 0 || configure_socket(server->listener) ||
        inet_pton(AF_INET, bind_ip, &address.sin_addr) != 1) goto fail;
    address.sin_port = htons(port);
    if (rbind(server->listener, (struct sockaddr *)&address, sizeof(address)) ||
        rlisten(server->listener, 16)) goto fail;
    if (pthread_create(&thread, NULL, accept_sessions, server)) goto fail;
    pthread_detach(thread);
    return 0;
fail:
    if (server->listener >= 0) rclose(server->listener);
    free(server);
    return -1;
}

static int connect_source(const struct sb_rs_client *client)
{
    struct sockaddr_in address = {.sin_family = AF_INET};
    int fd = rsocket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (configure_socket(fd) ||
        inet_pton(AF_INET, client->source_ip, &address.sin_addr) != 1) goto fail;
    address.sin_port = htons(client->port);
    if (rconnect(fd, (struct sockaddr *)&address, sizeof(address))) goto fail;
    return fd;
fail:
    rclose(fd);
    return -1;
}

static int finish_session(int fd)
{
    struct sb_rs_request request = {SB_RS_MAGIC, 0, 0};
    char response[2];
    if (send_all(fd, &request, sizeof(request)) ||
        receive_all(fd, response, sizeof(response), 0) != 1 ||
        memcmp(response, "OK", 2)) return -1;
    return 0;
}

static void *read_worker(void *argument)
{
    struct sb_rs_job *job = argument;
    struct sb_rs_client *client = job->client;
    int fd = reusable_fds[job->slot];
    uint64_t in_session = 0, bytes = 0, last_reported = 0;
    for (;;) {
        struct sb_rsocket_range range;
        pthread_mutex_lock(&client->lock);
        if (client->error || client->next == client->count) {
            pthread_mutex_unlock(&client->lock);
            break;
        }
        range = client->ranges[client->next++];
        pthread_mutex_unlock(&client->lock);
        while (range.length) {
            struct sb_rs_request request;
            uint64_t length;
            if (fd < 0) {
                fd = connect_source(client);
                if (fd < 0) goto fail;
                pr_info("SB_RSOCKET target_begin fd=%d offset=%llu\n",
                        fd, (unsigned long long)range.offset);
            }
            length = range.length;
            if (length > SB_RS_REQUEST) length = SB_RS_REQUEST;
            if (length > SB_RS_SESSION - in_session)
                length = SB_RS_SESSION - in_session;
            request = (struct sb_rs_request){SB_RS_MAGIC, length, range.offset};
            if (send_all(fd, &request, sizeof(request)) ||
                receive_all(fd, client->destination + range.offset, length, 0) != 1)
                goto fail;
            range.offset += length;
            range.length -= length;
            bytes += length;
            in_session += length;
            if (bytes - last_reported >= 256ULL * 1024 * 1024) {
                pr_info("SB_RSOCKET target_progress bytes=%llu\n",
                        (unsigned long long)bytes);
                last_reported = bytes;
            }
            if (in_session == SB_RS_SESSION) {
                if (finish_session(fd)) goto fail;
                pr_info("SB_RSOCKET target_session bytes=%llu\n",
                        (unsigned long long)in_session);
                rclose(fd);
                fd = -1;
                in_session = 0;
            }
        }
    }
    reusable_fds[job->slot] = fd;
    pr_info("SB_RSOCKET target_reusable slot=%u bytes=%llu fd=%d\n",
            job->slot, (unsigned long long)bytes, fd);
    pthread_mutex_lock(&client->lock);
    client->bytes += bytes;
    pthread_mutex_unlock(&client->lock);
    return NULL;
fail:
    if (fd >= 0) rclose(fd);
    reusable_fds[job->slot] = -1;
    pthread_mutex_lock(&client->lock);
    client->error = 1;
    pthread_mutex_unlock(&client->lock);
    return NULL;
}

int sb_rsocket_snapshot_read(void *destination, uint64_t length,
                            const char *source_ip, int port,
                            const struct sb_rsocket_range *ranges, size_t count,
                            unsigned workers, uint64_t *payload_bytes)
{
    struct sb_rs_client client = {
        .destination = destination, .length = length, .source_ip = source_ip,
        .port = port, .ranges = ranges, .count = count,
        .lock = PTHREAD_MUTEX_INITIALIZER,
    };
    pthread_t threads[SB_RS_WORKERS];
    struct sb_rs_job jobs[SB_RS_WORKERS];
    unsigned started = 0;
    int result;
    if (!destination || !length || !source_ip || port < 1 || port > 65535 ||
        (!ranges && count) || !workers || workers > SB_RS_WORKERS) return -1;
    if (!reusable_port) {
        if (strlen(source_ip) >= sizeof(reusable_ip)) return -1;
        strcpy(reusable_ip, source_ip);
        reusable_port = port;
    } else if (reusable_port != port || strcmp(reusable_ip, source_ip)) return -1;
    for (size_t i = 0; i < count; i++)
        if (!ranges[i].length || ranges[i].offset > length ||
            ranges[i].length > length - ranges[i].offset) return -1;
    if (workers > count) workers = count;
    for (unsigned i = 0; i < workers; i++) {
        jobs[i] = (struct sb_rs_job){&client, i};
        if (pthread_create(&threads[i], NULL, read_worker, &jobs[i])) {
            pthread_mutex_lock(&client.lock);
            client.error = 1;
            pthread_mutex_unlock(&client.lock);
            break;
        }
        started++;
    }
    for (unsigned i = 0; i < started; i++) pthread_join(threads[i], NULL);
    result = client.error ? -1 : 0;
    if (payload_bytes) *payload_bytes = client.bytes;
    pthread_mutex_destroy(&client.lock);
    return result;
}
