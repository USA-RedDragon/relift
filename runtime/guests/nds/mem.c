/**
 * @file mem.c
 * @brief The address maps of both CPUs with their mirrors, the shared work
 * RAM banks, the ARM9's TCMs, the BIOS stubs and the GBA slot's open bus;
 * the cost tables; creating the machine and its untimed accesses.
 *
 * Fresh C from GBATEK ("DS Memory Maps", "DS Memory Control", "DS Memory
 * Timings", "ARM CP15 Tightly Coupled Memory").
 */
#include "nds_int.h"

#include <stdlib.h>
#include <string.h>

/** @brief The decode of cpu's video RAM address addr through its view's page table. */
static nds_where vram_at(rl_nds *m, unsigned cpu, uint32_t addr)
{
    unsigned v = nds_vram_cpu_view(cpu, addr);
    const nds_vpage *pg = nds_vram_page(m, v, addr);
    if (pg->p)
        return (nds_where){.p = pg->p + (addr & m->vview[v].mask), .kind = NDS_K_MEM};
    return (nds_where){.kind = pg->banks ? NDS_K_VRAM : NDS_K_NONE};
}

/** @brief The shared work RAM bank cpu sees at addr, or NULL when it has none (WRAMCNT). */
static uint8_t *swram_at(rl_nds *m, unsigned cpu, uint32_t addr)
{
    unsigned c = m->wramcnt & 3;
    if (cpu == RL_NDS_ARM7)
        c ^= 3;
    switch (c) {
    case 0:
        return &m->swram[addr & 0x7FFF];
    case 1:
        return &m->swram[0x4000 | (addr & 0x3FFF)];
    case 2:
        return &m->swram[addr & 0x3FFF];
    default:
        return NULL;
    }
}

static nds_where at(uint8_t *p, bool main)
{
    return (nds_where){.p = p, .kind = NDS_K_MEM, .main = main};
}

static nds_where decode9(rl_nds *m, uint32_t addr, bool code, bool write, bool tcm_on)
{
    uint8_t *tcm = tcm_on ? nds_tcm(m, addr, code, write) : NULL;
    if (tcm)
        return (nds_where){.p = tcm, .kind = NDS_K_MEM, .tcm = true};
    switch (addr >> 24) {
    case 0x02:
        return at(&m->main[addr & (RL_NDS_MAIN_SIZE - 1)], true);
    case 0x03: {
        uint8_t *p = swram_at(m, RL_NDS_ARM9, addr);
        return p ? at(p, false) : (nds_where){.kind = NDS_K_NONE};
    }
    case 0x04:
        return (nds_where){.kind = NDS_K_IO};
    case 0x05:
        return at(&m->pal[addr & (RL_NDS_PAL_SIZE - 1)], false);
    case 0x06:
        return vram_at(m, RL_NDS_ARM9, addr);
    case 0x07:
        return at(&m->oam[addr & (RL_NDS_OAM_SIZE - 1)], false);
    case 0x08:
    case 0x09:
    case 0x0A:
        return (nds_where){.kind = NDS_K_SLOT};
    default:
        if (addr >= 0xFFFF0000u && addr < 0xFFFF8000u)
            return (nds_where){.kind = NDS_K_BIOS};
        return (nds_where){.kind = NDS_K_NONE};
    }
}

static nds_where decode7(rl_nds *m, uint32_t addr)
{
    switch (addr >> 24) {
    case 0x00:
        return (nds_where){.kind = addr < 0x4000 ? NDS_K_BIOS : NDS_K_NONE};
    case 0x02:
        return at(&m->main[addr & (RL_NDS_MAIN_SIZE - 1)], true);
    case 0x03: {
        uint8_t *p = addr < 0x03800000u ? swram_at(m, RL_NDS_ARM7, addr) : NULL;
        return at(p ? p : &m->wram7[addr & (RL_NDS_WRAM7_SIZE - 1)], false);
    }
    case 0x04:
        return (nds_where){.kind = addr < 0x04800000u ? NDS_K_IO : NDS_K_NONE};
    case 0x06:
        return vram_at(m, RL_NDS_ARM7, addr);
    case 0x08:
    case 0x09:
    case 0x0A:
        return (nds_where){.kind = NDS_K_SLOT};
    default:
        return (nds_where){.kind = NDS_K_NONE};
    }
}

nds_where nds_decode(rl_nds *m, unsigned cpu, uint32_t addr, bool code, bool write)
{
    return cpu == RL_NDS_ARM9 ? decode9(m, addr, code, write, true) : decode7(m, addr);
}

nds_where nds_decode_bus(rl_nds *m, unsigned cpu, uint32_t addr, bool write)
{
    return cpu == RL_NDS_ARM9 ? decode9(m, addr, false, write, false) : decode7(m, addr);
}

bool nds_drops_byte(unsigned cpu, uint32_t addr)
{
    unsigned r = addr >> 24;
    return cpu == RL_NDS_ARM9 && r >= 0x05 && r <= 0x07;
}

bool nds_shared(unsigned cpu, uint32_t addr)
{
    unsigned r = addr >> 24;
    if (cpu == RL_NDS_ARM9)
        return (r >= 0x02 && r <= 0x04) || (r >= 0x08 && r <= 0x0A);
    if (r == 0x03)
        return addr < 0x03800000u;
    return r == 0x02 || r == 0x04 || r == 0x06 || (r >= 0x08 && r <= 0x0A);
}

bool rl_nds_ordered(const rl_nds *m, unsigned cpu, uint32_t addr)
{
    if (cpu == RL_NDS_ARM9) {
        const rl_arm32_tcm *t = &m->tcm;
        if (t->itcm_on && addr < t->itcm_size)
            return false;
        if (t->dtcm_on && (uint64_t)(addr - t->dtcm_base) < t->dtcm_size)
            return false;
    }
    return nds_shared(cpu, addr);
}

uint32_t nds_slot_read(const rl_nds *m, unsigned cpu, uint32_t addr, unsigned size)
{
    unsigned owner = (m->exmem[RL_NDS_ARM9] >> 7) & 1;
    if (cpu != owner)
        return 0;
    if ((addr >> 24) == 0x0A)
        return nds_mask(size);
    uint32_t h0, h1;
    switch ((m->exmem[cpu] >> 2) & 3) {
    case 0:
        h0 = ((addr & ~3u) >> 1 & 0xFFFF) | 0xFE08;
        h1 = (((addr & ~3u) + 2) >> 1 & 0xFFFF) | 0xFE08;
        break;
    case 3:
        h0 = h1 = 0xFFFF;
        break;
    default:
        h0 = (addr & ~3u) >> 1 & 0xFFFF;
        h1 = ((addr & ~3u) + 2) >> 1 & 0xFFFF;
        break;
    }
    uint32_t w = h0 | h1 << 16;
    return (w >> (8 * (addr & 3))) & nds_mask(size);
}

/** @brief Bus cycles of N32, S32, N16, S16 per CPU, kind and region class, doubled (ticks). */
enum { C_MAIN, C_FAST, C_VRAM, C_CLASSES };
static const uint8_t base_costs[2][2][C_CLASSES][4] = {
    [RL_NDS_ARM9][RL_NDS_CODE] = {{18, 18, 9, 9}, {8, 8, 4, 4}, {10, 10, 5, 5}},
    [RL_NDS_ARM9][RL_NDS_DATA] = {{20, 4, 18, 2}, {8, 2, 8, 2}, {10, 4, 8, 2}},
    [RL_NDS_ARM7][RL_NDS_CODE] = {{18, 4, 16, 2}, {2, 2, 2, 2}, {4, 4, 2, 2}},
    [RL_NDS_ARM7][RL_NDS_DATA] = {{20, 4, 18, 2}, {2, 2, 2, 2}, {4, 4, 2, 2}},
};

static unsigned class_of(unsigned region)
{
    switch (region) {
    case 0x02:
        return C_MAIN;
    case 0x05:
    case 0x06:
        return C_VRAM;
    default:
        return C_FAST;
    }
}

static void put(rl_nds *m, unsigned cpu, unsigned kind, unsigned r, unsigned n32, unsigned s32, unsigned n16,
                unsigned s16)
{
    uint8_t (*t)[2] = m->costs.t[cpu][kind][r];
    t[0][0] = t[1][0] = (uint8_t)n16;
    t[0][1] = t[1][1] = (uint8_t)s16;
    t[2][0] = (uint8_t)n32;
    t[2][1] = (uint8_t)s32;
}

void nds_costs_rebuild(rl_nds *m)
{
    static const unsigned first[4] = {10, 8, 6, 18};
    static const unsigned second[2] = {6, 4};
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        for (unsigned kind = 0; kind < 2; kind++)
            for (unsigned r = 0; r < 16; r++) {
                const uint8_t *b = base_costs[cpu][kind][class_of(r)];
                put(m, cpu, kind, r, b[0], b[1], b[2], b[3]);
            }
        uint16_t x = m->exmem[cpu];
        unsigned n = first[(x >> 2) & 3], s = second[(x >> 4) & 1], ram = first[x & 3];
        for (unsigned r = 0x08; r <= 0x09; r++) {
            if (cpu == RL_NDS_ARM9) {
                unsigned w = 2 * (n + s + 3);
                put(m, cpu, RL_NDS_CODE, r, w, w, w / 2, w / 2);
                put(m, cpu, RL_NDS_DATA, r, w, 4 * s, 2 * (n + 3), 2 * s);
            } else {
                put(m, cpu, RL_NDS_CODE, r, 2 * (n + s), 4 * s, 2 * n, 2 * s);
                put(m, cpu, RL_NDS_DATA, r, 2 * (n - 1 + s), 4 * s, 2 * (n - 1), 2 * s);
            }
        }
        unsigned rn = cpu == RL_NDS_ARM9 ? ram + 3 : ram - 1;
        put(m, cpu, RL_NDS_CODE, 0x0A, 2 * rn, 2 * ram, 2 * rn, 2 * ram);
        put(m, cpu, RL_NDS_DATA, 0x0A, 2 * rn, 2 * ram, 2 * rn, 2 * ram);
    }
}

const rl_nds_costs *rl_nds_cost_table(const rl_nds *m) { return &m->costs; }

rl_nds *rl_nds_create(void)
{
    rl_nds *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    rl_arm32_cp15_reset(&m->cp15);
    nds_cache_reset(m);
    rl_arm32_cp15_tcm(&m->cp15, &m->tcm);
    nds_io_reset(m);
    nds_vram_rebuild(m);
    nds_costs_rebuild(m);
    return m;
}

void rl_nds_destroy(rl_nds *m)
{
    if (!m)
        return;
    nds_bk_free(&m->cart.bk);
    nds_spi_free(m);
    free(m);
}

int rl_nds_attach(rl_nds *m, rl_sched *s, rl_sched_cpu *arm9, rl_sched_cpu *arm7)
{
    if (m->sched || !s || !arm9 || !arm7 || arm9 == arm7 || rl_sched_of(arm9) != s || rl_sched_of(arm7) != s)
        return -1;
    m->sched = s;
    m->cpu[RL_NDS_ARM9] = arm9;
    m->cpu[RL_NDS_ARM7] = arm7;
    if (nds_io_attach(m) != 0 || nds_order_attach(m) != 0) {
        m->sched = NULL;
        m->cpu[0] = m->cpu[1] = NULL;
        return -1;
    }
    nds_timer_attach(m);
    nds_lcd_attach(m);
    nds_disp_resync(m, nds_now(m, RL_NDS_ARM9));
    nds_dma_attach(m);
    nds_gx_attach(m);
    nds_cart_attach(m);
    nds_spi_attach(m);
    return 0;
}

rl_sched_cpu *rl_nds_cpu(const rl_nds *m, unsigned cpu) { return cpu < 2 ? m->cpu[cpu] : NULL; }
uint64_t rl_nds_now(const rl_nds *m, unsigned cpu) { return cpu < 2 ? nds_now(m, cpu) : 0; }

static uint32_t io_untimed_read(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size)
{
    if (m->sched)
        return (uint32_t)rl_sched_read(m->cpu[cpu], nds_io_key(cpu, addr), size);
    return nds_io_read(m, cpu, addr, size, 0);
}

static void io_untimed_write(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value)
{
    if (m->sched)
        rl_sched_write(m->cpu[cpu], nds_io_key(cpu, addr), size, value);
    else
        nds_io_write(m, cpu, addr, size, value, 0);
}

uint32_t rl_nds_read(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size)
{
    addr &= ~(size - 1);
    nds_where w = nds_decode(m, cpu, addr, false, false);
    switch (w.kind) {
    case NDS_K_MEM:
        return nds_ld(w.p, size);
    case NDS_K_VRAM:
        return nds_vram_read_slow(m, nds_vram_cpu_view(cpu, addr), addr, size);
    case NDS_K_IO:
        return io_untimed_read(m, cpu, addr, size);
    case NDS_K_SLOT:
        return nds_slot_read(m, cpu, addr, size);
    default:
        return 0;
    }
}

void rl_nds_write(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value)
{
    addr &= ~(size - 1);
    value &= nds_mask(size);
    if (size == 1 && nds_drops_byte(cpu, addr))
        return;
    if (nds_seen_by_display(cpu, addr))
        nds_disp_sync(m, nds_now(m, cpu));
    nds_where w = nds_decode(m, cpu, addr, false, true);
    if (w.kind == NDS_K_MEM && w.p) {
        nds_st(w.p, size, value);
        nds_watch_store(m, w.p);
    } else if (w.kind == NDS_K_VRAM) {
        nds_vram_write_slow(m, nds_vram_cpu_view(cpu, addr), addr, size, value);
    } else if (w.kind == NDS_K_IO) {
        io_untimed_write(m, cpu, addr, size, value);
        m->halt_req = false;
    }
}

uint8_t *rl_nds_mem(rl_nds *m, rl_nds_region r, size_t *len)
{
    static const size_t sizes[RL_NDS_REGIONS] = {
        RL_NDS_MAIN_SIZE, RL_NDS_SWRAM_SIZE, RL_NDS_WRAM7_SIZE, RL_CP15_ITCM_SIZE,
        RL_CP15_DTCM_SIZE, RL_NDS_PAL_SIZE,  RL_NDS_VRAM_SIZE,  RL_NDS_OAM_SIZE,
    };
    uint8_t *p[RL_NDS_REGIONS] = {m->main, m->swram, m->wram7, m->itcm, m->dtcm, m->pal, m->vram, m->oam};
    if ((unsigned)r >= RL_NDS_REGIONS)
        return NULL;
    if (len)
        *len = sizes[r];
    return p[r];
}

int nds_watch_line(const rl_nds *m, const uint8_t *p)
{
    static const struct {
        size_t off, len;
        int first;
    } mems[] = {
        {offsetof(rl_nds, main), RL_NDS_MAIN_SIZE, NDS_WATCH_MAIN},
        {offsetof(rl_nds, swram), RL_NDS_SWRAM_SIZE, NDS_WATCH_SWRAM},
        {offsetof(rl_nds, wram7), RL_NDS_WRAM7_SIZE, NDS_WATCH_WRAM7},
        {offsetof(rl_nds, itcm), RL_CP15_ITCM_SIZE, NDS_WATCH_ITCM},
        {offsetof(rl_nds, vram), RL_NDS_VRAM_SIZE, NDS_WATCH_VRAM},
    };
    uintptr_t at = (uintptr_t)p - (uintptr_t)m;
    for (size_t i = 0; i < sizeof mems / sizeof mems[0]; i++)
        if (at - mems[i].off < mems[i].len)
            return mems[i].first + (int)((at - mems[i].off) / NDS_LINE);
    return -1;
}

/** @brief The host byte of cpu's code fetch at addr, or NULL when no memory answers it. */
static const uint8_t *code_at(rl_nds *m, unsigned cpu, uint32_t addr)
{
    nds_where w = nds_decode(m, cpu, addr, true, false);
    return w.kind == NDS_K_MEM ? w.p : NULL;
}

void rl_nds_code_read(rl_nds *m, unsigned cpu, uint32_t addr, uint8_t *out, uint32_t len)
{
    for (uint32_t i = 0; i < len;) {
        uint32_t a = addr + i;
        uint32_t n = NDS_LINE - (a & (NDS_LINE - 1));
        if (n > len - i)
            n = len - i;
        const uint8_t *p = code_at(m, cpu, a);
        if (p)
            memcpy(out + i, p, n);
        else
            memset(out + i, 0, n);
        i += n;
    }
}

bool rl_nds_code_equal(rl_nds *m, unsigned cpu, uint32_t addr, const uint8_t *want, uint32_t len)
{
    for (uint32_t i = 0; i < len;) {
        uint32_t a = addr + i;
        uint32_t n = NDS_LINE - (a & (NDS_LINE - 1));
        if (n > len - i)
            n = len - i;
        const uint8_t *p = code_at(m, cpu, a);
        if (!p || memcmp(p, want + i, n) != 0)
            return false;
        i += n;
    }
    return true;
}

void rl_nds_code_watch(rl_nds *m, unsigned cpu, uint32_t addr, uint32_t len)
{
    for (uint32_t i = 0; i < len;) {
        uint32_t a = addr + i;
        const uint8_t *p = code_at(m, cpu, a);
        int l = p ? nds_watch_line(m, p) : -1;
        if (l >= 0)
            m->watch[l >> 3] |= (uint8_t)(1u << (l & 7));
        i += NDS_LINE - (a & (NDS_LINE - 1));
    }
}

uint64_t rl_nds_code_epoch(const rl_nds *m) { return m->code_epoch; }

const rl_arm32_cp15 *rl_nds_cp15(const rl_nds *m) { return &m->cp15; }

unsigned rl_nds_cp15_write(rl_nds *m, uint32_t key, uint32_t value)
{
    unsigned what = rl_arm32_cp15_write(&m->cp15, key, value);
    if (what & RL_CP15_CHANGED_TCM)
        rl_arm32_cp15_tcm(&m->cp15, &m->tcm);
    if (what & RL_CP15_CHANGED_MPU)
        nds_cache_mpu(m);
    nds_cache_op(m, key, value);
    return what;
}
