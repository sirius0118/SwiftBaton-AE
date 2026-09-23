/* Linux built-in idle-page and soft-dirty sampling. No custom module ioctl. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "sb-heat.h"
#define PAGE 4096ULL
#define PRESENT (UINT64_C(1) << 63)
#define DIRTY (UINT64_C(1) << 55)
#define PFN_MASK ((UINT64_C(1) << 55) - 1)
#define CHUNK 1024
struct tracked { struct sb_heat_page page; uint64_t pfn; size_t process, word; };
struct idle_word { uint64_t offset, bits, observed; };
static int read_at(int fd, void *buffer, size_t length, uint64_t offset)
{
    size_t done = 0;
    while (done < length) {
        ssize_t n = pread(fd, (char *)buffer + done, length - done, offset + done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        done += n;
    }
    return 0;
}
static int track_process(pid_t pid, size_t process, int pm, struct tracked **items, size_t *used, size_t *capacity)
{
    char path[64], line[2048], permissions[5];
    unsigned long long start, end, inode, offset;
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *maps = fopen(path, "re");
    if (!maps) return -1;
    while (fgets(line, sizeof(line), maps)) {
        if (sscanf(line, "%llx-%llx %4s %llx %*s %llu", &start, &end, permissions, &offset, &inode) != 5) goto error;
        if (permissions[0] != 'r' || permissions[3] != 'p' || inode || end <= start) continue;
        for (uint64_t address = start; address < end;) {
            uint64_t entries[CHUNK];
            size_t n = (end - address) / PAGE;
            if (n > CHUNK) n = CHUNK;
            if (!n || read_at(pm, entries, n * sizeof(*entries), address / PAGE * 8)) goto error;
            for (size_t j = 0; j < n; j++) {
                if (!(entries[j] & PRESENT) || !(entries[j] & PFN_MASK)) continue;
                if (*used == *capacity) {
                    size_t next = *capacity ? *capacity * 2 : 4096;
                    if (next > SIZE_MAX / sizeof(**items)) goto error;
                    void *p = realloc(*items, next * sizeof(**items));
                    if (!p) goto error;
                    *items = p; *capacity = next;
                }
                (*items)[(*used)++] = (struct tracked){ .page = {.pid = pid, .address = address + j * PAGE},
                    .pfn = entries[j] & PFN_MASK, .process = process, .word = SIZE_MAX };
            }
            address += n * PAGE;
        }
    }
    if (ferror(maps)) goto error;
    fclose(maps); return 0;
error:
    fclose(maps); return -1;
}
static int by_pfn(const void *a, const void *b)
{
    const struct tracked *x = *(struct tracked *const *)a, *y = *(struct tracked *const *)b;
    return x->pfn < y->pfn ? -1 : x->pfn > y->pfn;
}
int sb_heat_collect(const pid_t *pids, size_t count, unsigned rounds, unsigned interval_us,
                    struct sb_heat_page **pages, size_t *nr_pages)
{
    struct tracked *items = NULL, **ordered = NULL;
    struct idle_word *words = NULL;
    int idle = -1, flags_fd = -1, *pm = NULL, result = -1;
    size_t used = 0, capacity = 0;
    if (!pages || !nr_pages || !count || count > 100 || !rounds || rounds > 64 || interval_us > 1000000) { errno = EINVAL; return -1; }
    *pages = NULL; *nr_pages = 0;
    pm = malloc(count * sizeof(*pm));
    if (!pm) goto out;
    for (size_t p = 0; p < count; p++) pm[p] = -1;
    idle = open("/sys/kernel/mm/page_idle/bitmap", O_RDWR | O_CLOEXEC);
    flags_fd = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
    if (idle < 0 || flags_fd < 0) goto out;
    for (size_t p = 0; p < count; p++) {
        char path[64]; snprintf(path, sizeof(path), "/proc/%d/pagemap", pids[p]);
        pm[p] = open(path, O_RDONLY | O_CLOEXEC);
        if (pm[p] < 0 || track_process(pids[p], p, pm[p], &items, &used, &capacity)) goto out;
    }
    ordered = malloc((used ? used : 1) * sizeof(*ordered));
    words = calloc(used ? used : 1, sizeof(*words));
    if (!ordered || !words) goto out;
    for (size_t i = 0; i < used; i++) ordered[i] = &items[i];
    for (unsigned round = 0; round < rounds; round++) {
        size_t nw = 0;
        uint64_t flags_buffer[CHUNK], flags_begin = 0;
        size_t flags_count = 0;
        qsort(ordered, used, sizeof(*ordered), by_pfn);
        for (size_t i = 0; i < used; i++) {
            struct tracked *p = ordered[i];
            uint64_t off = p->pfn / 64 * 8;
            p->word = SIZE_MAX;
            if (!p->pfn) continue;
            if (p->pfn < flags_begin || p->pfn - flags_begin >= flags_count) {
                uint64_t end = p->pfn;
                for (size_t j = i + 1; j < used && ordered[j]->pfn - p->pfn < CHUNK; j++) end = ordered[j]->pfn;
                flags_begin = p->pfn; flags_count = end - p->pfn + 1;
                if (read_at(flags_fd, flags_buffer, flags_count * sizeof(uint64_t), flags_begin * 8)) goto out;
            }
            uint64_t flags = flags_buffer[p->pfn - flags_begin];
            /* Huge-page tails and non-LRU pages have no reliable per-4K idle
             * observation. Do not fabricate access counts for them. */
            if (!(flags & (UINT64_C(1) << 5)) || (flags & (UINT64_C(1) << 16))) continue;
            if (!nw || words[nw - 1].offset != off) words[nw++] = (struct idle_word){.offset = off};
            p->word = nw - 1; words[nw - 1].bits |= UINT64_C(1) << (p->pfn % 64);
        }
        for (size_t w = 0; w < nw; w++)
            if (pwrite(idle, &words[w].bits, sizeof(uint64_t), words[w].offset) != sizeof(uint64_t)) goto out;
        for (size_t p = 0; p < count; p++) {
            char path[64]; snprintf(path, sizeof(path), "/proc/%d/clear_refs", pids[p]);
            int fd = open(path, O_WRONLY | O_CLOEXEC);
            if (fd < 0) goto out;
            ssize_t n = write(fd, "4\n", 2); close(fd);
            if (n != 2) goto out;
        }
        struct timespec delay = {.tv_sec = interval_us / 1000000, .tv_nsec = (interval_us % 1000000) * 1000};
        while (nanosleep(&delay, &delay)) if (errno != EINTR) goto out;
        for (size_t w = 0; w < nw; w++)
            if (read_at(idle, &words[w].observed, sizeof(uint64_t), words[w].offset)) goto out;
        for (size_t i = 0; i < used;) {
            uint64_t entries[CHUNK];
            size_t n = 1, process = items[i].process;
            while (n < CHUNK && i + n < used && items[i + n].process == process &&
                   items[i + n].page.address == items[i].page.address + n * PAGE) n++;
            if (read_at(pm[process], entries, n * sizeof(*entries), items[i].page.address / PAGE * 8)) goto out;
            for (size_t j = 0; j < n; j++) {
                struct tracked *p = &items[i + j];
                uint64_t current = entries[j] & PRESENT ? entries[j] & PFN_MASK : 0;
                bool stable = current && current == p->pfn;
                if (stable && p->word != SIZE_MAX) {
                    p->page.observations++;
                    p->page.accesses += !(words[p->word].observed & (UINT64_C(1) << (current % 64)));
                }
                p->page.writes += !stable || !!(entries[j] & DIRTY);
                p->pfn = current;
            }
            i += n;
        }
    }
    *pages = malloc((used ? used : 1) * sizeof(**pages));
    if (!*pages) goto out;
    for (size_t i = 0; i < used; i++) (*pages)[i] = items[i].page;
    *nr_pages = used; result = 0;
out:
    if (pm) for (size_t p = 0; p < count; p++) if (pm[p] >= 0) close(pm[p]);
    if (idle >= 0) close(idle);
    if (flags_fd >= 0) close(flags_fd);
    free(pm); free(items); free(ordered); free(words);
    return result;
}
