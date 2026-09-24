#ifndef SB_VMA_SYSCALLS_H
#define SB_VMA_SYSCALLS_H
/* x86-64 syscall numbers. Other ABIs are rejected by the cache loader. */
static inline int sb_vma_changes(long nr)
{
    switch(nr) {
    case 9: case 10: case 11: case 12:
    case 16: case 25: case 28:
    case 30: case 67:
    case 56: case 57: case 58: case 59:
    case 149: case 150: case 151: case 152:
    case 157: case 216:
    case 322: case 323: case 325: case 329:
    case 435: case 440:
        return 1;
    default: return nr >= 0x40000000L;
    }
}
struct sb_vma_epoch { unsigned long long generation, active, poisoned; };
#endif
