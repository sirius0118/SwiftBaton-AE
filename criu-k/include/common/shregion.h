#ifndef __CR_SHREGION_H__
#define __CR_SHREGION_H__

// TYPE of transmission
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include "log.h"
#include <common/lock.h>
#include <compel/plugins/std/syscall.h>
#include <compel/plugins/std/string.h>
#include "common/sb-fast-copy.h"


#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#define round_up(x, y)	   ((((x)-1) | __round_mask(x, y)) + 1)
#define round_down(x, y)   ((x) & ~__round_mask(x, y))

/**
 * shregion: communication between dumpee & page server
*/

#define PAGE_FAULT   1
#define PAGE_TRANSFER   2

// VALUE recore
// 单个进程可能有多个线程，多个线程可以独立的提出page fault请求
#define MAX_THREAD_SIZE 100
#define MAX_PROCESS_SIZE 100

// OFFSETS in shregion
#define ISPAGEFAULT 0
#define ISPAGEREADY 8

#define DATA_OFFSET 4096

#define MAX_PROCESS 100
#define MAX_THREADS 100
#define MAX_PREFETCH_CACHE 1000
#define PF_DATA_SIZE (4096 + 8)
#define SHMEM_REGION_SIZE (4096 * 256)
// 1030 = 1024 + 6, 其中5页用来放
#define TRANSFER_REGION_SIZE (4096 * (MAX_BUFFER_SIZE+1+(MAX_PI*20)/4096+1))
#define PREFETCH_REGION_SIZE (4096 * 35)
#define PREFETCH_BUFFER_SIZE 100
extern int item_num;
extern uint64_t pidset[MAX_PROCESS];
extern int socketset[MAX_PROCESS];

// extern void *sharemem_create(unsigned long size);
// extern void *sharemem_receive(unsigned long *size, long pid);

struct request_t {
    uint64_t pid;
    uint64_t address;
};

struct pagedata_t {
    uint64_t pid;
    uint64_t address;
    char page[4096];
};
// 头出尾进
struct work_queue {
    // int64_t length;
    int64_t head;
    int64_t tail;
    uint64_t data[MAX_THREAD_SIZE];
};

struct address_data {
    uint64_t address;
    void * page;
};

struct completion_queue {
    // int64_t length;
    int64_t head;
    int64_t tail;
    struct pagedata_t data[MAX_THREAD_SIZE];
    // uint64_t address[MAX_THREAD_SIZE];
    // char data[MAX_THREAD_SIZE][4096];
};

struct shregion_t {
    uint64_t isPageFault;
    uint64_t isPageReady;
    struct work_queue address_queue;
    struct completion_queue data_queue;
    struct sb_fast_queue fast[SB_FAST_LANES];
};

_Static_assert(sizeof(struct shregion_t) <= SHMEM_REGION_SIZE, "source queues exceed shared mapping");


struct mul_shregion_t {
    int num_process;
    uint64_t * PIDs;
    // 每个指针指向一个与相应进程共享的内存区域
    struct shregion_t ** shregions;
    struct request_t * request;
};

struct page_request_set_t{
    int head[MAX_PROCESS];
    int tail[MAX_PROCESS];
    uint64_t addr[MAX_PROCESS][MAX_THREADS];
    int local_head[MAX_PROCESS];
    int num[101];
    int response_tail[MAX_PROCESS];
    uint64_t precopy_done;
};

struct page_data_set_t{
    int head[MAX_PROCESS];
    int tail[MAX_PROCESS];
    char data[MAX_PROCESS][MAX_THREADS][PF_DATA_SIZE];
    int local_head[MAX_PROCESS];
    uint64_t imm_data[2];
    int num[101];
    int request_tail[MAX_PROCESS];
    uint64_t request_addr[MAX_PROCESS][MAX_THREADS];
    uint64_t precopy_done;
};

struct shmem_plugin_msg {
	unsigned long start;
	unsigned long len;
};

/**
 * shared memory for page server
*/

struct page_server_shregion_t {
    uint64_t head;
    uint64_t tail;
    struct request_t * request;
};

/**
 * shared memory for page client
*/

struct page_client_shregion_t {
    uint64_t head;
    uint64_t tail;
    struct pagedata_t * pagedata;
};

/**
 * page fault client
 */
struct PF_address {
    uint64_t address;
    int to_delete;
    struct PF_address *next;
};

struct PF_address_set {
    uint64_t pid;
    struct PF_address *head,*tail;
};

/**
 * action of PF_address_set
 * 
 */

static inline int PF_address_set_init(struct PF_address_set *set, uint64_t pid){
    set->pid = pid;
    set->head=NULL;
    set->tail=NULL;
    return 0;
}

static inline int PF_address_set_insert(struct PF_address_set *set, uint64_t address){
    struct PF_address *node = (struct PF_address *)malloc(sizeof(struct PF_address));
    node->address = address;
    node->to_delete = 0;
    node->next=NULL;
    if(set->head==NULL){
        set->head=node;
        set->tail=node;
    }else{
        set->tail->next=node;
        set->tail=node;
    }
    return 0;
}

static inline int PF_address_set_delete(struct PF_address_set *set,struct PF_address *prenode, struct PF_address *node){
    // pr_err("prepare to delete\n");
    // if(node==NULL){
    //     pr_err("node is NULL\n");
    // }
    if(node->next==NULL){
        // pr_err("to delete\n");
        node->to_delete=1;
        return 1;
    }
    // pr_err("can delete\n");
    if(prenode==NULL){
        set->head=node->next;
    }else{
        prenode->next=node->next;
    }
    // pr_err("delete success\n");
    free(node);
    return 0;
}

/**
 * action of shregion between dumpee & page server
*/

static inline void shregion_t_init(void * mem){
    struct shregion_t * shregion = (struct shregion_t *)mem;
    shregion->isPageFault = 0;
    shregion->isPageReady = 0;
    shregion->address_queue.head = 0;
    shregion->address_queue.tail = 0;
    shregion->data_queue.head = 0;
    shregion->data_queue.tail = 0;
    // data指向向上取整的一个页面
    // shregion->data = (void *)(round_up((uint64_t)(&shregion->data), 4096));
}

static inline void mul_shregion_t_init(void * mem, uint64_t num_process, uint64_t * pids){
    struct mul_shregion_t * mul_shregion = (struct mul_shregion_t *)mem;
    mul_shregion->num_process = num_process;
    mul_shregion->PIDs = (uint64_t *)mmap(NULL, sizeof(uint64_t) * num_process, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // mul_shregion->PIDs = (uint64_t *)malloc(sizeof(uint64_t) * num_process);
    for(int i = 0; i < num_process; i++){
        mul_shregion->PIDs[i] = pids[i];
    }
    mul_shregion->shregions = (struct shregion_t **)mmap(NULL, sizeof(struct shregion_t *) * num_process, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // mul_shregion->shregions = (struct shregion_t *)malloc(sizeof(struct shregion_t) * num_process);
}

static inline void WQenqueue(volatile struct work_queue *queue, long address){
    volatile int64_t *tail=&(queue->tail);
    queue->data[*tail] = address;
    queue->tail = (*tail + 1) % MAX_THREAD_SIZE;
}

static inline uint64_t WQdequeue(volatile struct work_queue *queue){
    volatile int64_t *head=&queue->head;
    uint64_t address = queue->data[*head];
    __atomic_store_n(&queue->head, (*head + 1) % MAX_THREAD_SIZE, __ATOMIC_RELEASE);
    return address;
}

static inline void CQenqueue(volatile struct completion_queue *queue, uint64_t pid, uint64_t data){
    // queue->address[queue->tail] = address;
    volatile int64_t *tail=&queue->tail;
    while ((*tail + 1) % MAX_THREAD_SIZE == __atomic_load_n(&queue->head, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
    char *page=(char*)queue->data[*tail].page;
    memcpy((void*)page, (void *)data, 4096);
    queue->data[*tail].address = data;
    queue->data[*tail].pid = pid;
    // queue->data[queue->tail] = data;
    __atomic_store_n(&queue->tail, (*tail + 1) % MAX_THREAD_SIZE, __ATOMIC_RELEASE);
}

static inline volatile struct pagedata_t * CQdequeue(volatile struct completion_queue *queue){
    volatile struct pagedata_t *result;
    volatile int64_t *head=&queue->head;
    result = &queue->data[*head];
    queue->head = (*head + 1) % MAX_THREAD_SIZE;
    return result;
}

// extern void GetRequest(struct shregion_t * shregion, uint64_t address){
//     shregion -> isPageFault += 1;
//     WQenqueue(&shregion->address_queue, address);
// }

// // 这个函数要在parasite中实现
// extern void HandleRequest(struct shregion_t * shregion){
//     uint64_t address;
//     address = WQdequeue(&shregion->address_queue);
//     // memcpy data of address to completion queue
//     // 
// }

// ############## For Tansfer Page ################
#define SB_MAX_COPY_WORKERS 32U
#define MAX_PI 1024
#define MAX_BUFFER_SIZE 1024
#define TRANSFER_BUFFER_SIZE 10

struct mem_iov{
    // int nr_before;
    int pid;
    uint64_t addr;
    uint64_t leng;
};

// 实际的页面放在 transfer_t 数据结构后面的空间, 申请这块区域的时候最好按页对齐，最小化页面浪费
// 关于这个shmem的传送应该是按次的，每次传送应该互斥的在这个区域内放入请求
extern mutex_t tsmutex;

struct transfer_t{
    int id;
    int is_fulled;
    int is_ready;
    int nr_pi;
    int nr_page;
    int pending_workers;
    unsigned copy_workers;
    uint64_t generation;
    struct{
        uint64_t pid;
        int done;
        /* Stable mailbox: init/clean must never clear it between batches. */
        uint64_t dispatch;
    }pid_info[MAX_PROCESS];
    struct mem_iov page_info[MAX_PI];
};

_Static_assert(sizeof(struct transfer_t) + MAX_BUFFER_SIZE * 4096ULL <= TRANSFER_REGION_SIZE,
               "background metadata and payload exceed shared mapping");

struct transfer_t_buffer{
    int head;
    int tail;
};


// get_mem函数不能进行内存对齐，否则会出错
// #define get_mem(p) (struct transfer_t*)((uint64_t)((uint64_t)(p) + sizeof(struct transfer_t) + (p)->nr_pi * sizeof(struct mem_iov)))
#define get_mem(p) (struct transfer_t*)((uint64_t)((uint64_t)(p) + sizeof(struct transfer_t) ))


static inline void init_transfer_t_workers(struct transfer_t *transfer, int item_num,
                                           uint64_t *pidset, unsigned workers)
{
    // struct transfer_t * transfer = (struct transfer_t *)mem;
    transfer->is_ready = 0;
    transfer->nr_pi = 0;
    // is_fulled还可以作为结束的中止信号。如果这个值为-1，那么就跳出
    transfer->is_fulled = 0;
    transfer->nr_page = 0;
    transfer->copy_workers = workers;
    transfer->pending_workers = item_num * workers;
    for (int i = 0; i < MAX_PROCESS; i++){
        if ( i < item_num)
            transfer->pid_info[i].pid = pidset[i];
        else
            transfer->pid_info[i].pid = 0;
        transfer->pid_info[i].done = 0;
    }

}

/* The legacy path creates one helper per process. */
static inline void init_transfer_t(struct transfer_t *transfer, int item_num, uint64_t *pidset)
{
    init_transfer_t_workers(transfer, item_num, pidset, 1);
}

// static inline void init_transfer_t_buffer(struct transfer_t_buffer *buffer)
// {
//     buffer->head = 0;
//     buffer->tail = 0;
// }

static inline void clean_transfer_t(struct transfer_t *ts, int item_num)
{
    if(ts->is_fulled != -1)
    {
        ts->is_fulled = 0;
        ts->nr_pi = 0;
        ts->nr_page = 0;
        ts->is_ready = 0;
        ts->pending_workers = item_num * ts->copy_workers;

        for(int i = 0; i < item_num; i++){
            ts->pid_info[i].done = 0;
        }
    }
}

/* Publish only after the complete batch is immutable. Each dumpee remembers
 * the generation, so a delayed old poll cannot read a batch being rebuilt. */
static inline void publish_transfer(struct transfer_t *ts)
{
    __atomic_store_n(&ts->is_fulled, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&ts->generation, 1, __ATOMIC_RELEASE);
}

static inline int send_request(struct transfer_t *ts, int pid, uint64_t addr, uint64_t leng)
{
    if(ts->nr_page + leng > MAX_BUFFER_SIZE)
    {
        publish_transfer(ts);
        return 0;
    }
    ts->page_info[ts->nr_pi].pid = pid;
    // pr_err("pid:%d\n",ts->page_info[ts->nr_pi].pid);
    ts->page_info[ts->nr_pi].addr = addr;
    ts->page_info[ts->nr_pi].leng = leng;
    // if (likely(ts->nr_pi > 0))
    //     ts->page_info[ts->nr_pi].nr_before = ts->page_info[ts->nr_pi - 1].nr_before + ts->page_info[ts->nr_pi - 1].leng;
    // else
    //     ts->page_info[ts->nr_pi].nr_before = 0;
    ts->nr_pi += 1;
    ts->nr_page += leng;
    // if(ts->nr_page == MAX_BUFFER_SIZE)
    // {
    //     ts->is_fulled = 1;
    // }
    // mutex_unlock(&tsmutex);    
    return 1;
}

/* Each worker owns a disjoint contiguous slice of the complete payload. An
 * iov can span several workers; each reads only its own process's pages. The
 * shared batch remains immutable until every participant has stopped reading
 * it. In particular idle workers must also acknowledge each generation. */
static inline int serve_request_worker(struct transfer_t *ts, uint64_t pid,
                                      unsigned worker, uint64_t *seen)
{
    int process = -1, nr;
    uint64_t off = 0, begin, end;
    unsigned workers, bit;
    uint64_t generation = __atomic_load_n(&ts->generation, __ATOMIC_ACQUIRE);
    if (generation == *seen) return 0;
    *seen = generation;
    workers = ts->copy_workers;
    if (!workers || workers > SB_MAX_COPY_WORKERS || worker >= workers) return -1;
    bit = 1U << worker;
    nr = ts->nr_pi;
    for (int i = 0; i < MAX_PROCESS && ts->pid_info[i].pid; i++) {
        if (ts->pid_info[i].pid == pid) { process = i; break; }
    }
    if (process < 0) return -1;
    if (__atomic_load_n(&ts->pid_info[process].done, __ATOMIC_ACQUIRE) & bit) return 0;
    begin = (uint64_t)ts->nr_page * worker / workers;
    end = (uint64_t)ts->nr_page * (worker + 1) / workers;
    for (int i = 0; i < nr; i++) {
        uint64_t next = off + ts->page_info[i].leng;
        uint64_t first = off > begin ? off : begin;
        uint64_t last = next < end ? next : end;
        if (ts->page_info[i].pid == pid && first < last)
            memcpy((char *)get_mem(ts) + first * 4096,
                   (void *)(ts->page_info[i].addr + (first - off) * 4096),
                   (last - first) * 4096);
        off = next;
    }
    __atomic_fetch_or(&ts->pid_info[process].done, bit, __ATOMIC_RELEASE);
    if (__atomic_fetch_sub(&ts->pending_workers, 1, __ATOMIC_ACQ_REL) == 1) {
        __atomic_store_n(&ts->is_fulled, 0, __ATOMIC_RELAXED);
        /* No access to the batch is permitted after publishing readiness. */
        __atomic_store_n(&ts->is_ready, 1, __ATOMIC_RELEASE);
    }
    return 1;
}

static inline int serve_request(struct transfer_t *ts, uint64_t pid, uint64_t *seen)
{
    return serve_request_worker(ts, pid, 0, seen);
}

/* Unlike the legacy broadcast, this publishes a stable per-process mailbox.
 * The low bits select the workers whose payload slices contain this PID.
 * Unselected workers read no mutable descriptor, so they need not join the
 * reuse barrier. Selected workers must all ACK before any batch is rebuilt. */
static inline int publish_transfer_selected(struct transfer_t *ts)
{
    unsigned masks[MAX_PROCESS] = {0}, participants = 0;
    unsigned workers = ts->copy_workers;
    uint64_t off = 0, generation = ts->generation;
    if (!workers || workers > SB_MAX_COPY_WORKERS || ts->nr_pi <= 0 ||
        ts->nr_pi > MAX_PI || ts->nr_page <= 0 || ts->nr_page > MAX_BUFFER_SIZE ||
        generation >= UINT32_MAX - 1) return -1;
    for (int i = 0; i < ts->nr_pi; i++) {
        struct mem_iov *page = &ts->page_info[i];
        int process = -1;
        if (!page->leng || page->leng > (uint64_t)ts->nr_page - off) return -1;
        uint64_t next = off + page->leng;
        for (int p = 0; p < MAX_PROCESS && ts->pid_info[p].pid; p++)
            if (ts->pid_info[p].pid == (uint64_t)page->pid) { process = p; break; }
        if (process < 0) return -1;
        for (unsigned w = 0; w < workers; w++) {
            uint64_t begin = (uint64_t)ts->nr_page * w / workers;
            uint64_t end = (uint64_t)ts->nr_page * (w + 1) / workers;
            if (begin < end && off < end && next > begin) masks[process] |= 1U << w;
        }
        off = next;
    }
    if (off != (uint64_t)ts->nr_page) return -1;
    for (int p = 0; p < MAX_PROCESS; p++) participants += __builtin_popcount(masks[p]);
    if (!participants) return -1;
    ts->pending_workers = participants;
    ts->generation = ++generation;
    ts->is_fulled = 1;
    for (int p = 0; p < MAX_PROCESS; p++)
        if (masks[p]) __atomic_store_n(&ts->pid_info[p].dispatch,
            (generation << 32) | masks[p], __ATOMIC_RELEASE);
    return participants;
}

static inline int serve_request_selected(struct transfer_t *ts, unsigned process,
                                        uint64_t pid, unsigned worker, uint64_t *seen)
{
    uint64_t token, off = 0, begin, end;
    unsigned workers, bit;
    if (process >= MAX_PROCESS || worker >= SB_MAX_COPY_WORKERS) return -1;
    token = __atomic_load_n(&ts->pid_info[process].dispatch, __ATOMIC_ACQUIRE);
    if (token == *seen) return 0;
    if (token == UINT64_MAX) return -1;
    *seen = token;
    bit = 1U << worker;
    if (!((uint32_t)token & bit)) return 0;
    /* Only selected workers may touch anything below this point. */
    workers = ts->copy_workers;
    if (!workers || workers > SB_MAX_COPY_WORKERS || worker >= workers ||
        ts->pid_info[process].pid != pid) return -1;
    begin = (uint64_t)ts->nr_page * worker / workers;
    end = (uint64_t)ts->nr_page * (worker + 1) / workers;
    for (int i = 0; i < ts->nr_pi; i++) {
        uint64_t next = off + ts->page_info[i].leng;
        uint64_t first = off > begin ? off : begin;
        uint64_t last = next < end ? next : end;
        if (ts->page_info[i].pid == pid && first < last)
            memcpy((char *)get_mem(ts) + first * 4096,
                   (void *)(ts->page_info[i].addr + (first - off) * 4096),
                   (last - first) * 4096);
        off = next;
    }
    __atomic_fetch_or(&ts->pid_info[process].done, bit, __ATOMIC_RELEASE);
    if (__atomic_fetch_sub(&ts->pending_workers, 1, __ATOMIC_ACQ_REL) == 1) {
        ts->is_fulled = 0;
        /* No descriptor or bookkeeping access after publishing readiness. */
        __atomic_store_n(&ts->is_ready, 1, __ATOMIC_RELEASE);
    }
    return 1;
}

/* Call only after all selected copies and the final RDMA publish complete. */
static inline void stop_transfer_selected(struct transfer_t *ts, unsigned processes)
{
    for (unsigned p = 0; p < processes; p++)
        __atomic_store_n(&ts->pid_info[p].dispatch, UINT64_MAX, __ATOMIC_RELEASE);
}


// ############# for TS shared memory  ################

#define SHM_SIZE 4096

static inline void * para_sharemem_open(char *path, int size)
{
    int dirfd, shm_fd;
    void *shm_ptr;
    // TODO: 创建共享内存
    // step1: 打开 /dev/shm，获得文件描述符
    dirfd = sys_open("/dev/shm", O_RDONLY | O_DIRECTORY, 0);
    
    // step2: 打开共享内存对象
    shm_fd = sys_openat(dirfd, path, O_RDWR | O_CREAT, 0666);
    sys_close(dirfd);
    // step3: 使用ftruncate函数调整共享内存对象的大小
    // sys_ftruncate(shm_fd, size);

    // step4: 使用mmap函数映射共享内存对象
    shm_ptr = (void *)sys_mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    sys_close(shm_fd);
    return shm_ptr;
}

static inline int para_sharemem_close(char *path)
{
    sys_unlink(path);
    return 0;
}

static inline void * sharemem_open(char *path, int size)
{
    int dirfd, shm_fd;
    void *shm_ptr;
    dirfd = open("/dev/shm", O_RDONLY | O_DIRECTORY);
    if (dirfd == -1) {
        pr_err("open /dev/shm");
        return NULL;
    }

    // 使用 openat 系统调用创建或打开共享内存对象
    shm_fd = syscall(SYS_openat, dirfd, path, O_CREAT | O_RDWR, 0666);
    close(dirfd);
    if (shm_fd == -1) {
        pr_err("syscall openat");
        return NULL;
    }

    // 使用 ftruncate 系统调用调整共享内存对象的大小
    if (ftruncate(shm_fd, size) == -1) {
        pr_err("syscall ftruncate");
        close(shm_fd);
        return NULL;
    }

    // 将共享内存对象映射到进程的地址空间
    shm_ptr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        pr_err("mmap");
        close(shm_fd);
        return NULL;
    }
    close(shm_fd);
    return shm_ptr;
}

static inline int sharemem_close(char *path){
    int result = syscall(SYS_unlink, path);
    return result;
}

static inline int pid2vpid(uint64_t pid, uint64_t *pidset, uint64_t *vpidset){
    int i;
    // pr_err("pid2vpid pid:%d,pidset[0]:%d\n",(int)pid,(int)pidset[0]);
    for(i = 0; i < MAX_PROCESS; i++){
        if (pidset[i] == 0)
            break;
        if (pidset[i] == pid)
            return vpidset[i];
    }
    return -1;
}

// ############# For Page Prefetching ################

#define MAX_CACHE_SIZE 1000

struct PF_FT_area {
	unsigned long addr[MAX_CACHE_SIZE];
	int head, tail;
};

struct pid_FT_area {
	unsigned long pid;
	struct PF_FT_area area;
};

static inline void enqueueFT(struct PF_FT_area *area, unsigned long addr)
{
    if((area->tail + 1) % MAX_CACHE_SIZE==area->head){
        return;
    }
	area->addr[area->tail] = addr;
	area->tail = (area->tail + 1) % MAX_CACHE_SIZE;
}

static inline unsigned long dequeueFT(struct PF_FT_area *area)
{
	unsigned long addr = area->addr[area->head];
	area->head = (area->head + 1) % MAX_CACHE_SIZE;
	return addr;
}




struct prefetch_t{
    int is_request;
    int is_ready;
    pid_t pid;
    uint64_t address1;
    uint64_t length1;
    uint64_t address2;
    uint64_t length2;
    uint64_t request_token;
    char data1[4096 * 16];
    char data2[4096 * 16];
};

struct prefetch_t_buffer{
    int head;
    int tail;
    struct prefetch_t data[PREFETCH_BUFFER_SIZE];
};

/* Target PID and generation are one atomic command: non-target helpers never
 * inspect mutable request fields while a different helper serves the request. */
static inline void publish_prefetch(volatile struct prefetch_t *pst)
{
    uint64_t token = __atomic_load_n(&pst->request_token, __ATOMIC_RELAXED);
    pst->is_ready = 0;
    pst->is_request = 1;
    token = ((token >> 32) + 1) << 32 | (uint32_t)pst->pid;
    __atomic_store_n(&pst->request_token, token, __ATOMIC_RELEASE);
}

static inline void send_prefetch(struct prefetch_t * pst, int type, pid_t pid, uint64_t address, uint64_t length)
{
    int ret, i;

    if (type == 1){
        pst->pid = pid;
        pst->address1 = address;
        pst->length1 = length;
    }else{
        pst->address2 = address;
        pst->length2 = length;
        publish_prefetch(pst);
    }
}

static inline void serve_prefetch(volatile struct prefetch_t * pst, uint64_t pid)
{
    volatile uint64_t *address1, *address2;
    volatile uint64_t *length1, *length2;
    volatile int * is_ready, *is_request;

    address1 = &pst->address1;
    length1 = &pst->length1;
    address2 = &pst->address2;
    length2 = &pst->length2;
    is_ready = &pst->is_ready;
    is_request = &pst->is_request;
    // pr_err("pie pst pid:%d  self pid:%lx\n",pst->pid,pid);
    if(*address1){
        memcpy((void *)pst->data1, (void *)pst->address1, pst->length1);
        #ifdef FT_DEBUG
        pr_err("FT服务1: src1:%lx, dst1:%lx\n",
            (uint64_t)pst->data1, pst->address1);
            #endif
    }
    if(*address2){
        memcpy((void *)pst->data2, (void *)pst->address2, pst->length2);
        #ifdef FT_DEBUG
        pr_err("FT服务2: src2:%lx, dst1:%lx\n",
            (uint64_t)pst->data2, pst->address2);
            #endif
    }
    __atomic_store_n(is_request, 0, __ATOMIC_RELAXED);
    __atomic_store_n(is_ready, 1, __ATOMIC_RELEASE);
    // if (pst->is_request == 1&&pst->pid==pid){
    //     // send prefetch request to dumpee
        
        
    // }
}

#endif
