/* The receiving parent builds anonymous staging VMAs before source freeze.
 * IS only discards invalid ranges and moves inherited mappings, preserving
 * anonymous-page COW and MADV_DONTNEED semantics. Unsupported VMAs fall back
 * to the independent pageclient cache. No application address is used in PS. */
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "log.h"
#include "sb-precopy.h"
#include "sb-stage.h"

#define PAGE_BYTES 4096ULL
#define STAGE_MAGIC 0x5342535441474532ULL
struct stage_header { uint64_t magic, length, pages, bitmap_bytes; };
struct stage_region {
    uint64_t start, end, first, count;
    pid_t source_pid, pid;
    int flags;
    void *mapping;
    bool inherit;
};
static struct sb_precopy_view stage_view;
static struct stage_region *regions;
static size_t region_count;
static unsigned char *committed, *valid;
static unsigned char *retained;
static uint64_t commit_pages;
static bool finalized;
static uint64_t copy_next, copied_pages;

static void *copy_stage_pages(void *unused)
{
    uint64_t begin;
    (void)unused;
    while ((begin = __atomic_fetch_add(&copy_next, 128, __ATOMIC_RELAXED)) < stage_view.count) {
        uint64_t end = begin + 128, copied = 0;
        size_t low = 0, high = region_count;
        while (low + 1 < high) {
            size_t middle = low + (high - low) / 2;
            if (regions[middle].first <= begin) low = middle;
            else high = middle;
        }
        if (end > stage_view.count) end = stage_view.count;
        for (uint64_t i = begin; i < end; i++) {
            const struct sb_precopy_page *page = &stage_view.pages[i];
            while (i >= regions[low].first + regions[low].count) low++;
            if (!regions[low].mapping || !page->copied) continue;
            memcpy((char *)regions[low].mapping + page->address - regions[low].start,
                   (const char *)stage_view.data + i * PAGE_BYTES, PAGE_BYTES);
            copied++;
        }
        __atomic_fetch_add(&copied_pages, copied, __ATOMIC_RELAXED);
    }
    return NULL;
}

void *sb_stage_allocate(uint64_t length, int *out_fd)
{
    void *mapping;
    int fd;
    if (!length || length > (64ULL << 30) || (length & (PAGE_BYTES - 1))) return NULL;
    fd = memfd_create("swiftbaton-ps-pages", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return NULL;
    if (ftruncate(fd, length) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW)) {
        close(fd); return NULL;
    }
    mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) { close(fd); return NULL; }
    *out_fd = fd;
    return mapping;
}

static int transfer_header(int fd, void *buffer, size_t length, bool send_header)
{
    char *p = buffer;
    while (length) {
        ssize_t n = send_header ? send(fd, p, length, MSG_NOSIGNAL) : recv(fd, p, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; length -= n;
    }
    return 0;
}

static int transfer_fds(int socket, int fds[2], bool sending)
{
    union { struct cmsghdr align; char bytes[CMSG_SPACE(2 * sizeof(int))]; } control;
    char byte = 'S';
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    struct cmsghdr *c;
    ssize_t n;
    memset(&control, 0, sizeof(control));
    if (sending) {
        c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(2 * sizeof(int));
        memcpy(CMSG_DATA(c), fds, 2 * sizeof(int));
    }
    do { n = sending ? sendmsg(socket, &msg, MSG_NOSIGNAL) : recvmsg(socket, &msg, MSG_CMSG_CLOEXEC); }
    while (n < 0 && errno == EINTR);
    if (n != 1) return -1;
    if (!sending) {
        c = CMSG_FIRSTHDR(&msg);
        if (byte != 'S' || (msg.msg_flags & MSG_CTRUNC) || !c ||
            c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
            c->cmsg_len != CMSG_LEN(2 * sizeof(int))) return -1;
        memcpy(fds, CMSG_DATA(c), 2 * sizeof(int));
    }
    return 0;
}

int sb_stage_send(int socket, int snapshot_fd, void *buffer, uint64_t length)
{
    struct stage_header header;
    struct sb_precopy_view view;
    int bitmap_fd, fds[2], rc;
    if (committed || sb_precopy_view(buffer, length, &view)) return -1;
    header = (struct stage_header){ .magic = STAGE_MAGIC, .length = length, .pages = view.count,
        .bitmap_bytes = (((view.count + 7) / 8 + PAGE_BYTES - 1) / PAGE_BYTES) * PAGE_BYTES };
    if (!header.bitmap_bytes) header.bitmap_bytes = PAGE_BYTES;
    committed = sb_stage_allocate(header.bitmap_bytes, &bitmap_fd);
    if (!committed) return -1;
    commit_pages = view.count;
    fds[0] = snapshot_fd; fds[1] = bitmap_fd;
    rc = transfer_header(socket, &header, sizeof(header), true) || transfer_fds(socket, fds, true);
    close(bitmap_fd);
    return rc ? -1 : 0;
}

int sb_stage_receive(int socket, unsigned workers)
{
    struct stage_header header;
    struct stat st;
    int fds[2] = {-1, -1};
    void *snapshot = MAP_FAILED;
    pthread_t threads[32];
    if (regions || !workers || workers > 32 || transfer_header(socket, &header, sizeof(header), false) ||
        header.magic != STAGE_MAGIC || !header.length || header.length > (64ULL << 30) ||
        header.pages > header.length / PAGE_BYTES || !header.bitmap_bytes ||
        header.bitmap_bytes != (((header.pages + 7) / 8 + PAGE_BYTES - 1) / PAGE_BYTES) * PAGE_BYTES +
                               (header.pages == 0 ? PAGE_BYTES : 0) ||
        transfer_fds(socket, fds, false)) return -1;
    if (fstat(fds[0], &st) || (uint64_t)st.st_size != header.length) goto fail;
    if (fstat(fds[1], &st) || (uint64_t)st.st_size != header.bitmap_bytes) goto fail;
    snapshot = mmap(NULL, header.length, PROT_READ, MAP_SHARED, fds[0], 0);
    if (snapshot == MAP_FAILED || sb_precopy_view(snapshot, header.length, &stage_view) ||
        stage_view.count != header.pages) goto fail;
    committed = mmap(NULL, header.bitmap_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fds[1], 0);
    if (committed == MAP_FAILED) { committed = NULL; goto fail; }
    commit_pages = header.pages;
    regions = calloc(stage_view.count ? stage_view.count : 1, sizeof(*regions));
    if (!regions) goto fail;
    for (uint64_t i = 0; i < stage_view.count; i++) {
        const struct sb_precopy_page *page = &stage_view.pages[i];
        struct stage_region *r = region_count ? &regions[region_count - 1] : NULL;
        if (page->region_flags & ~(MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE)) goto fail;
        if (!r || r->source_pid != (pid_t)page->pid || r->start != page->region_start || r->end != page->region_end) {
            if (r && r->source_pid == (pid_t)page->pid && r->end > page->region_start) goto fail;
            r = &regions[region_count++];
            *r = (struct stage_region){ .source_pid = page->pid, .start = page->region_start,
                .end = page->region_end, .first = i, .flags = page->region_flags };
            r->mapping = mmap(NULL, r->end - r->start, PROT_READ | PROT_WRITE,
                              r->flags, -1, 0);
            if (r->mapping == MAP_FAILED) {
                pr_perror("Anonymous PS stage allocation; use pageclient fallback");
                r->mapping = NULL;
            }
        }
        if (r->flags != (int)page->region_flags) goto fail;
        r->count++;
    }
    for (unsigned i = 0; i < workers; i++) {
        if (pthread_create(&threads[i], NULL, copy_stage_pages, NULL)) {
            for (unsigned j = 0; j < i; j++) pthread_join(threads[j], NULL);
            goto fail;
        }
    }
    for (unsigned i = 0; i < workers; i++) pthread_join(threads[i], NULL);
    /* The descriptor table remains useful in IS. The bulk payload has been
     * copied into anonymous VMAs and must not accompany every helper fork. */
    if (stage_view.count && munmap((void *)stage_view.data, stage_view.count * PAGE_BYTES)) goto fail;
    stage_view.data = NULL;
    if (sb_stage_inherit(false)) goto fail;
    close(fds[0]); close(fds[1]);
    pr_info("SB_STAGE parent_ready regions=%zu copied=%llu pid=%d workers=%u\n", region_count,
            (unsigned long long)copied_pages, getpid(), workers);
    return 0;
fail:
    pr_perror("Receive parent stage");
    if (fds[0] >= 0) close(fds[0]);
    if (fds[1] >= 0) close(fds[1]);
    return -1;
}

int sb_stage_inherit(bool enable)
{
    int rc = 0;
    for (size_t i = 0; i < region_count; i++) {
        struct stage_region *r = &regions[i];
        if (!r->mapping) continue;
        if (madvise(r->mapping, r->end - r->start,
                    enable ? MADV_DOFORK : MADV_DONTFORK)) {
            pr_perror("Set parent stage fork inheritance");
            rc = -1;
        } else r->inherit = enable;
    }
    /* Do not leave partially enabled ranges behind after a failed request. */
    if (rc && enable) sb_stage_inherit(false);
    return rc;
}

int sb_stage_prepare_fork(sb_stage_needed_fn needed, void *opaque)
{
    uint64_t bytes = 0, omitted = 0;
    if (!finalized || !needed) return -1;
    for (size_t i = 0; i < region_count; i++) {
        struct stage_region *r = &regions[i];
        bool inherit;
        if (!r->mapping) continue;
        inherit = r->pid > 0 && needed(r->pid, opaque);
        if (madvise(r->mapping, r->end - r->start, inherit ? MADV_DOFORK : MADV_DONTFORK)) {
            pr_perror("Select parent stages for restore child");
            sb_stage_inherit(false);
            return -1;
        }
        r->inherit = inherit;
        if (inherit) bytes += r->end - r->start;
        else omitted += r->end - r->start;
    }
    pr_info("SB_STAGE fork_selected bytes=%llu omitted=%llu\n",
            (unsigned long long)bytes, (unsigned long long)omitted);
    return 0;
}

int sb_stage_enter_child(void)
{
    /* These pointers describe holes in this child, not live mappings. Clear
     * them before allocations could reuse the holes or a helper is forked. */
    for (size_t i = 0; i < region_count; i++)
        if (!regions[i].inherit) regions[i].mapping = NULL;
    return sb_stage_inherit(false);
}

static void discard_invalid(const unsigned char *keep, const char *phase)
{
    uint64_t discarded = 0, calls = 0;
    for (size_t i = 0; i < region_count; i++) {
        struct stage_region *r = &regions[i];
        if (!r->mapping) continue;
        for (uint64_t j = r->first; j < r->first + r->count;) {
            uint64_t start, end;
            if ((keep[j / 8] & (1U << (j % 8))) ||
                (retained && !(retained[j / 8] & (1U << (j % 8))))) { j++; continue; }
            start = stage_view.pages[j].address;
            do {
                discarded += !retained || !!(retained[j / 8] & (1U << (j % 8)));
                j++;
            } while (j < r->first + r->count && !(keep[j / 8] & (1U << (j % 8))));
            end = stage_view.pages[j - 1].address + PAGE_BYTES;
            calls++;
            if (madvise((char *)r->mapping + start - r->start, end - start, MADV_DONTNEED)) {
                pr_perror("Discard invalid parent stage pages; use pageclient fallback");
                munmap(r->mapping, r->end - r->start); r->mapping = NULL;
                break;
            }
        }
    }
    pr_info("SB_STAGE %s pages=%llu discard_calls=%llu\n", phase,
            (unsigned long long)discarded, (unsigned long long)calls);
}

int sb_stage_prune(int image_dir_fd)
{
    unsigned char *keep;
    if (!regions || finalized || sb_precopy_ps_validity(&stage_view, image_dir_fd, &keep)) return -1;
    if (retained) {
        for (uint64_t j = 0; j < (stage_view.count + 7) / 8; j++) {
            if (keep[j] & ~retained[j]) {
                pr_err("PS refresh manifest reaccepted an invalid page\n");
                free(keep); return -1;
            }
        }
    }
    discard_invalid(keep, retained ? "ps_refreshed" : "ps_invalidated");
    free(retained);
    retained = keep;
    return 0;
}

int sb_stage_finalize(int image_dir_fd)
{
    struct sb_precopy_pid *pids;
    size_t count;
    if (!regions || finalized || sb_precopy_validity(&stage_view, image_dir_fd, &pids, &count, &valid)) return -1;
    /* An early rejected page was physically discarded and must never be
     * reaccepted even if a later snapshot could otherwise appear valid. */
    if (retained) {
        for (uint64_t j = 0; j < (stage_view.count + 7) / 8; j++) {
            if (valid[j] & ~retained[j]) {
                pr_err("Final stage manifest reaccepted a PS-invalid page\n");
                free(pids); return -1;
            }
        }
    }
    for (size_t i = 0; i < region_count; i++)
        for (size_t p = 0; p < count; p++)
            if (pids[p].source == regions[i].source_pid) regions[i].pid = pids[p].destination;
    discard_invalid(valid, "invalidated");
    free(pids); finalized = true;
    return 0;
}

int sb_stage_adopt(pid_t pid, uint64_t begin, uint64_t end, void *target, int flags, int prot,
                   unsigned long *page_bitmap, unsigned long *parent_bitmap)
{
    if (!finalized || !(flags & MAP_ANONYMOUS) || !(flags & MAP_PRIVATE) ||
        (flags & ~(MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE)) ||
        !target || ((uint64_t)target & (PAGE_BYTES - 1))) return 0;
    for (size_t i = 0; i < region_count; i++) {
        struct stage_region *r = &regions[i];
        void *moved;
        uint64_t adopted = 0;
        if (r->pid != pid || r->start != begin || r->end != end || !r->mapping ||
            r->flags != (flags & ~MAP_FIXED)) continue;
        moved = mremap(r->mapping, end - begin, end - begin, MREMAP_MAYMOVE | MREMAP_FIXED, target);
        if (moved == MAP_FAILED) { pr_perror("Adopt parent stage; use pageclient fallback"); return 0; }
        r->mapping = NULL;
        /* This is application memory now; normal fork/COW semantics apply. */
        if (madvise(target, end - begin, MADV_DOFORK) ||
            mprotect(target, end - begin, prot | PROT_WRITE)) return -1;
        for (uint64_t j = r->first; j < r->first + r->count; j++) {
            uint64_t bit = (stage_view.pages[j].address - begin) / PAGE_BYTES;
            if (!(valid[j / 8] & (1U << (j % 8)))) continue;
            if (page_bitmap) page_bitmap[bit / 64] |= 1UL << (bit % 64);
            if (parent_bitmap) parent_bitmap[bit / 64] &= ~(1UL << (bit % 64));
            __atomic_fetch_or(&committed[j / 8], 1U << (j % 8), __ATOMIC_RELEASE);
            adopted++;
        }
        pr_info("SB_STAGE adopted pid=%d begin=%llx end=%llx pages=%llu\n", pid,
                (unsigned long long)begin, (unsigned long long)end, (unsigned long long)adopted);
        return 1;
    }
    return 0;
}

int sb_stage_was_adopted(uint64_t index)
{
    return committed && index < commit_pages &&
        (__atomic_load_n(&committed[index / 8], __ATOMIC_ACQUIRE) & (1U << (index % 8)));
}
