#ifndef __SB_CLONE_H__
#define __SB_CLONE_H__

/* x86-64 parasite helper: a raw clone must not return into a C frame after
 * changing RSP. Only the parent leaves this asm block. The child enters the
 * function with a fresh ABI-aligned stack and exits through the raw syscall.
 * No libc/TLS is available in the child. The caller owns the stack lifetime. */
static inline long sb_clone_helper(unsigned long flags,
                                  unsigned long (*function)(void *),
                                  void *argument, void *stack_end)
{
    unsigned long *stack = (void *)((unsigned long)stack_end & ~15UL);
    long result;
    *--stack = (unsigned long)argument;
    *--stack = (unsigned long)function;
    __asm__ volatile(
        "xor %%edx, %%edx\n\t"
        "xor %%r10d, %%r10d\n\t"
        "xor %%r8d, %%r8d\n\t"
        "mov $56, %%eax\n\t"
        "syscall\n\t"
        "test %%rax, %%rax\n\t"
        "jnz 1f\n\t"
        "xor %%ebp, %%ebp\n\t"
        "pop %%rax\n\t"
        "pop %%rdi\n\t"
        "call *%%rax\n\t"
        "mov %%eax, %%edi\n\t"
        "mov $60, %%eax\n\t"
        "syscall\n\t"
        "ud2\n\t"
        "1:\n\t"
        : "=a"(result)
        : "D"(flags), "S"(stack)
        : "rcx", "r11", "rdx", "r10", "r8", "memory", "cc");
    return result;
}
#endif
