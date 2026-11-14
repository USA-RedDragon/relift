/*
 * Fiber switch for AArch64 hosts (AAPCS64: Linux, macOS; Windows arm64):
 * saves x19-x30 and d8-d15, on Windows also the TEB's StackBase, StackLimit
 * and DeallocationStack (x18 holds the TEB and is left alone), returns by
 * br x30. FPCR is not saved. Frame at the saved sp: x19-x28, x29, x30,
 * d8-d15 (160 bytes), then on Windows the three TEB fields (32 bytes).
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#define SYM(x) _##x
#define LOCAL(x) L##x
#define FN(x) .globl SYM(x) %% .private_extern SYM(x) %% .p2align 4 %% SYM(x):
#define END(x)
#elif defined(_WIN32)
#define SYM(x) x
#define LOCAL(x) .L##x
#define FN(x) .globl x ; .def x ; .scl 2 ; .type 32 ; .endef ; .p2align 4 ; x:
#define END(x)
#else
#define SYM(x) x
#define LOCAL(x) .L##x
#define FN(x) .globl x ; .hidden x ; .type x, %function ; .p2align 4 ; x:
#define END(x) .size x, . - x
#endif

#if defined(_WIN32)
#define FRAME 192
#else
#define FRAME 160
#endif

    .text

FN(rl_fiber_swap)
    sub sp, sp, #FRAME
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
#if defined(_WIN32)
    ldr x9, [x18, #8]
    ldr x10, [x18, #16]
    ldr x11, [x18, #0x1478]
    stp x9, x10, [sp, #160]
    str x11, [sp, #176]
#endif
    mov x9, sp
    str x9, [x0]
    mov sp, x1
#if defined(_WIN32)
    ldp x9, x10, [sp, #160]
    ldr x11, [sp, #176]
    str x9, [x18, #8]
    str x10, [x18, #16]
    str x11, [x18, #0x1478]
#endif
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #FRAME
    br x30
END(rl_fiber_swap)

#if defined(_WIN32)
FN(rl_fiber_start)
LOCAL(fiber_start):
    .seh_proc rl_fiber_start
    stp xzr, xzr, [sp, #-16]!
    .seh_save_fplr_x 16
    mov x29, sp
    .seh_set_fp
    .seh_endprologue
    mov x0, x20
    blr x19
    brk #0
    .seh_endproc
#else
FN(rl_fiber_start)
LOCAL(fiber_start):
    .cfi_startproc
    .cfi_undefined x30
    stp xzr, xzr, [sp, #-16]!
    .cfi_def_cfa_offset 16
    mov x29, sp
    mov x0, x20
    blr x19
    brk #0
    .cfi_endproc
END(rl_fiber_start)
#endif

FN(rl_fiber_prepare)
    and x0, x0, #-16
    sub x0, x0, #FRAME
    stp x1, x2, [x0, #0]
    stp xzr, xzr, [x0, #16]
    stp xzr, xzr, [x0, #32]
    stp xzr, xzr, [x0, #48]
    stp xzr, xzr, [x0, #64]
    adr x9, LOCAL(fiber_start)
    stp xzr, x9, [x0, #80]
    stp xzr, xzr, [x0, #96]
    stp xzr, xzr, [x0, #112]
    stp xzr, xzr, [x0, #128]
    stp xzr, xzr, [x0, #144]
#if defined(_WIN32)
    ldp x9, x10, [x3, #0]
    ldr x11, [x3, #16]
    stp x9, x10, [x0, #160]
    stp x11, xzr, [x0, #176]
#endif
    ret
END(rl_fiber_prepare)

FN(rl_fiber_fp_mode_diff)
    mrs x0, fpcr
    ret
END(rl_fiber_fp_mode_diff)

#endif

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack, "", %progbits
#endif
