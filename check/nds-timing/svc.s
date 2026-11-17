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
