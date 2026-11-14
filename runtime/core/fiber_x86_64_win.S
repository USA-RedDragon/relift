/*
 * Fiber switch for x86-64 Windows hosts (Microsoft x64 ABI): saves rbx,
 * rbp, rdi, rsi, r12-r15, xmm6-xmm15 and the TEB's StackBase, StackLimit
 * and DeallocationStack, returns by pop and an indirect jmp. No FP control
 * is saved. Frame at the saved sp: xmm6-xmm15 (160 bytes), the three TEB
 * fields, r15 r14 r13 r12 rsi rdi rbx rbp, then the return address.
 */
#if defined(__x86_64__) && defined(_WIN32)

#define FN(x) .globl x ; .def x ; .scl 2 ; .type 32 ; .endef ; .p2align 4 ; x:

    .text

FN(rl_fiber_swap)
    pushq %rbp
    pushq %rbx
    pushq %rdi
    pushq %rsi
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $184, %rsp
    movaps %xmm6, 0(%rsp)
    movaps %xmm7, 16(%rsp)
    movaps %xmm8, 32(%rsp)
    movaps %xmm9, 48(%rsp)
    movaps %xmm10, 64(%rsp)
    movaps %xmm11, 80(%rsp)
    movaps %xmm12, 96(%rsp)
    movaps %xmm13, 112(%rsp)
    movaps %xmm14, 128(%rsp)
    movaps %xmm15, 144(%rsp)
    movq %gs:0x8, %rax
    movq %rax, 160(%rsp)
    movq %gs:0x10, %rax
    movq %rax, 168(%rsp)
    movq %gs:0x1478, %rax
    movq %rax, 176(%rsp)
    movq %rsp, (%rcx)
    movq %rdx, %rsp
    movq 160(%rsp), %rax
    movq %rax, %gs:0x8
    movq 168(%rsp), %rax
    movq %rax, %gs:0x10
    movq 176(%rsp), %rax
    movq %rax, %gs:0x1478
    movaps 0(%rsp), %xmm6
    movaps 16(%rsp), %xmm7
    movaps 32(%rsp), %xmm8
    movaps 48(%rsp), %xmm9
    movaps 64(%rsp), %xmm10
    movaps 80(%rsp), %xmm11
    movaps 96(%rsp), %xmm12
    movaps 112(%rsp), %xmm13
    movaps 128(%rsp), %xmm14
    movaps 144(%rsp), %xmm15
    addq $184, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rsi
    popq %rdi
    popq %rbx
    popq %rbp
    popq %r8
    jmpq *%r8

FN(rl_fiber_start)
    .seh_proc rl_fiber_start
    subq $32, %rsp
    .seh_stackalloc 32
    .seh_endprologue
    movq %r13, %rcx
    callq *%r12
    int3
    .seh_endproc

FN(rl_fiber_prepare)
    leaq -48(%rcx), %rax
    andq $-16, %rax
    xorl %ecx, %ecx
    movq %rcx, (%rax)
    subq $256, %rax
    xorps %xmm0, %xmm0
    movaps %xmm0, 0(%rax)
    movaps %xmm0, 16(%rax)
    movaps %xmm0, 32(%rax)
    movaps %xmm0, 48(%rax)
    movaps %xmm0, 64(%rax)
    movaps %xmm0, 80(%rax)
    movaps %xmm0, 96(%rax)
    movaps %xmm0, 112(%rax)
    movaps %xmm0, 128(%rax)
    movaps %xmm0, 144(%rax)
    movq 0(%r9), %r10
    movq %r10, 160(%rax)
    movq 8(%r9), %r10
    movq %r10, 168(%rax)
    movq 16(%r9), %r10
    movq %r10, 176(%rax)
    movq %rcx, 184(%rax)
    movq %rcx, 192(%rax)
    movq %r8, 200(%rax)
    movq %rdx, 208(%rax)
    movq %rcx, 216(%rax)
    movq %rcx, 224(%rax)
    movq %rcx, 232(%rax)
    movq %rcx, 240(%rax)
    leaq rl_fiber_start(%rip), %r10
    movq %r10, 248(%rax)
    ret

FN(rl_fiber_fp_mode_diff)
    stmxcsr 8(%rsp)
    movl 8(%rsp), %eax
    andl $0xffc0, %eax
    xorl $0x1f80, %eax
    ret

#endif
