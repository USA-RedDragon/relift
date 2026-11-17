/* hwtest: a watchdog for the ARM9's BIOS call cases (included by the ARM9's main.c). For the time of
   a case, timer 3 overflows after about 2 s with its interrupt the only one enabled, and the BIOS's
   IRQ vector points at guard_irq. A case that returns stops the timer first; one that never does
   is interrupted: guard_irq makes the interrupt return into guard_recover in system mode, which
   longjmps back to guarded, and the case is reported as a timeout. */
#include <setjmp.h>

jmp_buf guard_jb;
void guard_recover(void);

/* the IRQ handler the BIOS calls (high vectors set for the case; IRQ mode; the BIOS's saved r0-r3, r12, lr on the
   IRQ stack): acknowledges IF and returns into guard_recover, interrupts off, in system mode */
__attribute__((naked, target("arm"))) void guard_irq(void)
{
    __asm__ volatile("mov r0, #0x04000000\n"
                     "ldr r1, [r0, #0x214]\n"
                     "str r1, [r0, #0x214]\n"
                     "ldr r1, =guard_recover\n"
                     "add r1, r1, #4\n"
                     "str r1, [sp, #20]\n"
                     "mov r1, #0x9F\n"
                     "msr spsr_cxsf, r1\n"
                     "bx lr\n"
                     ".ltorg\n");
}

__attribute__((naked, target("arm"))) void guard_recover(void)
{
    __asm__ volatile("ldr r0, =guard_jb\n"
                     "mov r1, #1\n"
                     "ldr r2, =longjmp\n"
                     "bx r2\n"
                     ".ltorg\n");
}

#define TM3_L (*(vu16 *)0x0400010C)
#define TM3_C (*(vu16 *)0x0400010E)

/* runs fn(arg) under the watchdog with the IRQ vector at vector: false when it timed out */
static bool guarded(void (*fn)(void *), void *arg, vu32 *vector)
{
    vu32 *ime = (vu32 *)0x04000208, *ie = (vu32 *)0x04000210, *ifl = (vu32 *)0x04000214;
    u32 old_ime = *ime, old_ie = *ie, old_vec = *vector;
    *ime = 0;
    u32 ctl = cp15_control();
    cp15_set_control(ctl | 1u << 13);
    *vector = (u32)guard_irq;
    *ie = 1u << 6;
    *ifl = ~0u;
    TM3_C = 0;
    TM3_L = 0;
    TM3_C = 0x00C3;
    bool ok;
    volatile u32 cpsr = cpsr_irq_off();
    cpsr_restore(cpsr);
    if (setjmp(guard_jb) == 0) {
        *ime = 1;
        fn(arg);
        *ime = 0;
        ok = true;
    } else {
        *ime = 0;
        cpsr_restore(cpsr);
        ok = false;
    }
    TM3_C = 0;
    *ifl = ~0u;
    *vector = old_vec;
    cp15_set_control(ctl);
    *ie = old_ie;
    *ime = old_ime;
    return ok;
}
