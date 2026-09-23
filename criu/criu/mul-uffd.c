
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <poll.h>
#include <time.h>
#include <emmintrin.h>

#include "uffd.h"
#include "rst-malloc.h"
#include "restorer.h"
#include "pstree.h"
#include "common/scm.h"
#include "fdstore.h"
#include "util.h"
#include "cr_options.h"

#include "mul-uffd.h"
#include "sb-lifecycle.h"

extern int item_num;
extern uint64_t pidset[MAX_PROCESS];    // pidset用的是容器内的virt pid

/* Stream sockets may return a prefix of the large UFFD region table. */
static int uffd_region_transfer(int fd, void *data, size_t size, bool sending)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = sending ? send(fd, (char *)data + done, size - done, MSG_NOSIGNAL)
                            : recv(fd, (char *)data + done, size - done, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            pr_err("Incomplete UFFD region table: %zu/%zu bytes\n", done, size);
            return -1;
        }
        done += n;
    }
    return 0;
}

volatile struct pid_uffd_region_set *PidUffdSet;

void * shmmap(uint64_t size){
    return mmap(0, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
}

/* All restore children inherit this shared lock. Their descriptor/table/ready
 * records share one stream socket and must remain indivisible. */
static mutex_t *uffd_send_mutex;

static int uffd_pid_index(int pid)
{
    for (int i = 0; i < item_num; i++)
        if (PidUffdSet[i].pid == (unsigned long)pid) return i;
    return -1;
}

int InitPidUffdSet(void)
{
    if (item_num <= 0 || item_num > MAX_PROCESS) return -1;
    PidUffdSet = shmmap(sizeof(*PidUffdSet) * item_num);
    uffd_send_mutex = shmmap(sizeof(*uffd_send_mutex));
    if (PidUffdSet == MAP_FAILED || uffd_send_mutex == MAP_FAILED) return -1;
    mutex_init(uffd_send_mutex);
    for (int i = 0; i < item_num; i++) PidUffdSet[i].pid = pidset[i];
    return 0;
}

void sb_uffd_send_lock(void) { mutex_lock(uffd_send_mutex); }
void sb_uffd_send_unlock(void) { mutex_unlock(uffd_send_mutex); }

int PidUffdSet_taskargs(void *args, int pid)
{
    struct task_restore_args *ta = args;
    int index = uffd_pid_index(pid);
    if (index < 0) return -1;
    memcpy(&ta->uffd_set, (const void *)&PidUffdSet[index], sizeof(ta->uffd_set));
    return 0;
}

int PidUffdSet_sendfd(int sockfd, int pid)
{
    int index = uffd_pid_index(pid), count, fds[MAX_UFFD_NUM];
    if (index < 0) return -1;
    count = PidUffdSet[index].nr_uffd_region;
    if (count <= 0 || count > MAX_UFFD_NUM) return -1;
    for (int i = 0; i < count; i++) fds[i] = PidUffdSet[index].uffd_region[i].uffd;
    if (uffd_region_transfer(sockfd, &pid, sizeof(pid), true) ||
        uffd_region_transfer(sockfd, &count, sizeof(count), true) ||
        send_fds(sockfd, NULL, 0, fds, count, NULL, 0) < 0) return -1;
    return 0;
}

int update_PidUffdSet(int pid, int nr_uffd, int *uffds)
{
    int index = uffd_pid_index(pid);
    if (index < 0 || nr_uffd <= 0 || nr_uffd > MAX_UFFD_NUM ||
        PidUffdSet[index].nr_uffd_region != nr_uffd) return -1;
    for (int i = 0; i < nr_uffd; i++) PidUffdSet[index].uffd_region[i].uffd = uffds[i];
    return 0;
}

int PidUffdSet_recvfd(int sockfd, int *pid, int **uffds, int *nr_uffd)
{
    if (uffd_region_transfer(sockfd, pid, sizeof(*pid), false) ||
        uffd_region_transfer(sockfd, nr_uffd, sizeof(*nr_uffd), false) ||
        uffd_pid_index(*pid) < 0 || *nr_uffd <= 0 || *nr_uffd > MAX_UFFD_NUM) return -1;
    *uffds = malloc(*nr_uffd * sizeof(**uffds));
    if (!*uffds || recv_fds(sockfd, *uffds, *nr_uffd, NULL, 0) < 0) return -1;
    return 0;
}

int PidUffdSet_send_region(int sockfd, int pid)
{
    int index = uffd_pid_index(pid);
    if (index < 0) return -1;
    return uffd_region_transfer(sockfd, (void *)&PidUffdSet[index], sizeof(*PidUffdSet), true);
}

int PidUffdSet_recv_region(int sockfd, int pid, int *uffdset)
{
    int index = uffd_pid_index(pid);
    volatile struct pid_uffd_region_set *set;
    (void)uffdset;
    if (index < 0) return -1;
    set = &PidUffdSet[index];
    if (uffd_region_transfer(sockfd, (void *)set, sizeof(*set), false) ||
        set->pid != (unsigned long)pid || set->nr_uffd_region <= 0 || set->nr_uffd_region > MAX_UFFD_NUM) return -1;
    for (int r = 0; r < set->nr_uffd_region; r++) {
        volatile struct uffd_region *region = &set->uffd_region[r];
        if (region->nr_vma <= 0 || region->nr_vma > MAX_VMA_NUM || region->start >= region->end) return -1;
        for (int v = 0; v < region->nr_vma; v++) {
            if (region->vma[v].start >= region->vma[v].end || region->vma[v].start < region->start ||
                region->vma[v].end > region->end || (v && region->vma[v-1].end > region->vma[v].start)) return -1;
        }
    }
    return 0;
}

int PidUffdSet_fullfill(int pid, int sockfd)
{
    struct pstree_item *item, *owner = NULL;
    struct vma_area *vma;
    volatile struct pid_uffd_region_set *set;
    volatile struct uffd_region *region = NULL;
    unsigned long bytes = 0;
    int index = uffd_pid_index(pid);
    (void)sockfd;
    if (index < 0) return -1;
    for_each_pstree_item(item) {
        if (item->pid->ns[0].virt == pid) { owner = item; break; }
    }
    if (!owner || PidUffdSet[index].nr_uffd_region) return -1;
    set = &PidUffdSet[index];
    /* Only this process owns these VMAs. Equal addresses in other processes
     * must never be assigned to this UFFD or folded into its bounds. */
    list_for_each_entry(vma, &rsti(owner)->vmas.h, list) {
        unsigned long length = vma->e->end - vma->e->start;
        if (!length || vma->e->end < vma->e->start) return -1;
        if (!region || bytes + length > REGION_SIZE || region->nr_vma == MAX_VMA_NUM) {
            unsigned long features = opts.sb_parallel_transfer ?
                UFFD_FEATURE_EVENT_FORK | UFFD_FEATURE_EVENT_REMAP |
                UFFD_FEATURE_EVENT_REMOVE | UFFD_FEATURE_EVENT_UNMAP : 0;
            int fd;
            if (set->nr_uffd_region == MAX_UFFD_NUM) return -1;
            fd = uffd_open(O_CLOEXEC | O_NONBLOCK, &features, NULL);
            if (fd < 0) return -1;
            region = &set->uffd_region[set->nr_uffd_region++];
            region->uffd = fd; region->start = vma->e->start;
            bytes = 0;
        }
        if (region->nr_vma && region->end > vma->e->start) return -1;
        region->vma[region->nr_vma].start = vma->e->start;
        region->vma[region->nr_vma++].end = vma->e->end;
        region->end = vma->e->end;
        bytes += length;
    }
    pr_info("SB_UFFD pid=%d regions=%d own_vmas=%d\n", pid, set->nr_uffd_region, rsti(owner)->vmas.nr);
    return set->nr_uffd_region ? 0 : -1;
}

static int err_time = 0;
// static volatile struct uffdio_copy uffdio_copy[100];
/* Positive errno is the contract used by the page-transfer callers. */
static int sb_ready_fd_plus_one[MAX_PROCESS];
static unsigned sb_ready[MAX_PROCESS];

int sb_uffd_set_ready_fd(int pid, int fd)
{
    for (int i = 0; i < item_num; i++) {
        if (pidset[i] != (uint64_t)pid) continue;
        if (fd < 0 || sb_ready_fd_plus_one[i]) return -1;
        sb_ready_fd_plus_one[i] = fd + 1;
        return 0;
    }
    return -1;
}

int sb_uffd_wait_ready(int pid)
{
    for (int i = 0; i < item_num; i++) {
        if (PidUffdSet[i].pid != pid) continue;
        /* SCM_RIGHTS arrival precedes final mremap/UFFD registration. The
         * restorer signals this eventfd only after every lazy VMA is ready.
         * Do not consume it: all concurrent writers may observe readiness. */
        if (opts.sb_u_precopy && !__atomic_load_n(&sb_ready[i], __ATOMIC_ACQUIRE)) {
            struct pollfd ready = { .fd = sb_ready_fd_plus_one[i] - 1, .events = POLLIN };
            int rc;
            if (ready.fd < 0) return EPROTO;
            do { rc = poll(&ready, 1, 10000); } while (rc < 0 && errno == EINTR);
            if (rc <= 0 || !(ready.revents & POLLIN)) return rc == 0 ? ETIMEDOUT : EIO;
            __atomic_store_n(&sb_ready[i], 1, __ATOMIC_RELEASE);
        }
        return 0;
    }
    return EFAULT;
}

static struct sb_lifecycle *lifecycle;
static int page_trace_fd = -1;
int sb_uffd_lifecycle_start(void)
{
    lifecycle = sb_lifecycle_create();
    if (!lifecycle) return -1;
    if (sb_lifecycle_reader_preference(lifecycle,opts.sb_reader_preferred_lock)) return -1;
    if (sb_lifecycle_spin_gate(lifecycle,opts.sb_spin_lifecycle)) return -1;
    pr_info("SB_LIFECYCLE_SPIN enabled=%u\n",opts.sb_spin_lifecycle);
    pr_info("SB_FAULT_GATE_POLICY reader_preferred=%u\n",opts.sb_reader_preferred_lock);
    if (opts.sb_page_trace) {
        page_trace_fd = open(opts.sb_page_trace, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (page_trace_fd < 0 || sb_lifecycle_enable_trace(lifecycle)) return -1;
    }
    for (int p = 0; p < item_num; p++) {
        for (int r = 0; r < PidUffdSet[p].nr_uffd_region; r++) {
            volatile struct uffd_region *region = &PidUffdSet[p].uffd_region[r];
            struct sb_lifecycle_range *ranges = calloc(region->nr_vma, sizeof(*ranges));
            int rc;
            if (!ranges) return -1;
            for (int v = 0; v < region->nr_vma; v++) {
                ranges[v].start = region->vma[v].start;
                ranges[v].end = region->vma[v].end;
            }
            rc = sb_lifecycle_add(lifecycle, PidUffdSet[p].pid, region->uffd, ranges, region->nr_vma);
            free(ranges);
            if (rc) { errno = rc; return -1; }
        }
    }
    if (opts.sb_fault_trace) {
        struct sb_gate_info info;
        for (unsigned i=0;sb_lifecycle_gate_info(lifecycle,i,&info)>0;i++)
            pr_info("SB_LIFECYCLE_GATE pid=%d address=%lu bytes=%zu spin=%u\n",
                    info.pid,(unsigned long)info.address,info.bytes,info.spin);
    }
    return 0;
}
int sb_uffd_lifecycle_next(int pid, uint64_t *address)
{ return sb_lifecycle_next(lifecycle, pid, address); }
int sb_uffd_lifecycle_next_batch(int pid, uint64_t *addresses, unsigned capacity)
{ return sb_lifecycle_next_batch(lifecycle,pid,addresses,capacity); }
static void log_fault_stats(const struct sb_lifecycle_stats *s, pid_t pid)
{
    char identity[48] = "";
    const char *prefix = pid ? "SB_PID_FAULT" : "SB_FAULT";
    if (pid) snprintf(identity, sizeof(identity), "pid=%d ", pid);
    uint64_t finished = s->fault_retired;
    pr_info("%s_GATE %scount=%llu total_ns=%llu max_ns=%llu\n",prefix,identity,
        (unsigned long long)s->event_gate_count,(unsigned long long)s->event_gate_total_ns,
        (unsigned long long)s->event_gate_max_ns);
    for (unsigned lane = 0; lane < SB_FAULT_LANES; lane++) {
        char histogram[1024]; size_t used = 0;
        for (unsigned bin = 0; bin < SB_FAULT_BUCKETS; bin++)
            used += snprintf(histogram + used, sizeof(histogram) - used, "%s%llu", bin ? "," : "",
                             (unsigned long long)s->fault_hist[lane][bin]);
        finished += s->fault_installed[lane];
        pr_info("%s_SERVICE %slane=%u count=%llu total_ns=%llu max_ns=%llu hist_us_pow2=%s\n",
            prefix, identity, lane, (unsigned long long)s->fault_installed[lane], (unsigned long long)s->fault_total_ns[lane],
            (unsigned long long)s->fault_max_ns[lane], histogram);
    }
    pr_info("%s_OBSERVATION %sevents=%llu tracked=%llu coalesced=%llu resolved_before_read=%llu retired=%llu unfinished=%llu\n",
        prefix, identity, (unsigned long long)s->fault_events, (unsigned long long)s->fault_tracked,
        (unsigned long long)s->fault_coalesced, (unsigned long long)s->fault_resolved_before_read,
        (unsigned long long)s->fault_retired, (unsigned long long)(s->fault_tracked - finished));
}
int sb_uffd_lifecycle_finish(void)
{
    struct sb_lifecycle_stats s;
    int rc = sb_lifecycle_finish(lifecycle);
    sb_lifecycle_stats(lifecycle, &s);
    pr_info("SB_LIFECYCLE remaps=%llu removes=%llu unmaps=%llu forks=%llu copies=%llu zeroes=%llu discarded=%llu retries=%llu\n",
            (unsigned long long)s.remaps, (unsigned long long)s.removes,
            (unsigned long long)s.unmaps, (unsigned long long)s.forks,
            (unsigned long long)s.copies, (unsigned long long)s.zeroes,
            (unsigned long long)s.discarded, (unsigned long long)s.retries);
    log_fault_stats(&s, 0);
    for (int p = 0; p < item_num; p++) {
        int error = sb_lifecycle_pid_stats(lifecycle, pidset[p], &s);
        if (error) { if (!rc) rc = error; }
        else log_fault_stats(&s, pidset[p]);
    }
    if (page_trace_fd >= 0) {
        int error = sb_lifecycle_write_trace(lifecycle, page_trace_fd);
        if (close(page_trace_fd) && !error) error = errno;
        page_trace_fd = -1;
        if (error && !rc) rc = error;
    }
    sb_lifecycle_destroy(lifecycle); lifecycle = NULL;
    if (rc) errno = rc;
    return rc ? -1 : 0;
}

int ioctl_mul(volatile struct uffdio_copy *data, int pid)
{
    return ioctl_mul_tagged(data, pid, SB_FAULT_LOCAL);
}
int ioctl_mul_tagged(volatile struct uffdio_copy *data, int pid, unsigned lane)
{
    return ioctl_mul_profiled(data, pid, lane, NULL);
}
int ioctl_mul_profiled(volatile struct uffdio_copy *data, int pid, unsigned lane,
                       struct sb_install_profile *profile)
{
    struct uffdio_copy copy = {
        .dst = data->dst, .src = data->src, .len = data->len,
        .mode = data->mode, .copy = 0,
    };
    if (lifecycle) {
        struct timespec begun, ended;
        if (profile && clock_gettime(CLOCK_MONOTONIC, &begun)) return errno;
        int rc = sb_uffd_wait_ready(pid);
        if (profile) {
            if (clock_gettime(CLOCK_MONOTONIC, &ended)) return errno;
            profile->ready_ns += (uint64_t)(ended.tv_sec - begun.tv_sec) * 1000000000 + ended.tv_nsec - begun.tv_nsec;
        }
        if (rc) return rc;
        if (copy.len != 4096 || copy.mode) return EINVAL;
        return sb_lifecycle_install_profiled(lifecycle, pid, copy.dst, (const void *)(uintptr_t)copy.src, lane, profile);
    }
    for (int i = 0; i < item_num; i++) {
        if (PidUffdSet[i].pid != pid)
            continue;
        if (opts.sb_u_precopy) {
            int rc = sb_uffd_wait_ready(pid);
            if (rc) return rc;
        }
        for (int j = 0; j < PidUffdSet[i].nr_uffd_region; j++) {
            volatile struct uffd_region *region = &PidUffdSet[i].uffd_region[j];
            if (copy.dst >= region->start && copy.dst < region->end &&
                copy.len <= region->end - copy.dst) {
                if (ioctl(region->uffd, UFFDIO_COPY, &copy) < 0)
                    return errno;
                return 0;
            }
        }
        break;
    }
    return EFAULT;
}
