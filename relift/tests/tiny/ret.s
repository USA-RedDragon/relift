// A guest program whose returns do not go back to their call sites.
// data+0x10: 0 runs a function that rewrites its saved x30 and returns to
// `moved`, which prints "ret moved" and exits; 1 runs a function whose
// return address loses its canonical form, as a failed pointer
// authentication leaves it, so the return faults.
// Rebuild ret.bin with `sh ret.sh`.
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
        adr x9, _start
        add x9, x9, #2, lsl #12
        ldrb w9, [x9, #0x10]
        cbnz w9, 1f
        bl swap
        b bad
1:      bl wild
        b bad

swap:   stp x29, x30, [sp, #-16]!
        mov x29, sp
        adr x9, moved
        str x9, [sp, #8]
        ldp x29, x30, [sp], #16
        ret

wild:   stp x29, x30, [sp, #-16]!
        mov x29, sp
        ldp x29, x30, [sp], #16
        eor x30, x30, #(1 << 53)
        ret

moved:  adr x0, s_moved
        mov x1, #9
        svc #0x27
        mov x0, #0
        svc #0x7
        b .

bad:    adr x0, s_bad
        mov x1, #7
        svc #0x27
        mov x0, #1
        svc #0x7
        b .

        .org 0x1000
s_moved: .ascii "ret moved"
s_bad:   .ascii "ret bad"
        .balign 4
eh_s:
