#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <poll.h>
#include <sys/epoll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "sb-lifecycle.h"
#include "sb-lifecycle-gate.h"

#define PAGE_BYTES UINT64_C(4096)
enum { PENDING, PRESENT, ZERO };
struct mapping { uint64_t start, end, origin, first; };
struct trace_page { uint64_t fault_ns, installed_ns; unsigned lane; };
struct context {
    int fd;
    bool dead;
    struct mapping *maps;
    size_t count;
    unsigned char *state;
    uint64_t *wait_since;
    struct trace_page *trace;
    uint64_t identity;
    struct sb_lifecycle_stats *stats;
    struct context *next;
};
struct fault { struct context *context; uint64_t address; struct fault *next; };
struct family {
    pid_t pid;
    int readiness_fd;
    uint64_t pending_faults;
    struct sb_lifecycle_gate gate;
    struct mapping *original;
    size_t count;
    uint64_t pages;
    struct sb_lifecycle_stats observations;
    struct context *contexts;
    struct fault *head, **tail;
    struct family *next;
};
struct sb_lifecycle { struct family *families; struct sb_lifecycle_stats stats; bool trace, reader_preference, spin_gate; uint64_t next_identity; };
static void bump(uint64_t *n) { __atomic_fetch_add(n, 1, __ATOMIC_RELAXED); }
static void relax_cpu(void) { __asm__ volatile("pause" ::: "memory"); }
static uint64_t observation_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}
static void retire_wait(struct context *c, uint64_t first, uint64_t count)
{
    for (uint64_t i = first; i < first + count; i++)
        if (__atomic_exchange_n(&c->wait_since[i], 0, __ATOMIC_ACQ_REL))
            bump(&c->stats->fault_retired);
}
static void finish_wait(struct context *c, uint64_t index, unsigned lane)
{
    uint64_t begun = __atomic_exchange_n(&c->wait_since[index], 0, __ATOMIC_ACQ_REL);
    if (!begun) return;
    uint64_t ended = observation_ns(), elapsed = ended >= begun ? ended - begun : 0;
    uint64_t us = elapsed / 1000 + !!(elapsed % 1000), ceiling = 1, maximum;
    unsigned bin = 0;
    while (ceiling < us && bin + 1 < SB_FAULT_BUCKETS) { ceiling <<= 1; bin++; }
    bump(&c->stats->fault_installed[lane]);
    __atomic_fetch_add(&c->stats->fault_total_ns[lane], elapsed, __ATOMIC_RELAXED);
    bump(&c->stats->fault_hist[lane][bin]);
    maximum = __atomic_load_n(&c->stats->fault_max_ns[lane], __ATOMIC_RELAXED);
    while (maximum < elapsed && !__atomic_compare_exchange_n(&c->stats->fault_max_ns[lane],
           &maximum, elapsed, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
static bool range_ok(uint64_t start, uint64_t end)
{ return start < end && !((start | end) & (PAGE_BYTES - 1)); }
static struct mapping *at_address(struct context *c, uint64_t address)
{
    for (size_t i = 0; i < c->count; i++)
        if (address >= c->maps[i].start && address < c->maps[i].end) return &c->maps[i];
    return NULL;
}
static struct mapping *at_origin(struct context *c, uint64_t origin)
{
    for (size_t i = 0; i < c->count; i++)
        if (origin >= c->maps[i].origin && origin - c->maps[i].origin < c->maps[i].end - c->maps[i].start)
            return &c->maps[i];
    return NULL;
}
static int compare_maps(const void *a, const void *b)
{
    const struct mapping *x = a, *y = b;
    return x->start < y->start ? -1 : x->start != y->start;
}
/* Split before moving/removing, preserving each page's original identity and
 * bitmap index. No target address is ever treated as a new source address. */
static int change_maps(struct context *c, uint64_t start, uint64_t end, uint64_t to, bool move)
{
    struct mapping *maps;
    size_t count = 0;
    if (!range_ok(start, end) || (move && (!range_ok(to, to + end - start)))) return EPROTO;
    if (c->count > SIZE_MAX / (3 * sizeof(*maps))) return EOVERFLOW;
    maps = calloc(c->count ? c->count * 3 : 1, sizeof(*maps));
    if (!maps) return ENOMEM;
    for (size_t i = 0; i < c->count; i++) {
        struct mapping m = c->maps[i];
        uint64_t a = m.start > start ? m.start : start, b = m.end < end ? m.end : end;
        if (a >= b) { maps[count++] = m; continue; }
        if (m.start < a) { maps[count] = m; maps[count++].end = a; }
        if (move) maps[count++] = (struct mapping){to + a - start, to + b - start,
            m.origin + a - m.start, m.first + (a - m.start) / PAGE_BYTES};
        if (b < m.end) maps[count++] = (struct mapping){b, m.end,
            m.origin + b - m.start, m.first + (b - m.start) / PAGE_BYTES};
    }
    qsort(maps, count, sizeof(*maps), compare_maps);
    for (size_t i = 1; i < count; i++)
        if (maps[i-1].end > maps[i].start) { free(maps); return EPROTO; }
    if (!move) for (size_t i = 0; i < c->count; i++) {
        struct mapping *m = &c->maps[i];
        uint64_t a = start > m->start ? start : m->start, b = end < m->end ? end : m->end;
        if (a < b) retire_wait(c, m->first + (a - m->start) / PAGE_BYTES, (b - a) / PAGE_BYTES);
    }
    free(c->maps); c->maps = maps; c->count = count;
    return 0;
}
struct sb_lifecycle *sb_lifecycle_create(void) { return calloc(1, sizeof(struct sb_lifecycle)); }
int sb_lifecycle_reader_preference(struct sb_lifecycle *l,int enabled)
{
    if (!l || l->families || (enabled && l->spin_gate)) return EINVAL;
    l->reader_preference=enabled!=0;
    return 0;
}
int sb_lifecycle_spin_gate(struct sb_lifecycle *l,int enabled)
{
    if (!l || l->families || (enabled && l->reader_preference)) return EINVAL;
    l->spin_gate=enabled!=0;
    return 0;
}
int sb_lifecycle_gate_info(struct sb_lifecycle *l,unsigned index,struct sb_gate_info *out)
{
    if (!l || !out) return -EINVAL;
    for (struct family *f=l->families;f;f=f->next) {
        if (index--) continue;
        *out=(struct sb_gate_info){f->pid,(uintptr_t)&f->gate.sleep,sizeof(f->gate.sleep),f->gate.spin};
        return 1;
    }
    return 0;
}
int sb_lifecycle_enable_trace(struct sb_lifecycle *l)
{
    if (!l || l->families) return EINVAL;
    l->trace = true;
    return 0;
}
int sb_lifecycle_add(struct sb_lifecycle *l, pid_t pid, int fd,
                     const struct sb_lifecycle_range *ranges, size_t count)
{
    struct family *f = NULL;
    struct context *c = NULL;
    int flags;
    if (!l || pid <= 0 || fd < 0 || !count || count > SIZE_MAX / sizeof(struct mapping)) return EINVAL;
    f = calloc(1, sizeof(*f)); c = calloc(1, sizeof(*c));
    if (f) f->readiness_fd = -1;
    if (!f || !c) goto nomem;
    f->original = calloc(count, sizeof(*f->original));
    c->maps = calloc(count, sizeof(*c->maps));
    if (!f->original || !c->maps) goto nomem;
    for (size_t i = 0; i < count; i++) {
        uint64_t pages = (ranges[i].end - ranges[i].start) / PAGE_BYTES;
        if (!range_ok(ranges[i].start, ranges[i].end) ||
            (i && ranges[i-1].end > ranges[i].start) || pages > SIZE_MAX - f->pages) goto invalid;
        c->maps[i] = (struct mapping){ranges[i].start, ranges[i].end, ranges[i].start, f->pages};
        f->pages += pages;
    }
    c->state = calloc(f->pages, 1);
    c->wait_since = calloc(f->pages, sizeof(*c->wait_since));
    c->stats = &f->observations;
    c->identity = __atomic_add_fetch(&l->next_identity, 1, __ATOMIC_RELAXED);
    if (l->trace) c->trace = calloc(f->pages, sizeof(*c->trace));
    if (!c->state || !c->wait_since || (l->trace && !c->trace)) goto nomem;
    memcpy(f->original, c->maps, count * sizeof(*c->maps));
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) goto invalid;
    f->readiness_fd = epoll_create1(EPOLL_CLOEXEC);
    if (f->readiness_fd < 0) goto fail;
    struct epoll_event ready = { .events = EPOLLIN };
    if (epoll_ctl(f->readiness_fd, EPOLL_CTL_ADD, fd, &ready)) goto fail;
    /* Installers take non-recursive shared locks. With glibc's default
     * reader preference a continuous batch can starve the UFFD event reader
     * before it even records the fault. Keep lifecycle acknowledgement atomic
     * but stop admitting new installers ahead of an already waiting reader. */
    pthread_rwlockattr_t lock_attr;
    int lock_error=pthread_rwlockattr_init(&lock_attr);
    if (lock_error) { errno=lock_error; goto fail; }
    lock_error=pthread_rwlockattr_setkind_np(&lock_attr,l->reader_preference ?
        PTHREAD_RWLOCK_PREFER_READER_NP : PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    if (!lock_error) lock_error=pthread_rwlock_init(&f->gate.sleep,&lock_attr);
    pthread_rwlockattr_destroy(&lock_attr);
    if (lock_error) { errno = lock_error; goto fail; }
    f->gate.spin = l->spin_gate;
    f->pid = pid; f->count = count; f->contexts = c; f->tail = &f->head;
    c->fd = fd; c->count = count;
    f->next = l->families; l->families = f;
    return 0;
nomem:
    errno = ENOMEM;
    goto fail;
invalid:
    errno = EINVAL;
fail:
    if (c) { free(c->maps); free(c->state); free(c->wait_since); free(c->trace); free(c); }
    if (f) { if (f->readiness_fd >= 0) close(f->readiness_fd); free(f->original); free(f); }
    return errno;
}
static int event(struct sb_lifecycle *l, struct family *f, struct context *c, const struct uffd_msg *msg)
{
    uint64_t start = msg->arg.remove.start, end = msg->arg.remove.end;
    switch (msg->event) {
    case UFFD_EVENT_PAGEFAULT: {
        struct fault *fault;
        if (msg->arg.pagefault.flags & ~UFFD_PAGEFAULT_FLAG_WRITE) return EPROTO;
        bump(&c->stats->fault_events);
        uint64_t address = msg->arg.pagefault.address & ~(PAGE_BYTES - 1);
        struct mapping *m = at_address(c, address);
        if (m && __atomic_load_n(&c->state[m->first + (address - m->start) / PAGE_BYTES], __ATOMIC_ACQUIRE) == PENDING) {
            uint64_t index = m->first + (address - m->start) / PAGE_BYTES;
            uint64_t expected = 0, now = observation_ns();
            if (!now) return EIO;
            if (c->trace && !c->trace[index].fault_ns) c->trace[index].fault_ns = now;
            if (__atomic_compare_exchange_n(&c->wait_since[index], &expected, now, false,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                bump(&c->stats->fault_tracked);
            else bump(&c->stats->fault_coalesced);
        } else bump(&c->stats->fault_resolved_before_read);
        fault = malloc(sizeof(*fault));
        if (!fault) return ENOMEM;
        *fault = (struct fault){c, msg->arg.pagefault.address & ~(PAGE_BYTES - 1), NULL};
        *f->tail = fault; f->tail = &fault->next;
        __atomic_fetch_add(&f->pending_faults, 1, __ATOMIC_RELEASE);
        return 0;
    }
    case UFFD_EVENT_REMOVE:
        if (!range_ok(start, end)) return EPROTO;
        for (size_t i = 0; i < c->count; i++) {
            struct mapping *m = &c->maps[i];
            uint64_t a = start > m->start ? start : m->start, b = end < m->end ? end : m->end;
            if (a < b) {
                uint64_t first = m->first + (a - m->start) / PAGE_BYTES, count = (b - a) / PAGE_BYTES;
                memset(c->state + first, ZERO, count);
                retire_wait(c, first, count);
            }
        }
        bump(&l->stats.removes);
        return 0;
    case UFFD_EVENT_UNMAP:
        bump(&l->stats.unmaps);
        return change_maps(c, start, end, 0, false);
    case UFFD_EVENT_REMAP: {
        uint64_t from = msg->arg.remap.from, to = msg->arg.remap.to, length = msg->arg.remap.len;
        if (!length || from + length < from || to + length < to) return EPROTO;
        bump(&l->stats.remaps);
        return change_maps(c, from, from + length, to, true);
    }
    case UFFD_EVENT_FORK: {
        struct context *child = calloc(1, sizeof(*child));
        int fd = msg->arg.fork.ufd, flags = fcntl(fd, F_GETFL);
        if (!child) { close(fd); return ENOMEM; }
        child->maps = malloc((c->count ? c->count : 1) * sizeof(*c->maps));
        child->state = malloc(f->pages);
        child->wait_since = calloc(f->pages, sizeof(*child->wait_since));
        child->stats = &f->observations;
        child->identity = __atomic_add_fetch(&l->next_identity, 1, __ATOMIC_RELAXED);
        if (l->trace) child->trace = calloc(f->pages, sizeof(*child->trace));
        if (!child->maps || !child->state || !child->wait_since || (l->trace && !child->trace) || flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
            free(child->maps); free(child->state); free(child->wait_since); free(child->trace); free(child); close(fd); return ENOMEM;
        }
        struct epoll_event ready = { .events = EPOLLIN };
        if (epoll_ctl(f->readiness_fd, EPOLL_CTL_ADD, fd, &ready)) {
            int error = errno;
            free(child->maps); free(child->state); free(child->wait_since); free(child->trace); free(child); close(fd); return error;
        }
        memcpy(child->maps, c->maps, c->count * sizeof(*c->maps));
        memcpy(child->state, c->state, f->pages);
        child->fd = fd; child->count = c->count;
        child->next = c->next; c->next = child;
        bump(&l->stats.forks);
        return 0;
    }
    default: return EPROTO;
    }
}
/* Retire readiness along with the dead context; a permanent HUP must not
 * turn the event reader back into a busy exclusive-lock loop. Gate held. */
static int retire_context(struct family *f, struct context *c)
{
    if (!__atomic_exchange_n(&c->dead, true, __ATOMIC_ACQ_REL)) {
        retire_wait(c, 0, f->pages);
        if (epoll_ctl(f->readiness_fd, EPOLL_CTL_DEL, c->fd, NULL) && errno != ENOENT)
            return errno;
    }
    return 0;
}
/* Caller holds the exclusive gate. Reading acknowledges an event to the
 * kernel. Keep the gate until its mapping/bitmap changes are published. The
 * kernel mmap_changing guard makes concurrent COPY return EAGAIN; installers
 * release their shared gate and drain events before retrying. */
static int drain(struct sb_lifecycle *l, struct family *f)
{
    for (struct context *c = f->contexts; c; c = c->next) {
        while (!c->dead) {
            struct uffd_msg msg;
            ssize_t n = read(c->fd, &msg, sizeof(msg));
            int rc;
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN) break;
            if (!n || (n < 0 && errno == ESRCH)) {
                rc = retire_context(f,c); if (rc) return rc;
                break;
            }
            if (n != sizeof(msg)) return n < 0 ? errno : EPROTO;
            rc = event(l, f, c, &msg);
            if (rc) return rc;
        }
    }
    return 0;
}
int sb_lifecycle_next(struct sb_lifecycle *l, pid_t pid, uint64_t *address)
{
    return sb_lifecycle_next_batch(l,pid,address,1);
}
int sb_lifecycle_next_batch(struct sb_lifecycle *l, pid_t pid, uint64_t *addresses, unsigned capacity)
{
    unsigned produced=0;
    if (!l || !addresses || !capacity) return -EINVAL;
    for (struct family *f = l->families; f; f = f->next) {
        int rc;
        if (f->pid != pid) continue;
        /* Readiness does not consume/acknowledge UFFD events. An arriving
         * event remains level-triggered; only drain() below acknowledges it
         * while holding the exclusive mapping/bitmap gate. A fault already
         * drained by an installer must also keep this reader runnable. */
        if (!__atomic_load_n(&f->pending_faults, __ATOMIC_ACQUIRE)) {
            struct pollfd ready = { .fd = f->readiness_fd, .events = POLLIN };
            int n = poll(&ready,1,0);
            if (n < 0) { if (errno == EINTR) continue; return -errno; }
            if (!n) continue;
            if (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) return -EIO;
        }
        uint64_t gate_started=l->trace ? observation_ns() : 0;
        sb_gate_wrlock(&f->gate);
        if (gate_started) {
            uint64_t elapsed=observation_ns()-gate_started,maximum;
            bump(&f->observations.event_gate_count);
            __atomic_fetch_add(&f->observations.event_gate_total_ns,elapsed,__ATOMIC_RELAXED);
            maximum=__atomic_load_n(&f->observations.event_gate_max_ns,__ATOMIC_RELAXED);
            while (maximum<elapsed && !__atomic_compare_exchange_n(&f->observations.event_gate_max_ns,
                &maximum,elapsed,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
        }
        rc = drain(l, f);
        while (!rc && f->head && produced<capacity) {
            struct fault *fault = f->head;
            struct context *c = fault->context;
            struct mapping *m = at_address(c, fault->address);
            unsigned state = m ? c->state[m->first + (fault->address - m->start) / PAGE_BYTES] : ZERO;
            if (!c->dead && state == PENDING) {
                addresses[produced++] = m->origin + fault->address - m->start;
            } else if (!c->dead && state == ZERO) {
                struct uffdio_zeropage zero = { .range = {fault->address, PAGE_BYTES} };
                if (!ioctl(c->fd, UFFDIO_ZEROPAGE, &zero)) bump(&l->stats.zeroes);
                else if (errno == EAGAIN) break;
                else if (errno != EEXIST && errno != ENOENT && errno != ESRCH) rc = errno;
            } else if (!c->dead) {
                struct uffdio_range range = {fault->address, PAGE_BYTES};
                if (ioctl(c->fd, UFFDIO_WAKE, &range) && errno != ENOENT && errno != ESRCH) rc = errno;
            }
            f->head = fault->next;
            if (!f->head) f->tail = &f->head;
            free(fault);
            __atomic_fetch_sub(&f->pending_faults, 1, __ATOMIC_RELEASE);
        }
        sb_gate_unlock(&f->gate);
        if (rc) return -rc;
        if (produced==capacity) return produced;
    }
    return produced;
}
int sb_lifecycle_install(struct sb_lifecycle *l, pid_t pid, uint64_t origin, const void *data)
{
    return sb_lifecycle_install_tagged(l, pid, origin, data, SB_FAULT_LOCAL);
}
int sb_lifecycle_install_tagged(struct sb_lifecycle *l, pid_t pid, uint64_t origin, const void *data, unsigned lane)
{
    return sb_lifecycle_install_profiled(l, pid, origin, data, lane, NULL);
}
int sb_lifecycle_install_profiled(struct sb_lifecycle *l, pid_t pid, uint64_t origin,
                                 const void *data, unsigned lane, struct sb_install_profile *profile)
{
    struct family *f;
    bool copied = false, present = false;
    uint64_t retry_started = 0;
    if (lane >= SB_FAULT_LANES || !l || !data || (origin & (PAGE_BYTES - 1))) return EINVAL;
    for (f = l->families; f; f = f->next) {
        bool found = false;
        if (f->pid != pid) continue;
        for (size_t i = 0; i < f->count; i++)
            if (origin >= f->original[i].start && origin < f->original[i].end) { found = true; break; }
        if (found) break;
    }
    if (!f) return EFAULT;
    for (;;) {
        bool retry = false;
        int result = 0;
        uint64_t measured = profile ? observation_ns() : 0;
        sb_gate_rdlock(&f->gate);
        if (profile) profile->read_gate_ns += observation_ns() - measured;
        for (struct context *c = f->contexts; c; c = c->next) {
            struct mapping *m;
            uint64_t index;
            struct uffdio_copy copy;
            if (profile) profile->contexts++;
            if (__atomic_load_n(&c->dead, __ATOMIC_ACQUIRE) || !(m = at_origin(c, origin))) continue;
            index = m->first + (origin - m->origin) / PAGE_BYTES;
            unsigned state = __atomic_load_n(&c->state[index], __ATOMIC_ACQUIRE);
            if (state == ZERO) continue;
            if (state == PRESENT) { present = true; continue; }
            copy = (struct uffdio_copy){ .dst = m->start + origin - m->origin, .src = (uintptr_t)data, .len = PAGE_BYTES };
            measured = profile ? observation_ns() : 0;
            int copied_rc = ioctl(c->fd, UFFDIO_COPY, &copy), copy_error = copied_rc ? errno : 0;
            if (profile) { profile->copy_ns += observation_ns() - measured; profile->copies++; }
            if (!copied_rc) { copied = true; bump(&l->stats.copies); }
            else if (copy_error == EEXIST) present = true;
            else if (copy_error == EAGAIN || copy_error == ENOENT) { retry = true; continue; }
            else if (copy_error == ESRCH || copy_error == ENOSPC) {
                measured = profile ? observation_ns() : 0;
                result = retire_context(f,c);
                if (profile) profile->retire_ns += observation_ns() - measured;
                if (result) break;
                continue;
            }
            else { result = copy_error; break; }
            __atomic_store_n(&c->state[index], PRESENT, __ATOMIC_RELEASE);
            if (c->trace) {
                uint64_t empty = 0;
                /* The first successful installation observer owns this record.
                 * Concurrent duplicate COPY/EEXIST calls cannot relabel it. */
                if (__atomic_compare_exchange_n(&c->trace[index].installed_ns, &empty, UINT64_MAX,
                                                 false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                    c->trace[index].lane = lane;
                    __atomic_store_n(&c->trace[index].installed_ns, observation_ns(), __ATOMIC_RELEASE);
                }
            }
            finish_wait(c, index, lane);
        }
        sb_gate_unlock(&f->gate);
        if (result) return result;
        if (!retry) break;
        bump(&l->stats.retries);
        if (profile) profile->retries++;
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return errno;
        uint64_t ns = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
        if (!retry_started) retry_started = ns;
        if (ns - retry_started > UINT64_C(15000000000)) return ETIMEDOUT;
        measured = profile ? observation_ns() : 0;
        sb_gate_wrlock(&f->gate);
        if (profile) profile->write_gate_ns += observation_ns() - measured;
        measured = profile ? observation_ns() : 0;
        result = drain(l, f);
        if (profile) profile->drain_ns += observation_ns() - measured;
        sb_gate_unlock(&f->gate);
        if (result) return result;
        relax_cpu();
    }
    if (copied) return 0;
    if (present) return EEXIST;
    bump(&l->stats.discarded);
    return ENODATA;
}
int sb_lifecycle_finish(struct sb_lifecycle *l)
{
    if (!l) return EINVAL;
    for (struct family *f = l->families; f; f = f->next) {
        int rc;
        sb_gate_wrlock(&f->gate);
        rc = drain(l, f);
        if (rc) { sb_gate_unlock(&f->gate); return rc; }
        close(f->readiness_fd); f->readiness_fd = -1;
        /* Restorers close their local copies after registration. These are
         * the final file refs, including every fd learned via FORK. */
        for (struct context *c = f->contexts; c; c = c->next) {
            if (c->fd >= 0) close(c->fd);
            c->fd = -1;
        }
        sb_gate_unlock(&f->gate);
    }
    return 0;
}
/* Per-family counters avoid a shared cross-process cache line on the fault
 * path. Aggregation is observational; callers needing a consistent final
 * snapshot must join all workers first. */
static void add_observations(struct sb_lifecycle_stats *out, const struct sb_lifecycle_stats *s)
{
#define ADD(x) out->x += __atomic_load_n(&s->x, __ATOMIC_RELAXED)
    ADD(fault_events); ADD(fault_tracked); ADD(fault_coalesced); ADD(fault_resolved_before_read); ADD(fault_retired);
    ADD(event_gate_count); ADD(event_gate_total_ns);
    uint64_t gate_max=__atomic_load_n(&s->event_gate_max_ns,__ATOMIC_RELAXED);
    if (gate_max>out->event_gate_max_ns) out->event_gate_max_ns=gate_max;
    for (unsigned lane = 0; lane < SB_FAULT_LANES; lane++) {
        ADD(fault_installed[lane]); ADD(fault_total_ns[lane]);
        uint64_t maximum = __atomic_load_n(&s->fault_max_ns[lane], __ATOMIC_RELAXED);
        if (maximum > out->fault_max_ns[lane]) out->fault_max_ns[lane] = maximum;
        for (unsigned bin = 0; bin < SB_FAULT_BUCKETS; bin++) ADD(fault_hist[lane][bin]);
    }
#undef ADD
}
int sb_lifecycle_pid_stats(struct sb_lifecycle *l, pid_t pid, struct sb_lifecycle_stats *out)
{
    bool found = false;
    if (!l || !out) return EINVAL;
    memset(out, 0, sizeof(*out));
    for (struct family *f = l->families; f; f = f->next) if (f->pid == pid) {
        add_observations(out, &f->observations); found = true;
    }
    return found ? 0 : ENOENT;
}
void sb_lifecycle_stats(struct sb_lifecycle *l, struct sb_lifecycle_stats *out)
{
    memset(out, 0, sizeof(*out));
#define GET(x) out->x = __atomic_load_n(&l->stats.x, __ATOMIC_RELAXED)
    GET(remaps); GET(removes); GET(unmaps); GET(forks); GET(copies); GET(zeroes); GET(discarded); GET(retries);
#undef GET
    for (struct family *f = l->families; f; f = f->next) add_observations(out, &f->observations);
}
int sb_lifecycle_write_trace(struct sb_lifecycle *l, int fd)
{
    if (!l || !l->trace || fd < 0) return EINVAL;
    int duplicate = dup(fd);
    if (duplicate < 0) return errno;
    FILE *out = fdopen(duplicate, "w");
    if (!out) { int error = errno; close(duplicate); return error; }
    fprintf(out, "origin_pid,context,origin_address,first_fault_read_ns,installed_ns,lane,mapped,dead\n");
    for (struct family *f = l->families; f; f = f->next)
        for (struct context *c = f->contexts; c; c = c->next)
            for (size_t m = 0; m < f->count; m++) {
                const struct mapping *original = &f->original[m];
                for (uint64_t address = original->start; address < original->end; address += PAGE_BYTES) {
                    uint64_t i = original->first + (address - original->start) / PAGE_BYTES;
                    const struct trace_page *t = &c->trace[i];
                    if (!t->installed_ns && !t->fault_ns) continue;
                    fprintf(out, "%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%u,%u\n",
                        f->pid, c->identity, address, t->fault_ns, t->installed_ns,
                        t->installed_ns ? t->lane : SB_FAULT_LOCAL,
                        at_origin(c, address) != NULL, c->dead);
                }
            }
    int error = ferror(out) ? EIO : 0;
    if (fclose(out) && !error) error = errno;
    return error;
}

void sb_lifecycle_destroy(struct sb_lifecycle *l)
{
    if (!l) return;
    struct family *f = l->families;
    while (f) {
        struct family *next = f->next;
        struct context *c = f->contexts;
        while (c) {
            struct context *n = c->next;
            if (c->fd >= 0) close(c->fd);
            free(c->maps); free(c->state); free(c->wait_since); free(c->trace); free(c); c = n;
        }
        while (f->head) { struct fault *n = f->head->next; free(f->head); f->head = n; }
        if (f->readiness_fd >= 0) close(f->readiness_fd);
        pthread_rwlock_destroy(&f->gate.sleep); free(f->original); free(f); f = next;
    }
    free(l);
}
