/* hwtest ARM9: runs the ARM9 timing cases itself and the ARM7 cases through the ARM7 program,
   prints each result and writes all of them to relift-hwtest.csv on the SD card. */
#include <calico.h>
#include <fat.h>
#include <malloc.h>
#include <nds.h>
#include <stdio.h>
#include <string.h>

#include "hwtest.h"
#include "cases.h"
#include "k9tab.h"
#include "swi.c"
#include "wake.c"

#define VERSION "relift hwtest 3"
#ifdef HWTEST_DSI
#define VARIANT "dsi"
#else
#define VARIANT "ntr"
#endif

static u8 dtcm_buf[2048] DTCM_BSS __attribute__((aligned(32)));
static u32 itcm_code[512] __attribute__((section(".itcm"), aligned(32)));
static u32 itcm_data[512] __attribute__((section(".itcm"), aligned(32)));
static u8 *main_buf, *reg_buf, *a7_slot, *a7_buf;
static hw_job7 *job;
static FILE *out;
static int test_region = -1;

#define CP15_R(op) ({ u32 v_; __asm__ volatile("mrc p15, 0, %0, " op : "=r"(v_)); v_; })
#define CP15_W(op, v) __asm__ volatile("mcr p15, 0, %0, " op ::"r"(v) : "memory")

static u32 region_get(int i)
{
    switch (i) {
    case 0: return CP15_R("c6, c0, 0");
    case 1: return CP15_R("c6, c1, 0");
    case 2: return CP15_R("c6, c2, 0");
    case 3: return CP15_R("c6, c3, 0");
    case 4: return CP15_R("c6, c4, 0");
    case 5: return CP15_R("c6, c5, 0");
    case 6: return CP15_R("c6, c6, 0");
    default: return CP15_R("c6, c7, 0");
    }
}

static void region_set(int i, u32 v)
{
    switch (i) {
    case 0: CP15_W("c6, c0, 0", v); break;
    case 1: CP15_W("c6, c1, 0", v); break;
    case 2: CP15_W("c6, c2, 0", v); break;
    case 3: CP15_W("c6, c3, 0", v); break;
    case 4: CP15_W("c6, c4, 0", v); break;
    case 5: CP15_W("c6, c5, 0", v); break;
    case 6: CP15_W("c6, c6, 0", v); break;
    default: CP15_W("c6, c7, 0", v); break;
    }
}

/* the highest enabled region holding addr in the table r, or -1 */
static int region_of(const u32 *r, u32 addr)
{
    for (int i = 7; i >= 0; i--) {
        if (!(r[i] & 1))
            continue;
        u32 size = 2u << ((r[i] >> 1) & 31);
        u32 base = r[i] & ~0xFFFu & ~(size - 1);
        if (addr - base < size)
            return i;
    }
    return -1;
}

/* picks the MPU region to give the test buffer: one whose change leaves every address the
   program needs in its current region */
static int pick_region(void)
{
    u32 r[8], sp;
    for (int i = 0; i < 8; i++)
        r[i] = region_get(i);
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    u32 critical[] = {sp, CP15_R("c9, c1, 0") & ~0xFFFu, (u32)dtcm_buf, (u32)itcm_code, (u32)&pick_region,
                      (u32)main_buf, (u32)a7_slot, (u32)job, 0x04000000, 0x04000400, 0x05000000, 0x06800000,
                      0x07000000, 0xFFFF0000, 0x027FFC00, 0x02FFFC00, (u32)k9_fn[0]};
    for (int cand = 7; cand >= 0; cand--) {
        u32 t[8];
        memcpy(t, r, sizeof t);
        t[cand] = (u32)reg_buf | (0x0E << 1) | 1;
        bool ok = true;
        for (unsigned k = 0; k < sizeof critical / sizeof critical[0]; k++)
            if (region_of(t, critical[k]) != region_of(r, critical[k]) || region_of(r, critical[k]) == cand)
                ok = false;
        if (ok)
            return cand;
    }
    return -1;
}

typedef struct {
    u32 reg, dc, ic, wb, dp, ip;
} mpu_save;

static void mpu_test_on(mpu_save *s, unsigned attr, bool code_uncached)
{
    int i = test_region;
    s->reg = region_get(i);
    s->dc = CP15_R("c2, c0, 0");
    s->ic = CP15_R("c2, c0, 1");
    s->wb = CP15_R("c3, c0, 0");
    s->dp = CP15_R("c5, c0, 2");
    s->ip = CP15_R("c5, c0, 3");
    bool c = attr == AT_CB11 || attr == AT_CB10, b = attr == AT_CB11 || attr == AT_CB01;
    armDCacheFlushAll();
    armDrainWriteBuffer();
    region_set(i, (u32)reg_buf | (0x0E << 1) | 1);
    CP15_W("c2, c0, 0", (s->dc & ~(1u << i)) | (u32)c << i);
    CP15_W("c2, c0, 1", (s->ic & ~(1u << i)) | (u32)(!code_uncached) << i);
    CP15_W("c3, c0, 0", (s->wb & ~(1u << i)) | (u32)b << i);
    CP15_W("c5, c0, 2", (s->dp & ~(15u << 4 * i)) | 3u << 4 * i);
    CP15_W("c5, c0, 3", (s->ip & ~(15u << 4 * i)) | 3u << 4 * i);
    armICacheInvalidateAll();
}

static void mpu_test_off(const mpu_save *s)
{
    int i = test_region;
    armDCacheFlushAll();
    armDrainWriteBuffer();
    CP15_W("c2, c0, 0", s->dc);
    CP15_W("c2, c0, 1", s->ic);
    CP15_W("c3, c0, 0", s->wb);
    CP15_W("c5, c0, 2", s->dp);
    CP15_W("c5, c0, 3", s->ip);
    region_set(i, s->reg);
    armICacheInvalidateAll();
}

typedef void kfn(unsigned, unsigned, unsigned);

static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void line(const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (out)
        fputs(b, out);
    else {
        /* no$gba debug string out: emulators log the line (read from main RAM, not the DTCM stack) */
        static char emu[256] __attribute__((aligned(32)));
        strcpy(emu, b);
        armDCacheFlush(emu, sizeof emu);
        *(vu32 *)0x04FFFA10 = (u32)emu;
    }
}

static u32 dbase9(unsigned region)
{
    switch (region) {
    case RG_DTCM: return (u32)dtcm_buf;
    case RG_ITCM: return (u32)itcm_data;
    case RG_MAIN: return (u32)main_buf;
    case RG_UNC:
    case RG_MX: return (u32)reg_buf;
    case RG_IO: return 0x04000400;
    case RG_VRAM: return 0x06800000;
    case RG_PAL: return 0x05000200;
    case RG_OAM: return 0x07000000;
    default: return (u32)dtcm_buf;
    }
}

static bool run9(const hw_case *c, u32 *t16, u32 *t48, u32 *kaddr, u32 *dbase)
{
    kfn *f = k9_fn[c->kernel];
    u32 len = (u32)k9_end[c->kernel] - (u32)f;
    bool test_mpu = c->region == RG_UNC || c->region == RG_MX || c->place == PL_UNC;
    if (test_mpu && test_region < 0)
        return false;
    if (c->place == PL_ITCM) {
        if (len > sizeof itcm_code)
            return false;
        memcpy(itcm_code, (const void *)f, len);
        f = (kfn *)itcm_code;
    } else if (c->place == PL_UNC) {
        if (len > 16384)
            return false;
        memcpy(reg_buf + 16384, (const void *)f, len);
        armDCacheFlush(reg_buf + 16384, len);
        f = (kfn *)(reg_buf + 16384);
    }
    armDCacheFlushAll();
    armICacheInvalidateAll();
    u32 base = dbase9(c->region), base2 = c->region == RG_IO ? base : base + 1024;
    *kaddr = (u32)f;
    *dbase = base;
    u32 ime = REG_IME;
    REG_IME = 0;
    mpu_save s;
    unsigned attr = c->region == RG_MX ? c->attr : AT_CB00;
    if (test_mpu)
        mpu_test_on(&s, attr, c->place == PL_UNC);
    f(4, base, base2);
    u32 best[2] = {~0u, ~0u};
    static const unsigned ns[2] = {HW_N1, HW_N2};
    bool cold = c->mode == MD_COLD || c->mode == MD_COLDDRAIN, drain = c->mode == MD_DRAIN || c->mode == MD_COLDDRAIN;
    for (int r = 0; r < HW_REPS; r++)
        for (int k = 0; k < 2; k++) {
            if (cold) {
                armDCacheFlushAll();
                armICacheInvalidateAll();
            }
            armDrainWriteBuffer();
            u32 t0 = hw_now();
            f(ns[k], base, base2);
            if (drain)
                armDrainWriteBuffer();
            u32 t1 = hw_now();
            if (t1 - t0 < best[k])
                best[k] = t1 - t0;
        }
    if (test_mpu)
        mpu_test_off(&s);
    REG_IME = ime;
    *t16 = best[0];
    *t48 = best[1];
    return true;
}

/* the ARM7 stopped answering (a case that never returns): the rest of its cases are skipped */
static bool a7_lost;

static bool run7(unsigned ci, u32 *t16, u32 *t48, u32 *kaddr, u32 *dbase)
{
    if (a7_lost)
        return false;
    memset(job, 0, sizeof *job);
    job->ci = ci;
    job->slot = (u32)a7_slot;
    job->mainbuf = (u32)a7_buf;
    armDCacheFlush(job, sizeof *job);
    pxiBeginReceive(PxiChannel_User0);
    pxiSend(PxiChannel_User0, (u32)job);
    for (u32 t0 = hw_now();;) {
        armDCacheInvalidate(job, sizeof *job);
        if (job->status)
            break;
        if (hw_now() - t0 > 20u * 33513982u) {
            a7_lost = true;
            line("# the ARM7 did not finish case %u in 20 s; its later cases are skipped\n", ci);
            return false;
        }
    }
    pxiEndReceive(PxiChannel_User0);
    armDCacheInvalidate(job, sizeof *job);
    *t16 = job->t16;
    *t48 = job->t48;
    *kaddr = job->kaddr;
    *dbase = job->dbase;
    return job->status == 1;
}

static const char *const rg_name[] = {"none", "dtcm", "itcm", "main", "unc", "io", "vram", "pal", "oam", "wram7", "mx", "m2w"};
static const char *const pl_name[] = {"main", "itcm", "unc", "wram"};
static const char *const md_name[] = {"warm", "cold", "drain", "colddrain"};
static const char *const at_name[] = {"-", "cb11", "cb10", "cb01", "cb00"};

static void result(int cpu, unsigned i, const hw_case *c, bool ok, u32 t16, u32 t48, u32 kaddr, u32 dbase)
{
    if (!ok) {
        line("%d,%u,%s,%s,%s,%s,%s,%u,skipped,,,,,\n", cpu, i, c->name, rg_name[c->region], pl_name[c->place],
             md_name[c->mode], at_name[c->attr], c->risky);
        return;
    }
    /* steady state per iteration over HW_N2 - HW_N1 iterations, in bus ticks x100 and CPU cycles x100 */
    s32 d = (s32)(t48 - t16);
    s32 bus100 = d * 100 / (HW_N2 - HW_N1);
    s32 cpu100 = cpu == 9 ? bus100 * 2 : bus100;
    line("%d,%u,%s,%s,%s,%s,%s,%u,%08lx,%08lx,%lu,%lu,%ld,%ld\n", cpu, i, c->name, rg_name[c->region],
         pl_name[c->place], md_name[c->mode], at_name[c->attr], c->risky, (unsigned long)kaddr, (unsigned long)dbase,
         (unsigned long)t16, (unsigned long)t48, (long)bus100, (long)cpu100);
    printf("%d %-12.12s %-4.4s%c%c %3ld.%02ld\n", cpu, c->name, rg_name[c->region], pl_name[c->place][0],
           md_name[c->mode][0], (long)(cpu100 / 100), (long)(cpu100 < 0 ? -cpu100 : cpu100) % 100);
}

/* the ARM7's cartridge case names, in its card_cases order */
static const char *const card_names7[] = {"card_cmd", "card_4", "card_512", "card_4k", "card_cmd_slow", "card_4_slow",
                                          "card_512_slow", "card_cmd_game", "card_4_game", "card_512_game",
                                          "card_4k_game", "card_512_game_slow"};
#define NCARD7 (sizeof card_names7 / sizeof card_names7[0])

/* a BIOS call or cartridge row in the cases' CSV format */
static void extra(int cpu, unsigned idx, const char *name, const char *region, const char *place, u32 t16, u32 t48,
                  u32 check, u32 units)
{
    s32 bus100 = (s32)(t48 - t16) * 100 / (s32)units;
    s32 cpu100 = cpu == 9 ? bus100 * 2 : bus100;
    line("%d,%u,%s,%s,%s,warm,-,1,%08lx,%08lx,%lu,%lu,%ld,%ld\n", cpu, idx, name, region, place, (unsigned long)check,
         (unsigned long)units, (unsigned long)t16, (unsigned long)t48, (long)bus100, (long)cpu100);
    printf("%d %-16.16s %-5.5s %6ld.%02ld%s\n", cpu, name, region, (long)(cpu100 / 100),
           (long)(cpu100 < 0 ? -cpu100 : cpu100) % 100, check && (check >> 16) != (check & 0xFFFF) ? " BAD" : "");
}

/* a wake case row: the least and the most latency in bus ticks */
static void wake_row(int cpu, unsigned k, const char *place, u32 lo, u32 hi)
{
    s32 cpu100 = (s32)lo * (cpu == 9 ? 200 : 100);
    line("%d,%u,%s,none,%s,warm,-,1,00000000,00000000,%lu,%lu,%ld,%ld\n", cpu, 3000 + k, wk_name[k], place,
         (unsigned long)lo, (unsigned long)hi, (long)lo * 100, (long)cpu100);
    printf("%d %-20.20s %5lu..%lu\n", cpu, wk_name[k], (unsigned long)lo, (unsigned long)hi);
}

/* startup marks without the console: the main engine's backdrop color, so a hang before the console
   shows a color instead of white (red: runtime started, blue: main entered) */
static void mark(u16 bgr555)
{
    *(vu32 *)0x04000304 |= 0x8003; /* POWCNT1: LCDs, 2D engine A, A on top */
    *(vu32 *)0x04000000 = 0x00010000; /* DISPCNT: display on, no layers: backdrop only */
    *(vu16 *)0x05000000 = bgr555;
}

__attribute__((constructor(101))) static void early_mark(void) { mark(0x001F); }

static void step(const char *s)
{
    printf("[%s]\n", s);
}

int main(void)
{
    mark(0x7C00);
    consoleDemoInit();
    printf("%s (%s)\n", VERSION, VARIANT);
    step("console up");
#ifndef HWTEST_DSI
    /* the DS-mode build measures a DS: refuse DSi mode and the 133 MHz ARM9 clock */
    u16 clk9 = *(vu16 *)0x04004004;
    if (isDSiMode() || (clk9 & 1)) {
        printf("\nwrong mode: dsi_mode=%d scfg_clk9=%04x\n", isDSiMode(), clk9);
        printf("This build needs DS mode at 67 MHz.\nNothing was measured or written.\nPress START to exit.\n");
        while (pmMainLoop()) {
            threadWaitForVBlank();
            scanKeys();
            if (keysDown() & KEY_START)
                break;
        }
        return 0;
    }
    step("mode ok: DS, 67 MHz");
#endif
    vramSetBankA(VRAM_A_LCD);
    hw_timer_start();

    main_buf = memalign(32, 4096);
    reg_buf = memalign(32768, 32768);
    a7_slot = memalign(32, 4096);
    a7_buf = memalign(32, 4096);
    job = memalign(32, sizeof *job);
    memset(main_buf, 0x5A, 4096);
    memset(reg_buf, 0x5A, 32768);
    memset(a7_buf, 0x5A, 4096);
    armDCacheFlushAll();
    test_region = pick_region();

    step("buffers ok");
    const char *path = NULL;
    bool skip_fat = !(*(vu16 *)0x04000130 & 2); /* B held at startup: no FAT init */
    printf(skip_fat ? "[FAT skipped: B held]\n" : "[FAT init... (hold B at start to skip)]\n");
    if (!skip_fat && fatInitDefault()) {
        step("FAT ok");
        static const char *const paths[] = {"sd:/relift-hwtest.csv", "fat:/relift-hwtest.csv", "/relift-hwtest.csv"};
        for (unsigned i = 0; i < 3 && !out; i++)
            if ((out = fopen(paths[i], "w")))
                path = paths[i];
    }
    else if (!skip_fat)
        step("FAT failed: results on screen only");
    printf("file: %s\n", path ? path : "NONE (no SD access)");

    line("# %s\n", VERSION);
    line("# variant=%s\n", VARIANT);
    line("# dsi_mode=%d scfg_clk9=%04x scfg_ext9=%08lx exmemcnt=%04x wramcnt=%02x\n", isDSiMode(),
         *(vu16 *)0x04004004, (unsigned long)*(vu32 *)0x04004008, *(vu16 *)0x04000204, *(vu8 *)0x04000247);
    line("# cp15 control=%08lx dtcm=%08lx itcm=%08lx dcache=%02lx icache=%02lx wbuf=%02lx test_region=%d\n",
         (unsigned long)CP15_R("c1, c0, 0"), (unsigned long)CP15_R("c9, c1, 0"), (unsigned long)CP15_R("c9, c1, 1"),
         (unsigned long)CP15_R("c2, c0, 0"), (unsigned long)CP15_R("c2, c0, 1"), (unsigned long)CP15_R("c3, c0, 0"),
         test_region);
    for (int i = 0; i < 8; i++)
        line("# region%d=%08lx\n", i, (unsigned long)region_get(i));
    line("# buffers main=%08lx test=%08lx a7slot=%08lx a7buf=%08lx dtcm=%08lx itcm=%08lx\n", (unsigned long)main_buf,
         (unsigned long)reg_buf, (unsigned long)a7_slot, (unsigned long)a7_buf, (unsigned long)dtcm_buf,
         (unsigned long)itcm_data);
    line("# timer ticks at 33.513982 MHz; t16/t48 = minimum of %d runs of %d and %d loop iterations;\n", HW_REPS,
         HW_N1, HW_N2);
    line("# bus_x100 = (t48-t16)*100/%d ticks per iteration; cpu_x100 = the same in that CPU's cycles\n",
         HW_N2 - HW_N1);
    line("cpu,idx,name,region,place,mode,attr,may_differ_dsi_3ds,kaddr,dbase,t16,t48,bus_x100,cpu_x100\n");
    printf("test MPU region %d\n", test_region);

    for (unsigned i = 0; i < NCASES9; i++) {
        u32 t16 = 0, t48 = 0, ka = 0, db = 0;
        bool ok = run9(&cases9[i], &t16, &t48, &ka, &db);
        result(9, i, &cases9[i], ok, t16, t48, ka, db);
        if (out && i % 32 == 31)
            fflush(out);
    }
    step("waiting for ARM7");
    pxiWaitRemote(PxiChannel_User0);
    step("ARM7 ready");
    for (unsigned i = 0; i < NCASES7; i++) {
        u32 t16 = 0, t48 = 0, ka = 0, db = 0;
        bool ok = run7(i, &t16, &t48, &ka, &db);
        result(7, i, &cases7[i], ok, t16, t48, ka, db);
        if (out && i % 32 == 31)
            fflush(out);
    }
    step("BIOS calls");
    line("# BIOS call and cartridge cases (idx 1000 on: BIOS calls, 2000 on: ROMCTRL transfers on the ARM7, which owns\n");
    line("# the slot): t16/t48 = minimum of %d runs on the small and the large input (or 16 and 48 calls, or one and two\n", HW_REPS);
    line("# transfers); kaddr = the result check (CRC16 wanted << 16 | got, 0 without output); dbase = units between\n");
    line("# them; bus_x100 = (t48-t16)*100/units. Wake cases (idx 3000 on): t16/t48 = the least and the most ticks of\n");
    line("# %d runs from the interrupt source to the instruction after the call; bus_x100 = t16*100\n", HW_REPS);
    for (unsigned k = 0; k < WK_KINDS; k++) {
        u32 dt = CP15_R("c9, c1, 0") & ~0xFFFu;
        u32 lo, hi;
        wake_measure(k, (vu32 *)(dt + 0x3FFC), (vu32 *)(dt + 0x3FF8), &lo, &hi);
        wake_row(9, k, "main", lo, hi);
    }
    for (unsigned i = 0; i < NSWI; i++) {
        u32 ts, tl, chk;
        u8 *dst = sw_cases[i].dst ? (u8 *)0x06800000 : main_buf;
        u32 ime = REG_IME;
        REG_IME = 0;
        sw_measure(i, dst, &ts, &tl, &chk);
        REG_IME = ime;
        armDCacheFlushAll();
        extra(9, 1000 + i, sw_name[sw_cases[i].kind], sw_cases[i].dst ? "vram" : "main", "main", ts, tl, chk, sw_units(i));
    }
    for (unsigned j = 0; j < NSWI + NCARD7 + WK_KINDS; j++) {
        unsigned i = (j + NSWI) % (NSWI + NCARD7 + WK_KINDS);
        u32 t16 = 0, t48 = 0, ka = 0, db = 0;
        bool ok = run7(NCASES7 + i, &t16, &t48, &ka, &db);
        if (!ok)
            continue;
        if (i >= NSWI + NCARD7) {
            wake_row(7, i - NSWI - NCARD7, "wram", t16, t48);
            continue;
        }
        bool card = i >= NSWI;
        extra(7, card ? 2000 + i - NSWI : 1000 + i, card ? card_names7[i - NSWI] : sw_name[sw_cases[i].kind],
              card ? "card" : sw_cases[i].dst ? "wram7" : "main", "wram", t16, t48, ka, db);
    }
    line("# end\n");
    if (out)
        fclose(out);
    printf("\nDone: %u ARM9 + %u ARM7 cases.\n%s %s\nPress START to exit.\n", NCASES9, NCASES7,
           path ? "Wrote" : "Could not write", path ? path : "the file");
    while (pmMainLoop()) {
        threadWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START)
            break;
    }
    return 0;
}
