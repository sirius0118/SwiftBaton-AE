/* Real PS snapshots, conservative soft-dirty invalidation and a local fault
 * cache. Sampling ranks candidates; only this final validator authorizes reuse.
 * No custom kernel/PTE manipulation is used for the correctness decision. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "log.h"
#include "sb-precopy.h"

#define SB_PAGE 4096ULL
#define SB_MAGIC 0x5342505245434f50ULL
#define SB_VALID_MAGIC (SB_MAGIC + 1)
#define SB_VERSION 3
#define SB_IMAGE "sb-precopy.img"
#define SB_PS_IMAGE "sb-precopy-ps.img"
#define SB_COPY_BATCH 128
#define SB_SCAN_BATCH 4096
#define SB_MAX_WORKERS 32
#define SB_PFN_MASK ((1ULL << 55) - 1)

struct cache_header { uint64_t magic, version, nonce, pages, data_offset, bytes; };
struct valid_header { uint64_t magic, version, nonce, pages, pids, bitmap_bytes; };
struct map {
    uint64_t start, end, offset, inode;
    unsigned major, minor;
    unsigned noreserve;
    char permissions[5], kind;
};
struct process {
    pid_t pid;
    uint64_t starttime;
    struct map *maps;
    size_t count;
    struct process *next;
};
static struct process *processes;
static struct cache_header *source;
static size_t source_next;
static unsigned char *source_allowed;
static bool trace_reasons;

void sb_precopy_trace_reasons(int enabled) { trace_reasons = !!enabled; }

static int page_compare(const void *left, const void *right)
{
    const struct sb_precopy_page *a = left, *b = right;
    if (a->pid != b->pid) return a->pid < b->pid ? -1 : 1;
    return a->address < b->address ? -1 : a->address != b->address;
}

static int read_maps(pid_t pid, struct map **maps, size_t *count)
{
    char path[64], *line = NULL;
    size_t capacity = 0, used = 0, line_size = 0;
    struct map *result = NULL;
    FILE *file;
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    file = fopen(path, "re");
    if (!file) return -1;
    while (getline(&line, &line_size, file) > 0) {
        struct map m = {0};
        unsigned long long begin, end, offset, inode;
        int position = 0;
        char *name;
        if (sscanf(line, "%llx-%llx %4s %llx %x:%x %llu %n", &begin, &end,
                   m.permissions, &offset, &m.major, &m.minor, &inode, &position) != 7)
            goto fail;
        m.start = begin; m.end = end; m.offset = offset; m.inode = inode;
        name = line + position;
        while (*name == ' ' || *name == '\t') name++;
        if (*name == '\0' || *name == '\n') m.kind = 'a';
        else if (!strncmp(name, "[heap]", 6)) m.kind = 'h';
        else if (!strncmp(name, "[stack", 6)) m.kind = 's';
        else if (!strncmp(name, "[anon:", 6)) m.kind = 'a';
        if (used == capacity) {
            struct map *next;
            capacity = capacity ? capacity * 2 : 64;
            next = realloc(result, capacity * sizeof(*result));
            if (!next) goto fail;
            result = next;
        }
        result[used++] = m;
    }
    if (ferror(file)) goto fail;
    free(line); fclose(file); *maps = result; *count = used;
    return 0;
fail:
    free(line); free(result); fclose(file);
    return -1;
}

static struct map *find_map(struct map *maps, size_t count, uint64_t address)
{
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (address < maps[middle].start) high = middle;
        else if (address >= maps[middle].end) low = middle + 1;
        else return &maps[middle];
    }
    return NULL;
}

static bool same_map(const struct map *a, const struct map *b)
{
    return a && b && a->start == b->start && a->end == b->end &&
        a->offset == b->offset && a->inode == b->inode && a->major == b->major &&
        a->minor == b->minor && a->kind == b->kind &&
        !strcmp(a->permissions, b->permissions);
}

static uint64_t process_starttime(pid_t pid)
{
    char path[64], text[4096], *last, *save, *value;
    FILE *file;
    uint64_t result = 0;
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    file = fopen(path, "re");
    if (!file) return 0;
    if (fgets(text, sizeof(text), file) && (last = strrchr(text, ')'))) {
        value = strtok_r(last + 1, " ", &save);
        for (int field = 3; value && field <= 22; field++, value = strtok_r(NULL, " ", &save))
            if (field == 22) result = strtoull(value, NULL, 10);
    }
    fclose(file);
    return result;
}

static struct process *prepare_process(pid_t pid)
{
    struct process *p;
    char path[64];
    int fd;
    for (p = processes; p; p = p->next)
        if (p->pid == pid) return p;
    p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->pid = pid;
    p->starttime = process_starttime(pid);
    if (!p->starttime || read_maps(pid, &p->maps, &p->count)) goto fail;
    /* Reservation policy is needed when constructing anonymous destination
     * VMAs. Read it in PS; final restore flags are checked again at adoption. */
    {
        FILE *smaps;
        struct map *current = NULL;
        char line[1024];
        snprintf(path, sizeof(path), "/proc/%d/smaps", pid);
        smaps = fopen(path, "re");
        if (!smaps) goto fail;
        while (fgets(line, sizeof(line), smaps)) {
            unsigned long long begin, end;
            if (sscanf(line, "%llx-%llx", &begin, &end) == 2) {
                current = find_map(p->maps, p->count, begin);
                if (current && (current->start != begin || current->end != end)) current = NULL;
            } else if (current && !strncmp(line, "VmFlags:", 8)) {
                char *save, *word = strtok_r(line + 8, " \t\n", &save);
                for (; word; word = strtok_r(NULL, " \t\n", &save))
                    if (!strcmp(word, "nr")) current->noreserve = 1;
            }
        }
        if (ferror(smaps)) { fclose(smaps); goto fail; }
        fclose(smaps);
    }
    snprintf(path, sizeof(path), "/proc/%d/clear_refs", pid);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) goto fail;
    /* Kernel write-protects the PTEs and performs the required TLB flush.
     * All writes from before the first copied byte through IS are covered. */
    if (write(fd, "4\n", 2) != 2) { close(fd); goto fail; }
    close(fd);
    p->next = processes; processes = p;
    return p;
fail:
    free(p->maps); free(p);
    return NULL;
}

static void *source_copy_worker(void *unused)
{
    struct sb_precopy_page *pages = (void *)(source + 1);
    size_t begin;
    (void)unused;
    while ((begin = __atomic_fetch_add(&source_next, SB_COPY_BATCH, __ATOMIC_RELAXED)) < source->pages) {
        size_t end = begin + SB_COPY_BATCH;
        if (end > source->pages) end = source->pages;
        while (begin < end) {
            struct iovec local[SB_COPY_BATCH], remote[SB_COPY_BATCH];
            size_t count = 0;
            pid_t pid = pages[begin].pid;
            ssize_t bytes;
            int pagemap;
            char path[64];
            while (begin + count < end && pages[begin + count].pid == (uint32_t)pid) {
                local[count].iov_base = (char *)source + source->data_offset + (begin + count) * SB_PAGE;
                remote[count].iov_base = (void *)pages[begin + count].address;
                local[count].iov_len = remote[count].iov_len = SB_PAGE;
                count++;
            }
            /* Soft-dirty alone does not cover discarding an anonymous page
             * and faulting the shared zero page back in. Also require the
             * final PFN to equal the resident PFN from before this read. */
            snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
            pagemap = open(path, O_RDONLY | O_CLOEXEC);
            for (size_t k = 0; pagemap >= 0 && k < count;) {
                uint64_t entries[SB_SCAN_BATCH], first = pages[begin + k].address;
                size_t end = k + 1, span;
                while (end < count && (pages[begin + end].address - first) / SB_PAGE < SB_SCAN_BATCH) end++;
                span = (pages[begin + end - 1].address - first) / SB_PAGE + 1;
                do { bytes = pread(pagemap, entries, span * sizeof(uint64_t), (first / SB_PAGE) * sizeof(uint64_t)); }
                while (bytes < 0 && errno == EINTR);
                if (bytes == (ssize_t)(span * sizeof(uint64_t)))
                    for (size_t j = k; j < end; j++) {
                        uint64_t entry = entries[(pages[begin + j].address - first) / SB_PAGE];
                        if ((entry & (1ULL << 63)) && !(entry & (1ULL << 62)))
                            pages[begin + j].pfn = entry & SB_PFN_MASK;
                    }
                k = end;
            }
            if (pagemap >= 0) close(pagemap);
            do { bytes = process_vm_readv(pid, local, count, remote, count, 0); }
            while (bytes < 0 && errno == EINTR);
            if (bytes > 0)
                for (size_t j = 0; j < (size_t)bytes / SB_PAGE; j++) pages[begin + j].copied = 1;
            /* Partial/unreadable pages are invalid, never silently reused. */
            begin += count;
        }
    }
    return NULL;
}

int sb_precopy_build(struct sb_precopy_page *candidates, size_t count,
                     uint64_t limit_bytes, unsigned workers, void **buffer, uint64_t *length)
{
    pthread_t threads[SB_MAX_WORKERS];
    size_t kept = 0, maximum, copied = 0;
    uint64_t offset, bytes;
    if (source || !workers || workers > SB_MAX_WORKERS || limit_bytes < 2 * SB_PAGE) return -1;
    maximum = (limit_bytes - SB_PAGE) / (SB_PAGE + sizeof(*candidates));
    if (count && !candidates) return -1;
    if (count) qsort(candidates, count, sizeof(*candidates), page_compare);
    for (size_t i = 0; i < count && kept < maximum; i++) {
        struct process *p;
        struct map *m;
        if (!candidates[i].pid || (candidates[i].address & (SB_PAGE - 1))) continue;
        if (kept && !page_compare(&candidates[i], &candidates[kept - 1])) continue;
        p = prepare_process(candidates[i].pid);
        if (!p) { pr_perror("Prepare soft-dirty epoch"); return -1; }
        m = find_map(p->maps, p->count, candidates[i].address);
        if (!m || !m->kind || m->inode || m->permissions[0] != 'r' || m->permissions[3] != 'p') continue;
        candidates[kept] = candidates[i];
        candidates[kept].region_start = m->start;
        candidates[kept].region_end = m->end;
        candidates[kept].region_flags = MAP_PRIVATE | MAP_ANONYMOUS | (m->noreserve ? MAP_NORESERVE : 0);
        candidates[kept].copied = 0; candidates[kept++].pfn = 0;
    }
    offset = (sizeof(*source) + kept * sizeof(*candidates) + SB_PAGE - 1) & ~(SB_PAGE - 1);
    bytes = offset + kept * SB_PAGE;
    source = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (source == MAP_FAILED) { source = NULL; return -1; }
    if (madvise(source, bytes, MADV_DONTFORK)) return -1;
    *source = (struct cache_header){ .magic = SB_MAGIC, .version = SB_VERSION,
        .pages = kept, .data_offset = offset, .bytes = bytes };
    if (getrandom(&source->nonce, sizeof(source->nonce), 0) != sizeof(source->nonce)) return -1;
    if (kept) memcpy(source + 1, candidates, kept * sizeof(*candidates));
    if (!kept) workers = 0; /* Empty control snapshot: no source application pages copied. */
    for (unsigned i = 0; i < workers; i++) {
        if (pthread_create(&threads[i], NULL, source_copy_worker, NULL)) {
            for (unsigned j = 0; j < i; j++) pthread_join(threads[j], NULL);
            return -1;
        }
    }
    for (unsigned i = 0; i < workers; i++) pthread_join(threads[i], NULL);
    for (size_t i = 0; i < kept; i++) copied += ((struct sb_precopy_page *)(source + 1))[i].copied;
    pr_info("SB_PRECOPY snapshot candidates=%zu copied=%zu bytes=%llu workers=%u\n",
            kept, copied, (unsigned long long)bytes, workers);
    *buffer = source; *length = bytes;
    return 0;
}

static int full_io(int fd, void *buffer, size_t size, bool writing)
{
    char *p = buffer;
    while (size) {
        ssize_t n = writing ? write(fd, p, size) : read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; size -= n;
    }
    return 0;
}

static int destination_pid(const struct sb_precopy_pid *pids, size_t count, pid_t pid)
{
    for (size_t i = 0; i < count; i++) if (pids[i].source == pid) return pids[i].destination;
    return 0;
}

/* The immutable snapshot is sorted by (pid, address) by sb_precopy_build().
 * Each process starts at its own range, including processes with no retained
 * candidates. Do not rescan every preceding process's pages in IS. */
static size_t source_pid_begin(const struct sb_precopy_page *pages, size_t count, uint32_t pid)
{
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (pages[middle].pid < pid) low = middle + 1;
        else high = middle;
    }
    return low;
}

/* Only used while walking one process's sorted candidate addresses. Both map
 * arrays remain immutable for the duration of this validation pass. */
static struct map *next_map(struct map *maps, size_t count, uint64_t address, size_t *cursor)
{
    while (*cursor < count && maps[*cursor].end <= address) (*cursor)++;
    if (*cursor == count || address < maps[*cursor].start) return NULL;
    return &maps[*cursor];
}

static uint64_t precopy_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

#define SB_VALIDATE_BATCH 8192
struct validate_process {
    struct process *original;
    struct map *maps;
    size_t map_count;
    int fd;
};
struct validate_job { struct validate_process *process; size_t begin, end; };
struct validate_context {
    struct validate_job *jobs;
    size_t job_count, next;
    unsigned char *valid;
    sb_precopy_eligible_fn eligible;
    int error;
};
struct validate_worker {
    struct validate_context *context;
    uint64_t accepted, pagemap_ns, scan_ns, reads, scanned;
    /* Private counters, aggregated only after all validator threads join. */
    uint64_t prior, page_state, not_copied, no_snapshot_pfn, absent, swapped;
    uint64_t dirty, pfn_changed, clean_pfn_changed, map_changed, ineligible;
} __attribute__((aligned(64)));

static void record_page_rejection(struct validate_worker *w,
                                  const struct sb_precopy_page *page, uint64_t entry)
{
    bool present = !!(entry & (1ULL << 63)), swapped = !!(entry & (1ULL << 62));
    bool dirty = !!(entry & (1ULL << 55));
    bool changed = (entry & SB_PFN_MASK) != page->pfn;
    w->page_state++;
    w->not_copied += !page->copied;
    w->no_snapshot_pfn += !page->pfn;
    w->absent += !present;
    w->swapped += swapped;
    w->dirty += dirty;
    w->pfn_changed += changed;
    /* This identifies an unchanged soft-dirty bit with a changed PFN, not
     * proof of NUMA migration or proof that the page contents stayed equal. */
    w->clean_pfn_changed += page->copied && page->pfn && present && !swapped && !dirty && changed;
}

static void *source_validate_worker(void *argument)
{
    struct validate_worker *worker = argument;
    struct validate_context *context = worker->context;
    const struct sb_precopy_page *pages = (void *)(source + 1);
    uint64_t entries[SB_SCAN_BATCH];
    size_t index;
    while (!__atomic_load_n(&context->error, __ATOMIC_RELAXED) &&
           (index = __atomic_fetch_add(&context->next, 1, __ATOMIC_RELAXED)) < context->job_count) {
        const struct validate_job *job = &context->jobs[index];
        struct validate_process *v = job->process;
        struct process *p = v->original;
        struct map *last_old = NULL, *last_new = NULL;
        size_t old_cursor = 0, new_cursor = 0, begin = job->begin;
        bool matches = false;
        while (begin < job->end) {
            uint64_t first = pages[begin].address, tick;
            size_t end = begin + 1, span;
            ssize_t n;
            while (end < job->end && (pages[end].address - first) / SB_PAGE < SB_SCAN_BATCH) end++;
            span = (pages[end - 1].address - first) / SB_PAGE + 1;
            tick = precopy_now_ns();
            do { n = pread(v->fd, entries, span * sizeof(uint64_t), (first / SB_PAGE) * sizeof(uint64_t)); }
            while (n < 0 && errno == EINTR);
            worker->pagemap_ns += precopy_now_ns() - tick;
            worker->reads++;
            if (n != (ssize_t)(span * sizeof(uint64_t))) {
                __atomic_store_n(&context->error, n < 0 ? errno : EIO, __ATOMIC_RELAXED);
                return NULL;
            }
            tick = precopy_now_ns();
            for (size_t i = begin; i < end; i++) {
                uint64_t entry = entries[(pages[i].address - first) / SB_PAGE];
                struct map *old_map, *new_map;
                if (source_allowed && !(source_allowed[i / 8] & (1U << (i % 8)))) {
                    if (trace_reasons) worker->prior++;
                    continue;
                }
                if (!pages[i].copied || !pages[i].pfn || (entry & SB_PFN_MASK) != pages[i].pfn ||
                    !(entry & (1ULL << 63)) || (entry & ((1ULL << 55) | (1ULL << 62)))) {
                    if (trace_reasons) record_page_rejection(worker, &pages[i], entry);
                    continue;
                }
                old_map = next_map(p->maps, p->count, pages[i].address, &old_cursor);
                new_map = next_map(v->maps, v->map_count, pages[i].address, &new_cursor);
                if (old_map != last_old || new_map != last_new) {
                    matches = same_map(old_map, new_map);
                    last_old = old_map; last_new = new_map;
                }
                if (!matches) {
                    if (trace_reasons) worker->map_changed++;
                    continue;
                }
                if (!context->eligible(p->pid, pages[i].address)) {
                    if (trace_reasons) worker->ineligible++;
                    continue;
                }
                /* Adjacent process/job ranges can share a bitmap byte. */
                __atomic_fetch_or(&context->valid[i / 8], (unsigned char)(1U << (i % 8)), __ATOMIC_RELAXED);
                worker->accepted++;
            }
            worker->scan_ns += precopy_now_ns() - tick;
            worker->scanned += end - begin;
            begin = end;
        }
    }
    return NULL;
}

static int source_validate_parallel(const struct sb_precopy_pid *pids, size_t count,
                                    int image_dir_fd, sb_precopy_eligible_fn eligible,
                                    bool preliminary, unsigned workers)
{
    struct valid_header header;
    const struct sb_precopy_page *pages;
    struct validate_process *scans = NULL;
    struct validate_context context = { .eligible = eligible };
    struct validate_worker worker[SB_MAX_WORKERS] = {0};
    pthread_t threads[SB_MAX_WORKERS];
    size_t process_count = 0, prepared = 0, accepted = 0, started_threads = 0;
    uint64_t started = precopy_now_ns(), metadata_ns, pagemap_ns = 0, scan_ns = 0;
    uint64_t reads = 0, scanned = 0, write_begin, validation_ns;
    int out, result = -1;
    if (!source || count > 4096 || !eligible || !workers || workers > SB_MAX_WORKERS) return -1;
    pages = (void *)(source + 1);
    for (struct process *p = processes; p; p = p->next) if (++process_count > 4096) return -1;
    header = (struct valid_header){ .magic = SB_VALID_MAGIC, .version = SB_VERSION,
        .nonce = source->nonce, .pages = source->pages, .pids = count,
        .bitmap_bytes = (source->pages + 7) / 8 };
    context.valid = calloc(header.bitmap_bytes ? header.bitmap_bytes : 1, 1);
    scans = calloc(process_count ? process_count : 1, sizeof(*scans));
    context.jobs = calloc(source->pages / SB_VALIDATE_BATCH + process_count + 1, sizeof(*context.jobs));
    if (!context.valid || !scans || !context.jobs) goto done;
    for (struct process *p = processes; p; p = p->next) {
        struct validate_process *v;
        size_t begin, end;
        char path[64];
        if (!destination_pid(pids, count, p->pid) || process_starttime(p->pid) != p->starttime) continue;
        v = &scans[prepared++]; v->original = p; v->fd = -1;
        if (read_maps(p->pid, &v->maps, &v->map_count)) goto done;
        snprintf(path, sizeof(path), "/proc/%d/pagemap", p->pid);
        v->fd = open(path, O_RDONLY | O_CLOEXEC);
        if (v->fd < 0) goto done;
        begin = source_pid_begin(pages, source->pages, p->pid);
        end = source_pid_begin(pages, source->pages, (uint32_t)p->pid + 1);
        while (begin < end) {
            size_t limit = end - begin > SB_VALIDATE_BATCH ? begin + SB_VALIDATE_BATCH : end;
            context.jobs[context.job_count++] = (struct validate_job){ .process = v, .begin = begin, .end = limit };
            begin = limit;
        }
    }
    metadata_ns = precopy_now_ns() - started;
    if (workers > context.job_count) workers = context.job_count ? context.job_count : 1;
    for (unsigned i = 0; i < workers; i++) worker[i].context = &context;
    for (unsigned i = 1; i < workers; i++) {
        int error = pthread_create(&threads[started_threads], NULL, source_validate_worker, &worker[i]);
        if (error) { __atomic_store_n(&context.error, error, __ATOMIC_RELAXED); break; }
        started_threads++;
    }
    source_validate_worker(&worker[0]);
    for (size_t i = 0; i < started_threads; i++) pthread_join(threads[i], NULL);
    if (context.error) { errno = context.error; goto done; }
    validation_ns = precopy_now_ns() - started - metadata_ns;
    for (unsigned i = 0; i < workers; i++) {
        accepted += worker[i].accepted; pagemap_ns += worker[i].pagemap_ns;
        scan_ns += worker[i].scan_ns; reads += worker[i].reads; scanned += worker[i].scanned;
    }
    if (trace_reasons) {
        struct validate_worker total = {0};
        for (unsigned i = 0; i < workers; i++) {
#define SUM_REJECT(name) total.name += worker[i].name
            SUM_REJECT(prior); SUM_REJECT(page_state); SUM_REJECT(not_copied);
            SUM_REJECT(no_snapshot_pfn); SUM_REJECT(absent); SUM_REJECT(swapped);
            SUM_REJECT(dirty); SUM_REJECT(pfn_changed); SUM_REJECT(clean_pfn_changed);
            SUM_REJECT(map_changed); SUM_REJECT(ineligible);
#undef SUM_REJECT
        }
        if (scanned > source->pages || scanned != accepted + total.prior + total.page_state +
                total.map_changed + total.ineligible) { errno = EINVAL; goto done; }
        /* Top-level categories partition scanned pages. Individual page-state
         * flags overlap (a page may be both dirty and have changed PFN). */
        pr_info("SB_PRECOPY_REJECT phase=%s accepted=%zu scanned=%llu unscanned=%llu prior=%llu page_state=%llu not_copied=%llu no_snapshot_pfn=%llu absent=%llu swapped=%llu dirty=%llu pfn_changed=%llu clean_pfn_changed=%llu map_changed=%llu ineligible=%llu\n",
                preliminary ? "ps" : "final", accepted,
                (unsigned long long)scanned, (unsigned long long)(source->pages - scanned),
                (unsigned long long)total.prior, (unsigned long long)total.page_state,
                (unsigned long long)total.not_copied, (unsigned long long)total.no_snapshot_pfn,
                (unsigned long long)total.absent, (unsigned long long)total.swapped,
                (unsigned long long)total.dirty, (unsigned long long)total.pfn_changed,
                (unsigned long long)total.clean_pfn_changed, (unsigned long long)total.map_changed,
                (unsigned long long)total.ineligible);
    }
    write_begin = precopy_now_ns();
    out = openat(image_dir_fd, preliminary ? SB_PS_IMAGE : SB_IMAGE,
                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) goto done;
    result = full_io(out, &header, sizeof(header), true) ||
             full_io(out, (void *)pids, count * sizeof(*pids), true) ||
             full_io(out, context.valid, header.bitmap_bytes, true);
    if (close(out)) result = -1;
    if (preliminary && !result) {
        free(source_allowed); source_allowed = context.valid; context.valid = NULL;
    }
    pr_info("SB_PRECOPY %s valid=%zu invalid=%llu bitmap_bytes=%llu\n",
            preliminary ? "ps_pruned" : "finalized", accepted,
            (unsigned long long)(source->pages - accepted), (unsigned long long)header.bitmap_bytes);
    /* Worker durations are sums across concurrent workers, not elapsed time. */
    pr_info("SB_PRECOPY_TIMING phase=%s workers=%u jobs=%zu total_us=%llu metadata_us=%llu validation_us=%llu pagemap_worker_us=%llu scan_worker_us=%llu manifest_us=%llu preads=%llu scanned=%llu\n",
            preliminary ? "ps" : "final", workers, context.job_count,
            (unsigned long long)((precopy_now_ns() - started) / 1000), (unsigned long long)(metadata_ns / 1000),
            (unsigned long long)(validation_ns / 1000), (unsigned long long)(pagemap_ns / 1000),
            (unsigned long long)(scan_ns / 1000), (unsigned long long)((precopy_now_ns() - write_begin) / 1000),
            (unsigned long long)reads, (unsigned long long)scanned);
 done:
    if (result) pr_perror("Finalize pre-copy validity");
    for (size_t i = 0; i < prepared; i++) { if (scans[i].fd >= 0) close(scans[i].fd); free(scans[i].maps); }
    free(scans); free(context.jobs); free(context.valid);
    return result ? -1 : 0;
}

static int source_validate(const struct sb_precopy_pid *pids, size_t count,
                           int image_dir_fd, sb_precopy_eligible_fn eligible, bool preliminary)
{
    return source_validate_parallel(pids, count, image_dir_fd, eligible, preliminary, 1);
}

int sb_precopy_finalize_workers(const struct sb_precopy_pid *pids, size_t count,
                               int image_dir_fd, sb_precopy_eligible_fn eligible, unsigned workers)
{
    return source_validate_parallel(pids, count, image_dir_fd, eligible, false, workers);
}

int sb_precopy_finalize(const struct sb_precopy_pid *pids, size_t count,
                        int image_dir_fd, sb_precopy_eligible_fn eligible)
{
    return source_validate(pids, count, image_dir_fd, eligible, false);
}

static int preliminary_eligible(pid_t pid, uint64_t address)
{
    (void)pid; (void)address;
    return 1;
}

int sb_precopy_prune(int image_dir_fd)
{
    struct sb_precopy_pid *pids;
    size_t count = 0;
    int result;
    for (struct process *p = processes; p; p = p->next) count++;
    if (count > 4096) return -1;
    pids = calloc(count ? count : 1, sizeof(*pids));
    if (!pids) return -1;
    count = 0;
    for (struct process *p = processes; p; p = p->next)
        pids[count++] = (struct sb_precopy_pid){ .source = p->pid, .destination = p->pid };
    /* These IDs identify source processes only; destination PID mapping is
     * authoritative only in the separate final manifest after suspension. */
    result = source_validate(pids, count, image_dir_fd, preliminary_eligible, true);
    free(pids);
    return result;
}

struct cached_page { pid_t pid; unsigned state; uint64_t address, index; };
struct cached_group { uint32_t source; pid_t destination; uint64_t begin, end; };
static struct cache_header *cache;
static struct cached_page *cached_pages;
static struct cached_group *cached_groups;
static struct sb_precopy_view client_view;
static uint64_t client_length;
static size_t cached_group_count;
static size_t cached_count, client_next;
static unsigned client_threads, demand_active;
static pthread_t client_workers[SB_MAX_WORKERS];
static sb_precopy_install_fn install_page;
static unsigned long demand_hits, installed, adopted_pages, discarded_pages;
static sb_precopy_adopted_fn already_adopted;

void sb_precopy_set_adopted(sb_precopy_adopted_fn callback) { already_adopted = callback; }

static int cached_compare(const void *left, const void *right)
{
    const struct cached_page *a = left, *b = right;
    if (a->pid != b->pid) return a->pid < b->pid ? -1 : 1;
    return a->address < b->address ? -1 : a->address != b->address;
}

int sb_precopy_view(void *buffer, uint64_t length, struct sb_precopy_view *view)
{
    struct cache_header *header = buffer;
    const struct sb_precopy_page *pages;
    if (!buffer || !view || length < sizeof(*header) || header->magic != SB_MAGIC ||
        header->version != SB_VERSION || header->bytes != length ||
        header->pages > (length - sizeof(*header)) / sizeof(*pages) ||
        header->data_offset < sizeof(*header) + header->pages * sizeof(*pages) ||
        header->data_offset > length || (header->data_offset & (SB_PAGE - 1)) ||
        header->pages != (length - header->data_offset) / SB_PAGE ||
        (length - header->data_offset) % SB_PAGE) return -1;
    pages = (const void *)(header + 1);
    for (uint64_t i = 0; i < header->pages; i++) {
        if (!pages[i].pid || pages[i].region_end <= pages[i].region_start ||
            ((pages[i].address | pages[i].region_start | pages[i].region_end) & (SB_PAGE - 1)) ||
            pages[i].address < pages[i].region_start || pages[i].address >= pages[i].region_end ||
            (i && page_compare(&pages[i - 1], &pages[i]) >= 0)) return -1;
    }
    *view = (struct sb_precopy_view){ .pages = pages, .count = header->pages,
        .data = (char *)buffer + header->data_offset, .nonce = header->nonce };
    return 0;
}

static int read_validity(const struct sb_precopy_view *view, int image_dir_fd, const char *name,
                         struct sb_precopy_pid **out_pids, size_t *count, unsigned char **out_bitmap)
{
    struct valid_header valid;
    struct stat st;
    struct sb_precopy_pid *pids = NULL;
    unsigned char *bitmap = NULL;
    int fd = openat(image_dir_fd, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) || full_io(fd, &valid, sizeof(valid), false)) goto fail;
    if (valid.magic != SB_VALID_MAGIC || valid.version != SB_VERSION || valid.nonce != view->nonce ||
        valid.pages != view->count || valid.pids > 4096 || valid.bitmap_bytes != (view->count + 7) / 8 ||
        (uint64_t)st.st_size != sizeof(valid) + valid.pids * sizeof(*pids) + valid.bitmap_bytes) goto fail;
    pids = calloc(valid.pids ? valid.pids : 1, sizeof(*pids));
    bitmap = malloc(valid.bitmap_bytes ? valid.bitmap_bytes : 1);
    if (!pids || !bitmap || full_io(fd, pids, valid.pids * sizeof(*pids), false) ||
        full_io(fd, bitmap, valid.bitmap_bytes, false)) goto fail;
    for (size_t i = 0; i < valid.pids; i++) {
        if (pids[i].source <= 0 || pids[i].destination <= 0) goto fail;
        for (size_t j = 0; j < i; j++)
            if (pids[j].source == pids[i].source || pids[j].destination == pids[i].destination) goto fail;
    }
    for (uint64_t i = 0; i < view->count; i++)
        if ((bitmap[i / 8] & (1U << (i % 8))) &&
            (!view->pages[i].copied || !destination_pid(pids, valid.pids, view->pages[i].pid))) goto fail;
    close(fd);
    *out_pids = pids; *count = valid.pids; *out_bitmap = bitmap;
    return 0;
fail:
    if (fd >= 0) close(fd);
    free(pids); free(bitmap);
    return -1;
}

int sb_precopy_validity(const struct sb_precopy_view *view, int image_dir_fd,
                       struct sb_precopy_pid **out_pids, size_t *count, unsigned char **out_bitmap)
{
    return read_validity(view, image_dir_fd, SB_IMAGE, out_pids, count, out_bitmap);
}

int sb_precopy_ps_validity(const struct sb_precopy_view *view, int image_dir_fd, unsigned char **bitmap)
{
    struct sb_precopy_pid *pids = NULL;
    size_t count;
    int rc = read_validity(view, image_dir_fd, SB_PS_IMAGE, &pids, &count, bitmap);
    free(pids);
    return rc;
}

int sb_precopy_client_prepare(void *buffer, uint64_t length)
{
    struct sb_precopy_view view;
    size_t groups = 0;
    if (cache) return cache == buffer && client_length == length ? 0 : -1;
    if (sb_precopy_view(buffer, length, &view) ||
        view.count > SIZE_MAX / sizeof(*cached_pages)) return -1;
    for (uint64_t i = 0; i < view.count; i++)
        if (!i || view.pages[i].pid != view.pages[i - 1].pid) groups++;
    if (groups > 4096) return -1;
    cached_pages = calloc(view.count ? view.count : 1, sizeof(*cached_pages));
    cached_groups = calloc(groups ? groups : 1, sizeof(*cached_groups));
    if (!cached_pages || !cached_groups) {
        free(cached_pages); free(cached_groups);
        cached_pages = NULL; cached_groups = NULL;
        return -1;
    }
    for (uint64_t i = 0; i < view.count; i++) {
        if (!i || view.pages[i].pid != view.pages[i - 1].pid) {
            if (cached_group_count) cached_groups[cached_group_count - 1].end = i;
            cached_groups[cached_group_count++] = (struct cached_group){
                .source = view.pages[i].pid, .begin = i, .end = view.count };
        }
        /* Touch the actual index storage during PS, before suspension. No
         * entry is searchable until the final manifest has been accepted. */
        cached_pages[i] = (struct cached_page){ .address = view.pages[i].address, .index = i };
    }
    cache = buffer;
    client_length = length;
    client_view = view;
    pr_info("SB_PRECOPY index_prepared pages=%llu groups=%zu\n",
            (unsigned long long)view.count, cached_group_count);
    return 0;
}

static int group_compare(const void *left, const void *right)
{
    const struct cached_group *a = left, *b = right;
    return a->destination < b->destination ? -1 : a->destination != b->destination;
}

int sb_precopy_client_init(void *buffer, uint64_t length, int image_dir_fd,
                           sb_precopy_install_fn install)
{
    struct sb_precopy_pid *pids = NULL;
    unsigned char *bitmap = NULL;
    size_t pid_count = 0;
    int result = -1;
    if (install_page || client_threads) return -1;
    cached_count = 0;
    if (!install || sb_precopy_client_prepare(buffer, length) ||
        sb_precopy_validity(&client_view, image_dir_fd, &pids, &pid_count, &bitmap)) goto out;
    /* The snapshot is already strictly sorted by source PID/address. A
     * final one-to-one PID map can reorder whole groups, never their pages.
     * Sort only the (few) process groups and filter them in one linear pass. */
    for (size_t g = 0; g < cached_group_count; g++)
        cached_groups[g].destination = destination_pid(pids, pid_count, cached_groups[g].source);
    qsort(cached_groups, cached_group_count, sizeof(*cached_groups), group_compare);
    for (size_t g = 0; g < cached_group_count; g++) {
        const struct cached_group *group = &cached_groups[g];
        if (group->destination <= 0) continue; /* read_validity rejects any accepted page here. */
        for (uint64_t i = group->begin; i < group->end; i++) {
            if (!(bitmap[i / 8] & (1U << (i % 8)))) continue;
            cached_pages[cached_count++] = (struct cached_page){
                .pid = group->destination, .address = client_view.pages[i].address, .index = i };
        }
    }
    install_page = install;
    result = 0;
    pr_info("SB_PRECOPY client valid=%zu total=%llu\n", cached_count, (unsigned long long)cache->pages);
out:
    free(pids); free(bitmap);
    if (result) {
        cached_count = 0;
        pr_err("Invalid pre-copy snapshot/validity manifest\n");
    }
    return result;
}

static void install_cached(struct cached_page *page)
{
    unsigned expected = 0;
    int rc;
    /* 0=available, 1=owned by one copier, 2=committed. No stale page can
     * overwrite an application's new write: UFFDIO_COPY also rejects EEXIST. */
    if (!__atomic_compare_exchange_n(&page->state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    rc = already_adopted ? already_adopted(page->pid, page->index) : 0;
    if (rc > 0) {
        __atomic_fetch_add(&adopted_pages, 1, __ATOMIC_RELAXED);
        rc = 0;
    } else if (!rc) {
        rc = install_page(page->pid, page->address, (char *)cache + cache->data_offset + page->index * SB_PAGE);
    }
    if (rc && rc != EEXIST && rc != ENODATA) {
        pr_err("Pre-copy install failed pid=%d address=%llx rc=%d\n", page->pid,
               (unsigned long long)page->address, rc);
        exit(EXIT_FAILURE);
    }
    __atomic_store_n(&page->state, 2, __ATOMIC_RELEASE);
    __atomic_fetch_add(rc == ENODATA ? &discarded_pages : &installed, 1, __ATOMIC_RELAXED);
}

int sb_precopy_client_has_page(pid_t pid, uint64_t address)
{
    struct cached_page key={ .pid=pid, .address=address };
    return install_page && bsearch(&key,cached_pages,cached_count,sizeof(*cached_pages),cached_compare)!=NULL;
}

int sb_precopy_client_fault(pid_t pid, uint64_t address)
{
    struct cached_page key = { .pid = pid, .address = address }, *page;
    if (!install_page) return 0;
    page = bsearch(&key, cached_pages, cached_count, sizeof(*cached_pages), cached_compare);
    if (!page) return 0;
    __atomic_fetch_add(&demand_active, 1, __ATOMIC_ACQ_REL);
    install_cached(page);
    __atomic_fetch_sub(&demand_active, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&demand_hits, 1, __ATOMIC_RELAXED);
    return 1;
}

static void *client_copy_worker(void *unused)
{
    size_t next;
    (void)unused;
    while ((next = __atomic_fetch_add(&client_next, 1, __ATOMIC_RELAXED)) < cached_count) {
        while (__atomic_load_n(&demand_active, __ATOMIC_ACQUIRE)) __asm__ volatile("pause" ::: "memory");
        install_cached(&cached_pages[next]);
    }
    return NULL;
}

int sb_precopy_client_start(unsigned workers)
{
    if (!install_page || !workers || workers > SB_MAX_WORKERS || client_threads) return -1;
    for (unsigned i = 0; i < workers; i++) {
        if (pthread_create(&client_workers[i], NULL, client_copy_worker, NULL)) return -1;
        client_threads++;
    }
    return 0;
}

int sb_precopy_client_wait(void)
{
    if (!install_page) return 0;
    for (unsigned i = 0; i < client_threads; i++) pthread_join(client_workers[i], NULL);
    client_threads = 0;
    for (size_t i = 0; i < cached_count; i++) {
        while (__atomic_load_n(&cached_pages[i].state, __ATOMIC_ACQUIRE) == 1) __asm__ volatile("pause" ::: "memory");
        if (__atomic_load_n(&cached_pages[i].state, __ATOMIC_ACQUIRE) != 2) return -1;
    }
    pr_info("SB_PRECOPY complete installed=%lu demand_hits=%lu parent_adopted=%lu discarded=%lu\n", installed, demand_hits, adopted_pages, discarded_pages);
    return 0;
}

size_t sb_precopy_client_valid_pages(void) { return cached_count; }
