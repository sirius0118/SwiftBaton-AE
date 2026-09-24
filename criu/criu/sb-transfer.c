/* SwiftBaton-U AS transport. Three independent RC QPs carry demand, adjacent
 * prefetch and hot-first background traffic. Dumpee helpers copy through the
 * existing shared mappings; source ownership lasts through target install ACK.
 * Target lifecycle tracking translates mutable VMAs to immutable source pages. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "cr_options.h"
#include "cr-sync.h"
#include "log.h"
#include "RDMA.h"
#include "transfer.h"
#include "mul-uffd.h"
#include "pre-transfer.h"
#include "sb-precopy.h"
#include "sb-sched.h"
#include "sb-transfer.h"
#include "sb-trace.h"
#include "sb-rdma-tx.h"
#include "sb-rdma-write.h"
#include "sb-install-queue.h"
#include "sb-bg-install.h"
#include "sb-lifecycle.h"
#include "sb-pf-install.h"

#define P 4096ULL
#define MAX_BATCH 256U
#define PROTOCOL_MAGIC UINT64_C(0x53425452414e5331)
extern volatile struct mul_shregion_t *SharedRegions;
extern volatile struct transfer_t *TransferRegions;
extern volatile struct prefetch_t *PrefetchRegions;
extern volatile struct pid_vmas *volatile PidVma[MAX_PROCESS];
extern uint64_t vpidset[MAX_PROCESS];
extern struct pid_data_list *pid_data_list;
extern int list_length;

struct span { uint64_t start, end, first; int process, vma; };
static struct span *spans;
static size_t span_count, begin_span[MAX_PROCESS], end_span[MAX_PROCESS];
static uint64_t source_pages, seed_pages;
static struct sb_sched *scheduler;
static bool feed_done, source_done, source_demand_drained, client_done;
static bool source_seed_committing;
static pthread_mutex_t pf_client_lock = PTHREAD_MUTEX_INITIALIZER;
static bool client_producers_done;
static unsigned batch_pages, copy_workers;
static uint64_t target_copied[SB_LANES], target_existing[SB_LANES], target_discarded[SB_LANES];
#define FAULT_TRACE_CAPACITY 262144U
struct fault_event { uint64_t ns,address; uint32_t pid,stage; };
#define INSTALL_TRACE_CAPACITY 32768U
#define INSTALL_TRACE_BG_THRESHOLD_NS UINT64_C(100000)
struct install_event {
    struct sb_install_profile timing;
    uint64_t begun, ended, address;
    uint32_t pid, lane, role, result;
};
struct fault_trace {
    unsigned linux_tid, origin_pid;
    const char *role;
    struct fault_event *events;
    unsigned count, capacity;
    uint64_t dropped;
    struct install_event *installs;
    unsigned install_count;
    uint64_t install_seen, install_skipped, install_dropped;
};
static struct fault_trace source_trace, target_tx_trace, target_trace[MAX_PROCESS];
static struct fault_trace source_ft_trace, target_ft_trace;
enum { PF_SOURCE_RECEIVE, PF_SOURCE_COPY_SUBMIT, PF_SOURCE_READY,
       PF_SOURCE_POST, PF_SOURCE_CQ, PF_TARGET_QUEUE, PF_TARGET_POST,
       PF_TARGET_RECEIVE, PF_TARGET_INSTALL };
enum { FT_SOURCE_CLAIM=10, FT_SOURCE_COPY_SUBMIT, FT_SOURCE_READY,
       FT_SOURCE_POST, FT_SOURCE_CQ, FT_TARGET_RECEIVE, FT_TARGET_INSTALL, FT_TARGET_DISPATCH };
static struct ft_installer {
    struct sb_install_queue queue;
    struct fault_trace trace;
    pid_t pid;
    uint64_t installed;
} ft_installers[MAX_PROCESS];
static struct sb_install_completion ft_completed[PREFETCH_BUFFER_SIZE];
static bool ft_installers_stop;
_Static_assert(SB_INSTALL_QUEUE_SLOTS==PREFETCH_BUFFER_SIZE,"FT descriptor capacity");
_Static_assert(SB_BG_SLOTS==TRANSFER_BUFFER_SIZE && SB_BG_MAX_PAGES==MAX_BATCH,"BG descriptor capacity");
enum { PF_TARGET_DISPATCH=23 };
enum { PF_SOURCE_OBSERVE=24 };
static struct fault_trace target_pf_dispatch_trace;
static struct pf_installer {
    struct fault_trace trace;
    struct sb_pf_aux_queue aux;
    pthread_t thread;
    int local,peer;
    unsigned active;
    uint64_t installed,assisted;
} *pf_installers[MAX_PROCESS];
static struct pf_process {
    struct sb_pf_queue queue;
    unsigned next_aux;
    uint64_t aux_enqueued,aux_fallback,inline_installed;
} pf_processes[MAX_PROCESS];
static bool pf_installers_stop;
static unsigned pf_allocated_workers;
_Static_assert(SB_PF_SLOTS==MAX_THREADS,"PF response capacity");
enum { BG_ASSIST_BEGIN=18, BG_ASSIST_END, BG_BATCH_OBSERVED, BG_INSTALL_BEGIN, BG_INSTALL_END };
struct bg_range { uint64_t start,end,first; };
static struct bg_directory {
    struct bg_range *ranges;
    unsigned count;
    uint64_t pages,*cells;
    struct sb_bg_assist_queue assists;
    uint64_t direct,queued,overflow;
} bg_directories[MAX_PROCESS];
static struct bg_descriptor { struct sb_bg_owner owner; uint64_t *directory; } bg_descriptors[SB_BG_PAGE_SLOTS];
static struct sb_bg_cursor bg_cursors[SB_BG_SLOTS];
static uint64_t bg_observed[SB_BG_SLOTS];
static struct bg_worker { struct fault_trace trace; uint64_t installed; unsigned index; } bg_workers[32];
static bool bg_workers_stop;
static unsigned bg_oldest;

static void relax_cpu(void) { __asm__ volatile("pause" ::: "memory"); }
static __attribute__((noreturn)) void die(const char *why)
{ pr_perror("SB_TRANSFER %s", why); exit(EXIT_FAILURE); }
static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) die("clock");
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
/* Bind once, outside service, so scheduler observations can identify the
 * original PID event reader and the worker owning each private trace. */
static void fault_trace_bind(struct fault_trace *t,const char *role,unsigned pid)
{
    if (!t->events && !t->installs) return;
    t->linux_tid=syscall(SYS_gettid);t->origin_pid=pid;t->role=role;
}
static void fault_trace_init_capacity(struct fault_trace *t,unsigned capacity)
{
    if (!opts.sb_fault_trace) return;
    t->capacity=capacity;
    t->events=mmap(NULL,(size_t)t->capacity*sizeof(*t->events),PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0);
    if (t->events==MAP_FAILED) die("fault trace allocation");
}
static void fault_trace_init(struct fault_trace *t)
{ fault_trace_init_capacity(t,FAULT_TRACE_CAPACITY); }
/* Each target worker owns its entire buffer. Prefault before service starts;
 * no output, allocation or shared logging lock on the installation path. */
static void target_trace_init(struct fault_trace *t)
{
    fault_trace_init(t);
    if (!opts.sb_install_trace) return;
    t->installs=mmap(NULL,INSTALL_TRACE_CAPACITY*sizeof(*t->installs),PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0);
    if (t->installs==MAP_FAILED) die("install trace allocation");
}
static void install_trace_finish(struct fault_trace *t)
{
    if (!t->installs) return;
    pr_info("SB_INSTALL_TRACE_SUMMARY seen=%llu count=%u skipped=%llu dropped=%llu bg_threshold_ns=%llu\n",
        (unsigned long long)t->install_seen,t->install_count,(unsigned long long)t->install_skipped,
        (unsigned long long)t->install_dropped,(unsigned long long)INSTALL_TRACE_BG_THRESHOLD_NS);
    for (unsigned i=0;i<t->install_count;i++) {
        struct install_event *e=&t->installs[i];
        struct sb_install_profile *p=&e->timing;
        pr_info("SB_INSTALL_TRACE pid=%u address=%llu lane=%u role=%u result=%u begin=%llu end=%llu ready_ns=%llu read_gate_ns=%llu write_gate_ns=%llu copy_ns=%llu drain_ns=%llu retire_ns=%llu copies=%llu contexts=%llu retries=%llu\n",
            e->pid,(unsigned long long)e->address,e->lane,e->role,e->result,
            (unsigned long long)e->begun,(unsigned long long)e->ended,
            (unsigned long long)p->ready_ns,(unsigned long long)p->read_gate_ns,
            (unsigned long long)p->write_gate_ns,(unsigned long long)p->copy_ns,
            (unsigned long long)p->drain_ns,(unsigned long long)p->retire_ns,
            (unsigned long long)p->copies,(unsigned long long)p->contexts,(unsigned long long)p->retries);
    }
    munmap(t->installs,INSTALL_TRACE_CAPACITY*sizeof(*t->installs));t->installs=NULL;
}
static void fault_trace_note_at(struct fault_trace *t,unsigned stage,uint64_t pid,uint64_t address,uint64_t ns)
{
    if (!t->events) return;
    if (t->count==t->capacity) { t->dropped++; return; }
    t->events[t->count++]=(struct fault_event){ns,address,pid,stage};
}
static void fault_trace_note(struct fault_trace *t,unsigned stage,uint64_t pid,uint64_t address)
{ if (t->events) fault_trace_note_at(t,stage,pid,address,now_ns()); }
static void fault_trace_finish(struct fault_trace *t,const char *side)
{
    if (t->linux_tid) pr_info("SB_TRACE_OWNER tid=%u role=%s pid=%u\n",t->linux_tid,t->role,t->origin_pid);
    install_trace_finish(t);
    if (!t->events) return;
    pr_info("SB_PF_TRACE_SUMMARY side=%s count=%u dropped=%llu capacity=%u clock=host_monotonic\n",
        side,t->count,(unsigned long long)t->dropped,t->capacity);
    for (unsigned i=0;i<t->count;i++) {
        struct fault_event *e=&t->events[i];
        pr_info("SB_PF_TRACE side=%s stage=%u pid=%u address=%llu ns=%llu\n",side,e->stage,e->pid,
            (unsigned long long)e->address,(unsigned long long)e->ns);
    }
    munmap(t->events,(size_t)t->capacity*sizeof(*t->events));t->events=NULL;
}

/* One owner per QP/CQ; only the synchronous ablation uses pf_client_lock. Never
 * use the old global completion counters across these concurrent lanes. Both
 * data and publication are completed locally before either buffer is reused. */
static void putv(struct resources *r, const struct write_part *parts, unsigned count)
{
    struct ibv_send_wr wr[SB_RDMA_WRITE_MAX], *bad;
    struct ibv_sge sg[SB_RDMA_WRITE_MAX];
    struct ibv_wc wc;
    uint64_t started = now_ns(), spins = 0;
    int rc;
    if (sb_rdma_write_chain(parts,count,r->remote_props.addr,r->remote_props.rkey,PROTOCOL_MAGIC,wr,sg))
        die("RDMA write bounds");
    rc = ibv_post_send(r->qp, wr, &bad);
    if (rc) { errno = rc; die("post RDMA write"); }
    while (!(rc = ibv_poll_cq(r->cq, 1, &wc))) {
        relax_cpu();
        if (!(++spins & 1023) && now_ns() - started > UINT64_C(15000000000)) die("RDMA completion timeout");
    }
    if (rc < 0 || wc.status != IBV_WC_SUCCESS || wc.wr_id != PROTOCOL_MAGIC) die("RDMA completion");
}
static void put(struct resources *r, struct ibv_mr *m1, const void *a1, unsigned n1, uint64_t o1,
                struct ibv_mr *m2, const void *a2, unsigned n2, uint64_t o2)
{
    struct write_part parts[2] = {{m1,a1,n1,o1},{m2,a2,n2,o2}};
    putv(r, parts, n2 ? 2 : 1);
}
#define PUT(r,m,a,n,o) put((r),(m),(const void *)(a),(n),(o),NULL,NULL,0,0)

static struct sb_rdma_tx fault_tx(void)
{
    return (struct sb_rdma_tx){ .qp=PF_res.qp, .cq=PF_res.cq,
        .remote_addr=PF_res.remote_props.addr, .rkey=PF_res.remote_props.rkey };
}
static void fault_post(struct sb_rdma_tx *tx, const struct sb_rdma_part *parts, unsigned n, uint64_t cookie)
{
    int rc = sb_rdma_tx_post(tx, parts, n, cookie);
    if (rc) { errno=-rc; die("async fault post"); }
}
static int fault_poll(struct sb_rdma_tx *tx, uint64_t *cookies, uint64_t *polls)
{
    int n = sb_rdma_tx_poll(tx, cookies);
    if (n < 0) { errno=-n; die("async fault completion"); }
    if (!(++*polls & 4095) && sb_rdma_tx_expired(tx)) { errno=ETIMEDOUT; die("async fault timeout"); }
    return n;
}
static void fault_stats(const char *side, const struct sb_rdma_tx *tx)
{
    pr_info("SB_FAULT_TX side=%s synchronous=%u posted=%llu completed=%llu max_inflight=%u\n",
        side,opts.sb_sync_fault_transport,(unsigned long long)tx->posted,
        (unsigned long long)tx->completed,tx->maximum);
}

int sb_parallel_negotiate(int socket)
{
    struct { uint64_t magic; uint32_t version, enabled; } local = {
        PROTOCOL_MAGIC, 7, opts.sb_parallel_transfer | (opts.sb_no_prefetch << 1) |
        (opts.sb_no_hot_first << 2) | (opts.sb_no_pretransfer << 3) }, remote;
    if (sync_transfer(socket, &local, sizeof(local), true) ||
        sync_transfer(socket, &remote, sizeof(remote), false)) return -1;
    if (memcmp(&local, &remote, sizeof(local)) ||
        (opts.sb_defer_fault_credits && (!opts.sb_parallel_transfer || opts.sb_sync_fault_transport)) ||
        (!opts.sb_parallel_transfer && (opts.sb_no_prefetch || opts.sb_no_hot_first || opts.sb_no_pretransfer)) ||
        (opts.sb_parallel_transfer && (!opts.sb_u_precopy || !opts.sb_image_rdma))) {
        pr_err("SB_TRANSFER incompatible peer/options; requires u-precopy and image-rdma\n");
        return -1;
    }
    return 0;
}

static struct span *locate(int process, uint64_t address)
{
    size_t low, high;
    if (process < 0 || process >= item_num || (address & (P - 1))) return NULL;
    low = begin_span[process]; high = end_span[process];
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (address < spans[mid].start) high = mid;
        else if (address >= spans[mid].end) low = mid + 1;
        else return &spans[mid];
    }
    return NULL;
}
static struct span *job_span(const struct sb_job *job)
{
    size_t low = 0, high = span_count;
    if (job->page >= source_pages) die("job page bounds");
    while (low + 1 < high) {
        size_t mid = low + (high - low) / 2;
        if (spans[mid].first <= job->page) low = mid;
        else high = mid;
    }
    return &spans[low];
}
static uint64_t job_address(const struct sb_job *job, const struct span *s)
{ return s->start + (job->page - s->first) * P; }
static int queue_page(int process, uint64_t address, enum sb_lane lane)
{
    struct span *s = locate(process, address);
    if (!s) return -ENOENT;
    return sb_sched_request(scheduler, s->first + (address - s->start) / P, lane);
}
static void commit(const struct sb_job *job)
{ if (sb_sched_commit(scheduler, job)) die("stale installation ACK"); }

static void build_catalog(void)
{
    size_t maximum = 0, bytes;
    void *memory;
    for (int p = 0; p < item_num; p++) {
        if (!PidVma[p] || PidVma[p]->num_vma < 0 || !SharedRegions->shregions[p]) die("source process metadata");
        maximum += PidVma[p]->num_vma;
    }
    spans = calloc(maximum ? maximum : 1, sizeof(*spans));
    if (!spans) die("page catalog allocation");
    for (int p = 0; p < item_num; p++) {
        begin_span[p] = span_count;
        for (int v = 0; v < PidVma[p]->num_vma; v++) {
            volatile struct vmas_t *area = &PidVma[p]->vmas[v];
            if (!PidVma[p]->can_lazy[v] || !area->prot) continue;
            if (area->end <= area->start || ((area->start | area->end) & (P - 1)) ||
                (span_count > begin_span[p] && spans[span_count - 1].end > area->start)) die("source VMA bounds/order");
            spans[span_count++] = (struct span){ area->start, area->end, source_pages, p, v };
            source_pages += (area->end - area->start) / P;
        }
        end_span[p] = span_count;
    }
    if (!source_pages) return;
    bytes = sb_sched_size(source_pages, 4096);
    if (!bytes) die("scheduler size");
    memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED || !(scheduler = sb_sched_init(memory, bytes, source_pages, 4096))) die("scheduler allocation");
    for (size_t r = 0; r < span_count; r++) {
        struct span *s = &spans[r];
        volatile unsigned long *bitmap = PidVma[s->process]->vmas[s->vma].bitmap;
        for (uint64_t page = 0; page < (s->end - s->start) / P; page++) {
            if ((bitmap[page / (8 * sizeof(long))] >> (page % (8 * sizeof(long)))) & 1UL) {
                if (sb_sched_seed(scheduler, s->first + page)) die("precopy reservation");
                seed_pages++;
            }
        }
    }
    pr_info("SB_TRANSFER catalog pages=%llu precopy_pending=%llu ranges=%zu\n",
            (unsigned long long)source_pages, (unsigned long long)seed_pages, span_count);
}

static void *feed_background(void *unused)
{
    uint64_t hot = 0, fallback = 0;
    (void)unused;
    /* Heat priority is global across processes, not process-major. */
    for (int heat = PRIORITY_QUEUE_LEVEL - 1; !opts.sb_no_hot_first && heat >= 0; heat--) {
        for (int p = 0; p < list_length; p++) {
            int index = -1;
            for (int i = 0; i < item_num; i++) if (pidset[i] == (uint64_t)pid_data_list[p].pid) index = i;
            if (index < 0) continue;
            for (volatile struct score_list *page = pid_data_list[p].dirtylist[heat]; page; page = page->next) {
                int rc;
                do { rc = queue_page(index, page->addr, SB_BACKGROUND); if (rc == -EAGAIN) relax_cpu(); } while (rc == -EAGAIN);
                if (rc < 0 && rc != -ENOENT) die("hot page enqueue");
                hot += rc > 0;
            }
        }
    }
    for (uint64_t page = 0; page < source_pages; page++) {
        int rc;
        do { rc = sb_sched_request(scheduler, page, SB_BACKGROUND); if (rc == -EAGAIN) relax_cpu(); } while (rc == -EAGAIN);
        if (rc < 0) die("background enqueue");
        fallback += rc > 0;
    }
    pr_info("SB_BACKGROUND_ORDER hot_queued=%llu fallback_queued=%llu\n",
        (unsigned long long)hot, (unsigned long long)fallback);
    __atomic_store_n(&feed_done, true, __ATOMIC_RELEASE);
    return NULL;
}

struct fast_pending {
    struct sb_job copying[SB_FAST_SLOTS];
    bool busy[SB_FAST_SLOTS];
    bool dma[SB_FAST_SLOTS];
    unsigned next, ready_next;
};
struct pf_pending {
    struct fast_pending fast;
    struct sb_job sent[MAX_THREADS];
    unsigned acknowledged;
    bool request_observed;
    unsigned request_credited;
};
/* The peer can publish at most MAX_THREADS-1 entries beyond the most recently
 * advertised tail. Consequently equal ring positions mean no credit is due,
 * even across wraps. This acknowledgement only releases consumed request
 * slots; page ownership and response-buffer credit still require install ACK. */
static bool request_credit_due(volatile struct page_request_set_t *wire,struct pf_pending *pending)
{
    for (int p=0;p<item_num;p++) if (pending[p].request_credited!=wire->tail[p]) return true;
    return false;
}
static bool post_request_credit(struct sb_rdma_tx *tx,volatile struct page_request_set_t *wire,
                                struct pf_pending *q,int p)
{
    unsigned tail=wire->tail[p];
    if (q->request_credited==tail || sb_rdma_tx_pending(tx)==SB_RDMA_TX_DEPTH) return false;
    struct sb_rdma_part part={NULL,&tail,sizeof(tail),offsetof(struct page_data_set_t,request_tail)+p*sizeof(int)};
    fault_post(tx,&part,1,0);
    q->request_credited=tail;
    return true;
}
static int fast_submit(struct fast_pending *pending, int process, unsigned lane, const struct sb_job *job)
{
    volatile struct sb_fast_queue *q = &SharedRegions->shregions[process]->fast[lane];
    for (unsigned i = 0; i < SB_FAST_SLOTS; i++) {
        unsigned slot = (pending->next + i) % SB_FAST_SLOTS;
        if (pending->busy[slot]) continue;
        struct span *s = job_span(job);
        pending->copying[slot] = *job;
        if (!lane) fault_trace_note(&source_trace,PF_SOURCE_COPY_SUBMIT,vpidset[process],job_address(job,s));
        else fault_trace_note(&source_ft_trace,FT_SOURCE_COPY_SUBMIT,vpidset[process],job_address(job,s));
        if (!sb_fast_submit(q, slot, vpidset[process], job_address(job,s))) die("fast slot ownership");
        pending->busy[slot] = true; pending->next = (slot + 1) % SB_FAST_SLOTS;
        return 1;
    }
    return 0;
}
/* The receiver's aggregate ACK proves every PS page was installed/adopted.
 * Converting these reserved tickets is O(total pages), and must not stall the
 * request/response coordinator. PRECOPY_PENDING remains unavailable to every
 * transfer lane until the sole ACK worker commits it. All accounting is atomic;
 * background completion still waits for the last reserved ticket to commit. */
static void apply_precopy_ack(uint64_t acknowledged)
{
    uint64_t started = now_ns(), committed = 0;
    if (acknowledged != seed_pages + 1) die("precopy ACK count");
    __atomic_store_n(&source_seed_committing, true, __ATOMIC_RELEASE);
    for (uint64_t page = 0; page < source_pages; page++) {
        if (sb_sched_state(scheduler, page) != SB_PAGE_PRECOPY_PENDING) continue;
        if (sb_sched_seed_commit(scheduler, page)) die("precopy ACK");
        committed++;
    }
    __atomic_store_n(&source_seed_committing, false, __ATOMIC_RELEASE);
    if (committed != seed_pages) die("precopy committed count");
    pr_info("SB_PRECOPY_ACK serial=%u scan_pages=%llu committed=%llu scan_ns=%llu\n",
        opts.sb_serial_precopy_ack, (unsigned long long)source_pages,
        (unsigned long long)committed, (unsigned long long)(now_ns() - started));
}
static void *source_precopy_ack(void *unused)
{
    volatile struct page_request_set_t *wire = (void *)PF_res.buf;
    uint64_t started = now_ns(), acknowledged, spins = 0;
    (void)unused;
    while (!(acknowledged = __atomic_load_n(&wire->precopy_done, __ATOMIC_ACQUIRE))) {
        relax_cpu();
        if (!(++spins & 1023) && now_ns() - started > UINT64_C(15000000000))
            die("precopy ACK timeout");
    }
    apply_precopy_ack(acknowledged);
    return NULL;
}
static void *source_demand(void *unused)
{
    volatile struct page_request_set_t *wire = (void *)PF_res.buf;
    struct pf_pending *pending = calloc(item_num, sizeof(*pending));
    struct sb_job held;
    bool holding = false, seed_ack = false;
    uint64_t seen_state[SB_PAGE_PRECOPY_PENDING + 1] = {0}, neighbors = 0, hints_queued = 0, hints_busy = 0;
    uint64_t requests_during_seed_ack = 0, responses_during_seed_ack = 0;
    uint64_t observed = 0, accepted = 0, queue_retries = 0, tx_blocked_polls = 0;
    uint64_t consumed = 0, credit_updates = 0, admitted_without_tx = 0;
    struct sb_rdma_tx tx = fault_tx();
    uint64_t cookies[SB_RDMA_TX_DEPTH], polls=0;
    unsigned first_process=0;
    (void)unused;
    if (!pending) die("demand bookkeeping");
    if (opts.sb_defer_fault_credits && opts.sb_sync_fault_transport) die("deferred credit requires asynchronous transport");
    fault_trace_bind(&source_trace,"source_demand",0);
    while (!__atomic_load_n(&source_done, __ATOMIC_ACQUIRE)) {
        if (!opts.sb_sync_fault_transport) {
            int n = fault_poll(&tx,cookies,&polls);
            for (int i=0;i<n;i++) if (cookies[i]) {
                unsigned p=(cookies[i]-1)/SB_FAST_SLOTS, c=(cookies[i]-1)%SB_FAST_SLOTS;
                struct sb_job *job=&pending[p].fast.copying[c];
                fault_trace_note(&source_trace,PF_SOURCE_CQ,vpidset[p],job_address(job,job_span(job)));
                sb_fast_release(&SharedRegions->shregions[p]->fast[0],c);
                pending[p].fast.dma[c]=false; pending[p].fast.busy[c]=false;
            }
        }
        if (opts.sb_serial_precopy_ack && !seed_ack && __atomic_load_n(&wire->precopy_done, __ATOMIC_ACQUIRE)) {
            apply_precopy_ack(wire->precopy_done);
            seed_ack = true;
        }
        for (int k = 0; k < item_num; k++) {
            int p=(first_process+k)%item_num;
            struct pf_pending *q = &pending[p];
            volatile struct shregion_t *sh = SharedRegions->shregions[p];
            unsigned ack = __atomic_load_n(&wire->response_tail[p], __ATOMIC_ACQUIRE);
            if (ack >= MAX_THREADS) die("demand response credit");
            while (q->acknowledged != ack) {
                commit(&q->sent[q->acknowledged]);
                q->acknowledged = (q->acknowledged + 1) % MAX_THREADS;
            }
            bool request_waiting = wire->tail[p] != __atomic_load_n(&wire->head[p], __ATOMIC_ACQUIRE);
            bool tx_available = opts.sb_sync_fault_transport || sb_rdma_tx_pending(&tx)<SB_RDMA_TX_DEPTH;
            /* First observation precedes TX-credit and scheduler admission.
             * Retain one observation across EAGAIN retries of this ring slot;
             * it does not mean NIC arrival, and all timestamps stay local. */
            if (source_trace.events && request_waiting) {
                if (!q->request_observed) {
                    fault_trace_note(&source_trace,PF_SOURCE_OBSERVE,vpidset[p],wire->addr[p][wire->tail[p]]);
                    q->request_observed = true;
                    observed++;
                }
                tx_blocked_polls += !tx_available;
            }
            if ((tx_available || opts.sb_defer_fault_credits) && request_waiting) {
                uint64_t address = wire->addr[p][wire->tail[p]];
                struct span *s = locate(p, address);
                enum sb_page_state state = s ? sb_sched_state(scheduler, s->first + (address - s->start) / P) : SB_PAGE_IDLE;
                int rc = s ? queue_page(p, address, SB_DEMAND) : -ENOENT;
                if (rc != -EAGAIN) {
                    if (rc < 0) die("demand address/queue");
                    fault_trace_note(&source_trace,PF_SOURCE_RECEIVE,vpidset[p],address);
                    consumed++; admitted_without_tx += !tx_available;
                    if (q->request_observed) { accepted++; q->request_observed = false; }
                    seen_state[state]++;
                    requests_during_seed_ack += __atomic_load_n(&source_seed_committing, __ATOMIC_ACQUIRE);
                    for (unsigned d = 1; !opts.sb_no_prefetch && d <= 2; d++) {
                        if (address - s->start >= d * P) {
                            rc = queue_page(p, address - d * P, SB_PREFETCH);
                            if (rc < 0 && rc != -EAGAIN) die("lower prefetch enqueue");
                            neighbors++; hints_queued += rc > 0; hints_busy += rc == -EAGAIN;
                        }
                        if (s->end - address > d * P) {
                            rc = queue_page(p, address + d * P, SB_PREFETCH);
                            if (rc < 0 && rc != -EAGAIN) die("upper prefetch enqueue");
                            neighbors++; hints_queued += rc > 0; hints_busy += rc == -EAGAIN;
                        }
                    }
                    wire->tail[p] = (wire->tail[p] + 1) % MAX_THREADS;
                    if (opts.sb_sync_fault_transport)
                        PUT(&PF_res, PF_res.mr_buf, &wire->tail[p], sizeof(int), offsetof(struct page_data_set_t, request_tail) + p * sizeof(int));
                    else if (!opts.sb_defer_fault_credits) {
                        struct sb_rdma_part part={NULL,(const void *)&wire->tail[p],sizeof(int),offsetof(struct page_data_set_t,request_tail)+p*sizeof(int)};
                        fault_post(&tx,&part,1,0);
                    }
                    if (!opts.sb_defer_fault_credits) {
                        q->request_credited=wire->tail[p];credit_updates++;
                    }
                } else if (source_trace.events) queue_retries++;
            }
            volatile struct sb_fast_queue *copies = &sh->fast[0];
            for (unsigned issued = 0; issued < SB_FAST_SLOTS; issued++) {
                unsigned slot = wire->local_head[p], next = (slot + 1) % MAX_THREADS;
                if (next == ack) break;
                if (!opts.sb_sync_fault_transport && sb_rdma_tx_pending(&tx)==SB_RDMA_TX_DEPTH) break;
                if (opts.sb_fixed_ready_scan) q->fast.ready_next=0;
                int selected=sb_fast_pick_ready(copies,q->fast.busy,q->fast.dma,&q->fast.ready_next);
                if (selected<0) break;
                unsigned c=selected;
                volatile struct sb_fast_page *data = &copies->slots[c].page;
                struct sb_job job = q->fast.copying[c];
                struct span *s = job_span(&job);
                if (data->pid != vpidset[p] || data->address != job_address(&job,s)) die("fast demand response");
                fault_trace_note(&source_trace,PF_SOURCE_READY,vpidset[p],data->address);
                q->sent[slot] = job;
                wire->local_head[p] = next;
                fault_trace_note(&source_trace,PF_SOURCE_POST,vpidset[p],data->address);
                if (opts.sb_sync_fault_transport) {
                    put(&PF_res, PF_res.mr[p], (const void *)&data->address, PF_DATA_SIZE,
                    offsetof(struct page_data_set_t, data) + ((uint64_t)p * MAX_THREADS + slot) * PF_DATA_SIZE,
                    PF_res.mr_buf, (const void *)&wire->local_head[p], sizeof(int), offsetof(struct page_data_set_t, head) + p * sizeof(int));
                    fault_trace_note(&source_trace,PF_SOURCE_CQ,vpidset[p],data->address);
                    sb_fast_release(copies,c); q->fast.busy[c] = false;
                } else {
                    struct sb_rdma_part parts[2]={
                        {PF_res.mr[p],(const void *)&data->address,PF_DATA_SIZE,offsetof(struct page_data_set_t,data)+((uint64_t)p*MAX_THREADS+slot)*PF_DATA_SIZE},
                        {NULL,(const void *)&wire->local_head[p],sizeof(int),offsetof(struct page_data_set_t,head)+p*sizeof(int)}};
                    fault_post(&tx,parts,2,1+(uint64_t)p*SB_FAST_SLOTS+c);
                    q->fast.dma[c]=true;
                }
                responses_during_seed_ack += __atomic_load_n(&source_seed_committing, __ATOMIC_ACQUIRE);
            }
            /* Ready demand data has first use of SQ space. The single SQ owner
             * can advertise several consumed requests in one inline write. */
            if (opts.sb_defer_fault_credits)
                credit_updates += post_request_credit(&tx,wire,q,p);
        }
        first_process=(first_process+1)%item_num;
        for (unsigned issued = 0; issued < SB_FAST_SLOTS; issued++) {
            if (!holding && source_pages) {
                int rc = sb_sched_claim(scheduler, SB_LANE_MASK(SB_DEMAND), &held);
                if (rc < 0) die("demand claim");
                holding = rc == 1;
            }
            if (!holding) break;
            struct span *s = job_span(&held);
            if (!fast_submit(&pending[s->process].fast, s->process, 0, &held)) break;
            holding = false;
        }
        relax_cpu();
    }
    while (!opts.sb_sync_fault_transport && (sb_rdma_tx_pending(&tx) || request_credit_due(wire,pending))) {
        int n=fault_poll(&tx,cookies,&polls);
        for (int i=0;i<n;i++) if(cookies[i]) {
            unsigned p=(cookies[i]-1)/SB_FAST_SLOTS,c=(cookies[i]-1)%SB_FAST_SLOTS;
            struct sb_job *job=&pending[p].fast.copying[c];
            fault_trace_note(&source_trace,PF_SOURCE_CQ,vpidset[p],job_address(job,job_span(job)));
            sb_fast_release(&SharedRegions->shregions[p]->fast[0],c);
            pending[p].fast.dma[c]=false;pending[p].fast.busy[c]=false;
        }
        for (int p=0;p<item_num;p++) credit_updates += post_request_credit(&tx,wire,&pending[p],p);
        relax_cpu();
    }
    fault_stats("source",&tx);
    for (unsigned state = 0; state <= SB_PAGE_PRECOPY_PENDING; state++)
        pr_info("SB_DEMAND_SEEN state=%u count=%llu\n", state, (unsigned long long)seen_state[state]);
    pr_info("SB_PREFETCH_HINTS candidates=%llu queued=%llu queue_busy=%llu\n",
        (unsigned long long)neighbors, (unsigned long long)hints_queued, (unsigned long long)hints_busy);
    pr_info("SB_DEMAND_ACK_OVERLAP requests=%llu responses=%llu\n",
        (unsigned long long)requests_during_seed_ack, (unsigned long long)responses_during_seed_ack);
    if (source_trace.events)
        pr_info("SB_PF_OBSERVE observed=%llu accepted=%llu pending=%llu queue_retries=%llu tx_blocked_polls=%llu\n",
            (unsigned long long)observed,(unsigned long long)accepted,(unsigned long long)(observed-accepted),
            (unsigned long long)queue_retries,(unsigned long long)tx_blocked_polls);
    if (credit_updates>consumed || request_credit_due(wire,pending)) die("request credit accounting");
    pr_info("SB_PF_CREDIT deferred=%u accepted=%llu updates=%llu coalesced=%llu admitted_without_tx=%llu pending=%u\n",
        opts.sb_defer_fault_credits,(unsigned long long)consumed,(unsigned long long)credit_updates,
        (unsigned long long)(consumed-credit_updates),(unsigned long long)admitted_without_tx,
        request_credit_due(wire,pending));
    free(pending);
    sb_trace("source.pf_drained");
    __atomic_store_n(&source_demand_drained,true,__ATOMIC_RELEASE);
    return NULL;
}

/* FT pages remain independent of TS batch formation and its copy barrier.
 * Up to four ready pages share one RDMA completion; an incomplete group is
 * sent immediately. No extra staging memcpy and no wait for the other copies. */
struct sb_ft_ring { unsigned head, tail; struct sb_fast_page data[PREFETCH_BUFFER_SIZE]; };
_Static_assert(sizeof(struct sb_ft_ring) <= sizeof(struct prefetch_t_buffer), "FT allocation bounds");
static void *source_prefetch(void *unused)
{
    volatile struct sb_ft_ring *ring = (void *)FT_res.buf;
    struct sb_job sent[PREFETCH_BUFFER_SIZE], held;
    struct fast_pending *pending = calloc(item_num, sizeof(*pending));
    unsigned acknowledged = 0, inflight = 0, maximum = 0, start = 0;
    unsigned active_maximum = 0, window = opts.sb_prefetch_window;
    uint64_t completions = 0, transmitted = 0;
    bool holding = false;
    (void)unused;
    if (!pending) die("prefetch bookkeeping");
    fault_trace_bind(&source_ft_trace,"source_prefetch",0);
    while (!__atomic_load_n(&source_done, __ATOMIC_ACQUIRE)) {
        unsigned tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
        if (tail >= PREFETCH_BUFFER_SIZE) die("prefetch credit");
        while (acknowledged != tail) { commit(&sent[acknowledged]); acknowledged = (acknowledged + 1) % PREFETCH_BUFFER_SIZE; }
        for (unsigned issued = 0; issued < SB_FAST_SLOTS; issued++) {
            /* A claimed FT page cannot be promoted until install ACK. Bound
             * all such pages, including the remote ring, to keep speculative
             * backlog from trapping a later demand fault behind bulk work.
             * Unclaimed queued hints remain promotable by the demand lane. */
            unsigned active = inflight + (ring->head + PREFETCH_BUFFER_SIZE - tail) % PREFETCH_BUFFER_SIZE;
            if (!holding && window && active >= window) break;
            if (!holding && source_pages) {
                int rc = sb_sched_claim(scheduler, SB_LANE_MASK(SB_PREFETCH), &held);
                if (rc < 0) die("prefetch claim");
                holding = rc == 1;
                if (holding) {
                    struct span *s=job_span(&held);
                    fault_trace_note(&source_ft_trace,FT_SOURCE_CLAIM,vpidset[s->process],job_address(&held,s));
                }
            }
            if (!holding) break;
            struct span *s = job_span(&held);
            if (!fast_submit(&pending[s->process], s->process, 1, &held)) break;
            holding = false; inflight++;
            if (inflight > maximum) maximum = inflight;
            if (active + 1 > active_maximum) active_maximum = active + 1;
        }
        struct write_part parts[5];
        struct { unsigned process, slot; } releases[4];
        unsigned count = 0, head = ring->head;
        for (unsigned k = 0; k < (unsigned)item_num && count < 4; k++) {
            unsigned p = (start + k) % item_num;
            volatile struct sb_fast_queue *q = &SharedRegions->shregions[p]->fast[1];
            while (count < 4) {
                unsigned next = (head + 1) % PREFETCH_BUFFER_SIZE;
                if (next == tail) break;
                if (opts.sb_fixed_ready_scan) pending[p].ready_next=0;
                int selected=sb_fast_pick_ready(q,pending[p].busy,pending[p].dma,&pending[p].ready_next);
                if (selected<0) break;
                unsigned c=selected;
                pending[p].dma[c]=true;
                volatile struct sb_fast_page *data = &q->slots[c].page;
                struct sb_job job = pending[p].copying[c];
                if (data->pid != vpidset[p] || data->address != job_address(&job,job_span(&job))) die("fast prefetch response");
                fault_trace_note(&source_ft_trace,FT_SOURCE_READY,vpidset[p],data->address);
                sent[head] = job;
                parts[count] = (struct write_part){FT_res.mr[p], (const void *)data, sizeof(*data),
                    offsetof(struct sb_ft_ring,data) + (uint64_t)head * sizeof(*data)};
                releases[count].process = p; releases[count].slot = c;
                count++; head = next;
            }
        }
        start = (start + 1) % item_num;
        if (count) {
            ring->head = head;
            parts[count] = (struct write_part){FT_res.mr_buf, (const void *)&ring->head, sizeof(ring->head), offsetof(struct sb_ft_ring,head)};
            for (unsigned i=0;i<count;i++) {
                unsigned p=releases[i].process,c=releases[i].slot;
                struct sb_job *job=&pending[p].copying[c];
                fault_trace_note(&source_ft_trace,FT_SOURCE_POST,vpidset[p],job_address(job,job_span(job)));
            }
            putv(&FT_res,parts,count+1);
            for (unsigned i = 0; i < count; i++) {
                unsigned p = releases[i].process, c = releases[i].slot;
                struct sb_job *job=&pending[p].copying[c];
                fault_trace_note(&source_ft_trace,FT_SOURCE_CQ,vpidset[p],job_address(job,job_span(job)));
                sb_fast_release(&SharedRegions->shregions[p]->fast[1],c);
                pending[p].dma[c] = false;
                pending[p].busy[c] = false;
            }
            inflight -= count; completions++; transmitted += count;
        }
        relax_cpu();
    }
    pr_info("SB_TRANSFER prefetch_pipeline pages=%llu completions=%llu max_copy_inflight=%u\n",
        (unsigned long long)transmitted,(unsigned long long)completions,maximum);
    pr_info("SB_PREFETCH_WINDOW limit=%u max_active=%u\n",window,active_maximum);
    free(pending);
    return NULL;
}

struct batch_jobs { unsigned count; struct sb_job jobs[MAX_BATCH]; };
_Static_assert(sizeof(struct transfer_t)+MAX_BATCH*P <= (SB_RDMA_WRITE_MAX-1)*8*P,
               "minimum segment size exceeds WR capacity");
static void *source_background(void *unused)
{
    volatile struct transfer_t_buffer *ring = (void *)TS_res.buf;
    struct batch_jobs sent[TRANSFER_BUFFER_SIZE] = {{0}};
    unsigned acknowledged = 0;
    struct transfer_t *batch = (void *)TransferRegions;
    uint64_t begun = now_ns(), copy_ns = 0, publish_ns = 0, credit_ns = 0;
    uint64_t credit_since = 0, batches = 0, metadata_ns = 0, t;
    uint64_t participants = 0;
    uint64_t data_wrs = 0, wire_bytes = 0;
    uint64_t omitted_bytes = 0;
    unsigned largest_write = 0;
    (void)unused;
    for (;;) {
        struct sb_sched_stats stats = {0};
        unsigned tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE), slot = ring->head;
        struct batch_jobs *jobs = &sent[slot];
        if (tail >= TRANSFER_BUFFER_SIZE) die("background credit");
        while (acknowledged != tail) {
            for (unsigned i = 0; i < sent[acknowledged].count; i++) commit(&sent[acknowledged].jobs[i]);
            sent[acknowledged].count = 0;
            acknowledged = (acknowledged + 1) % TRANSFER_BUFFER_SIZE;
        }
        if (source_pages) sb_sched_stats(scheduler, &stats);
        if (__atomic_load_n(&feed_done, __ATOMIC_ACQUIRE) && stats.committed == source_pages &&
            __atomic_load_n(&((volatile struct page_request_set_t *)PF_res.buf)->precopy_done, __ATOMIC_ACQUIRE) == seed_pages + 1) {
            if (credit_since) { credit_ns += now_ns() - credit_since; credit_since = 0; }
            /* The final TS frame lets the target stop and destroy its QPs.
             * Stop admission and finish all local PF/control completions first,
             * including consumed request credits deferred under SQ pressure.
             * Target workers remain alive until this existing final frame. */
            __atomic_store_n(&source_done,true,__ATOMIC_RELEASE);
            while (!__atomic_load_n(&source_demand_drained,__ATOMIC_ACQUIRE)) relax_cpu();
            sb_trace("source.final_frame_begin");
            t = now_ns();
            batch->nr_pi = batch->nr_page = 0; batch->id = -1;
            ring->head = (slot + 1) % TRANSFER_BUFFER_SIZE;
            put(&TS_res, TS_res.mr[0], batch, sizeof(*batch),
                2 * sizeof(int) + (uint64_t)slot * TRANSFER_REGION_SIZE,
                TS_res.mr_buf, (const void *)&ring->head, sizeof(int), 0);
            while (__atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE) != (unsigned)ring->head) relax_cpu();
            pr_info("SB_TRANSFER source_bg_profile batches=%llu metadata_ns=%llu copy_wait_ns=%llu publish_ns=%llu credit_ns=%llu final_ack_ns=%llu total_ns=%llu copy_participants=%llu broadcast_participants=%llu\n",
                (unsigned long long)batches,(unsigned long long)metadata_ns,(unsigned long long)copy_ns,
                (unsigned long long)publish_ns,(unsigned long long)credit_ns,
                (unsigned long long)(now_ns()-t),(unsigned long long)(now_ns()-begun),
                (unsigned long long)participants,(unsigned long long)(batches * item_num * copy_workers));
            pr_info("SB_BG_SEGMENT pages=%u data_wrs=%llu wire_bytes=%llu max_write_bytes=%u batches=%llu\n",
                opts.sb_bg_segment_pages,(unsigned long long)data_wrs,(unsigned long long)wire_bytes,
                largest_write,(unsigned long long)batches);
            pr_info("SB_BG_WIRE compact=%u prefix_bytes=%zu descriptor_bytes=%zu full_metadata_bytes=%zu omitted_bytes=%llu\n",
                opts.sb_compact_bg_wire,offsetof(struct transfer_t,pid_info),sizeof(struct mem_iov),sizeof(*batch),
                (unsigned long long)omitted_bytes);
            pr_info("SB_TRANSFER complete pages=%llu committed=%llu precopy=%llu demand=%llu prefetch=%llu background=%llu promotions=%llu\n",
                    (unsigned long long)source_pages, (unsigned long long)stats.committed, (unsigned long long)seed_pages,
                    (unsigned long long)stats.claimed[SB_DEMAND], (unsigned long long)stats.claimed[SB_PREFETCH],
                    (unsigned long long)stats.claimed[SB_BACKGROUND], (unsigned long long)stats.promoted);
            return NULL;
        }
        if ((slot + 1) % TRANSFER_BUFFER_SIZE == tail) {
            if (!credit_since) credit_since = now_ns();
            relax_cpu(); continue;
        }
        if (credit_since) { credit_ns += now_ns() - credit_since; credit_since = 0; }
        t = now_ns();
        jobs->count = 0;
        while (jobs->count < batch_pages && source_pages) {
            int rc = sb_sched_claim(scheduler, SB_LANE_MASK(SB_BACKGROUND), &jobs->jobs[jobs->count]);
            if (rc < 0) die("background claim");
            if (!rc) break;
            jobs->count++;
        }
        if (!jobs->count) { relax_cpu(); continue; }
        init_transfer_t_workers(batch, item_num, vpidset, copy_workers);
        batch->id = 1;
        for (unsigned i = 0; i < jobs->count; i++) {
            struct span *s = job_span(&jobs->jobs[i]);
            if (!send_request(batch, vpidset[s->process], job_address(&jobs->jobs[i], s), 1)) die("batch overflow");
        }
        metadata_ns += now_ns() - t; t = now_ns();
        int selected = publish_transfer_selected(batch);
        if (selected < 0) die("background participants");
        participants += selected;
        while (!__atomic_load_n(&batch->is_ready, __ATOMIC_ACQUIRE)) relax_cpu();
        copy_ns += now_ns() - t; t = now_ns();
        /* One ordered RC chain publishes data before head. The final CQ covers
         * both local buffers; remote ring credit still requires install ACK. */
        ring->head = (slot + 1) % TRANSFER_BUFFER_SIZE;
        unsigned bytes=sizeof(*batch)+jobs->count*P;
        uint64_t offset=2*sizeof(int)+(uint64_t)slot*TRANSFER_REGION_SIZE;
        if (opts.sb_bg_segment_pages || opts.sb_compact_bg_wire) {
            struct write_part parts[SB_RDMA_WRITE_MAX];
            struct write_part publish={TS_res.mr_buf,(const void *)&ring->head,sizeof(int),0};
            struct write_part ranges[3]={{TS_res.mr[0],batch,bytes,offset}};
            unsigned count=1;
            if (opts.sb_compact_bg_wire) {
                /* The target reads only prefix counts/ID, live page_info, and
                 * payload. Source helper mailboxes and unused entries remain
                 * local. Keep their destination holes and payload offset so
                 * both target installers retain the same layout and protocol. */
                ranges[0].length=offsetof(struct transfer_t,pid_info);
                ranges[1]=(struct write_part){TS_res.mr[0],batch->page_info,jobs->count*sizeof(struct mem_iov),
                                             offset+offsetof(struct transfer_t,page_info)};
                ranges[2]=(struct write_part){TS_res.mr[0],get_mem(batch),jobs->count*P,offset+sizeof(*batch)};
                count=3;
            }
            int n=sb_rdma_write_ranges(parts,SB_RDMA_WRITE_MAX,ranges,count,opts.sb_bg_segment_pages*P,publish);
            if (n<0) die("background RDMA segments");
            putv(&TS_res,parts,n);
            data_wrs+=n-1;
            unsigned sent_bytes=0;
            for (int i=0;i<n-1;i++) {
                sent_bytes+=parts[i].length;
                if (parts[i].length>largest_write) largest_write=parts[i].length;
            }
            if (sent_bytes>bytes) die("background wire accounting");
            omitted_bytes+=bytes-sent_bytes;
            wire_bytes+=sent_bytes;
        } else {
            put(&TS_res,TS_res.mr[0],batch,bytes,offset,
                TS_res.mr_buf,(const void *)&ring->head,sizeof(int),0);
            data_wrs++;
            if (bytes>largest_write) largest_write=bytes;
            wire_bytes+=bytes;
        }
        publish_ns += now_ns() - t; batches++;
    }
}

int sb_parallel_server(int socket)
{
    pthread_t feeder, demand, prefetch, background, precopy_ack;
    uint32_t count = item_num;
    batch_pages = opts.sb_batch_pages ? opts.sb_batch_pages : 64;
    copy_workers = opts.sb_copy_workers ? opts.sb_copy_workers : 4;
    if (opts.sb_bg_segment_pages && (opts.sb_bg_segment_pages<8 || opts.sb_bg_segment_pages>256)) return -1;
    if (!opts.sb_u_precopy || count > MAX_PROCESS || !count || batch_pages > MAX_BATCH || copy_workers > SB_MAX_COPY_WORKERS) return -1;
    for (unsigned i = 0; i < count; i++) {
        if (!vpidset[i]) return -1;
        for (unsigned j = 0; j < i; j++) if (vpidset[i] == vpidset[j]) return -1;
    }
    if (sync_transfer(socket, &count, sizeof(count), true) ||
        sync_transfer(socket, vpidset, count * sizeof(*vpidset), true)) return -1;
    pr_info("SB_TRANSFER source copy_workers_per_process=%u batch_pages=%u processes=%u\n",
            copy_workers, batch_pages, count);
    pr_info("SB_TRANSFER policy pretransfer=%u prefetch=%u hot_first=%u\n",
        !opts.sb_no_pretransfer, !opts.sb_no_prefetch, !opts.sb_no_hot_first);
    pr_info("SB_READY_SCAN fixed=%u\n",opts.sb_fixed_ready_scan);
    build_catalog();
    /* This one buffer aggregates six PF stages plus coalesced requests from
     * every process. Keep a bounded larger capacity than per-worker traces. */
    fault_trace_init_capacity(&source_trace,4*FAULT_TRACE_CAPACITY);
    fault_trace_init(&source_ft_trace);
    if (!opts.sb_serial_precopy_ack && pthread_create(&precopy_ack, NULL, source_precopy_ack, NULL))
        die("source precopy ACK thread");
    if (pthread_create(&feeder, NULL, feed_background, NULL) || pthread_create(&demand, NULL, source_demand, NULL) ||
        pthread_create(&prefetch, NULL, source_prefetch, NULL) || pthread_create(&background, NULL, source_background, NULL)) die("source threads");
    pthread_join(feeder, NULL); pthread_join(background, NULL);
    pthread_join(demand, NULL); pthread_join(prefetch, NULL);
    if (!opts.sb_serial_precopy_ack) pthread_join(precopy_ack, NULL);
    fault_trace_finish(&source_trace,"source");
    fault_trace_finish(&source_ft_trace,"source");
    stop_transfer_selected((struct transfer_t *)TransferRegions, item_num);
    for (int p = 0; p < item_num; p++)
        for (unsigned lane = 0; lane < SB_FAST_LANES; lane++) {
            volatile struct sb_fast_queue *q = &SharedRegions->shregions[p]->fast[lane];
            uint64_t copied = 0;
            unsigned active = 0;
            for (unsigned worker = 0; worker < SB_FAST_WORKERS; worker++) {
                uint64_t n = __atomic_load_n(&q->completed[worker], __ATOMIC_RELAXED);
                copied += n; active += n != 0;
            }
            pr_info("SB_TRANSFER fast_copies source_pid=%llu lane=%u pages=%llu active_workers=%u\n",
                (unsigned long long)pidset[p],lane,(unsigned long long)copied,active);
            __atomic_store_n(&q->stop, 1, __ATOMIC_RELEASE);
        }
    return 0;
}

static void install(pid_t pid, uint64_t address, const void *data, enum sb_lane lane,
                    struct fault_trace *trace, unsigned role)
{
    struct uffdio_copy copy = { .dst = address, .src = (uintptr_t)data, .len = P };
    struct install_event event={0};
    struct sb_install_profile *profile=trace->installs ? &event.timing : NULL;
    if (profile) event.begun=now_ns();
    int rc = ioctl_mul_profiled(&copy, pid, lane, profile);
    if (profile) {
        event.ended=now_ns();event.pid=pid;event.address=address;event.lane=lane;event.role=role;event.result=rc;
        trace->install_seen++;
        /* Every demand/FT/assist call is retained. Normal BG calls retain only
         * slow installations, so large images cannot exhaust this diagnostic. */
        if (!role && event.ended-event.begun<INSTALL_TRACE_BG_THRESHOLD_NS) trace->install_skipped++;
        else if (trace->install_count==INSTALL_TRACE_CAPACITY) trace->install_dropped++;
        else trace->installs[trace->install_count++]=event;
    }
    if (rc == 0) __atomic_fetch_add(&target_copied[lane], 1, __ATOMIC_RELAXED);
    else if (rc == EEXIST) __atomic_fetch_add(&target_existing[lane], 1, __ATOMIC_RELAXED);
    else if (rc == ENODATA) __atomic_fetch_add(&target_discarded[lane], 1, __ATOMIC_RELAXED);
    else { errno = rc; die("UFFD installation"); }
}
static int bg_range_order(const void *a,const void *b)
{
    const struct bg_range *x=a,*y=b;
    return x->start<y->start ? -1 : x->start!=y->start;
}
static void bg_directory_init(void)
{
    for (int p=0;p<item_num;p++) {
        struct bg_directory *d=&bg_directories[p];
        volatile struct pid_uffd_region_set *set=&PidUffdSet[p];
        unsigned count=0;
        for (int r=0;r<set->nr_uffd_region;r++) count+=set->uffd_region[r].nr_vma;
        d->ranges=calloc(count,sizeof(*d->ranges));
        if (!count || !d->ranges) die("BG directory ranges");
        for (int r=0;r<set->nr_uffd_region;r++)
            for (int v=0;v<set->uffd_region[r].nr_vma;v++) {
                volatile struct vma *m=&set->uffd_region[r].vma[v];
                d->ranges[d->count++]=(struct bg_range){m->start,m->end,0};
            }
        qsort(d->ranges,d->count,sizeof(*d->ranges),bg_range_order);
        for (unsigned i=0;i<d->count;i++) {
            struct bg_range *r=&d->ranges[i];
            if (r->start>=r->end || ((r->start|r->end)&(P-1)) ||
                (i && d->ranges[i-1].end>r->start)) die("BG directory overlap");
            r->first=d->pages;
            if ((r->end-r->start)/P>SIZE_MAX/sizeof(uint64_t)-d->pages) die("BG directory size");
            d->pages+=(r->end-r->start)/P;
        }
        d->cells=mmap(NULL,d->pages*sizeof(*d->cells),PROT_READ|PROT_WRITE,
            MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0);
        if (d->cells==MAP_FAILED) die("BG directory allocation");
    }
    for (unsigned i=0;i<(opts.sb_install_workers?opts.sb_install_workers:4);i++) {
        bg_workers[i].index=i;
        target_trace_init(&bg_workers[i].trace);
    }
}
static uint64_t *bg_cell(int local,uint64_t address)
{
    struct bg_directory *d=&bg_directories[local];
    unsigned low=0,high=d->count;
    while (low<high) {
        unsigned mid=low+(high-low)/2;
        struct bg_range *r=&d->ranges[mid];
        if (address<r->start) high=mid;
        else if (address>=r->end) low=mid+1;
        else return &d->cells[r->first+(address-r->start)/P];
    }
    die("BG original address");
}
static struct transfer_t *bg_batch(unsigned slot)
{ return (void *)(TS_res.buf+2*sizeof(int)+(uint64_t)slot*TRANSFER_REGION_SIZE); }
/* The owner CAS pins the payload against remote reuse. Neither a popped queue
 * descriptor nor an advanced work cursor is an installation acknowledgement. */
static bool bg_take(uint64_t handle,pid_t expected_pid,struct fault_trace *trace,bool assist)
{
    if (!handle) return false;
    unsigned index=sb_bg_index(handle),slot=index/SB_BG_MAX_PAGES,page=index%SB_BG_MAX_PAGES;
    struct bg_descriptor *d=&bg_descriptors[index];
    if (!sb_bg_claim(&d->owner,handle)) return false;
    struct transfer_t *batch=bg_batch(slot);
    struct mem_iov *m=&batch->page_info[page];
    if (page>=(unsigned)batch->nr_pi || (expected_pid && m->pid!=expected_pid)) die("BG claim identity");
    uint64_t begin=opts.sb_fault_trace?now_ns():0;
    install(m->pid,m->addr,(char *)get_mem(batch)+page*P,SB_BACKGROUND,trace,assist?3:0);
    uint64_t word=sb_bg_unbind(d->directory);
    if ((word>>1)!=handle) die("BG directory ownership");
    if (assist || (word&1)) {
        fault_trace_note_at(trace,BG_BATCH_OBSERVED,m->pid,m->addr,bg_observed[slot]);
        fault_trace_note_at(trace,assist?BG_ASSIST_BEGIN:BG_INSTALL_BEGIN,m->pid,m->addr,begin);
        fault_trace_note(trace,assist?BG_ASSIST_END:BG_INSTALL_END,m->pid,m->addr);
    }
    if (!sb_bg_finish(&d->owner,handle)) die("BG duplicate completion");
    if (!__atomic_fetch_sub(&bg_cursors[slot].remaining,1,__ATOMIC_ACQ_REL)) die("BG completion underflow");
    return true;
}

/* The response coordinator never enters UFFD or a lifecycle lock. A slow
 * COPY in one installer cannot stop publication/retirement for other PIDs. */
static void *client_fault_dispatch(void *unused)
{
    fault_trace_bind(&target_pf_dispatch_trace,"demand_dispatch",0);
    volatile struct page_data_set_t *wire=(void *)PF_res.buf;
    unsigned start=0;
    (void)unused;
    while (!__atomic_load_n(&client_done,__ATOMIC_ACQUIRE)) {
        for (unsigned k=0;k<(unsigned)item_num;k++) {
            unsigned p=(start+k)%item_num,peer=pf_installers[p][0].peer;
            struct sb_pf_queue *q=&pf_processes[p].queue;
            unsigned slot=q->published%MAX_THREADS;
            unsigned head=__atomic_load_n(&wire->head[peer],__ATOMIC_ACQUIRE);
            if (head>=MAX_THREADS) die("PF response head");
            while (slot!=head) {
                uint64_t address;
                memcpy(&address,(const void *)wire->data[peer][slot],sizeof(address));
                fault_trace_note(&target_pf_dispatch_trace,PF_TARGET_DISPATCH,pidset[p],address);
                if (!sb_pf_publish(q)) die("PF response publication");
                slot=(slot+1)%MAX_THREADS;
            }
            bool changed=false;
            while (sb_pf_retire(q)) changed=true;
            if (changed) {
                __atomic_store_n(&wire->tail[peer],q->retired%MAX_THREADS,__ATOMIC_RELEASE);
                if (opts.sb_sync_fault_transport) {
                    pthread_mutex_lock(&pf_client_lock);
                    PUT(&PF_res,PF_res.mr_buf,&wire->tail[peer],sizeof(int),
                        offsetof(struct page_request_set_t,response_tail)+peer*sizeof(int));
                    pthread_mutex_unlock(&pf_client_lock);
                }
            }
        }
        start=(start+1)%item_num;
        relax_cpu();
    }
    for (int p=0;p<item_num;p++) {
        struct sb_pf_queue *q=&pf_processes[p].queue;
        unsigned peer=pf_installers[p][0].peer;
        if (q->published!=q->retired || q->claimed!=q->retired ||
            wire->tail[peer]!=wire->head[peer]) die("PF final response credit");
    }
    return NULL;
}
static void *client_fault_install(void *opaque)
{
    struct pf_installer *worker=opaque;
    fault_trace_bind(&worker->trace,"demand_install",pidset[worker->local]);
    int local=worker->local,peer=worker->peer;
    struct sb_pf_queue *q=&pf_processes[local].queue;
    volatile struct page_data_set_t *wire=(void *)PF_res.buf;
    for (;;) {
        uint64_t sequence;
        __atomic_store_n(&worker->active,1,__ATOMIC_RELEASE);
        int claimed=sb_pf_claim(q,&sequence);
        if (claimed<0) die("PF install ownership");
        if (claimed) {
            unsigned slot=sequence%MAX_THREADS;
            uint64_t address;
            memcpy(&address,(const void *)wire->data[peer][slot],sizeof(address));
            fault_trace_note(&worker->trace,PF_TARGET_RECEIVE,pidset[local],address);
            install(pidset[local],address,(const void *)&wire->data[peer][slot][sizeof(address)],
                    SB_DEMAND,&worker->trace,2);
            fault_trace_note(&worker->trace,PF_TARGET_INSTALL,pidset[local],address);
            worker->installed++;
            /* No payload or record reads after DONE: coordinator may return
             * this remote slot as soon as the completed prefix reaches it. */
            if (!sb_pf_complete(q,sequence)) die("PF duplicate installation");
        } else {
            struct sb_pf_aux_job job;
            if (sb_pf_aux_pop(&worker->aux,&job)) {
                if (job.kind==SB_PF_AUX_PRECOPY) {
                    if (!sb_precopy_client_fault(pidset[local],job.value)) die("PF precopy lookup changed");
                } else if (job.kind==SB_PF_AUX_BG_DIRECT || job.kind==SB_PF_AUX_BG_QUEUED) {
                    if (bg_take(job.value,pidset[local],&worker->trace,true)) {
                        uint64_t *counter=job.kind==SB_PF_AUX_BG_DIRECT ?
                            &bg_directories[local].direct : &bg_directories[local].queued;
                        __atomic_fetch_add(counter,1,__ATOMIC_RELAXED);
                    }
                } else die("PF auxiliary kind");
                worker->assisted++;
            } else {
                __atomic_store_n(&worker->active,0,__ATOMIC_RELEASE);
                if (__atomic_load_n(&pf_installers_stop,__ATOMIC_ACQUIRE)) break;
                relax_cpu();continue;
            }
        }
        __atomic_store_n(&worker->active,0,__ATOMIC_RELEASE);
    }
    return NULL;
}
/* Sole producer is this PID's event reader. Prefer a worker without queued or
 * active work; round-robin breaks ties. Full queues fall back to the existing
 * background/PS owner, which still owes installation before source completion. */
static bool pf_aux_enqueue(int local,unsigned kind,uint64_t value)
{
    struct pf_process *p=&pf_processes[local];
    unsigned workers=opts.sb_fault_install_workers,best=0;
    uint64_t minimum=UINT64_MAX;
    for (unsigned k=0;k<workers;k++) {
        unsigned i=(p->next_aux+k)%workers;
        struct pf_installer *w=&pf_installers[local][i];
        uint64_t pending=sb_pf_aux_pending(&w->aux);
        if (pending==SB_PF_AUX_SLOTS) continue;
        uint64_t load=pending+__atomic_load_n(&w->active,__ATOMIC_ACQUIRE);
        if (load<minimum) { minimum=load;best=i; }
    }
    if (minimum==UINT64_MAX) { p->aux_fallback++;return false; }
    if (!sb_pf_aux_push(&pf_installers[local][best].aux,(struct sb_pf_aux_job){value,kind})) die("PF auxiliary producer");
    p->next_aux=(best+1)%workers;p->aux_enqueued++;return true;
}

struct client_process { int local, peer; };
static void *client_demand(void *opaque)
{
    struct client_process *process = opaque;
    int local = process->local, peer = process->peer;
    fault_trace_bind(&target_trace[local],"demand_event",pidset[local]);
    volatile struct page_data_set_t *wire = (void *)PF_res.buf;
    volatile struct pid_uffd_region_set *set = &PidUffdSet[local];
    if (set->pid != pidset[local]) die("client PID index");
    for (int r = 0; r < set->nr_uffd_region; r++) {
        int fd = set->uffd_region[r].uffd, flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) die("UFFD nonblocking");
    }
    while (!__atomic_load_n(&client_done, __ATOMIC_ACQUIRE)) {
        unsigned tail = __atomic_load_n(&wire->tail[peer],__ATOMIC_ACQUIRE);
        if (!opts.sb_fault_install_workers && tail != __atomic_load_n(&wire->head[peer], __ATOMIC_ACQUIRE)) {
            uint64_t address;
            memcpy(&address, (const void *)wire->data[peer][tail], sizeof(address));
            fault_trace_note(&target_trace[local],PF_TARGET_DISPATCH,pidset[local],address);
            fault_trace_note(&target_trace[local],PF_TARGET_RECEIVE,pidset[local],address);
            install(pidset[local], address, (const void *)&wire->data[peer][tail][sizeof(address)], SB_DEMAND,&target_trace[local],2);
            fault_trace_note(&target_trace[local],PF_TARGET_INSTALL,pidset[local],address);
            pf_processes[local].inline_installed++;
            __atomic_store_n(&wire->tail[peer],(tail + 1) % MAX_THREADS,__ATOMIC_RELEASE);
            if (opts.sb_sync_fault_transport) {
                pthread_mutex_lock(&pf_client_lock);
                PUT(&PF_res, PF_res.mr_buf, &wire->tail[peer], sizeof(int), offsetof(struct page_request_set_t, response_tail) + peer * sizeof(int));
                pthread_mutex_unlock(&pf_client_lock);
            }
        }
        if (!opts.sb_serial_background_install && !opts.sb_no_bg_fault_assist &&
            (opts.sb_fault_install_workers || __atomic_load_n(&wire->tail[peer],__ATOMIC_ACQUIRE)==__atomic_load_n(&wire->head[peer],__ATOMIC_ACQUIRE))) {
            uint64_t handle;
            if (sb_bg_assist_pop(&bg_directories[local].assists,&handle)) {
                if (opts.sb_fault_install_workers) pf_aux_enqueue(local,SB_PF_AUX_BG_QUEUED,handle);
                else if (bg_take(handle,pidset[local],&target_trace[local],true)) bg_directories[local].queued++;
            }
        }
        unsigned head = wire->local_head[peer], next = (head + 1) % MAX_THREADS;
        if (next == __atomic_load_n(&wire->request_tail[peer], __ATOMIC_ACQUIRE)) { relax_cpu(); continue; }
        uint64_t addresses[64];
        unsigned available=(__atomic_load_n(&wire->request_tail[peer],__ATOMIC_ACQUIRE)+MAX_THREADS-head-1)%MAX_THREADS;
        unsigned capacity=opts.sb_fault_install_workers ? opts.sb_fault_read_batch : 1;
        if (capacity>available) capacity=available;
        if (!capacity) { relax_cpu();continue; }
        int events=sb_uffd_lifecycle_next_batch(pidset[local],addresses,capacity);
        if (events<0) { errno=-events;die("UFFD lifecycle event"); }
        for (int i=0;i<events;i++) {
            uint64_t address=addresses[i];
            if (opts.sb_fault_install_workers && sb_precopy_client_has_page(pidset[local],address)) {
                pf_aux_enqueue(local,SB_PF_AUX_PRECOPY,address);
                continue;
            }
            if (!opts.sb_fault_install_workers && sb_precopy_client_fault(pidset[local],address)) continue;
            uint64_t handle=sb_bg_request(bg_cell(local,address));
            if (!opts.sb_serial_background_install && !opts.sb_no_bg_fault_assist) {
                if (opts.sb_fault_install_workers) {
                    if (handle && pf_aux_enqueue(local,SB_PF_AUX_BG_DIRECT,handle)) continue;
                } else if (bg_take(handle,pidset[local],&target_trace[local],true)) {
                    bg_directories[local].direct++;
                    continue;
                }
            }
            wire->request_addr[peer][head] = address;
            fault_trace_note(&target_trace[local],PF_TARGET_QUEUE,pidset[local],address);
            __atomic_store_n(&wire->local_head[peer],next,__ATOMIC_RELEASE);
            if (opts.sb_sync_fault_transport) {
                pthread_mutex_lock(&pf_client_lock);
                fault_trace_note(&target_trace[local],PF_TARGET_POST,pidset[local],address);
                put(&PF_res, PF_res.mr_buf, (const void *)&wire->request_addr[peer][head], sizeof(uint64_t),
                offsetof(struct page_request_set_t, addr) + ((uint64_t)peer * MAX_THREADS + head) * sizeof(uint64_t),
                PF_res.mr_buf, (const void *)&wire->local_head[peer], sizeof(int), offsetof(struct page_request_set_t, head) + peer * sizeof(int));
                pthread_mutex_unlock(&pf_client_lock);
            }
            head=next;next=(head+1)%MAX_THREADS;
        }
        relax_cpu();
    }
    return NULL;
}
/* Per-PID workers publish request heads and installed tails with release stores.
 * This sole SQ/CQ owner snapshots each small control inline, so producers never
 * hold a transport lock or wait for another process's RDMA completion. Ring
 * credits prevent address reuse until the source has consumed each request. */
static void *client_fault_tx(void *unused)
{
    fault_trace_bind(&target_tx_trace,"demand_tx",0);
    uint64_t *peer_ids=unused;
    volatile struct page_data_set_t *wire=(void *)PF_res.buf;
    struct sb_rdma_tx tx=fault_tx();
    unsigned sent[MAX_PROCESS]={0},acked[MAX_PROCESS]={0},start=0;
    uint64_t cookies[SB_RDMA_TX_DEPTH],polls=0;
    bool seed_sent=false;
    (void)unused;
    for (;;) {
        bool pending=false;
        fault_poll(&tx,cookies,&polls);
        for (unsigned k=0;k<(unsigned)item_num;k++) {
            unsigned p=(start+k)%item_num;
            unsigned head=__atomic_load_n(&wire->local_head[p],__ATOMIC_ACQUIRE);
            unsigned tail=__atomic_load_n(&wire->tail[p],__ATOMIC_ACQUIRE);
            if (sent[p]!=head) {
                pending=true;
                if (sb_rdma_tx_pending(&tx)<SB_RDMA_TX_DEPTH) {
                    unsigned next=(sent[p]+1)%MAX_THREADS;
                    uint64_t addr=wire->request_addr[p][sent[p]];
                    fault_trace_note(&target_tx_trace,PF_TARGET_POST,peer_ids[p],addr);
                    struct sb_rdma_part parts[2]={
                        {NULL,&addr,sizeof(addr),offsetof(struct page_request_set_t,addr)+((uint64_t)p*MAX_THREADS+sent[p])*sizeof(addr)},
                        {NULL,&next,sizeof(next),offsetof(struct page_request_set_t,head)+p*sizeof(int)}};
                    fault_post(&tx,parts,2,0);sent[p]=next;
                }
            }
            if (acked[p]!=tail) {
                pending=true;
                if (sb_rdma_tx_pending(&tx)<SB_RDMA_TX_DEPTH) {
                    struct sb_rdma_part part={NULL,&tail,sizeof(tail),offsetof(struct page_request_set_t,response_tail)+p*sizeof(int)};
                    fault_post(&tx,&part,1,0);acked[p]=tail;
                }
            }
        }
        uint64_t seed=__atomic_load_n(&wire->precopy_done,__ATOMIC_ACQUIRE);
        if (seed && !seed_sent) {
            pending=true;
            if (sb_rdma_tx_pending(&tx)<SB_RDMA_TX_DEPTH) {
                struct sb_rdma_part part={NULL,&seed,sizeof(seed),offsetof(struct page_request_set_t,precopy_done)};
                fault_post(&tx,&part,1,0);seed_sent=true;
            }
        }
        start=(start+1)%item_num;
        if (__atomic_load_n(&client_producers_done,__ATOMIC_ACQUIRE) && !pending && !sb_rdma_tx_pending(&tx)) break;
        relax_cpu();
    }
    fault_stats("target",&tx);
    return NULL;
}
static void *client_prefetch_install(void *opaque)
{
    struct ft_installer *worker=opaque;
    fault_trace_bind(&worker->trace,"prefetch_install",worker->pid);
    volatile struct sb_ft_ring *ring=(void *)FT_res.buf;
    for (;;) {
        unsigned slot;
        if (!sb_install_pop(&worker->queue,&slot)) {
            if (__atomic_load_n(&ft_installers_stop,__ATOMIC_ACQUIRE)) break;
            relax_cpu();continue;
        }
        if (slot>=PREFETCH_BUFFER_SIZE) die("prefetch install slot");
        volatile struct sb_fast_page *page=&ring->data[slot];
        if (page->pid!=(uint64_t)worker->pid) die("prefetch install PID");
        fault_trace_note(&worker->trace,FT_TARGET_RECEIVE,page->pid,page->address);
        install(page->pid,page->address,(const void *)page->data,SB_PREFETCH,&worker->trace,1);
        fault_trace_note(&worker->trace,FT_TARGET_INSTALL,page->pid,page->address);
        worker->installed++;
        /* No remote DMA reuse until the sole ACK coordinator sees this store.
         * Popping the descriptor above never returns source ring credit. */
        if (!sb_install_done(&ft_completed[slot])) die("duplicate prefetch install completion");
    }
    return NULL;
}
static void *client_prefetch(void *unused)
{
    fault_trace_bind(&target_ft_trace,"prefetch_dispatch",0);
    volatile struct sb_ft_ring *ring = (void *)FT_res.buf;
    pthread_t threads[MAX_PROCESS];
    unsigned dispatch=ring->tail;
    uint64_t dispatched=0,installed=0,acknowledged=0;
    (void)unused;
    if (!opts.sb_serial_prefetch_install)
        for (int p=0;p<item_num;p++)
            if (pthread_create(&threads[p],NULL,client_prefetch_install,&ft_installers[p])) die("prefetch install worker");
    while (!__atomic_load_n(&client_done, __ATOMIC_ACQUIRE)) {
        unsigned slot = ring->tail, head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
        if (head >= PREFETCH_BUFFER_SIZE) die("prefetch head");
        if (slot == head) { relax_cpu(); continue; }
        if (opts.sb_serial_prefetch_install) {
            while (slot != head) {
                volatile struct sb_fast_page *page = &ring->data[slot];
                if (!page->pid || page->pid > INT32_MAX || (page->address & (P - 1))) die("prefetch frame");
                fault_trace_note(&target_ft_trace,FT_TARGET_DISPATCH,page->pid,page->address);
                fault_trace_note(&target_ft_trace,FT_TARGET_RECEIVE,page->pid,page->address);
                install(page->pid, page->address, (const void *)page->data, SB_PREFETCH,&target_ft_trace,1);
                fault_trace_note(&target_ft_trace,FT_TARGET_INSTALL,page->pid,page->address);
                slot = (slot + 1) % PREFETCH_BUFFER_SIZE;
                dispatched++;installed++;acknowledged++;
            }
            dispatch=slot;
        } else {
            while (dispatch!=head) {
                volatile struct sb_fast_page *page=&ring->data[dispatch];
                int local=-1;
                if (!page->pid || page->pid>INT32_MAX || (page->address&(P-1))) die("prefetch frame");
                for (int p=0;p<item_num;p++) if (ft_installers[p].pid==(pid_t)page->pid) { local=p;break; }
                if (local<0) die("prefetch dispatch PID");
                fault_trace_note(&target_ft_trace,FT_TARGET_DISPATCH,page->pid,page->address);
                if (!sb_install_push(&ft_installers[local].queue,dispatch)) die("prefetch descriptor overflow");
                dispatch=(dispatch+1)%PREFETCH_BUFFER_SIZE;dispatched++;
            }
            /* Return only the contiguous installed prefix. A slow process
             * cannot block other workers installing already dispatched pages,
             * but its unacknowledged DMA buffer remains protected. */
            while (slot!=dispatch && sb_install_retire(&ft_completed[slot])) {
                slot=(slot+1)%PREFETCH_BUFFER_SIZE;acknowledged++;
            }
        }
        if (slot==ring->tail) { relax_cpu();continue; }
        ring->tail = slot;
        PUT(&FT_res, FT_res.mr_buf, &ring->tail, sizeof(ring->tail), offsetof(struct sb_ft_ring,tail));
    }
    if (dispatch!=ring->tail || dispatch!=__atomic_load_n(&ring->head,__ATOMIC_ACQUIRE)) die("prefetch final credit");
    __atomic_store_n(&ft_installers_stop,true,__ATOMIC_RELEASE);
    if (!opts.sb_serial_prefetch_install)
        for (int p=0;p<item_num;p++) { pthread_join(threads[p],NULL);installed+=ft_installers[p].installed; }
    if (installed!=dispatched || installed!=acknowledged) die("prefetch install accounting");
    pr_info("SB_PREFETCH_INSTALL serial=%u workers=%u dispatched=%llu installed=%llu acknowledged=%llu\n",
        opts.sb_serial_prefetch_install,opts.sb_serial_prefetch_install?1:item_num,
        (unsigned long long)dispatched,(unsigned long long)installed,(unsigned long long)acknowledged);
    return NULL;
}

static struct { struct transfer_t *batch; unsigned epoch, next, done, workers; bool stop; } installers;
static void *install_worker(void *opaque)
{
    struct bg_worker *worker=opaque;
    fault_trace_bind(&worker->trace,"background_install",0);
    unsigned observed = 0;
    while (!__atomic_load_n(&installers.stop, __ATOMIC_ACQUIRE)) {
        unsigned epoch = __atomic_load_n(&installers.epoch, __ATOMIC_ACQUIRE);
        if (epoch == observed) { relax_cpu(); continue; }
        struct transfer_t *batch = installers.batch;
        unsigned i;
        while ((i = __atomic_fetch_add(&installers.next, 1, __ATOMIC_RELAXED)) < (unsigned)batch->nr_pi) {
            struct mem_iov *page = &batch->page_info[i];
            install(page->pid, page->addr, (char *)get_mem(batch) + i * P, SB_BACKGROUND,&worker->trace,0);
        }
        observed = epoch;
        __atomic_fetch_add(&installers.done, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}
static void *client_background_serial(void *unused)
{
    volatile struct transfer_t_buffer *ring = (void *)TS_res.buf;
    pthread_t threads[32];
    uint64_t begun = now_ns(), install_ns = 0, ack_ns = 0, idle_ns = 0, idle_since = 0, batches = 0, t;
    (void)unused;
    installers.workers = opts.sb_install_workers ? opts.sb_install_workers : 4;
    for (unsigned i = 0; i < installers.workers; i++) if (pthread_create(&threads[i], NULL, install_worker, &bg_workers[i])) die("install workers");
    for (;;) {
        unsigned slot = ring->tail;
        if (slot == __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE)) {
            if (!idle_since) idle_since = now_ns();
            relax_cpu(); continue;
        }
        if (idle_since) { idle_ns += now_ns() - idle_since; idle_since = 0; }
        struct transfer_t *batch = (void *)(TS_res.buf + 2 * sizeof(int) + (uint64_t)slot * TRANSFER_REGION_SIZE);
        bool final = batch->id == -1;
        if (batch->nr_pi < 0 || batch->nr_pi > MAX_BATCH || batch->nr_page != batch->nr_pi || (final && batch->nr_pi)) die("background frame size");
        for (int i = 0; i < batch->nr_pi; i++)
            if (batch->page_info[i].leng != 1 || (batch->page_info[i].addr & (P - 1))) die("background page shape");
        if (!final) {
            t = now_ns();
            installers.batch = batch;
            __atomic_store_n(&installers.next, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&installers.done, 0, __ATOMIC_RELAXED);
            __atomic_fetch_add(&installers.epoch, 1, __ATOMIC_RELEASE);
            while (__atomic_load_n(&installers.done, __ATOMIC_ACQUIRE) != installers.workers) relax_cpu();
            install_ns += now_ns() - t; batches++;
        }
        t = now_ns();
        ring->tail = (slot + 1) % TRANSFER_BUFFER_SIZE;
        PUT(&TS_res, TS_res.mr_buf, &ring->tail, sizeof(int), sizeof(int));
        ack_ns += now_ns() - t;
        if (final) break;
    }
    pr_info("SB_TRANSFER target_bg_profile batches=%llu install_ns=%llu ack_ns=%llu idle_ns=%llu total_ns=%llu\n",
        (unsigned long long)batches,(unsigned long long)install_ns,(unsigned long long)ack_ns,
        (unsigned long long)idle_ns,(unsigned long long)(now_ns()-begun));
    __atomic_store_n(&installers.stop, true, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < installers.workers; i++) pthread_join(threads[i], NULL);
    __atomic_store_n(&client_done, true, __ATOMIC_RELEASE);
    return NULL;
}
static void *bg_pipeline_worker(void *opaque)
{
    struct bg_worker *worker=opaque;
    fault_trace_bind(&worker->trace,"background_install",0);
    unsigned start=worker->index%SB_BG_SLOTS;
    while (!__atomic_load_n(&bg_workers_stop,__ATOMIC_ACQUIRE)) {
        if (!opts.sb_bg_round_robin) start=__atomic_load_n(&bg_oldest,__ATOMIC_ACQUIRE);
        bool found=false;
        for (unsigned k=0;k<SB_BG_SLOTS;k++) {
            unsigned slot=(start+k)%SB_BG_SLOTS;
            uint64_t handle;
            if (!sb_bg_next(&bg_cursors[slot],slot,&handle)) continue;
            found=true;
            if (bg_take(handle,0,&worker->trace,false)) worker->installed++;
            start=(slot+1)%SB_BG_SLOTS;
            break;
        }
        if (!found) relax_cpu();
    }
    return NULL;
}
static void *client_background(void *unused)
{
    if (opts.sb_serial_background_install) return client_background_serial(unused);
    volatile struct transfer_t_buffer *ring=(void *)TS_res.buf;
    pthread_t threads[32];
    unsigned workers=opts.sb_install_workers?opts.sb_install_workers:4;
    installers.workers=workers;
    unsigned receive=ring->tail,retire=ring->tail,active=0,maximum=0;
    uint32_t generation=0;
    uint64_t batches=0,pages=0,begun=now_ns(),publish_ns=0,ack_ns=0;
    bool final=false;
    __atomic_store_n(&bg_oldest,retire,__ATOMIC_RELEASE);
    for (unsigned i=0;i<workers;i++)
        if (pthread_create(&threads[i],NULL,bg_pipeline_worker,&bg_workers[i])) die("BG pipeline worker");
    while (!final) {
        unsigned head=__atomic_load_n(&ring->head,__ATOMIC_ACQUIRE);
        if (head>=SB_BG_SLOTS) die("BG incoming head");
        while (receive!=head) {
            struct transfer_t *batch=bg_batch(receive);
            if (batch->nr_pi<0 || batch->nr_pi>MAX_BATCH || batch->nr_page!=batch->nr_pi) die("BG pipeline frame");
            if (batch->id==-1) {
                if (batch->nr_pi || active || retire!=receive) die("BG premature final frame");
                receive=(receive+1)%SB_BG_SLOTS;retire=receive;final=true;break;
            }
            uint64_t started=now_ns();
            if (!++generation || __atomic_load_n(&bg_cursors[receive].remaining,__ATOMIC_ACQUIRE)) die("BG generation/credit");
            bg_observed[receive]=started;
            __atomic_store_n(&bg_cursors[receive].remaining,batch->nr_pi,__ATOMIC_RELAXED);
            for (unsigned i=0;i<(unsigned)batch->nr_pi;i++) {
                struct mem_iov *m=&batch->page_info[i];
                int local=-1;
                if (m->leng!=1 || (m->addr&(P-1))) die("BG pipeline page");
                for (int p=0;p<item_num;p++) if (pidset[p]==(uint64_t)m->pid) {local=p;break;}
                if (local<0) die("BG pipeline PID");
                struct bg_descriptor *d=&bg_descriptors[receive*SB_BG_MAX_PAGES+i];
                uint64_t handle=sb_bg_handle(generation,receive,i);
                d->directory=bg_cell(local,m->addr);
                sb_bg_publish_owner(&d->owner,handle);
                uint64_t old=sb_bg_bind(d->directory,handle);
                if (old>>1) die("BG duplicate directory binding");
                if ((old&1) && !opts.sb_no_bg_fault_assist &&
                    !sb_bg_assist_push(&bg_directories[local].assists,handle)) bg_directories[local].overflow++;
            }
            sb_bg_publish_cursor(&bg_cursors[receive],generation,batch->nr_pi);
            pages+=batch->nr_pi;batches++;active++;
            if (active>maximum) maximum=active;
            receive=(receive+1)%SB_BG_SLOTS;
            publish_ns+=now_ns()-started;
        }
        while (!final && retire!=receive && !__atomic_load_n(&bg_cursors[retire].remaining,__ATOMIC_ACQUIRE)) {
            retire=(retire+1)%SB_BG_SLOTS;active--;
        }
        if ((unsigned)ring->tail!=retire) {
            __atomic_store_n(&bg_oldest,retire,__ATOMIC_RELEASE);
            uint64_t started=now_ns();
            ring->tail=retire;
            PUT(&TS_res,TS_res.mr_buf,&ring->tail,sizeof(int),sizeof(int));
            ack_ns+=now_ns()-started;
        }
        relax_cpu();
    }
    __atomic_store_n(&bg_workers_stop,true,__ATOMIC_RELEASE);
    for (unsigned i=0;i<workers;i++) pthread_join(threads[i],NULL);
    pr_info("SB_BG_ORDER round_robin=%u\n",opts.sb_bg_round_robin);
    pr_info("SB_BG_PIPELINE batches=%llu pages=%llu max_batches=%u publish_ns=%llu ack_ns=%llu total_ns=%llu\n",
        (unsigned long long)batches,(unsigned long long)pages,maximum,(unsigned long long)publish_ns,
        (unsigned long long)ack_ns,(unsigned long long)(now_ns()-begun));
    __atomic_store_n(&client_done,true,__ATOMIC_RELEASE);
    return NULL;
}

static void *client_precopy_ack(void *unused)
{
    volatile struct page_data_set_t *wire = (void *)PF_res.buf;
    (void)unused;
    if (sb_precopy_client_wait()) die("precopy installation");
    __atomic_store_n(&wire->precopy_done,sb_precopy_client_valid_pages() + 1,__ATOMIC_RELEASE);
    if (opts.sb_sync_fault_transport) {
        pthread_mutex_lock(&pf_client_lock);
        PUT(&PF_res, PF_res.mr_buf, &wire->precopy_done, sizeof(uint64_t), offsetof(struct page_request_set_t, precopy_done));
        pthread_mutex_unlock(&pf_client_lock);
    }
    return NULL;
}
int sb_parallel_client(int socket)
{
    uint32_t count;
    uint64_t peers[MAX_PROCESS];
    struct client_process process[MAX_PROCESS];
    pthread_t demand[MAX_PROCESS], prefetch, background, precopy, fault_tx_thread, fault_dispatch_thread;
    if (sync_transfer(socket, &count, sizeof(count), false) || count != (unsigned)item_num || count > MAX_PROCESS ||
        sync_transfer(socket, peers, count * sizeof(*peers), false)) return -1;
    if (sb_uffd_lifecycle_start()) return -1;
    bg_directory_init();
    fault_trace_init(&target_tx_trace);
    fault_trace_init(&target_pf_dispatch_trace);
    pr_info("SB_FAULT_READ_BATCH configured=%u effective=%u\n",opts.sb_fault_read_batch,opts.sb_fault_install_workers?opts.sb_fault_read_batch:1);
    pf_allocated_workers=opts.sb_fault_install_workers>2 ? opts.sb_fault_install_workers : 2;
    target_trace_init(&target_ft_trace);
    for (int p=0;p<item_num;p++) {
        target_trace_init(&target_trace[p]);
        ft_installers[p].pid=pidset[p];
        /* Allocate equally in the serial ablation, outside the hot path. */
        target_trace_init(&ft_installers[p].trace);
        if (posix_memalign((void **)&pf_installers[p],64,pf_allocated_workers*sizeof(*pf_installers[p]))) die("PF installer allocation");
        memset(pf_installers[p],0,pf_allocated_workers*sizeof(*pf_installers[p]));
        for (unsigned w=0;w<pf_allocated_workers;w++) target_trace_init(&pf_installers[p][w].trace);
    }
    if (!opts.sb_sync_fault_transport && pthread_create(&fault_tx_thread,NULL,client_fault_tx,peers)) die("client fault transmitter");
    for (int p = 0; p < item_num; p++) {
        int found = -1;
        for (unsigned q = 0; q < count; q++) if (peers[q] == pidset[p]) {
            if (found >= 0) return -1;
            found = q;
        }
        if (found < 0) return -1;
        process[p] = (struct client_process){p, found};
        for (unsigned w=0;w<pf_allocated_workers;w++) {
            struct pf_installer *worker=&pf_installers[p][w];
            worker->local=p;worker->peer=found;
            if (w<opts.sb_fault_install_workers && pthread_create(&worker->thread,NULL,client_fault_install,worker)) die("PF installer thread");
        }
        if (pthread_create(&demand[p], NULL, client_demand, &process[p])) die("client demand workers");
    }
    if (opts.sb_fault_install_workers && pthread_create(&fault_dispatch_thread,NULL,client_fault_dispatch,NULL)) die("PF dispatcher thread");
    if (sb_precopy_client_start(opts.sb_precopy_workers ? opts.sb_precopy_workers : 4)) return -1;
    if (pthread_create(&precopy, NULL, client_precopy_ack, NULL) || pthread_create(&prefetch, NULL, client_prefetch, NULL) ||
        pthread_create(&background, NULL, client_background, NULL)) die("client lanes");
    pthread_join(precopy, NULL); pthread_join(background, NULL); pthread_join(prefetch, NULL);
    for (int p = 0; p < item_num; p++) pthread_join(demand[p], NULL);
    if (opts.sb_fault_install_workers) pthread_join(fault_dispatch_thread,NULL);
    __atomic_store_n(&pf_installers_stop,true,__ATOMIC_RELEASE);
    for (int p=0;p<item_num;p++)
        for (unsigned w=0;w<opts.sb_fault_install_workers;w++) pthread_join(pf_installers[p][w].thread,NULL);
    __atomic_store_n(&client_producers_done,true,__ATOMIC_RELEASE);
    if (!opts.sb_sync_fault_transport) pthread_join(fault_tx_thread,NULL);
    /* Close the remaining UFFDs before potentially large diagnostic output.
     * The application can keep discarding/forking after source pages finish;
     * leaving a live UFFD without readers would stall those native faults. */
    int lifecycle_result=sb_uffd_lifecycle_finish();
    uint64_t bg_direct=0,bg_queued=0,bg_overflow=0,bg_normal=0;
    for (int p=0;p<item_num;p++) {
        bg_direct+=bg_directories[p].direct;bg_queued+=bg_directories[p].queued;bg_overflow+=bg_directories[p].overflow;
        munmap(bg_directories[p].cells,bg_directories[p].pages*sizeof(uint64_t));
        free(bg_directories[p].ranges);
    }
    for (unsigned i=0;i<(opts.sb_install_workers?opts.sb_install_workers:4);i++) {
        bg_normal+=bg_workers[i].installed;
        fault_trace_finish(&bg_workers[i].trace,"target");
    }
    pr_info("SB_BG_ASSIST serial=%u disabled=%u direct=%llu queued=%llu normal=%llu overflow=%llu\n",
        opts.sb_serial_background_install,opts.sb_no_bg_fault_assist,(unsigned long long)bg_direct,
        (unsigned long long)bg_queued,(unsigned long long)bg_normal,(unsigned long long)bg_overflow);

    uint64_t pf_dispatched=0,pf_installed=0,pf_retired=0,aux_queued=0,aux_finished=0,aux_fallback=0;
    for (int p=0;p<item_num;p++) {
        struct pf_process *process=&pf_processes[p];
        pf_dispatched+=opts.sb_fault_install_workers ? process->queue.published : process->inline_installed;
        pf_retired+=opts.sb_fault_install_workers ? process->queue.retired : process->inline_installed;
        pf_installed+=process->inline_installed;aux_queued+=process->aux_enqueued;aux_fallback+=process->aux_fallback;
        for (unsigned w=0;w<pf_allocated_workers;w++) {
            pf_installed+=pf_installers[p][w].installed;aux_finished+=pf_installers[p][w].assisted;
            fault_trace_finish(&pf_installers[p][w].trace,"target");
        }
        free(pf_installers[p]);
    }
    if (pf_dispatched!=pf_installed || pf_installed!=pf_retired || aux_queued!=aux_finished) die("PF installer accounting");
    pr_info("SB_DEMAND_INSTALL workers_per_pid=%u processes=%u dispatched=%llu installed=%llu acknowledged=%llu aux_queued=%llu aux_completed=%llu aux_fallback=%llu\n",
        opts.sb_fault_install_workers,item_num,(unsigned long long)pf_dispatched,(unsigned long long)pf_installed,
        (unsigned long long)pf_retired,(unsigned long long)aux_queued,(unsigned long long)aux_finished,(unsigned long long)aux_fallback);
    fault_trace_finish(&target_pf_dispatch_trace,"target");
    fault_trace_finish(&target_tx_trace,"target");
    fault_trace_finish(&target_ft_trace,"target");
    for (int p=0;p<item_num;p++) fault_trace_finish(&ft_installers[p].trace,"target");
    for (int p=0;p<item_num;p++) fault_trace_finish(&target_trace[p],"target");
    pr_info("SB_TRANSFER installed demand=%llu prefetch=%llu background=%llu existing=%llu install_workers=%u discarded=%llu\n",
            (unsigned long long)target_copied[SB_DEMAND], (unsigned long long)target_copied[SB_PREFETCH],
            (unsigned long long)target_copied[SB_BACKGROUND],
            (unsigned long long)(target_existing[0] + target_existing[1] + target_existing[2]), installers.workers,
            (unsigned long long)(target_discarded[0] + target_discarded[1] + target_discarded[2]));
    if (lifecycle_result) return -1;
    return 0;
}
