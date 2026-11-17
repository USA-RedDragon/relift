/* hwtest, bare-metal ARM7 (no SDK): runs the ARM7 cases the ARM9 hands over (job address at the
   end of main RAM, IPCSYNC handshake), as sdk/arm7/main.c does. Linked to run in WRAM. */
#include "hwtest.h"
#include "cases.h"
#include "k7tab.h"
#ifdef HWTEST_CALLS
#include "ct.h"
#include "swi.c"
#include "card.c"
#include "wake.c"
#include "fixed.c"
#endif

#define R16(a) (*(vu16 *)(a))
#define R32(a) (*(vu32 *)(a))

__attribute__((noinline)) void *hw_memcpy(void *d, const void *s, unsigned n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--)
        *a++ = *b++;
    return d;
}

void *hw_memset(void *d, int c, unsigned n)
{
    unsigned char *a = d;
    while (n--)
        *a++ = (unsigned char)c;
    return d;
}

static u8 wram_buf[4096] __attribute__((aligned(32)));
/* the ARM7 program's link address, and where the cartridge image the boot stub copied it from sits */
#define BASE7 0x037F8000u
#define IMAGE7 0x02380100u
#define BASE7_ASM "0x037F8000"
#define IMAGE7_ASM "0x02380100"
#include "calls7.h"

#ifdef HWTEST_CALLS
/* contend.c's hooks: main RAM code runs from the cartridge image in main RAM (call7) */
static void ct_prep7(hw_job7 *j) { (void)j; }
static void ct_k7(unsigned k, unsigned n, hw_job7 *j)
{
    call7(ct7_kernel[k], ct7_code[k], n, j->mainbuf, j->mainbuf + 1024);
}
#include "contend.c"

/* the calls build's jobs: contention (mode 2), and the BIOS call, cartridge, wake and fixed-cost cases
   numbered after the kernel cases */
static void run_calls(hw_job7 *j)
{
    if (j->mode == 2) {
        ct_job7(j);
        j->status = 1;
        return;
    }
    {
        unsigned i = j->ci - NCASES7;
        if (i < NSWI && sw_cases[i].only9) {
            j->status = 2;
            return;
        }
        if (i < NSWI) {
            sw_measure(i, sw_cases[i].dst ? wram_buf : (u8 *)j->mainbuf, &j->t16, &j->t48, &j->kaddr);
            j->dbase = sw_units(i);
        } else if (i - NSWI < NCARD) {
            card_measure(i - NSWI, &j->t16, &j->t48);
            j->kaddr = 0;
            j->dbase = 1;
        } else if (i - NSWI - NCARD < WK_KINDS) {
            __asm__ volatile("mrs r0, cpsr\n bic r0, r0, #0x80\n msr cpsr_c, r0" ::: "r0", "memory");
            wake_measure(i - NSWI - NCARD, (vu32 *)0x0380FFFC, (vu32 *)0x0380FFF8, &j->t16, &j->t48);
            j->kaddr = 0;
            j->dbase = 0;
        } else if (i - NSWI - NCARD - WK_KINDS < FX_KINDS) {
            fx_measure(i - NSWI - NCARD - WK_KINDS, (u8 *)j->mainbuf, &j->t16, &j->t48);
            j->kaddr = 0;
            j->dbase = 0;
        }
        j->status = i < NSWI + NCARD + WK_KINDS + FX_KINDS ? 1 : 2;
    }
}
#endif

static void run(hw_job7 *j)
{
#ifdef HWTEST_CALLS
    if (j->mode == 2 || j->ci >= NCASES7) {
        run_calls(j);
        return;
    }
#endif
    if (j->ci >= NCASES7) {
        j->status = 2;
        return;
    }
    const hw_case *c = &cases7[j->ci];
    int in_main = c->place == PL_MAIN;
    u32 base, base2;
    switch (c->region) {
    case RG_MAIN: base = j->mainbuf; base2 = base + 1024; break;
    case RG_IO: base = base2 = 0x04000520; break;
    case RG_M2W: base = j->mainbuf; base2 = (u32)wram_buf; break;
    default: base = (u32)wram_buf; base2 = base + 1024; break;
    }
    j->dbase = base;
    j->kaddr = call7(c->kernel, in_main, 4, base, base2);
    u32 best[2] = {~0u, ~0u};
    static const unsigned ns[2] = {HW_N1, HW_N2};
    for (int r = 0; r < HW_REPS; r++)
        for (int k = 0; k < 2; k++) {
            u32 t0 = hw_now();
            call7(c->kernel, in_main, ns[k], base, base2);
            u32 t1 = hw_now();
            if (t1 - t0 < best[k])
                best[k] = t1 - t0;
        }
    j->t16 = best[0];
    j->t48 = best[1];
    j->status = 1;
}

#ifdef HWTEST_CALLS
/* the BIOS calls' read callbacks, which only swi.c's table refers to: called here once so that a
   translator that follows calls finds them (the call never runs) */
static volatile u32 never;
static void reach_callbacks(void)
{
    if (never) {
        cb_open(0, 0, 0);
        cb_close(0);
        cb_get8(0);
        cb_get16(0);
        cb_get32(0);
    }
}
#endif

__attribute__((naked, section(".text.start"))) void _start(void)
{
#ifdef HWTEST_CALLS
    __asm__ volatile("msr cpsr_c, #0xd2\n ldr sp, =0x0380ffb0\n"
                     "msr cpsr_c, #0xd3\n ldr sp, =0x0380ffdc\n"
                     "msr cpsr_c, #0xdf\n ldr sp, =0x0380fd00\n b main\n");
#else
    __asm__ volatile("ldr sp, =0x0380fd00\n b main\n");
#endif
}

int main(void)
{
#ifdef HWTEST_CALLS
    reach_callbacks();
#endif
    hw_timer_start();
    for (;;) {
        unsigned in;
        while ((in = R16(0x04000180) & 15) == 0)
            ;
        if (in == 15)
            break;
        hw_job7 *j = (hw_job7 *)R32(0x023FFE00);
        run(j);
        R16(0x04000180) = 1 << 8 | 1 << 13;
        while ((R16(0x04000180) & 15) != 0)
            ;
        R16(0x04000180) = 0;
    }
    R16(0x040001C0) = 0x8800;
    R16(0x040001C2) = 0x00;
    while (R16(0x040001C0) & 0x80)
        ;
    R16(0x040001C0) = 0x8000;
    R16(0x040001C2) = 0x40;
    for (;;)
        ;
}
