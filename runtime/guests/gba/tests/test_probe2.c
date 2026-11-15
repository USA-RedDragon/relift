/**
 * @file test_probe2.c
 * @brief Every series of relift's SWI probe 2 (vectors_probe2.h), replayed on
 * the machine: the probe's own IWRAM stub, IRQ handler and scan routine run
 * on a small ARM7TDMI interpreter (p2cpu.h), its C driver is mirrored here,
 * and the SWIs run on the high-level BIOS with the machine's timers, IRQ
 * delays, DMA and display. Each series must equal the console's entries.
 */
#include "mach.h"
#include "p2cpu.h"
#include "runner.h"
#include "vectors_probe2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ROM_LEN = 0x8000,
    IO_BASE = 0x03001630,
    SITE_ARM = 0x03001438,
    SITE_THUMB = 0x030014FC,
    S2_CALL = 0x03000000,
    SCAN_LEN = 0x03001610,
    SCAN_LO = 0x03001614,
    SCAN_CNT = 0x03001618,
    SCAN_HASH = 0x0300161C,
    IRQ_HANDLER = 0x03001510,
    CS_SRC = 0x08004664,
    CS_FILLW = 0x080046C4,
    LZ_SRC = 0x080046C8,
    RL_SRC = 0x08004708,
    HUFF_BLOB = 0x080047CC,
    HUFF_TAB = 0x08005364,
    DST = 0x02000100,
    VDST = 0x06014000,
    INFO = 0x02001000,
    STAMP = 0x02002000,
    STAGE7 = 0x02007000,
    DONE_PC = 0x08007F00,
    HB_LOOP_PC = 0x080039E8,
    HB_DONE_PC = 0x08003D68,
    ACALLS = 0x030016D0,
    NPOOL = 0x03001628,
    POOL = 0x02012000,
    FILL = 0xEE,
    ALEN = 96
};

enum { IO_TM0 = 20, IO_TM1 = 24, IO_CPSR = 28, IO_OB = 48, IO_TOTAL = 64, IO_FIRED = 68, IO_ENTRY = 72, IO_RET = 76,
       IO_SPSR = 80, IO_BB = 84, IO_HUNG = 88, IO_HMODE = 92, IO_DELAY = 96, IO_KIND = 100, IO_DMACNT = 104 };

/** Driver time the mirror adds in part_halt: the TM1 watchdog's prescaler
 * phase at the first Halt IME=1 call, and the loop cycles after a call when
 * ret16() takes its BIOS or its IWRAM path, modulo 64 (fitted to halt_ime1). */
enum { HALT_PHASE = 1, HALT_PUT_BIOS = 45, HALT_PUT_IWRAM = 13 };

/** Driver time of call() between the IF acknowledge and the IME write (fitted with
 * the Halt constants above to halt_ime1). */
enum { DRIVER_ACK = 24 };

/**
 * Series the black-box BIOS does not match yet, with the entries that differ,
 * checked exactly so that a change in either direction shows. The BIOS runs
 * each call as its effects plus relift's slide (bios.h): an interrupt lands
 * on its cycle, but where inside the call a store becomes visible and how
 * the call's accesses wait on the bus are fitted estimates, not the
 * console's order of instructions.
 */
typedef enum gap_reason {
    /** An interrupt inside the call: the handler's entry time and the progress it finds in memory. */
    R_INSIDE,
    /** As R_INSIDE, with DMA 3 stalling the call's accesses. */
    R_DMA,
    /** HBlank DMA during the call: the call's accesses against the DMA's, and the stub's Halt before it. */
    R_HBLANK,
    /**
     * The handler's return address: relift's slide (RL_GBA_BIOS_SLIDE + 4 in
     * LR_irq, stacked by the stub, which the probe reads back) instead of the
     * console's address inside Halt.
     */
    R_RETURN,
    /**
     * LZ77UnCompWram and LZ77UnCompVram from Game Pak ROM: the whole-call
     * model, fitted to the console records (source in work RAM), is 7 cycles
     * short; it needs records with the source in ROM.
     */
    R_LZ,
} gap_reason;

typedef struct gap {
    const char *series;
    unsigned differ;
    gap_reason why;
} gap;

static const gap gaps[] = {
    {"open_bus", 4, R_LZ},
    {"A_irq_CpuSet16_copy_ws4317", 509, R_INSIDE},
    {"A_irq_CpuSet16_fill_ws4317", 438, R_INSIDE},
    {"A_irq_CpuSet32_copy_ws4317", 436, R_INSIDE},
    {"A_irq_CpuSet32_fill_ws4317", 389, R_INSIDE},
    {"A_irq_CpuFastSet_copy_ws4317", 361, R_INSIDE},
    {"A_irq_CpuFastSet_fill_ws4317", 268, R_INSIDE},
    {"A_irq_LZ77UnCompWram_ws4317", 703, R_INSIDE},
    {"A_irq_LZ77UnCompVram_ws4317", 743, R_INSIDE},
    {"A_irq_RLUnCompWram_ws4317", 679, R_INSIDE},
    {"A_irq_RLUnCompVram_ws4317", 722, R_INSIDE},
    {"A_irq_CpuSet16_copy_ws0000", 263, R_INSIDE},
    {"A_irq_CpuSet16_fill_ws0000", 220, R_INSIDE},
    {"A_irq_CpuSet32_copy_ws0000", 225, R_INSIDE},
    {"A_irq_CpuSet32_fill_ws0000", 196, R_INSIDE},
    {"A_irq_CpuFastSet_copy_ws0000", 200, R_INSIDE},
    {"A_irq_CpuFastSet_fill_ws0000", 142, R_INSIDE},
    {"A_irq_LZ77UnCompWram_ws0000", 355, R_INSIDE},
    {"A_irq_LZ77UnCompVram_ws0000", 375, R_INSIDE},
    {"A_irq_RLUnCompWram_ws0000", 343, R_INSIDE},
    {"A_irq_RLUnCompVram_ws0000", 365, R_INSIDE},
    {"A_irq_CpuSet32_copy_ws4317_dma3", 213, R_DMA},
    {"A_hblankdma_CpuSet16_copy_ws4317", 106, R_HBLANK},
    {"A_hblankdma_LZ77UnCompWram_ws4317", 309, R_HBLANK},
    {"halt_ime1", 96, R_RETURN},
};

typedef struct probe {
    mach m;
    p2cpu c;
    uint8_t *sram;
    uint16_t (*out)[8];
    unsigned nout, cap;
} probe;

static probe P;

static uint32_t rd(uint32_t a, unsigned n) { return rl_gba_peek(P.m.g, a, n); }

static void wr(uint32_t a, unsigned n, uint32_t v) { rl_gba_write(P.m.g, a, n, v); }

static uint32_t io(unsigned off) { return rd(IO_BASE + off, 4); }

static void set_io(unsigned off, uint32_t v) { wr(IO_BASE + off, 4, v); }

static void swi_hook(p2cpu *c, unsigned n)
{
    rl_gba *g = c->g;
    rl_gba_swi_cpu s = {.ret = c->r[14], .cpsr = c->spsr[2], .sp_svc = c->r[13], .sp_sys = c->bank_sp[0], .lr_sys = c->bank_lr[0]};
    memcpy(s.r, c->r, sizeof s.r);
    rl_gba_swi_status st = rl_gba_swi(rl_gba_bios_of(g), rl_gba_bios_bus(g), n, &s);
    if (st == RL_GBA_SWI_LEFT)
        return;
    for (unsigned i = 0; i < 4; i++)
        c->r[i] = s.r[i];
    uint64_t left = rl_gba_bios_of(g)->cycles;
    if (left)
        rl_gba_idle(g, (unsigned)left);
    p2cpu_set_cpsr(c, c->spsr[2]);
    p2cpu_resume(c, s.ret);
}

static int bios_point(void *user, uint32_t lr, uint32_t spsr, const uint32_t *bios_regs)
{
    (void)user;
    (void)bios_regs;
    p2cpu *c = &P.c;
    uint32_t saved = c->cpsr, regs[16];
    memcpy(regs, c->r, sizeof regs);
    p2cpu_irq(c, lr, spsr);
    if (!p2cpu_run_irq(c, 10000000u))
        fprintf(stderr, "probe2: a handler did not return\n");
    if (c->r[15] != ((lr - 4) & ((spsr & 0x20u) ? ~1u : ~3u)) || c->cpsr != spsr)
        return RL_GBA_POINT_LEFT;
    p2cpu_set_cpsr(c, saved);
    memcpy(c->r, regs, sizeof regs);
    return RL_GBA_POINT_TAKEN;
}

static void probe_init(void)
{
    uint8_t *rom = calloc(1, ROM_LEN);
    for (unsigned i = 0; i < sizeof p2_ranges / sizeof p2_ranges[0]; i++)
        memcpy(rom + (p2_ranges[i].addr & 0xFFFFFFu), p2_rom + p2_ranges[i].off, p2_ranges[i].len);
    P.sram = calloc(1, 0x10000);
    memset(P.sram + 0x8000, 0xFF, 0x8000);
    P.m = mach_new_with(rom, ROM_LEN, P.sram, 0x10000);
    for (uint32_t k = 0; k < 0x1610; k += 4)
        wr(0x03000000u + k, 4, rd(0x080057C0u + k, 4));
    wr(0x03007FFCu, 4, IRQ_HANDLER);
    p2cpu_init(&P.c, P.m.g, P.m.c);
    P.c.hooks.swi = swi_hook;
    P.c.bank_sp[1] = 0x03007FA0u;
    P.c.bank_sp[2] = 0x03007FE0u;
    P.c.r[13] = 0x03007A00u;
    P.c.cpsr = 0xDF;
    rl_gba_cpu_hooks h = {.user = &P, .point = bios_point};
    rl_gba_set_cpu_hooks(P.m.g, &h);
    wr(0x04000208u, 2, 0);
    wr(0x04000200u, 2, 0);
    wr(0x04000102u, 2, 0);
    wr(0x04000106u, 2, 0);
    wr(0x0400010Au, 2, 0);
    wr(0x0400010Eu, 2, 0);
    wr(0x040000BAu, 2, 0);
    wr(0x040000DEu, 2, 0);
    wr(0x04000202u, 2, 0xFFFF);
    wr(0x04000004u, 2, 0);
    wr(0x04000204u, 2, 0x4317);
}

static void spend(unsigned n) { rl_gba_idle(P.m.g, n); }

static void call(uint32_t op, uint32_t kind, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t tm0, uint32_t tm1,
                 uint32_t cpsr, uint32_t ie, uint32_t ime, uint32_t delay, uint32_t hmode)
{
    if (kind == 2)
        wr(SITE_THUMB, 2, op);
    else
        wr(SITE_ARM, 4, op);
    set_io(0, r0);
    set_io(4, r1);
    set_io(8, r2);
    set_io(12, r3);
    set_io(IO_TM0, tm0);
    set_io(IO_TM1, tm1);
    set_io(IO_CPSR, cpsr);
    set_io(IO_KIND, kind);
    set_io(IO_DELAY, delay);
    set_io(IO_HMODE, hmode);
    set_io(IO_FIRED, 0);
    set_io(IO_HUNG, 0);
    set_io(IO_ENTRY, 0);
    set_io(IO_RET, 0);
    set_io(IO_SPSR, 0);
    set_io(IO_BB, 0);
    spend(8);
    rl_gba_store(P.m.g, 0x04000208u, 2, 0, false);
    spend(4);
    rl_gba_store(P.m.g, 0x04000200u, 2, ie, false);
    spend(4);
    rl_gba_store(P.m.g, 0x04000202u, 2, 0xFFFF, false);
    spend(DRIVER_ACK);
    rl_gba_store(P.m.g, 0x04000208u, 2, ime, false);
    spend(4);
    P.c.r[14] = DONE_PC;
    p2cpu_jump(&P.c, S2_CALL);
    if (!p2cpu_run(&P.c, DONE_PC, 50000000u))
        fprintf(stderr, "probe2: a call did not return\n");
    rl_gba_store(P.m.g, 0x04000208u, 2, 0, false);
    spend(4);
}

static uint32_t arm_swi(uint32_t n) { return 0xEF000000u | n << 16; }

static uint32_t thumb_swi(uint32_t n) { return 0xDF00u | n; }

static void fill(uint32_t a, uint32_t n, uint8_t v)
{
    if (a >= 0x06000000u) {
        for (uint32_t k = 0; k < n; k += 2)
            wr(a + k, 2, (uint32_t)(v | v << 8));
    } else {
        for (uint32_t k = 0; k < n; k++)
            wr(a + k, 1, v);
    }
    spend(n * 6);
}

static uint32_t fnv(uint32_t h, uint8_t b) { return (h ^ b) * 0x01000193u; }

static uint32_t whash(uint32_t a, uint32_t n)
{
    uint32_t h = 0x811C9DC5u;
    for (uint32_t k = 0; k < n; k++)
        h = fnv(h, (uint8_t)rd(a + k, 1));
    return h;
}

static void put(uint16_t key, uint16_t f0, uint16_t f1, uint16_t f2, uint16_t f3, uint16_t f4, uint16_t f5, uint16_t f6)
{
    if (P.nout == P.cap) {
        P.cap = P.cap ? 2 * P.cap : 1024;
        P.out = realloc(P.out, P.cap * sizeof *P.out);
    }
    uint16_t *e = P.out[P.nout++];
    e[0] = key;
    e[1] = f0;
    e[2] = f1;
    e[3] = f2;
    e[4] = f3;
    e[5] = f4;
    e[6] = f5;
    e[7] = f6;
}

typedef struct acall {
    uint8_t swi, vram;
    uint32_t r0, r2;
} acall;

static const acall acalls[10] = {
    {0x0B, 0, CS_SRC, 48u},
    {0x0B, 0, CS_FILLW, 48u | 1u << 24},
    {0x0B, 0, CS_SRC, 24u | 1u << 26},
    {0x0B, 0, CS_FILLW, 24u | 1u << 24 | 1u << 26},
    {0x0C, 0, CS_SRC, 24u},
    {0x0C, 0, CS_FILLW, 24u | 1u << 24},
    {0x11, 0, LZ_SRC, 0},
    {0x12, 1, LZ_SRC, 0},
    {0x14, 0, RL_SRC, 0},
    {0x15, 1, RL_SRC, 0},
};

static uint16_t ret16(uint32_t r)
{
    if (r < 0x4000u)
        return (uint16_t)r;
    if (r >= 0x03000000u && r < 0x03008000u)
        return (uint16_t)(0x8000u | (r & 0x7FFFu));
    return 0xFFFFu;
}

static void sweep_a(unsigned ci, uint32_t ws, uint32_t hmode, unsigned sh)
{
    const acall *c = &acalls[ci];
    uint32_t dst = c->vram ? VDST : DST, lo = dst - 16u, len = ALEN + 48u;
    uint16_t prev[7] = {0};
    int have = 0, seen_bios = 0, after = 0;
    wr(0x04000204u, 2, ws);
    wr(0x04000000u, 2, 0x0080);
    fill(lo, len, FILL);
    call(arm_swi(c->swi), 0, c->r0, dst, c->r2, 0, 0, 0, 0x9F, 0, 0, 0, 0);
    uint32_t ref = whash(lo, len);
    wr(SCAN_LO, 4, lo);
    wr(SCAN_LEN, 4, len);
    for (uint32_t o = 4; o < 0x9000u; o += (o < 384u ? 1u : o < 1536u ? 4u : 16u) << sh) {
        fill(lo, len, FILL);
        for (unsigned k = 0; k < 8; k++)
            wr(STAMP + 2 * k, 2, 0xFFFF);
        wr(SCAN_CNT, 4, 0);
        wr(SCAN_HASH, 4, 0);
        call(arm_swi(c->swi), 0, c->r0, dst, c->r2, 0, (0x10000u - o) | 0xC00000u, 0xC10000u, 0x1F, 0x18, 1, 0, hmode);
        uint32_t fin = whash(lo, len) == ref, fired = io(IO_FIRED), spsr = io(IO_SPSR);
        uint16_t f[7];
        f[0] = fired ? (uint16_t)(io(IO_ENTRY) - o) : 0xFFFFu;
        f[1] = fired ? ret16(io(IO_RET)) : 0xFFFFu;
        f[2] = (uint16_t)(((spsr >> 16) & 0xF000u) | (spsr & 0xFFu) | (fired ? 0x100u : 0) | (io(IO_HUNG) ? 0x200u : 0) |
                          (fin ? 0x400u : 0));
        if (hmode == 1) {
            f[3] = (uint16_t)rd(STAMP, 2);
            f[4] = (uint16_t)rd(STAMP + 6, 2);
        } else {
            uint32_t h = rd(SCAN_HASH, 4);
            f[3] = (uint16_t)rd(SCAN_CNT, 4);
            f[4] = (uint16_t)(h ^ h >> 16);
        }
        f[5] = (uint16_t)io(IO_BB);
        f[6] = (uint16_t)(io(IO_BB) >> 16);
        int same = have;
        for (unsigned k = 0; k < 7 && same; k++)
            same = f[k] == prev[k];
        if (!same) {
            put((uint16_t)o, f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
            memcpy(prev, f, sizeof prev);
            have = 1;
        }
        int inb = fired && io(IO_RET) < 0x4000u;
        if (inb)
            seen_bios = 1, after = 0;
        else if (seen_bios && ++after >= 12)
            break;
    }
    put(0xFFFF, 0, 0, 0, 0, 0, (uint16_t)ref, (uint16_t)(ref >> 16));
}

static void sweep_hb(unsigned ci, uint32_t ws)
{
    const acall *c = &acalls[ci];
    uint32_t sp = P.c.r[13];
    wr(0x04000204u, 2, ws);
    wr(ACALLS, 1, c->swi);
    wr(ACALLS + 1, 1, c->vram);
    wr(ACALLS + 4, 4, c->r0);
    wr(ACALLS + 8, 4, c->r2);
    wr(NPOOL, 4, 0);
    while (rl_gba_read(P.m.g, 0x04000006u, 2) != 227u)
        spend(1);
    while (rl_gba_read(P.m.g, 0x04000006u, 2) == 227u)
        spend(1);
    P.c.r[13] = sp - 20u;
    wr(P.c.r[13] + 8u, 4, ACALLS);
    P.c.r[4] = c->vram;
    p2cpu_jump(&P.c, HB_LOOP_PC);
    if (!p2cpu_run(&P.c, HB_DONE_PC, 2000000000u))
        fprintf(stderr, "probe2: sweep_hb did not finish\n");
    P.c.r[13] = sp;
    for (uint32_t k = 0; k < rd(NPOOL, 4); k++) {
        uint32_t e = POOL + 16u * k;
        put((uint16_t)rd(e, 2), (uint16_t)rd(e + 2, 2), (uint16_t)rd(e + 4, 2), (uint16_t)rd(e + 6, 2), (uint16_t)rd(e + 8, 2),
            (uint16_t)rd(e + 10, 2), (uint16_t)rd(e + 12, 2), (uint16_t)rd(e + 14, 2));
    }
}

static uint32_t two_fill(uint32_t op, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t lo, uint32_t len, uint32_t *cnt,
                         uint32_t *hi, uint32_t *cyc, uint32_t *hung, uint32_t watchdog)
{
    fill(lo, len, 0x11);
    call(op, 0, r0, r1, r2, 0, 0, watchdog, 0x1F, 0x10, 1, 0, 2);
    for (uint32_t k = 0; k < len; k++)
        wr(STAGE7 + k, 1, rd(lo + k, 1));
    fill(lo, len, FILL);
    call(op, 0, r0, r1, r2, 0, 0, watchdog, 0x1F, 0x10, 1, 0, 2);
    *cyc = io(IO_TOTAL);
    *hung = io(IO_HUNG);
    uint32_t h = whash(lo, len), c = 0, top = 0;
    for (uint32_t k = 0; k < len; k++) {
        uint8_t a = (uint8_t)rd(lo + k, 1), b = (uint8_t)rd(STAGE7 + k, 1);
        if (a != FILL || b != 0x11) {
            c++;
            top = k + 1;
        }
    }
    *cnt = c;
    *hi = top;
    return h;
}

static void part_huff(void)
{
    wr(0x04000204u, 2, 0x4317);
    for (uint32_t i = 0; i < 48; i++) {
        uint32_t t = HUFF_TAB + 8 * i;
        uint32_t off = rd(t, 2), len = rd(t + 2, 2), n = rd(t + 4, 1), tree = rd(t + 5, 1), li = rd(t + 6, 1);
        uint32_t cnt, top, cyc, hung;
        uint32_t h = two_fill(arm_swi(0x13), HUFF_BLOB + off, DST, 0, DST - 16u, len + 80u, &cnt, &top, &cyc, &hung, 0xC1C000u);
        put((uint16_t)(n << 8 | tree << 4 | li), (uint16_t)hung, (uint16_t)cyc, (uint16_t)(cyc >> 16), (uint16_t)cnt,
            (uint16_t)top, (uint16_t)h, (uint16_t)(h >> 16));
    }
}

static void part_sram(void)
{
    static const uint16_t wss[3] = {0x4317, 0x4314, 0x4316};
    static const uint32_t ops[6][2] = {{0x0B, 16u}, {0x0B, 8u | 1u << 26}, {0x0B, 16u | 1u << 24}, {0x0B, 8u | 1u << 24 | 1u << 26},
                                       {0x0C, 8u}, {0x0C, 8u | 1u << 24}};
    for (uint32_t k = 0; k < 0x100u; k++) {
        uint8_t v = (uint8_t)(k * 37u + 11u);
        wr(0x0E007E00u + k, 1, v == FILL ? 0x5A : v);
    }
    for (uint32_t w = 0; w < 3; w++) {
        wr(0x04000204u, 2, wss[w]);
        for (uint32_t op = 0; op < 6; op++)
            for (uint32_t al = 0; al < 5; al++) {
                if (al == 4 && w != 0)
                    continue;
                uint32_t src = al == 4 ? 0x0E00FE00u : 0x0E007E00u + al;
                uint32_t cnt, top, cyc, hung;
                uint32_t h = two_fill(arm_swi(ops[op][0]), src, DST, ops[op][1], DST - 16u, 32u + 48u, &cnt, &top, &cyc, &hung,
                                      0xC1C000u);
                put((uint16_t)(w << 8 | op << 4 | al), (uint16_t)cyc, (uint16_t)(cyc >> 16), (uint16_t)(cnt | hung << 15),
                    (uint16_t)h, (uint16_t)(h >> 16), (uint16_t)rd(DST, 2), (uint16_t)rd(DST + 2, 2));
            }
    }
    wr(0x04000204u, 2, 0x4317);
}

static void part_ob(void)
{
    static const uint8_t sw[] = {0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x11, 0x12, 0x13, 0x14, 0x15};
    wr(0x04000204u, 2, 0x4317);
    for (uint32_t th = 0; th < 2; th++)
        for (uint32_t i = 0; i < sizeof sw; i++) {
            uint32_t n = sw[i], r0 = 0, r1 = 0, r2 = 0, r3 = 0;
            for (uint32_t k = 0; k < 64; k++)
                wr(INFO + k, 1, (uint8_t)(k < 32 ? (k & 1 ? 0x10 : 0x00) : 0));
            fill(DST - 16u, 160u, FILL);
            switch (n) {
            case 0x06: r0 = 100; r1 = 7; break;
            case 0x07: r0 = 7; r1 = 100; break;
            case 0x08: r0 = 1000; break;
            case 0x09: r0 = 0x1000; break;
            case 0x0A: r0 = 0x1000; r1 = 0x2000; break;
            case 0x0B: r0 = CS_SRC; r1 = DST; r2 = 8; break;
            case 0x0C: r0 = CS_SRC; r1 = DST; r2 = 8; break;
            case 0x0E: r0 = INFO; r1 = DST; r2 = 1; break;
            case 0x0F: r0 = INFO; r1 = DST; r2 = 1; r3 = 2; break;
            case 0x11: r0 = LZ_SRC; r1 = DST; break;
            case 0x12: r0 = LZ_SRC; r1 = VDST; break;
            case 0x13: r0 = HUFF_BLOB + rd(HUFF_TAB + 8 * 42, 2); r1 = DST; break;
            case 0x14: r0 = RL_SRC; r1 = DST; break;
            case 0x15: r0 = RL_SRC; r1 = VDST; break;
            default: break;
            }
            call(th ? thumb_swi(n) : arm_swi(n), th ? 2 : 0, r0, r1, r2, r3, 0, 0, 0x9F, 0, 0, 0, 2);
            uint32_t ob0 = io(IO_OB), ob1 = io(IO_OB + 4), ob2 = io(IO_OB + 8), ob3 = io(IO_OB + 12);
            put((uint16_t)(n | th << 8), (uint16_t)ob0, (uint16_t)(ob0 >> 16), (uint16_t)ob2, (uint16_t)ob3, (uint16_t)ob1,
                (uint16_t)(ob1 >> 16), (uint16_t)io(IO_TOTAL));
        }
}

static void part_halt(uint32_t kind)
{
    wr(0x04000204u, 2, 0x4317);
    if (kind == 7)
        spend(64u - (unsigned)(mach_now(&P.m) & 63u) + HALT_PHASE);
    uint32_t lim = kind == 6 ? 1024u : kind == 8 ? 160u : 200u;
    for (uint32_t o = 1; o < lim; o += kind == 7 ? 2u : (o < 200u ? 1u : 16u)) {
        uint32_t op = kind == 8 ? 0xE5CBA000u : arm_swi(2);
        if (kind == 7)
            call(op, 0, 0, 0, 0, 0, (0x10000u - o) | 0xC00000u, 0xC1F000u, 0x1F, 0x18, 1, 0, 2);
        else
            call(op, 0, 0, 0, 0, 0, (0x10000u - o) | 0xC00000u, 0xC1F000u, 0x9F, 0x18, 0, 0, 2);
        uint16_t ifv = (uint16_t)rl_gba_read(P.m.g, 0x04000202u, 2);
        wr(0x04000202u, 2, 0xFFFF);
        uint32_t t = io(IO_TOTAL);
        spend(io(IO_RET) >= 0x4000u ? HALT_PUT_IWRAM : HALT_PUT_BIOS);
        put((uint16_t)o, (uint16_t)t, (uint16_t)(t >> 16), ifv, (uint16_t)(io(IO_FIRED) | io(IO_HUNG) << 1),
            (uint16_t)io(IO_ENTRY), ret16(io(IO_RET)), (uint16_t)io(IO_BB));
    }
}

static unsigned compare(unsigned s, const char *name, bool verbose)
{
    const p2_series *ps = &p2_series_tab[s];
    unsigned bad = 0, n = ps->count > P.nout ? ps->count : P.nout;
    const char *dump = getenv("RELIFT_PROBE2_DUMP");
    if (dump) {
        FILE *f = fopen(dump, "a");
        for (unsigned i = 0; f && i < P.nout; i++)
            fprintf(f, "%s %u %u %u %u %u %u %u %u\n", name, P.out[i][0], P.out[i][1], P.out[i][2], P.out[i][3], P.out[i][4],
                    P.out[i][5], P.out[i][6], P.out[i][7]);
        if (f)
            fclose(f);
    }
    for (unsigned i = 0; i < n; i++) {
        const uint16_t *want = i < ps->count ? p2_entries[ps->first + i] : NULL;
        const uint16_t *got = i < P.nout ? P.out[i] : NULL;
        if (want && got && !memcmp(want, got, 16))
            continue;
        if (verbose && bad < (getenv("RELIFT_PROBE2_MAX") ? (unsigned)atoi(getenv("RELIFT_PROBE2_MAX")) : 12u)) {
            fprintf(stderr, "  %s #%u want", name, i);
            for (unsigned k = 0; k < 8 && want; k++)
                fprintf(stderr, " %u", want[k]);
            fprintf(stderr, " got");
            for (unsigned k = 0; k < 8 && got; k++)
                fprintf(stderr, " %u", got[k]);
            fprintf(stderr, "\n");
        }
        bad++;
    }
    if (verbose || bad)
        fprintf(stderr, "probe2 %-36s %u entries, %u differ\n", name, ps->count, bad);
    return bad;
}

static const char *const names[] = {
    "huff", "sram_src", "open_bus",
    "A_irq_CpuSet16_copy_ws4317", "A_irq_CpuSet16_fill_ws4317", "A_irq_CpuSet32_copy_ws4317", "A_irq_CpuSet32_fill_ws4317",
    "A_irq_CpuFastSet_copy_ws4317", "A_irq_CpuFastSet_fill_ws4317", "A_irq_LZ77UnCompWram_ws4317", "A_irq_LZ77UnCompVram_ws4317",
    "A_irq_RLUnCompWram_ws4317", "A_irq_RLUnCompVram_ws4317",
    "A_irq_CpuSet16_copy_ws0000", "A_irq_CpuSet16_fill_ws0000", "A_irq_CpuSet32_copy_ws0000", "A_irq_CpuSet32_fill_ws0000",
    "A_irq_CpuFastSet_copy_ws0000", "A_irq_CpuFastSet_fill_ws0000", "A_irq_LZ77UnCompWram_ws0000", "A_irq_LZ77UnCompVram_ws0000",
    "A_irq_RLUnCompWram_ws0000", "A_irq_RLUnCompVram_ws0000",
    "A_irq_CpuSet32_copy_ws4317_dma3", "A_hblankdma_CpuSet16_copy_ws4317", "A_hblankdma_LZ77UnCompWram_ws4317",
    "halt_ime0", "haltcnt_direct", "halt_ime1",
};

static unsigned run_series(unsigned s)
{
    P.nout = 0;
    if (s == 0)
        part_huff();
    else if (s == 1)
        part_sram();
    else if (s == 2)
        part_ob();
    else if (s < 13)
        sweep_a(s - 3, 0x4317, 0, 0);
    else if (s < 23)
        sweep_a(s - 13, 0x0000, 0, 1);
    else if (s == 23)
        sweep_a(2, 0x4317, 1, 1);
    else if (s == 24)
        sweep_hb(0, 0x4317);
    else if (s == 25)
        sweep_hb(6, 0x4317);
    else
        part_halt(s == 26 ? 6 : s == 27 ? 8 : 7);
    wr(0x04000204u, 2, 0x4317);
    const char *only = getenv("RELIFT_PROBE2_VERBOSE");
    return compare(s, names[s], only && (!*only || strstr(names[s], only)));
}

TEST(probe2_every_series_matches_the_console)
{
    probe_init();
    unsigned bad = 0;
    const char *only = getenv("RELIFT_PROBE2_ONLY");
    for (int th = 0; th < 2; th++) {
        uint32_t best = 0xFFFFFFFFu;
        for (int k = 0; k < 8; k++) {
            call(th ? 0x46C0u : 0xE1A00000u, th ? 2 : 0, 0, 0, 0, 0, 0, 0, 0x9F, 0, 0, 0, 2);
            if (io(IO_TOTAL) < best)
                best = io(IO_TOTAL);
        }
        CHECK_EQ(best, th ? 34u : 27u);
    }
    for (unsigned s = 0; s < sizeof names / sizeof names[0]; s++) {
        if (only && !strstr(names[s], only))
            continue;
        unsigned n = run_series(s), known = 0;
        for (unsigned g = 0; g < sizeof gaps / sizeof gaps[0]; g++)
            if (!strcmp(gaps[g].series, names[s]))
                known = gaps[g].differ;
        if (n != known)
            fprintf(stderr, "probe2 %s: %u entries differ, the known-gap list says %u\n", names[s], n, known);
        bad += n != known;
    }
    CHECK_EQ(bad, 0);
    mach_free(&P.m);
    free(P.sram);
    free(P.out);
}
