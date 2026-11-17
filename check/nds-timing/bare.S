@ hwtest, bare-metal builds only: the timed code of the BIOS call, cartridge and wake cases as
@ the SDK build's compiler laid it out (sdk/: gcc -O2 ARM on the ARM9, -Os Thumb on the ARM7),
@ instruction for instruction, so the bare build times the same code the consoles ran: the
@ decompression callbacks, the Div and Sqrt call loops, the ARM7's ROMCTRL transfer and the
@ stretch of each wake case from its first timer read to its last.
.syntax unified
.text

#ifdef ARM9
.arm

.macro fn name
.global \name
.type \name, %function
.balign 4
\name:
.endm

fn cb_open
    ldr r0, [r0]
    bx lr
fn cb_close
    mov r0, #0
    bx lr
fn cb_get8
    ldrb r0, [r0]
    bx lr
fn cb_get16
    ldrh r0, [r0]
    bx lr
fn cb_get32
    ldr r0, [r0]
    bx lr

@ sw_div_calls(n): n calls of Div(100000 + i, 37)
fn sw_div_calls
    push {r4, lr}
    sub sp, sp, #8
    ldr r3, =100000
    add r4, r3, r0
1:  mov r0, r3
    mov r1, #37
    str r3, [sp, #4]
    bl svc_div
    ldr r3, [sp, #4]
    add r3, r3, #1
    cmp r4, r3
    bne 1b
    add sp, sp, #8
    pop {r4, pc}

@ sw_sqrt_calls(n): n calls of Sqrt(0x7FFFFFFF - i)
fn sw_sqrt_calls
    push {r4, lr}
    sub sp, sp, #8
    mvn r3, #0x80000000
    sub r4, r3, r0
1:  mov r0, r3
    str r3, [sp, #4]
    bl svc_sqrt
    ldr r3, [sp, #4]
    sub r3, r3, #1
    cmp r3, r4
    bne 1b
    add sp, sp, #8
    pop {r4, pc}

@ wake_go(k): starts timer 2 (Halt, IntrWait) or waits for the VBlank (VBlankIntrWait), calls
@ the BIOS and returns the ticks from the source to the return; the caller set up IE, IF, the
@ timer's reload and, for Halt, the I bit
fn wake_go
    push {r4-r9, lr}
    mov r7, r0
    mov r4, #0x04000000
    ldr r9, =0x04000100
    cmp r7, #2
    beq 3f
1:  ldrh r3, [r9, #4]
    ldrh r1, [r9]
    ldrh r2, [r9, #4]
    cmp r3, r2
    bne 1b
    orr r1, r1, r3, lsl #16
    mov r3, #192
    cmp r7, #0
    add r5, r1, #2000
    strh r3, [r9, #10]
    bne 6f
    bl svc_halt
2:  ldrh r2, [r9, #4]
    ldrh r3, [r9]
    ldrh r1, [r9, #4]
    cmp r2, r1
    bne 2b
    cmp r7, #0
    orr r3, r3, r2, lsl #16
7:  sub r0, r3, r5
    pop {r4-r9, pc}
6:  mov r1, #32
    mov r0, #1
    bl svc_intrwait
    b 2b
3:  ldrh r3, [r4, #6]
    cmp r3, #191
    bne 3b
4:  ldrh r3, [r4, #6]
    cmp r3, #191
    beq 4b
5:  ldrh r3, [r9, #4]
    ldrh r1, [r9]
    ldrh r2, [r9, #4]
    cmp r3, r2
    bne 5b
    ldr r5, =560190
    orr r1, r1, r3, lsl #16
    add r5, r1, r5
8:  ldrh r3, [r4, #6]
    cmp r3, #100
    bne 8b
    mvn r3, #0
    str r3, [r4, #0x214]
    bl svc_vblankintrwait
9:  ldrh r2, [r9, #4]
    ldrh r3, [r9]
    ldrh r1, [r9, #4]
    cmp r2, r1
    bne 9b
    orr r3, r3, r2, lsl #16
    b 7b
.ltorg

#else
.thumb

.macro fn name
.global \name
.type \name, %function
.thumb_func
.balign 4
\name:
.endm

@ the SDK build's Thumb-to-ARM veneer of an ARM stub
.macro veneer name, to
.type \name, %function
.thumb_func
.balign 4
\name:
    bx pc
    b \name
.arm
    b \to
.thumb
.endm

@ the callbacks in ARM state: on the ARM7 in work RAM each Thumb instruction of the SDK build's
@ takes the cycle its ARM twin here does
.arm
.macro afn name
.global \name
.type \name, %function
.balign 4
\name:
.endm
afn cb_open
    ldr r0, [r0]
    bx lr
afn cb_close
    mov r0, #0
    bx lr
afn cb_get8
    ldrb r0, [r0]
    lsl r0, r0, #24
    lsr r0, r0, #24
    bx lr
afn cb_get16
    ldrh r0, [r0]
    lsl r0, r0, #16
    lsr r0, r0, #16
    bx lr
afn cb_get32
    ldr r0, [r0]
    bx lr
.thumb

veneer v_div, svc_div
veneer v_sqrt, svc_sqrt
veneer v_halt, svc_halt
veneer v_intrwait, svc_intrwait
veneer v_vblankintrwait, svc_vblankintrwait

@ sw_div_calls(n): n calls of Div(100000 + i, 37)
fn sw_div_calls
    push {r4, r5, r6, lr}
    movs r4, r0
    movs r5, #0
1:  ldr r3, =100000
    movs r1, #37
    adds r0, r5, r3
    adds r5, #1
    bl v_div
    cmp r5, r4
    bne 1b
    pop {r4, r5, r6}
    pop {r0}
    bx r0

@ sw_sqrt_calls(n): n calls of Sqrt(0x7FFFFFFF - i)
fn sw_sqrt_calls
    push {r4, r5, r6, lr}
    movs r4, r0
    movs r5, #0
1:  ldr r0, =0x7FFFFFFF
    subs r0, r0, r5
    adds r5, #1
    bl v_sqrt
    cmp r4, r5
    bne 1b
    pop {r4, r5, r6}
    pop {r0}
    bx r0
.ltorg

@ the SDK build's hw_now
fn hw_now7
    push {r4, lr}
    ldr r2, =0x04000104
    ldr r4, =0x04000100
1:  ldrh r3, [r2, #0]
    ldrh r1, [r4, #0]
    ldrh r0, [r2, #0]
    cmp r3, r0
    bne 1b
    lsls r0, r3, #16
    orrs r0, r1
    pop {r4}
    pop {r1}
    bx r1
.ltorg

@ card_one(ctl): one ROMCTRL transfer of the chip ID command, every word read as it is ready
fn card_one
    movs r2, #184
    movs r1, #0
    ldr r3, =0x040001a8
    strb r2, [r3, #0]
    ldr r3, =0x040001a9
    ldr r2, =0x040001b0
1:  strb r1, [r3, #0]
    adds r3, #1
    cmp r3, r2
    bne 1b
    movs r3, #160
    movs r1, #128
    lsls r3, r3, #24
    orrs r0, r3
    ldr r3, =0x040001a4
    str r0, [r3, #0]
    lsls r1, r1, #16
2:  ldr r2, [r3, #0]
    cmp r2, #0
    blt 3f
    bx lr
3:  ldr r2, [r3, #0]
    tst r2, r1
    beq 2b
    ldr r2, =0x04100010
    ldr r2, [r2, #0]
    b 2b
.ltorg

@ card_time(ctl, twice): ticks of one transfer, or of two back to back
fn card_time
    push {r4, r5, lr}
    sub sp, sp, #20
    str r0, [sp, #8]
    str r1, [sp, #4]
    bl hw_now7
    str r0, [sp, #12]
    ldr r0, [sp, #8]
    bl card_one
    ldr r3, [sp, #4]
    cmp r3, #0
    beq 1f
    ldr r0, [sp, #8]
    bl card_one
1:  bl hw_now7
    ldr r2, [sp, #12]
    subs r0, r0, r2
    add sp, sp, #20
    pop {r4, r5}
    pop {r1}
    bx r1

@ wake_go(k): as the ARM9's
fn wake_go
    push {r4, r5, r6, lr}
    sub sp, sp, #24
    str r0, [sp, #0]
    cmp r0, #2
    beq 3f
    ldr r4, =0x0400010a
    bl hw_now7
    movs r3, #250
    lsls r3, r3, #3
    adds r3, r0, r3
    str r3, [sp, #20]
    movs r3, #192
    strh r3, [r4, #0]
    ldr r3, [sp, #0]
    cmp r3, #0
    bne 6f
    bl v_halt
2:  bl hw_now7
    ldr r3, [sp, #0]
    str r0, [sp, #12]
7:  ldr r1, [sp, #20]
    ldr r2, [sp, #12]
    subs r0, r2, r1
    add sp, sp, #24
    pop {r4, r5, r6}
    pop {r1}
    bx r1
6:  movs r1, #32
    movs r0, #1
    bl v_intrwait
    b 2b
3:  ldr r4, =0x04000006
4:  ldrh r3, [r4, #0]
    cmp r3, #191
    bne 4b
5:  ldrh r3, [r4, #0]
    cmp r3, #191
    beq 5b
    bl hw_now7
    ldr r3, =560190
    adds r3, r0, r3
    str r3, [sp, #20]
8:  ldrh r3, [r4, #0]
    cmp r3, #100
    bne 8b
    movs r2, #1
    ldr r3, =0x04000214
    negs r2, r2
    str r2, [r3, #0]
    bl v_vblankintrwait
    bl hw_now7
    str r0, [sp, #12]
    b 7b
.ltorg
#endif
