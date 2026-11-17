/* hwtest's main RAM arbitration ROM (make ARB=1; sdk/arm9 main.c includes it): whether the CPU EXMEMCNT bit 15
   does not favor ever gets main RAM during the other's gapless stream. The ARM7's side is its job mode 4 (a poll
   of IPCFIFOCNT from main RAM code, at most ARB_SPINS polls) and contend.c's hold kernels. Rows, in the order
   they run:
   - 7000 fifo_main p9: e2e_nds's FIFO program with ARM9 priority, the ARM9's side run with both caches off, so
     each of its fetches goes to main RAM;
   - 7010, 7011 fifo_itcm p9, p7: the same trade with the ARM9's side in ITCM, its data and stack in DTCM and
     interrupts off: no main RAM access at all;
   - 7012, 7013 load_itcm p9, p7: as 7010 and 7011, but the ARM9 first waits 2048 ticks (the ARM7 is polling by
     then) and makes one uncached main RAM load;
   - 4100 to 4103 hold_vs_code, hold_vs_poll at p9, then p7: the ARM9 times each uncached main RAM load while the
     ARM7 runs main RAM code 32768 times (contend.c's hold kernels), its loop in ITCM;
   - then the file is closed, and 7001 fifo_main p7 runs last: if it returns, its row is appended.
   fifo and load rows: kaddr bit 0 PASS (the answer is right), bit 1 the ARM7 gave up (HUNG), bit 2 the ARM9 timed
   out; dbase = ticks from the ARM9's start to the ARM7's report on IPCSYNC; t16 = the ARM9's ticks (load_itcm: its
   load); t48 = the ARM7's ticks polling. hold rows: t16 = the loads the ARM9 got in (the least of the runs), t48 =
   the longest time between two (the most). On the consoles (DS mode): every fifo row passes but 7001 (HUNG) and
   7013 (the ARM9's load takes until the ARM7 gives up); at p7 the hold rows get 5 loads in, the gap the whole
   stream. */

#ifndef ARB_WAIT9
#define ARB_WAIT9 (4u * 33513982u)
#endif
#define ARB_FIFO_CR (*(vu16 *)0x04000184)
#define ARB_SYNC (*(vu16 *)0x04000180)
#define ARB_ITCM __attribute__((section(".itcm.arb"), long_call, noinline, target("arm")))

static u32 arb_q DTCM_BSS, arb_ans DTCM_BSS, arb_t1 DTCM_BSS, arb_rep DTCM_BSS, arb_load DTCM_BSS, arb_flags DTCM_BSS;
static u32 arb_hold_n[2] DTCM_BSS;

static inline u32 arb_now(void)
{
    u16 h1, lo, h2;
    do {
        h1 = *(vu16 *)0x04000104;
        lo = *(vu16 *)0x04000100;
        h2 = *(vu16 *)0x04000104;
    } while (h1 != h2);
    return (u32)h1 << 16 | lo;
}

static inline void arb_out(u32 v) { ARB_SYNC = (u16)((ARB_SYNC & 0x4000) | v << 8); }

/* 7000, 7001: run with both caches off, so each fetch and data access of it goes to main RAM */
__attribute__((noinline)) static void arb_fifo9(void)
{
    *(vu16 *)0x04000280 = 0;
    *(vu32 *)0x04000290 = 1000;
    *(vu32 *)0x04000294 = 0;
    *(vu32 *)0x04000298 = 7;
    *(vu32 *)0x0400029C = 0;
    while (*(vu16 *)0x04000280 & 0x8000)
        ;
    u32 q = *(vu32 *)0x040002A0;
    u32 t0 = hw_now();
    *(vu32 *)0x04000188 = q;
    while ((ARB_FIFO_CR & 0x100) && hw_now() - t0 < ARB_WAIT9)
        ;
    arb_ans = (ARB_FIFO_CR & 0x100) ? 0 : *(vu32 *)0x04100000;
    arb_q = q;
    arb_t1 = hw_now();
}

/* 7010 to 7013: one uncached load of at (when at), then the trade; times the ARM7's report */
ARB_ITCM static void arb_trade9(u32 at)
{
    u32 t0 = arb_now();
    if (at) {
        while (arb_now() - t0 < 2048u)
            ;
        u32 tl = arb_now();
        (void)*(vu32 *)at;
        arb_load = arb_now() - tl;
    }
    *(vu16 *)0x04000280 = 0;
    *(vu32 *)0x04000290 = 1000;
    *(vu32 *)0x04000294 = 0;
    *(vu32 *)0x04000298 = 7;
    *(vu32 *)0x0400029C = 0;
    while (*(vu16 *)0x04000280 & 0x8000)
        ;
    u32 q = *(vu32 *)0x040002A0;
    arb_q = q;
    *(vu32 *)0x04000188 = q;
    arb_flags = 0;
    arb_rep = 0;
    for (;;) {
        u32 s = ARB_SYNC & 15, t = arb_now() - t0;
        if ((s == 11 || s == 12) && !arb_rep)
            arb_rep = t;
        if (!(ARB_FIFO_CR & 0x100) || s == 11)
            break;
        if (t > ARB_WAIT9 / 2u * 3u) {
            arb_flags |= 4;
            break;
        }
    }
    arb_ans = (ARB_FIFO_CR & 0x100) ? 0 : *(vu32 *)0x04100000;
    while ((ARB_SYNC & 15) != 11 && (ARB_SYNC & 15) != 12 && arb_now() - t0 < ARB_WAIT9 * 2u)
        ;
    if (!arb_rep)
        arb_rep = arb_now() - t0;
    arb_t1 = arb_now() - t0;
    arb_out(CT_ACK);
    while ((ARB_SYNC & 15) != 0 && arb_now() - t0 < ARB_WAIT9 / 4u * 9u)
        ;
    arb_out(0);
}

static void arb_line(unsigned idx, const char *name, int p7, u32 flags, u32 rep, u32 t9, u32 t7)
{
    line("9,%u,%s,%s,unc,warm,-,1,%08lx,%08lx,%lu,%lu,0,0\n", idx, name, p7 ? "p7" : "p9", (unsigned long)flags,
         (unsigned long)rep, (unsigned long)t9, (unsigned long)t7);
    if (out)
        fflush(out);
    printf("%u %s %s: %s%s\n ARM9 %lu.%03lu ms ARM7 %lu.%03lu ms\n", idx, name, p7 ? "p7" : "p9",
           (flags & 1) ? "PASS" : (flags & 2) ? "HUNG (ARM7 gave up)" : "FAIL", (flags & 4) ? " ARM9 timeout" : "",
           (unsigned long)(t9 / 33514u), (unsigned long)(t9 % 33514u * 1000u / 33514u),
           (unsigned long)(t7 / 33514u), (unsigned long)(t7 % 33514u * 1000u / 33514u));
}

static u16 arb_priority(int p7)
{
    vu16 *exmem = (vu16 *)0x04000204;
    u16 old = *exmem;
    *exmem = (u16)((old & 0x7FFF) | (p7 ? 0x8000 : 0));
    return old;
}

static void arb_fifo(unsigned idx, int p7)
{
    u16 exmem_old = arb_priority(p7);
    ct_out(0);
    hw_job7 *j = send7(0, 4);
    u32 ime = REG_IME;
    REG_IME = 0;
    go7();
    ct_wait(CT_READY);
    for (u32 w0 = hw_now(); hw_now() - w0 < 33514u;)
        ;
    armDCacheFlushAll();
    u32 ctl = cp15_control();
    u32 t_start = hw_now();
    cp15_set_control(ctl & ~(1u << 12 | 1u << 2));
    arb_fifo9();
    cp15_set_control(ctl);
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" ::"r"(0));
    u32 took = arb_t1 - t_start;
    while (ct_in() != 11 && ct_in() != 12)
        ;
    ct_out(CT_ACK);
    ct_wait(0);
    ct_out(0);
    REG_IME = ime;
    finish7(0);
    *(vu16 *)0x04000204 = exmem_old;
    u32 flags = (arb_ans == 3u * arb_q + 1u ? 1u : 0u) | (j->arg[0] == ~0u ? 2u : 0u);
    arb_line(idx, "fifo_main", p7, flags, 0, took, j->arg[1]);
}

static void arb_itcm(unsigned idx, int p7, bool load)
{
    u16 exmem_old = arb_priority(p7);
    ct_prep9(0);
    armDCacheFlushAll();
    ct_out(0);
    hw_job7 *j = send7(0, 4);
    u32 ime = REG_IME;
    REG_IME = 0;
    go7();
    ct_wait(CT_READY);
    arb_trade9(load ? (u32)reg_buf : 0);
    ct_unprep9();
    REG_IME = ime;
    finish7(0);
    *(vu16 *)0x04000204 = exmem_old;
    u32 flags = arb_flags | (arb_ans == 3u * arb_q + 1u ? 1u : 0u) | (j->arg[0] == ~0u ? 2u : 0u);
    arb_line(idx, load ? "load_itcm" : "fifo_itcm", p7, flags, arb_rep, load ? arb_load : arb_t1, j->arg[1]);
}

/* the hold rows' ARM9 side: uncached loads of at until the ARM7 is done; the count and the longest gap in
   arb_hold_n */
ARB_ITCM static void arb_hold9(u32 at)
{
    while ((ARB_SYNC & 15) != CT_READY)
        ;
    arb_out(CT_GO);
    u32 prev = arb_now(), gap = 0, n = 0;
    while ((ARB_SYNC & 15) != CT_DONE) {
        (void)*(vu32 *)at;
        u32 t = arb_now();
        if (t - prev > gap)
            gap = t - prev;
        prev = t;
        n++;
    }
    arb_hold_n[0] = n;
    arb_hold_n[1] = gap;
    arb_out(CT_ACK);
}

static void arb_hold(unsigned idx, unsigned h, int p7, u8 *dmabuf)
{
    u16 exmem_old = arb_priority(p7);
    ct_out(0);
    hw_job7 *j = ct_send7(CT7_N + h, 0, 32768u, (u32)dmabuf);
    ct_prep9(0);
    u32 ime = REG_IME;
    REG_IME = 0;
    u32 loads = ~0u, gap = 0;
    for (int r = 0; r < HW_REPS; r++) {
        arb_hold9((u32)reg_buf);
        if (arb_hold_n[0] < loads)
            loads = arb_hold_n[0];
        if (arb_hold_n[1] > gap)
            gap = arb_hold_n[1];
    }
    ct_unprep9();
    REG_IME = ime;
    ct_finish7(j);
    *(vu16 *)0x04000204 = exmem_old;
    char name[32];
    ct_name(name, "hold", ct7_name[CT7_N + h]);
    extra(9, idx, name, p7 ? "p7" : "p9", "main", loads, gap, 0, 1);
    if (out)
        fflush(out);
    printf("%u %s %s: %lu loads in\n longest gap %lu ticks (%lu us)\n", idx, name, p7 ? "p7" : "p9",
           (unsigned long)loads, (unsigned long)gap, (unsigned long)(gap / 34u));
}

static void arb_all(const char *path)
{
    step("waiting for ARM7");
    pxiWaitRemote(PxiChannel_User0);
    step("ARM7 ready");
    line("# relift main RAM arbitration ROM (arb.c): fifo/load rows: kaddr bit 0 PASS, bit 1 the ARM7 gave up\n");
    line("# (HUNG), bit 2 the ARM9 timed out; dbase = ticks to the ARM7's report; t16 = the ARM9's ticks (load_itcm:\n");
    line("# its load), t48 = the ARM7's ticks polling; hold rows: t16 = loads in, t48 = the longest gap in ticks\n");
    arb_fifo(7000, 0);
    arb_itcm(7010, 0, false);
    arb_itcm(7011, 1, false);
    arb_itcm(7012, 0, true);
    arb_itcm(7013, 1, true);
    for (int p = 0; p < 2; p++)
        for (unsigned h = 0; h < CT_HOLD_N; h++)
            arb_hold(4100 + (unsigned)p * CT_HOLD_N + h, h, p, dma_buf);
    line("# end\n");
    if (out) {
        fclose(out);
        out = NULL;
    }
    printf("file written. Last: 7001 fifo_main p7\n");
    out = path ? fopen(path, "a") : NULL;
    arb_fifo(7001, 1);
    line("# end 7001\n");
}
