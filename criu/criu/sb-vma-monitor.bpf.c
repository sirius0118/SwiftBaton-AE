#include <linux/bpf.h>
#define SEC(name) __attribute__((section(name), used))
static void *(*bpf_map_lookup_elem)(void *, const void *) = (void *)BPF_FUNC_map_lookup_elem;
static long (*bpf_map_update_elem)(void *, const void *, const void *, unsigned long long) = (void *)BPF_FUNC_map_update_elem;
static long (*bpf_map_delete_elem)(void *, const void *) = (void *)BPF_FUNC_map_delete_elem;
static unsigned long long (*bpf_get_current_pid_tgid)(void) = (void *)BPF_FUNC_get_current_pid_tgid;
#include "include/sb-vma-syscalls.h"
struct bpf_map_def { unsigned type, key_size, value_size, max_entries, map_flags; };
struct bpf_map_def SEC("maps") watched = { BPF_MAP_TYPE_HASH, 4, 4, 4096, 0 };
struct bpf_map_def SEC("maps") pending = { BPF_MAP_TYPE_HASH, 8, 4, 16384, 0 };
struct bpf_map_def SEC("maps") epoch = { BPF_MAP_TYPE_ARRAY, 4, sizeof(struct sb_vma_epoch), 1, 0 };
SEC("raw_tracepoint/sys_enter")
int vma_enter(struct bpf_raw_tracepoint_args *ctx)
{
    unsigned long long tid = bpf_get_current_pid_tgid();
    unsigned pid = tid >> 32, zero = 0, one = 1;
    long nr = ctx->args[1];
    if (!sb_vma_changes(nr)) return 0;
    /* Remote madvise can modify a watched mm from an outside process. */
    if (nr != 440 && !bpf_map_lookup_elem(&watched, &pid)) return 0;
    struct sb_vma_epoch *state = bpf_map_lookup_elem(&epoch, &zero);
    if (!state) return 0;
    __sync_fetch_and_add(&state->active, 1);
    __sync_fetch_and_add(&state->generation, 1);
    if (bpf_map_update_elem(&pending, &tid, &one, BPF_NOEXIST))
        __sync_fetch_and_add(&state->poisoned, 1);
    return 0;
}
SEC("raw_tracepoint/sys_exit")
int vma_exit(struct bpf_raw_tracepoint_args *ctx)
{
    (void)ctx;
    unsigned long long tid = bpf_get_current_pid_tgid();
    unsigned zero = 0;
    if (!bpf_map_lookup_elem(&pending, &tid)) return 0;
    struct sb_vma_epoch *state = bpf_map_lookup_elem(&epoch, &zero);
    if (!state) return 0;
    if (bpf_map_delete_elem(&pending, &tid)) __sync_fetch_and_add(&state->poisoned, 1);
    __sync_fetch_and_add(&state->generation, 1);
    __sync_fetch_and_add(&state->active, -1);
    return 0;
}
char LICENSE[] SEC("license") = "GPL";
