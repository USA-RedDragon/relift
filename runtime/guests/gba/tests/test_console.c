/**
 * @file test_console.c
 * @brief Every record of the console SWI probe, replayed against the HLE BIOS
 * over a small memory: registers, output windows, stack frames and cycles
 * must equal what the console did, with the cycles of the data accesses
 * from a fixed table and from the machine's wait table.
 */
#include "bios/bios_int.h"
#include "mach.h"
#include "runner.h"
#include "swicase.h"
#include "vectors_console.h"

#include <string.h>

enum {
    RESET_SWI = 0x01,
    HUFF_SWI = 0x13,
    WAITCNT_PROBE = 0x4317,
    ARM_RETURN = 0x030000A0,
    THUMB_RETURN = 0x030001B6
};

typedef struct machine {
    uint8_t ewram[0x40000], iwram[0x8000], pal[0x400], vram[0x18000], oam[0x400];
    uint16_t io[0x200];
    rl_gba_bios bios;
} machine;

static machine m;

static uint8_t *ptr(uint32_t a)
{
    switch (a >> 24) {
    case 2:
        return &m.ewram[a & 0x3FFFFu];
    case 3:
        return &m.iwram[a & 0x7FFFu];
    case 5:
        return &m.pal[a & 0x3FFu];
    case 6: {
        uint32_t o = a & 0x1FFFFu;
        return &m.vram[o >= 0x18000u ? o - 0x8000u : o];
    }
    case 7:
        return &m.oam[a & 0x3FFu];
    default:
        return NULL;
    }
}

static uint16_t io_mask(uint32_t off)
{
    switch (off) {
    case 0x00:
        return 0xFFF7;
    case 0x08:
    case 0x0A:
        return 0xDFFF;
    case 0x48:
    case 0x4A:
        return 0x3F3F;
    case 0x50:
        return 0x3FFF;
    case 0x52:
        return 0x1F1F;
    case 0x80:
        return 0xFF77;
    case 0x82:
        return 0x770F;
    case 0x84:
        return 0x0080;
    case 0x88:
        return 0xC3FE;
    case 0xBA:
    case 0xC6:
    case 0xD2:
        return 0xF7E0;
    case 0xDE:
        return 0xFFE0;
    case 0x200:
        return 0x3FFF;
    case 0x204:
        return 0x5FFF;
    case 0x208:
        return 1;
    default:
        return 0xFFFF;
    }
}

static uint8_t rd8(void *u, uint32_t a)
{
    (void)u;
    if ((a >> 24) == 4) {
        uint32_t o = a & 0x3FFu;
        return (uint8_t)(m.io[o / 2] >> ((o & 1u) * 8));
    }
    if ((a >> 24) == 0 && a < 0x4000u)
        return (uint8_t)rl_gba_bios_read(&m.bios, a, 1);
    if ((a >> 24) == 0x0E || (a >> 24) == 0x0F)
        return 0xFF;
    uint8_t *p = ptr(a);
    return p ? *p : (uint8_t)(m.bios.open_bus >> ((a & 3u) * 8));
}

static void wr8(void *u, uint32_t a, uint8_t v)
{
    (void)u;
    if ((a >> 24) == 4) {
        uint32_t o = a & 0x3FFu;
        if (o == 0x301u)
            return;
        uint16_t cur = m.io[(o & ~1u) / 2];
        cur = (o & 1u) ? (uint16_t)((cur & 0xFFu) | v << 8) : (uint16_t)((cur & 0xFF00u) | v);
        if ((o & ~1u) == 0x202u)
            m.io[0x202 / 2] &= (uint16_t)~((o & 1u) ? v << 8 : v);
        else
            m.io[(o & ~1u) / 2] = cur & io_mask(o & ~1u);
        return;
    }
    if ((a >> 24) == 5) {
        a &= ~1u;
        m.pal[a & 0x3FFu] = v;
        m.pal[(a & 0x3FFu) + 1] = v;
        return;
    }
    if ((a >> 24) == 6) {
        uint8_t *p = ptr(a & ~1u);
        if ((a & 0x1FFFFu) < 0x14000u) {
            p[0] = v;
            p[1] = v;
        }
        return;
    }
    if ((a >> 24) == 7)
        return;
    uint8_t *p = ptr(a);
    if (p)
        *p = v;
}

static void wr16(void *u, uint32_t a, uint16_t v)
{
    (void)u;
    if ((a >> 24) == 4) {
        uint32_t o = a & 0x3FEu;
        if (o == 0x202u)
            m.io[o / 2] &= (uint16_t)~v;
        else if (o == 0x140u)
            m.io[o / 2] = v & 0x40u;
        else
            m.io[o / 2] = v & io_mask(o);
        return;
    }
    uint8_t *p = ptr(a & ~1u);
    if (p) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    }
}

static void wr32(void *u, uint32_t a, uint32_t v)
{
    wr16(u, a & ~3u, (uint16_t)v);
    wr16(u, (a & ~3u) + 2, (uint16_t)(v >> 16));
}

static uint32_t bus_read(void *u, uint32_t a, unsigned size)
{
    if ((a >> 24) == 0 && a < 0x4000u)
        return rl_gba_bios_read(&m.bios, a, size);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint32_t)rd8(u, a + i) << (8 * i);
    return v;
}

static void bus_write(void *u, uint32_t a, unsigned size, uint32_t v)
{
    if (size == 1)
        wr8(u, a, (uint8_t)v);
    else if (size == 2)
        wr16(u, a, (uint16_t)v);
    else
        wr32(u, a, v);
}

static unsigned bus_cycles(void *u, uint32_t a, unsigned size, bool seq)
{
    (void)u;
    (void)seq;
    switch (a >> 24) {
    case 2:
        return size == 4 ? 6 : 3;
    case 5:
    case 6:
        return size == 4 ? 2 : 1;
    case 0x0E:
    case 0x0F:
        return 9;
    default:
        return 1;
    }
}

static void put8(void *u, uint32_t a, uint8_t v) { wr8(u, a, v); }

static uint32_t hash_stack(void)
{
    uint32_t h = SWI_FNV_INIT;
    for (uint32_t a = SWI_SP_SYS - SWI_STK_SYS; a < SWI_SP_SYS; a++)
        h = swi_fnv(h, rd8(NULL, a));
    for (uint32_t a = SWI_SP_SVC - SWI_STK_SVC; a < SWI_SP_SVC; a++)
        h = swi_fnv(h, rd8(NULL, a));
    return h;
}

typedef struct totals {
    unsigned checked, skipped, bad_regs, bad_mem, bad_stack, bad_cycles;
} totals;

static void run_record(unsigned i, const rl_gba_bus *bus, totals *t)
{
    const swi_mem mem = {NULL, rd8, put8, wr16, wr32};
    const gbat_rec *want = &gbat_console[i];
    unsigned flags = swi_record_flags(i), calls = swi_record_calls(i);
    if (flags & SWI_REC_IRQ) {
        t->skipped++;
        return;
    }
    uint32_t fr = SWI_FNV_INIT, fm = SWI_FNV_INIT, fs = SWI_FNV_INIT, cycles = 0;
    gbat_rec got = {{0}, 0, 0, 0, 0, 0};
    bool unmodelled = false;
    bool reset = false;
    for (unsigned j = 0; j < calls; j++) {
        swi_case c;
        swi_case_make(i, j, &c, &mem);
        for (uint32_t a = SWI_SP_SYS - SWI_STK_SYS; a < SWI_SP_SYS; a++)
            wr8(NULL, a, SWI_FILL);
        for (uint32_t a = SWI_SP_SVC - SWI_STK_SVC; a < SWI_SP_SVC; a++)
            wr8(NULL, a, SWI_FILL);
        rl_gba_swi_cpu cpu = {
            .r = {c.r[0], c.r[1], c.r[2], c.r[3], SWI_R4, SWI_R5, SWI_R6, SWI_R7, SWI_R8, SWI_R9, SWI_R10, SWI_R11,
                  c.thumb ? THUMB_RETURN - 1 : 0xCCCCCCCCu},
            .ret = c.thumb ? THUMB_RETURN : ARM_RETURN,
            .cpsr = SWI_CPSR | (c.thumb ? 0x20u : 0u),
            .sp_svc = SWI_SP_SVC,
            .sp_sys = SWI_SP_SYS,
            .lr_sys = SWI_LR_SYS,
        };
        rl_gba_swi_status st = rl_gba_swi(&m.bios, bus, c.swi, &cpu);
        uint32_t h = swi_case_hash(i, j, &c, &mem);
        uint32_t cpsr = cpu.cpsr & ~0x20u;
        if (st == RL_GBA_SWI_HANG)
            cpu.r[0] = 0xDEADDEADu;
        reset = c.swi == RESET_SWI;
        unmodelled |= c.swi == HUFF_SWI && m.bios.events[BE_HUFF_ODD_SIZE] != 0;
        if (calls == 1) {
            for (int k = 0; k < 4; k++)
                got.r[k] = cpu.r[k];
            got.r12 = cpu.r[12];
            got.cpsr = cpsr;
            got.mem = h;
            got.stk = hash_stack();
            got.cycles = (uint32_t)m.bios.cycles;
        } else {
            uint32_t v[6] = {cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[12], cpsr};
            for (int k = 0; k < 6; k++)
                fr = swi_fold(fr, v[k]);
            fm = swi_fold(fm, h);
            fs = swi_fold(fs, hash_stack());
            cycles += (uint32_t)m.bios.cycles;
        }
    }
    if (calls > 1) {
        got.r[0] = fr;
        got.mem = fm;
        got.stk = fs;
        got.cycles = cycles;
    }
    if (unmodelled) {
        t->skipped++;
        return;
    }
    t->checked++;
    t->bad_regs += memcmp(got.r, want->r, sizeof got.r) != 0 || got.r12 != want->r12 || got.cpsr != want->cpsr;
    t->bad_mem += !reset && got.mem != want->mem;
    t->bad_stack += got.stk != want->stk;
    t->bad_cycles += got.cycles != want->cycles;
}

static rl_gba *machine_bus;

static unsigned machine_cycles(void *u, uint32_t a, unsigned size, bool seq)
{
    (void)u;
    const rl_gba_bus *b = rl_gba_bios_bus(machine_bus);
    return b->cycles(b->user, a, size, seq);
}

static totals replay(const rl_gba_bus *bus)
{
    memset(&m, 0, sizeof m);
    rl_gba_bios_init(&m.bios);
    m.io[0x204 / 2] = WAITCNT_PROBE;
    totals t = {0};
    for (unsigned i = 0; i < swi_record_count(); i++)
        run_record(i, bus, &t);
    return t;
}

TEST(console_records_replay_exactly)
{
    unsigned count = sizeof gbat_console / sizeof gbat_console[0];
    CHECK_EQ(count, swi_record_count());
    totals t = replay(&(rl_gba_bus){.read = bus_read, .write = bus_write, .cycles = bus_cycles});
    CHECK(t.checked > 3400);
    CHECK_EQ(t.skipped + t.checked, count);
    CHECK_EQ(t.bad_regs, 0);
    CHECK_EQ(t.bad_mem, 0);
    CHECK_EQ(t.bad_stack, 0);
    CHECK_EQ(t.bad_cycles, 0);
}

TEST(console_records_keep_their_cycles_on_the_machines_wait_table)
{
    mach x = mach_new();
    CHECK(x.g != NULL);
    rl_gba_write(x.g, 0x04000204u, 2, WAITCNT_PROBE);
    machine_bus = x.g;
    totals t = replay(&(rl_gba_bus){.read = bus_read, .write = bus_write, .cycles = machine_cycles});
    CHECK(t.checked > 3400);
    CHECK_EQ(t.bad_regs, 0);
    CHECK_EQ(t.bad_cycles, 0);
    mach_free(&x);
}
