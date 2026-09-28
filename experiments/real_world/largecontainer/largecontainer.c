#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct state {
    uint8_t *data;
    size_t bytes, range, slots;
    double *cdf, write_ratio;
    unsigned sleep_us, max_iops;
    atomic_ullong operations;
};

static uint64_t next(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return *s = x;
}

static size_t choose(struct state *st, double u) {
    size_t lo = 0, hi = st->slots;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (st->cdf[mid] < u) lo = mid + 1;
        else hi = mid;
    }
    return lo < st->slots ? lo : st->slots - 1;
}

static void *worker(void *arg) {
    struct state *st = arg;
    uint64_t rng = ((uint64_t)(uintptr_t)pthread_self() << 1) ^ (uint64_t)time(NULL) ^ 0x9e3779b97f4a7c15ULL;
    unsigned delay = st->sleep_us;
    if (st->max_iops && delay < 1000000U / st->max_iops)
        delay = 1000000U / st->max_iops;
    struct timespec pause = {.tv_sec = delay / 1000000U, .tv_nsec = (delay % 1000000U) * 1000U};
    volatile uint64_t sink = 0;
    for (;;) {
        double u = (next(&rng) >> 11) * (1.0 / 9007199254740992.0);
        size_t slot = choose(st, u);
        uint8_t *p = st->data + slot * st->range;
        int write = ((next(&rng) >> 11) * (1.0 / 9007199254740992.0)) < st->write_ratio;
        for (size_t at = 0; at < st->range; at += 64) {
            if (write) p[at] += 1;
            else sink += p[at];
        }
        atomic_fetch_add_explicit(&st->operations, 1, memory_order_relaxed);
        if (delay) nanosleep(&pause, NULL);
    }
    return (void *)(uintptr_t)sink;
}

static unsigned long parse(const char *s) {
    char *end;
    errno = 0;
    unsigned long x = strtoul(s, &end, 10);
    if (errno || !*s || *end) { fprintf(stderr, "bad integer: %s\n", s); exit(2); }
    return x;
}

int main(int argc, char **argv) {
    unsigned mib = 4096, threads = 32, range_kib = 16, sleep_us = 10, max_iops = 100000;
    double zipf = .99, write_ratio = .5;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 2; }
        if (!strcmp(argv[i], "--mib")) mib = parse(argv[i + 1]);
        else if (!strcmp(argv[i], "--threads")) threads = parse(argv[i + 1]);
        else if (!strcmp(argv[i], "--range-kib")) range_kib = parse(argv[i + 1]);
        else if (!strcmp(argv[i], "--sleep-us")) sleep_us = parse(argv[i + 1]);
        else if (!strcmp(argv[i], "--max-iops")) max_iops = parse(argv[i + 1]);
        else if (!strcmp(argv[i], "--zipf")) zipf = strtod(argv[i + 1], NULL);
        else if (!strcmp(argv[i], "--write-ratio")) write_ratio = strtod(argv[i + 1], NULL);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!mib || !threads || threads > 256 || !range_kib || !(zipf > 0 && zipf < 2) || !(write_ratio >= 0 && write_ratio <= 1)) return 2;
    struct state st = {.bytes = (size_t)mib << 20, .range = (size_t)range_kib << 10,
                       .write_ratio = write_ratio, .sleep_us = sleep_us, .max_iops = max_iops};
    st.slots = st.bytes / st.range;
    if (!st.slots) return 2;
    st.data = mmap(NULL, st.bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (st.data == MAP_FAILED) { perror("mmap"); return 1; }
    st.cdf = malloc(st.slots * sizeof(double));
    if (!st.cdf) { perror("malloc"); return 1; }
    double sum = 0;
    for (size_t i = 0; i < st.slots; ++i) sum += 1.0 / pow((double)(i + 1), zipf);
    double cumulative = 0;
    for (size_t i = 0; i < st.slots; ++i) {
        cumulative += (1.0 / pow((double)(i + 1), zipf)) / sum;
        st.cdf[i] = cumulative;
    }
    st.cdf[st.slots - 1] = 1.0;
    // Commit every page before declaring readiness; the entire footprint is
    // present during the pre-transfer phase.
    for (size_t i = 0; i < st.bytes; i += 4096) st.data[i] = (uint8_t)(i >> 12);
    pthread_t *workers = calloc(threads, sizeof(*workers));
    if (!workers) return 1;
    for (unsigned i = 0; i < threads; ++i)
        if (pthread_create(&workers[i], NULL, worker, &st)) { perror("pthread_create"); return 1; }
    int server = socket(AF_INET, SOCK_STREAM, 0), reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(6379), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(server, (void *)&addr, sizeof(addr)) || listen(server, 128)) { perror("listen"); return 1; }
    fprintf(stderr, "largecontainer ready mib=%u threads=%u range_kib=%u zipf=%g write_ratio=%g sleep_us=%u max_iops=%u\n",
            mib, threads, range_kib, zipf, write_ratio, sleep_us, max_iops);
    for (;;) {
        int fd = accept(server, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("accept"); return 1; }
        char request[32] = {0}, answer[64];
        ssize_t n = recv(fd, request, sizeof(request) - 1, 0);
        if (n > 0) {
            if (!strncmp(request, "PING", 4)) strcpy(answer, "PONG\n");
            else if (!strncmp(request, "STATS", 5))
                snprintf(answer, sizeof(answer), "%llu\n", atomic_load_explicit(&st.operations, memory_order_relaxed));
            else strcpy(answer, "ERR\n");
            send(fd, answer, strlen(answer), MSG_NOSIGNAL);
        }
        close(fd);
    }
}
