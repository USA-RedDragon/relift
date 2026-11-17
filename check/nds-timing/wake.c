/* hwtest: how long the BIOS's waits take to return after their interrupt (both CPUs; included by
   each main.c). For the time of a case the program owns the interrupt: IE holds only the one
   source, the BIOS's IRQ vector points at wake_irq (which acknowledges IF and sets the BIOS's
   IntrWait flags), and everything is put back afterwards. A timer 2 overflow X ticks after its
   start is the source of Halt and IntrWait; VBlankIntrWait waits for the VBlank whose start the
   program timed by polling VCOUNT a frame earlier. The latency is from the source to the
   instruction after the call. */

void svc_halt(void);
void svc_intrwait(u32 discard, u32 mask);
void svc_vblankintrwait(void);
u32 cpsr_irq_off(void);
void cpsr_restore(u32 cpsr);

#define REG_IE_W (*(vu32 *)0x04000210)
#define REG_IF_W (*(vu32 *)0x04000214)
#define REG_IME_W (*(vu32 *)0x04000208)
#define TM2_L (*(vu16 *)0x04000108)
#define TM2_C (*(vu16 *)0x0400010A)
#define DISPSTAT_W (*(vu16 *)0x04000004)
#define VCOUNT_W (*(vu16 *)0x04000006)

/* the BIOS's IntrWait flags word, which wake_irq sets */
vu32 *wake_flags;

/* the IRQ handler the BIOS calls (r0 = 0x04000000, IRQ mode, ARM state) */
__attribute__((naked, target("arm"))) void wake_irq(void)
{
    __asm__ volatile("ldr r1, [r0, #0x210]\n"
                     "ldr r2, [r0, #0x214]\n"
                     "and r1, r1, r2\n"
                     "str r1, [r0, #0x214]\n"
                     "ldr r3, =wake_flags\n"
                     "ldr r3, [r3]\n"
                     "ldr r2, [r3]\n"
                     "orr r2, r2, r1\n"
                     "str r2, [r3]\n"
                     "bx lr\n"
                     ".ltorg\n");
}

enum { WK_HALT, WK_INTRWAIT, WK_VBLANK, WK_KINDS };
static const char *const wk_name[WK_KINDS] = {"halt_wake", "intrwait_wake", "vblankintrwait_wake"};
#define FRAME_TICKS 560190u
#define WAKE_X 2000u

/* one run of case k with the vector at vec: ticks from the source to the return */
static u32 wake_one(unsigned k)
{
    u32 due, back;
    if (k == WK_VBLANK) {
        REG_IME_W = 0;
        REG_IE_W = 1;
        DISPSTAT_W |= 1u << 3;
        while (VCOUNT_W != 191)
            ;
        while (VCOUNT_W == 191)
            ;
        due = hw_now() + FRAME_TICKS;
        while (VCOUNT_W != 100)
            ;
        REG_IF_W = ~0u;
        svc_vblankintrwait();
        back = hw_now();
    } else {
        REG_IE_W = 1u << 5;
        TM2_C = 0;
        TM2_L = (u16)(0x10000u - WAKE_X);
        REG_IF_W = ~0u;
        u32 cpsr = k == WK_HALT ? cpsr_irq_off() : 0;
        due = hw_now() + WAKE_X;
        TM2_C = 0x00C0;
        if (k == WK_HALT)
            svc_halt();
        else
            svc_intrwait(1, 1u << 5);
        back = hw_now();
        if (k == WK_HALT)
            cpsr_restore(cpsr);
        TM2_C = 0;
    }
    REG_IF_W = ~0u;
    return back - due;
}

/* runs case k HW_REPS times: the least and the most latency */
static void wake_measure(unsigned k, vu32 *vector, vu32 *flags, u32 *lo, u32 *hi)
{
    u32 ime = REG_IME_W, ie = REG_IE_W, vec = *vector;
    u16 dispstat = DISPSTAT_W;
    REG_IME_W = 0;
    wake_flags = flags;
    *vector = (u32)wake_irq;
    *lo = ~0u;
    *hi = 0;
    for (int r = 0; r < HW_REPS; r++) {
        REG_IME_W = 1;
        u32 t = wake_one(k);
        REG_IME_W = 0;
        if (t < *lo)
            *lo = t;
        if (t > *hi)
            *hi = t;
    }
    *vector = vec;
    DISPSTAT_W = dispstat;
    REG_IE_W = ie;
    REG_IF_W = ~0u;
    REG_IME_W = ime;
}
