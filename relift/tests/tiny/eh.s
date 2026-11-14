// A guest program that throws through two frames into a catch, runs a cleanup
// pad that resumes the unwind, and rethrows from the catch. It has its own
// unwinder: a table from call-site return addresses to pads, frames chained
// through x29, and one of two register installers chosen by the byte at
// data+0x10: 0 is the libgcc shape (set sp, `br` to the pad), 1 is the LLVM
// libunwind shape (load a register block, `ret` to the pc in x30).
// Rebuild eh.bin with `sh eh.sh`.
        .text
_start: b main
        .long mod0 - _start
        .org 8
mod0:   .ascii "MOD0"
        .long 0x2000 - 8
        .long 0x3000 - 8
        .long 0x4000 - 8
        .long eh_s - mod0
        .long eh_s - mod0
        .long 0x2000 - 8

main:   adr x9, _start
        add x9, x9, #4, lsl #12
        mov sp, x9
        stp x29, x30, [sp, #-32]!
        mov x29, sp
m_call: bl m2
m_ret:  b bad
p_m:    mov x21, x0
        mov x22, x1
        adr x0, s_outer
        mov x1, #12
        svc #0x27
        mov x0, x21
        mov x1, x22
        svc #0x27
        svc #0x7
        b .

main_end:

m2:     stp x29, x30, [sp, #-32]!
        mov x29, sp
m2_call: bl f1
m2_ret: b bad
p_m2:   mov x21, x0
        mov x22, x1
        mov x9, #0xabc
        cmp x20, x9
        b.ne bad
        fmov x9, d8
        movz x10, #0x4020, lsl #48
        cmp x9, x10
        b.ne bad
        adr x0, s_caught
        mov x1, #6
        svc #0x27
        mov x0, x21
        mov x1, x22
        svc #0x27
        adr x0, s_rethrown
        mov x1, #8
        bl throw
        b bad
m2_end:

f1:     stp x29, x30, [sp, #-32]!
        mov x29, sp
f1_call: bl f0
f1_ret: b bad
p_f1:   mov x21, x0
        mov x22, x1
        adr x0, s_cleanup
        mov x1, #7
        svc #0x27
        mov x0, x21
        mov x1, x22
        bl resume
        b bad
f1_end:

f0:     stp x29, x30, [sp, #-16]!
        mov x29, sp
        mov x20, #0xabc
        movz x9, #0x4020, lsl #48
        fmov d8, x9
        adr x0, s_thrown
        mov x1, #6
        bl throw
        b bad
f0_end:

        .balign 16
throw:  mov x2, x30
        mov x3, x29
        b unwind
throw_end:

resume: mov x2, x30
        mov x3, x29
        b unwind

unwind: adr x12, _start
1:      sub x10, x2, x12
        adr x13, ptable
2:      ldr w14, [x13]
        cbz w14, 3f
        cmp w14, w10
        b.eq 4f
        add x13, x13, #8
        b 2b
3:      cbz x3, bad
        ldr x2, [x3, #8]
        ldr x3, [x3]
        b 1b
4:      ldr w11, [x13, #4]
        add x11, x11, x12
        add x9, x12, #2, lsl #12
        ldrb w9, [x9, #0x10]
        cbnz w9, 5f
        mov sp, x3
        mov x29, x3
        br x11
5:      sub sp, sp, #0x210
        mov x16, sp
        stp x0, x1, [x16]
        stp x19, x20, [x16, #0x98]
        stp x21, x22, [x16, #0xa8]
        stp x23, x24, [x16, #0xb8]
        stp x25, x26, [x16, #0xc8]
        stp x27, x28, [x16, #0xd8]
        str x3, [x16, #0xe8]
        str x3, [x16, #0xf8]
        str x11, [x16, #0x100]
        stp d8, d9, [x16, #0x150]
        stp d10, d11, [x16, #0x160]
        stp d12, d13, [x16, #0x170]
        stp d14, d15, [x16, #0x180]
        mov x20, xzr
        fmov d8, xzr
        mov x0, x16
        bl jumpto
        b bad

        .balign 16
jumpto: ldp x2, x3, [x0, #0x10]
        ldp x4, x5, [x0, #0x20]
        ldp x6, x7, [x0, #0x30]
        ldp x8, x9, [x0, #0x40]
        ldp x10, x11, [x0, #0x50]
        ldp x12, x13, [x0, #0x60]
        ldp x14, x15, [x0, #0x70]
        ldp x16, x17, [x0, #0x80]
        ldp x18, x19, [x0, #0x90]
        ldp x20, x21, [x0, #0xa0]
        ldp x22, x23, [x0, #0xb0]
        ldp x24, x25, [x0, #0xc0]
        ldp x26, x27, [x0, #0xd0]
        ldp x28, x29, [x0, #0xe0]
        ldr x30, [x0, #0x100]
        ldr x1, [x0, #0xf8]
        mov sp, x1
        ldp d0, d1, [x0, #0x110]
        ldp d2, d3, [x0, #0x120]
        ldp d4, d5, [x0, #0x130]
        ldp d6, d7, [x0, #0x140]
        ldp d8, d9, [x0, #0x150]
        ldp d10, d11, [x0, #0x160]
        ldp d12, d13, [x0, #0x170]
        ldp d14, d15, [x0, #0x180]
        ldp d16, d17, [x0, #0x190]
        ldp d18, d19, [x0, #0x1a0]
        ldp d20, d21, [x0, #0x1b0]
        ldp d22, d23, [x0, #0x1c0]
        ldp d24, d25, [x0, #0x1d0]
        ldp d26, d27, [x0, #0x1e0]
        ldp d28, d29, [x0, #0x1f0]
        ldr d30, [x0, #0x200]
        ldr d31, [x0, #0x208]
        ldp x0, x1, [x0]
        ret

bad:    adr x0, s_bad
        mov x1, #3
        svc #0x27
        svc #0x7
        b .


        .org 0x1000
s_thrown:   .ascii "thrown"
s_rethrown: .ascii "rethrown"
s_cleanup:  .ascii "cleanup"
s_caught:   .ascii "caught"
s_outer:    .ascii "outer caught"
s_bad:      .ascii "bad"
        .balign 8
ptable: .long m_ret - _start
        .long p_m - _start
        .long m2_ret - _start
        .long p_m2 - _start
        .long f1_ret - _start
        .long p_f1 - _start
        .long 0

eh_s:
cie:    .long cie_end - cie_body
cie_body: .long 0
        .byte 1
        .asciz "zPLR"
        .uleb128 4
        .sleb128 -8
        .byte 30
        .uleb128 7
        .byte 0x03
        .long 0
        .byte 0x1b
        .byte 0x1b
        .byte 0x0c, 31, 0
        .balign 4
cie_end:

        .macro fde name, lsda
\name\()_fde: .long \name\()_fde_end - \name\()_fde_body
\name\()_fde_body: .long \name\()_fde_body - cie
        .long \name - .
        .long \name\()_end - \name
        .uleb128 4
        .ifnb \lsda
        .long \lsda - .
        .else
        .long 0
        .endif
        .balign 4
\name\()_fde_end:
        .endm

        fde main, lsda_main
        fde m2, lsda_m2
        fde f1, lsda_f1
        fde f0
        fde throw
        .long 0

        .macro lsda fn, call, pad
lsda_\fn: .byte 0xff, 0xff, 0x01
        .uleb128 \fn\()_cs_end - \fn\()_cs
\fn\()_cs: .uleb128 \call - \fn
        .uleb128 4
        .uleb128 \pad - \fn
        .uleb128 0
\fn\()_cs_end:
        .endm

        lsda main, m_call, p_m
        lsda m2, m2_call, p_m2
        lsda f1, f1_call, p_f1
