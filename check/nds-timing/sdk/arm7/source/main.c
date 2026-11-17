/* hwtest ARM7: system services, and the ARM7 timing cases on request from the ARM9. */
#include <calico.h>
#include <nds.h>
#include <string.h>

#include "hwtest.h"
#include "cases.h"
#include "k7tab.h"
#include "swi.c"
#include "card.c"
#include "wake.c"

static u8 wram_buf[4096] __attribute__((aligned(32)));
static Mailbox mb;
static u32 mb_slots[4];

typedef void kfn(unsigned, unsigned, unsigned);

static void run(hw_job7 *j)
{
    if (j->ci >= NCASES7) {
        unsigned i = j->ci - NCASES7;
        u32 ime = REG_IME;
        REG_IME = 0;
        if (i < NSWI) {
            sw_measure(i, sw_cases[i].dst ? wram_buf : (u8 *)j->mainbuf, &j->t16, &j->t48, &j->kaddr);
            j->dbase = sw_units(i);
        } else if (i - NSWI < NCARD) {
            card_measure(i - NSWI, &j->t16, &j->t48);
            j->kaddr = 0;
            j->dbase = 1;
        } else if (i - NSWI - NCARD < WK_KINDS) {
            wake_measure(i - NSWI - NCARD, (vu32 *)0x0380FFFC, (vu32 *)0x0380FFF8, &j->t16, &j->t48);
            j->kaddr = 0;
            j->dbase = 0;
        }
        REG_IME = ime;
        j->status = i < NSWI + NCARD + WK_KINDS ? 1 : 2;
        return;
    }
    const hw_case *c = &cases7[j->ci];
    kfn *f = k7_fn[c->kernel];
    u32 len = (u32)k7_end[c->kernel] - (u32)f;
    if (c->place == PL_MAIN) {
        if (len > 4096) {
            j->status = 2;
            return;
        }
        memcpy((void *)j->slot, (const void *)f, len);
        f = (kfn *)j->slot;
    }
    u32 base, base2;
    switch (c->region) {
    case RG_MAIN: base = j->mainbuf; base2 = base + 1024; break;
    case RG_IO: base = base2 = 0x04000520; break;
    case RG_M2W: base = j->mainbuf; base2 = (u32)wram_buf; break;
    default: base = (u32)wram_buf; base2 = base + 1024; break;
    }
    j->kaddr = (u32)f;
    j->dbase = base;
    u32 ime = REG_IME;
    REG_IME = 0;
    f(4, base, base2);
    u32 best[2] = {~0u, ~0u};
    static const unsigned ns[2] = {HW_N1, HW_N2};
    for (int r = 0; r < HW_REPS; r++)
        for (int k = 0; k < 2; k++) {
            u32 t0 = hw_now();
            f(ns[k], base, base2);
            u32 t1 = hw_now();
            if (t1 - t0 < best[k])
                best[k] = t1 - t0;
        }
    REG_IME = ime;
    j->t16 = best[0];
    j->t48 = best[1];
    j->status = 1;
}

int main(void)
{
    envReadNvramSettings();
    keypadStartExtServer();
    lcdSetIrqMask(DISPSTAT_IE_ALL, DISPSTAT_IE_VBLANK);
    irqEnable(IRQ_VBLANK);
    pmInit();
    blkInit();
    hw_timer_start();
    mailboxPrepare(&mb, mb_slots, 4);
    pxiSetMailbox(PxiChannel_User0, &mb);

    while (pmMainLoop()) {
        u32 msg;
        while (mailboxTryRecv(&mb, &msg)) {
            run((hw_job7 *)msg);
            pxiReply(PxiChannel_User0, 0);
        }
        threadWaitForVBlank();
    }
    return 0;
}
