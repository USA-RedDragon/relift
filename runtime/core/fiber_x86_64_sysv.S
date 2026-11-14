/*
 * Fiber switch for x86-64 System V hosts (Linux, macOS): saves rbx, rbp and
 * r12-r15, returns by pop and an indirect jmp. No FP control is saved.
 * Frame at the saved sp: r15 r14 r13 r12 rbx rbp, then the return address.
 */
#if defined(__x86_64__) && !defined(_WIN32)

#if defined(__APPLE__)
#define SYM(x) _##x
#define LOCAL(x) L##x
#define FN(x) .globl SYM(x) ; .private_extern SYM(x) ; .p2align 4 ; SYM(x):
#define END(x)
#else
#define SYM(x) x
#define LOCAL(x) .L##x
#define FN(x) .globl x ; .hidden x ; .type x, @function ; .p2align 4 ; x:
#define END(x) .size x, . - x
#endif

    .text

FN(rl_fiber_swap)
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    popq %r8
    jmpq *%r8
END(rl_fiber_swap)

FN(rl_fiber_start)
LOCAL(fiber_start):
    .cfi_startproc
    .cfi_undefined rip
    pushq %rbp
    .cfi_adjust_cfa_offset 8
    movq %rsp, %rbp
    movq %r13, %rdi
    callq *%r12
    ud2
    .cfi_endproc
END(rl_fiber_start)

FN(rl_fiber_prepare)
    leaq -96(%rdi), %rax
    andq $-16, %rax
    xorl %ecx, %ecx
    movq %rcx, 0(%rax)
    movq %rcx, 8(%rax)
    movq %rdx, 16(%rax)
    movq %rsi, 24(%rax)
    movq %rcx, 32(%rax)
    movq %rcx, 40(%rax)
    leaq LOCAL(fiber_start)(%rip), %rdx
    movq %rdx, 48(%rax)
    movq %rcx, 56(%rax)
    ret
END(rl_fiber_prepare)

FN(rl_fiber_fp_mode_diff)
    stmxcsr -8(%rsp)
    movl -8(%rsp), %eax
    andl $0xffc0, %eax
    xorl $0x1f80, %eax
    ret
END(rl_fiber_fp_mode_diff)

#endif

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack, "", @progbits
#endif
