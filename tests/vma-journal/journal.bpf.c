#include <linux/bpf.h>
#include "syscalls.h"
#include "journal.h"
#define SEC(name) __attribute__((section(name), used))
static void *(*lookup)(void *, const void *) = (void *)BPF_FUNC_map_lookup_elem;
static long (*update)(void *, const void *, const void *, unsigned long long) = (void *)BPF_FUNC_map_update_elem;
static long (*remove_key)(void *, const void *) = (void *)BPF_FUNC_map_delete_elem;
static unsigned long long (*pid_tgid)(void) = (void *)BPF_FUNC_get_current_pid_tgid;
static unsigned (*cpu_id)(void) = (void *)BPF_FUNC_get_smp_processor_id;
static long (*read_kernel)(void *, unsigned, const void *) = (void *)BPF_FUNC_probe_read;
struct bpf_map_def { unsigned type, key_size, value_size, max_entries, map_flags; };
struct pending_call { struct journal_key key; unsigned long long nr, global_scope, args[6]; };
/* Native x86-64 layout; the consumer must reject compat/x32 execution. */
struct syscall_regs { unsigned long long r15,r14,r13,r12,bp,bx,r11,r10,r9,r8,ax,cx,dx,si,di,orig_ax,ip,cs,flags,sp,ss; };
struct bpf_map_def SEC("maps") watched = {BPF_MAP_TYPE_HASH,4,4,4096,0};
struct bpf_map_def SEC("maps") pending = {BPF_MAP_TYPE_HASH,8,sizeof(struct pending_call),16384,0};
struct bpf_map_def SEC("maps") epoch = {BPF_MAP_TYPE_ARRAY,4,sizeof(struct journal_epoch),1,0};
struct bpf_map_def SEC("maps") journal = {BPF_MAP_TYPE_HASH,sizeof(struct journal_key),sizeof(struct journal_event),SB_VMA_JOURNAL_CAPACITY,0};
struct bpf_map_def SEC("maps") sequences = {BPF_MAP_TYPE_PERCPU_ARRAY,4,8,1,0};
SEC("raw_tracepoint/sys_enter")
int journal_enter(struct bpf_raw_tracepoint_args *ctx)
{
    unsigned long long tid=pid_tgid(); unsigned pid=tid>>32,zero=0;
    long nr=ctx->args[1]; struct syscall_regs r={};
    if(!sb_vma_changes(nr))return 0;
    int watched_pid=lookup(&watched,&pid)!=0;
    if(!watched_pid && nr!=440 && nr!=16)return 0;
    struct journal_epoch *s=lookup(&epoch,&zero);if(!s)return 0;
    if(read_kernel(&r,sizeof(r),(void *)ctx->args[0])) {
        __sync_fetch_and_add(&s->poisoned,1);return 0;
    }
    /* An external holder can issue UFFD ioctls against a watched mm. A global
     * conservative invalidation avoids silently reusing its old VMA state. */
    if(!watched_pid && nr==16 && ((r.si>>8)&255)!=0xaa)return 0;
    __sync_fetch_and_add(&s->active,1);
    __sync_fetch_and_add(&s->generation,1);
    __sync_fetch_and_add(&s->events,1);
    unsigned long long *sequence=lookup(&sequences,&zero);
    if(!sequence){__sync_fetch_and_add(&s->poisoned,1);return 0;}
    /* Syscall tracepoint BPF execution is nonpreemptible. No second syscall
     * program can run concurrently on this CPU; exit may migrate, so retain
     * this key in pending. No modern fetch-add instruction is required. */
    struct pending_call p={.key={.sequence=*sequence,.cpu=cpu_id()},.nr=nr,
        .global_scope=nr==440 || (nr==16 && ((r.si>>8)&255)==0xaa),
        .args={r.di,r.si,r.dx,r.r10,r.r8,r.r9}};
    (*sequence)++;
    if(!*sequence)__sync_fetch_and_add(&s->poisoned,1);
    if(update(&pending,&tid,&p,BPF_NOEXIST))__sync_fetch_and_add(&s->poisoned,1);
    return 0;
}
SEC("raw_tracepoint/sys_exit")
int journal_exit(struct bpf_raw_tracepoint_args *ctx)
{
    unsigned long long tid=pid_tgid();unsigned zero=0;
    struct pending_call *p=lookup(&pending,&tid);if(!p)return 0;
    struct journal_epoch *s=lookup(&epoch,&zero);if(!s)return 0;
    {
        struct journal_key key=p->key;
        struct journal_event e={.key=p->key,.pid_tgid=tid,.syscall_nr=p->nr,
            .global_scope=p->global_scope,.result=(long long)ctx->args[1]};
        #pragma unroll
        for(int i=0;i<6;i++)e.args[i]=p->args[i];
        if(update(&journal,&key,&e,BPF_NOEXIST))__sync_fetch_and_add(&s->poisoned,1);
    }
    if(remove_key(&pending,&tid))__sync_fetch_and_add(&s->poisoned,1);
    __sync_fetch_and_add(&s->generation,1);
    __sync_fetch_and_add(&s->active,-1);
    return 0;
}
char LICENSE[] SEC("license")="GPL";
