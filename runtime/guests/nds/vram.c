/**
 * @file vram.c
 * @brief The nine video RAM banks A to I and VRAMCNT: each write rebuilds
 * the page tables of every view (the ARM9's LCDC, engine BG and OBJ
 * windows, the ARM7's work RAM window, the texture and texture palette
 * slots and the extended palettes), so an access looks up one page and does
 * not search the banks. A page where several banks overlap reads the OR of
 * them and a write goes to each; a page no bank covers reads 0 and drops
 * writes.
 *
 * Fresh C from GBATEK ("DS Memory Control - VRAM"); the mirrors of the small
 * banks, the overlap and the unused MST and OFS values follow the oracle's
 * answers to relift's probe ROM (A and B at OBJ with OFS bit 1 map nowhere,
 * MST bit 2 of A, B, H and I is ignored, E, F and G with MST 6 or 7 map
 * nowhere, H mirrors at BG-B pages 0, 1, 4 and 5 and I at 2, 3, 6 and 7, I
 * fills all of OBJ-B, the LCDC window mirrors every 1 MiB).
 */
#include "nds_int.h"

#include <string.h>

/** @brief Offset of each bank in rl_nds.vram (the LCDC order) and its size. */
static const struct {
    uint32_t at, size;
} banks[NDS_BANKS] = {
    {0x00000, 0x20000}, {0x20000, 0x20000}, {0x40000, 0x20000}, {0x60000, 0x20000}, {0x80000, 0x10000},
    {0x90000, 0x4000},  {0x94000, 0x4000},  {0x98000, 0x8000},  {0xA0000, 0x4000},
};

/** @brief First page, page count and page size (log2) of each view. */
static const struct {
    uint16_t first, count;
    uint8_t shift;
} views[NDS_VIEWS] = {
    [NDS_V_LCDC] = {0, 64, 14},       [NDS_V_BGA] = {64, 32, 14},     [NDS_V_BGB] = {96, 8, 14},
    [NDS_V_OBJA] = {104, 16, 14},     [NDS_V_OBJB] = {120, 8, 14},    [NDS_V_ARM7] = {128, 16, 14},
    [NDS_V_TEX] = {144, 32, 14},      [NDS_V_TEXPAL] = {176, 8, 14},  [NDS_V_BGEXTA] = {184, 4, 13},
    [NDS_V_OBJEXTA] = {188, 1, 13},   [NDS_V_BGEXTB] = {189, 4, 13},  [NDS_V_OBJEXTB] = {193, 1, 13},
};

/** @brief Maps n pages of bank b, from byte off of the bank on, to pages page.. of view v. */
static void put(rl_nds *m, unsigned v, unsigned page, unsigned b, uint32_t off, unsigned n)
{
    uint32_t step = 1u << views[v].shift;
    for (unsigned i = 0; i < n; i++) {
        nds_vpage *pg = &m->vpg[views[v].first + page + i];
        pg->banks |= (uint16_t)(1u << b);
        pg->at[b] = banks[b].at + (off + i * step) % banks[b].size;
    }
}

/** @brief The mapping of bank b under its VRAMCNT value c. */
static void map_bank(rl_nds *m, unsigned b, uint8_t c)
{
    unsigned mst = c & 7, ofs = (c >> 3) & 3;
    if (b <= NDS_BANK_B || b >= NDS_BANK_H)
        mst &= 3;
    if (mst == 0) {
        put(m, NDS_V_LCDC, banks[b].at >> 14, b, 0, (banks[b].size + 0x3FFF) >> 14);
        return;
    }
    switch (b) {
    case NDS_BANK_A:
    case NDS_BANK_B:
    case NDS_BANK_C:
    case NDS_BANK_D:
        if (mst == 1)
            put(m, NDS_V_BGA, ofs * 8, b, 0, 8);
        else if (mst == 2 && b <= NDS_BANK_B && ofs < 2)
            put(m, NDS_V_OBJA, ofs * 8, b, 0, 8);
        else if (mst == 2 && b >= NDS_BANK_C && ofs < 2)
            put(m, NDS_V_ARM7, ofs * 8, b, 0, 8);
        else if (mst == 3)
            put(m, NDS_V_TEX, ofs * 8, b, 0, 8);
        else if (mst == 4 && b == NDS_BANK_C)
            put(m, NDS_V_BGB, 0, b, 0, 8);
        else if (mst == 4 && b == NDS_BANK_D)
            put(m, NDS_V_OBJB, 0, b, 0, 8);
        return;
    case NDS_BANK_E:
        if (mst == 1)
            put(m, NDS_V_BGA, 0, b, 0, 4);
        else if (mst == 2)
            put(m, NDS_V_OBJA, 0, b, 0, 4);
        else if (mst == 3)
            put(m, NDS_V_TEXPAL, 0, b, 0, 4);
        else if (mst == 4)
            put(m, NDS_V_BGEXTA, 0, b, 0, 4);
        return;
    case NDS_BANK_F:
    case NDS_BANK_G: {
        unsigned page = (ofs & 1) + 4 * (ofs >> 1);
        if (mst == 1)
            put(m, NDS_V_BGA, page, b, 0, 1);
        else if (mst == 2)
            put(m, NDS_V_OBJA, page, b, 0, 1);
        else if (mst == 3)
            put(m, NDS_V_TEXPAL, page, b, 0, 1);
        else if (mst == 4)
            put(m, NDS_V_BGEXTA, 2 * (ofs & 1), b, 0, 2);
        else if (mst == 5)
            put(m, NDS_V_OBJEXTA, 0, b, 0, 1);
        return;
    }
    case NDS_BANK_H:
        if (mst == 1) {
            put(m, NDS_V_BGB, 0, b, 0, 2);
            put(m, NDS_V_BGB, 4, b, 0, 2);
        } else if (mst == 2) {
            put(m, NDS_V_BGEXTB, 0, b, 0, 4);
        }
        return;
    default:
        if (mst == 1) {
            static const uint8_t pages[4] = {2, 3, 6, 7};
            for (unsigned i = 0; i < 4; i++)
                put(m, NDS_V_BGB, pages[i], b, 0, 1);
        } else if (mst == 2) {
            for (unsigned p = 0; p < 8; p++)
                put(m, NDS_V_OBJB, p, b, 0, 1);
        } else if (mst == 3) {
            put(m, NDS_V_OBJEXTB, 0, b, 0, 1);
        }
        return;
    }
}

void nds_vram_rebuild(rl_nds *m)
{
    for (unsigned v = 0; v < NDS_VIEWS; v++)
        m->vview[v] = (nds_vview){.first = views[v].first, .pages = (uint16_t)(views[v].count - 1u), .shift = views[v].shift,
                                  .mask = (1u << views[v].shift) - 1u};
    memset(m->vpg, 0, sizeof m->vpg);
    for (unsigned b = 0; b < NDS_BANKS; b++)
        if (m->vramcnt[b] & 0x80)
            map_bank(m, b, m->vramcnt[b]);
    for (unsigned i = 0; i < NDS_VPAGES; i++) {
        nds_vpage *pg = &m->vpg[i];
        if (pg->banks && !(pg->banks & (pg->banks - 1))) {
            unsigned b = 0;
            while (!(pg->banks >> b & 1))
                b++;
            pg->p = &m->vram[pg->at[b]];
        }
    }
    uint8_t stat = 0;
    for (unsigned b = NDS_BANK_C; b <= NDS_BANK_D; b++)
        if ((m->vramcnt[b] & 0x87) == 0x82)
            stat |= (uint8_t)(1u << (b - NDS_BANK_C));
    m->vramstat = stat;
}

void nds_vram_write_cnt(rl_nds *m, unsigned b, uint8_t v)
{
    if (b >= NDS_BANKS)
        return;
    m->vramcnt[b] = v & 0x9F;
    nds_vram_rebuild(m);
    nds_disp_log_vramcnt(m, b, v);
}

uint32_t nds_vram_read_slow(const rl_nds *m, unsigned view, uint32_t addr, unsigned size)
{
    const nds_vpage *pg = nds_vram_page(m, view, addr);
    uint32_t in = addr & m->vview[view].mask, v = 0;
    for (unsigned b = 0; b < NDS_BANKS; b++)
        if (pg->banks >> b & 1)
            v |= nds_ld(&m->vram[pg->at[b] + in], size);
    return v;
}

void nds_vram_write_slow(rl_nds *m, unsigned view, uint32_t addr, unsigned size, uint32_t value)
{
    const nds_vpage *pg = nds_vram_page(m, view, addr);
    uint32_t in = addr & m->vview[view].mask;
    for (unsigned b = 0; b < NDS_BANKS; b++)
        if (pg->banks >> b & 1) {
            uint8_t *p = &m->vram[pg->at[b] + in];
            nds_st(p, size, value);
            nds_watch_store(m, p, size);
        }
}

unsigned nds_vram_cpu_view(unsigned cpu, uint32_t addr)
{
    static const uint8_t arm9[8] = {NDS_V_BGA, NDS_V_BGB, NDS_V_OBJA, NDS_V_OBJB, NDS_V_LCDC, NDS_V_LCDC, NDS_V_LCDC, NDS_V_LCDC};
    return cpu == RL_NDS_ARM9 ? arm9[(addr >> 21) & 7] : NDS_V_ARM7;
}

uint8_t *nds_vram_bank(rl_nds *m, unsigned b) { return b < NDS_BANKS ? &m->vram[banks[b].at] : NULL; }
