@ hwtest: the BIOS calls the BIOS call cases time, one ARM stub each (r0-r3 pass through)
.syntax unified
.text
.arm

.macro stub name, number
.global \name
.type \name, %function
.align 2
\name:
    svc #(\number << 16)
    bx lr
.endm

stub svc_waitbyloop, 0x03
stub svc_div, 0x09
stub svc_cpuset, 0x0B
stub svc_cpufastset, 0x0C
stub svc_sqrt, 0x0D
stub svc_crc16, 0x0E
stub svc_lz77wram, 0x11
stub svc_lz77cb, 0x12
stub svc_huffcb, 0x13
stub svc_rlwram, 0x14
stub svc_rlcb, 0x15
stub svc_intrwait, 0x04
stub svc_vblankintrwait, 0x05
stub svc_halt, 0x06
stub svc_bitunpack, 0x10
stub svc_diff8, 0x16
stub svc_diff16, 0x18

@ cp15_control (ARM9): reads the CP15 control register
.global cp15_control
.type cp15_control, %function
.align 2
cp15_control:
    mrc p15, 0, r0, c1, c0, 0
    bx lr

@ cp15_set_control (ARM9): writes the CP15 control register, returns the old value
.global cp15_set_control
.type cp15_set_control, %function
.align 2
cp15_set_control:
    mov r1, r0
    mrc p15, 0, r0, c1, c0, 0
    mcr p15, 0, r1, c1, c0, 0
    bx lr

@ cpsr_irq_off: sets the CPSR's I bit, returns the old CPSR; cpsr_restore: puts it back
.global cpsr_irq_off
.type cpsr_irq_off, %function
.align 2
cpsr_irq_off:
    mrs r0, cpsr
    orr r1, r0, #0x80
    msr cpsr_c, r1
    bx lr
.global cpsr_restore
.type cpsr_restore, %function
.align 2
cpsr_restore:
    msr cpsr_c, r0
    bx lr

@ hwtest 3c's fixed-cost cases: t_<call>(r0-r3) makes the call with r0-r3 as given and returns the timer 0
@ ticks from the read before the SVC instruction to the read after it (the same code in every build); t_null
@ times the two reads alone
.macro tstub name, number
.global \name
.type \name, %function
.align 2
\name:
    push {r4-r6, lr}
    ldr r5, =0x04000100
    ldrh r6, [r5]
.if \number >= 0
    svc #(\number << 16)
.endif
    ldrh r1, [r5]
    sub r0, r1, r6
    lsl r0, r0, #16
    lsr r0, r0, #16
    pop {r4-r6, lr}
    bx lr
.ltorg
.endm

tstub t_null, -1
tstub t_waitbyloop, 0x03
tstub t_div, 0x09
tstub t_cpuset, 0x0B
tstub t_cpufastset, 0x0C
tstub t_sqrt, 0x0D
tstub t_crc16, 0x0E
tstub t_bitunpack, 0x10
tstub t_lz77wram, 0x11
tstub t_rlwram, 0x14
