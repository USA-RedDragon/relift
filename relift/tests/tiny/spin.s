// A guest program whose main thread (priority 44, core 0) spins on a word
// that a higher-priority thread on the same core stores after a 10 ms sleep.
// Horizon preempts the spinner when the sleeper wakes; without preemption
// points the sleeper never gets the core and the spin never ends.
// Rebuild spin.bin with `sh spin.sh`.
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
        mov x0, #0
        adr x1, high
        mov x2, #0
        adr x3, _start
        add x3, x3, #3, lsl #12
        add x3, x3, #0x800
        mov w4, #0x20
        mov w5, #0
        svc #0x8
        cbnz w0, bad
        mov w0, w1
        svc #0x9
        cbnz w0, bad
        adr x8, _start
        add x8, x8, #2, lsl #12
spin:   ldr w0, [x8]
        cbz w0, spin
        adr x0, s_done
        mov x1, #9
        svc #0x27
        mov x0, #0
        svc #0x7
        b .

bad:    adr x0, s_bad
        mov x1, #8
        svc #0x27
        mov x0, #1
        svc #0x7
        b .

high:   movz x0, #0x9680
        movk x0, #0x98, lsl #16
        svc #0xb
        adr x8, _start
        add x8, x8, #2, lsl #12
        mov w0, #1
        str w0, [x8]
        svc #0xa
        b .

        .org 0x1000
s_done: .ascii "spin done"
s_bad:  .ascii "spin bad"
        .balign 4
eh_s:
