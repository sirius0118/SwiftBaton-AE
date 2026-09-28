/* End-to-end transport probe for the K rsocket proxy, using one immutable
 * source page. This deliberately does not claim to test CRIU/PTE integration. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <rdma/rsocket.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include "sb-kernel-rsocket-proxy.h"

void print_on_level(unsigned int level, const char *fmt, ...)
{
    va_list ap;
    (void)level;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

#define PAGE 4096
#define MAGIC UINT32_C(0x53425058)
struct req { uint32_t magic, pid; uint64_t address, pages, index, sequence; };
struct reply { uint32_t magic; int32_t status; uint64_t sequence; };
static int send_all(int fd, const void *p, size_t n)
{
    while (n) {
        ssize_t x = rsend(fd, p, n, 0);
        if (x < 0 && errno == EINTR) continue;
        if (x <= 0) return -1;
        p = (const char *)p + x; n -= x;
    }
    return 0;
}
static int recv_all(int fd, void *p, size_t n)
{
    while (n) {
        ssize_t x = rrecv(fd, p, n, 0);
        if (x < 0 && errno == EINTR) continue;
        if (x <= 0) return -1;
        p = (char *)p + x; n -= x;
    }
    return 0;
}
static void die(const char *s) { perror(s); exit(1); }
int main(int argc, char **argv)
{
    struct sockaddr_in a = {.sin_family = AF_INET};
    int port, fd;
    if (argc == 2 && !strcmp(argv[1], "holder")) {
        uint8_t *page;
        if (posix_memalign((void **)&page, PAGE, PAGE)) die("alloc holder");
        for (unsigned i = 0; i < PAGE; i++) page[i] = (uint8_t)((i * 17 + 13) % 251);
        printf("HOLDER_READY pid=%d base=%" PRIuPTR "\n", getpid(), (uintptr_t)page);
        fflush(stdout);
        sleep(30);
        free(page);
        return 0;
    }
    if (argc < 4) return 2;
    port = atoi(argv[3]);
    if (port < 1 || port > 65535 || inet_pton(AF_INET, argv[2], &a.sin_addr) != 1)
        return 2;
    a.sin_port = htons(port);
    if (!strcmp(argv[1], "source")) {
        uint8_t *page;
        struct sbk_catalog_final region = {};
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a))) die("bind control");
        if (posix_memalign((void **)&page, PAGE, PAGE)) die("alloc");
        for (unsigned i = 0; i < PAGE; i++) page[i] = (uint8_t)((i * 17 + 13) % 251);
        region.record.source_pid = argc == 6 ? (uint32_t)strtoul(argv[4], NULL, 10) : (uint32_t)getpid();
        region.record.remote.address = argc == 6 ? strtoull(argv[5], NULL, 10) : (uintptr_t)page;
        region.record.remote.pages = 1;
        if (sbk_rsocket_source_start(fd, port, &region, 1, 1)) die("start source");
        printf("SOURCE_READY pid=%u base=%" PRIu64 "\n",
               region.record.source_pid, (uint64_t)region.record.remote.address);
        fflush(stdout);
        sleep(15);
        sbk_rsocket_source_stop();
        close(fd); free(page);
        puts("SOURCE_DONE");
        return 0;
    }
    if (strcmp(argv[1], "target") || argc != 6) return 2;
    uint32_t pid = (uint32_t)strtoul(argv[4], NULL, 10);
    uint64_t base = strtoull(argv[5], NULL, 10);
    uint8_t *page;
    if (posix_memalign((void **)&page, PAGE, PAGE)) die("alloc");
    memset(page, 0, PAGE);
    fd = rsocket(AF_INET, SOCK_STREAM, 0);
    int maps = 1, queue = 512, bytes = 8 * 1024 * 1024;
    if (fd < 0 ||
        rsetsockopt(fd, SOL_RDMA, RDMA_IOMAPSIZE, &maps, sizeof(maps)) ||
        rsetsockopt(fd, SOL_RDMA, RDMA_RQSIZE, &queue, sizeof(queue)) ||
        rsetsockopt(fd, SOL_RDMA, RDMA_SQSIZE, &queue, sizeof(queue)) ||
        rsetsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) ||
        rsetsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) ||
        rconnect(fd, (struct sockaddr *)&a, sizeof(a))) die("connect");
    off_t offset = riomap(fd, page, PAGE, PROT_WRITE, 0, -1);
    if (offset == (off_t)-1) die("riomap");
    uint64_t wire_offset = offset;
    if (send_all(fd, &wire_offset, sizeof(wire_offset))) die("send map");
    for (uint64_t i = 1; i <= 20000; i++) {
        struct req r = {.magic=MAGIC, .pid=pid, .address=base,
                        .pages=1, .index=0, .sequence=i};
        struct reply reply;
        if (send_all(fd, &r, sizeof(r)) || recv_all(fd, &reply, sizeof(reply)) ||
            reply.magic != MAGIC || reply.sequence != i || reply.status)
            die("fetch");
        for (unsigned j = 0; j < PAGE; j++)
            if (page[j] != (uint8_t)((j * 17 + 13) % 251)) die("page mismatch");
    }
    printf("RSOCKET_PROXY_TRANSPORT_PASS pages=20000 bytes=%u\n", 20000 * PAGE);
    fflush(stdout);
    fprintf(stderr, "TARGET_CLEANUP_START\n");
    if (riounmap(fd, page, PAGE)) perror("riounmap");
    fprintf(stderr, "TARGET_CLEANUP_UNMAPPED\n");
    rshutdown(fd, SHUT_RDWR);
    rclose(fd); free(page);
    return 0;
}
