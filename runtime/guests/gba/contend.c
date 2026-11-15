/**
 * @file contend.c
 * @brief Video memory contention: the cycles in which the picture unit
 * itself reads video RAM, palette RAM and OAM, so that a CPU or DMA access
 * meeting such a read waits one more cycle.
 *
 * Three units are followed cycle by cycle, lazily up to the time of each
 * access or register write: the background fetch from the start of a visible
 * line, the palette reads of the pixel merge from dot 46, and the sprite
 * unit's OAM walk and pattern fetch, which starts at dot 40 of the line
 * before the one it draws. The behavior was measured against NanoBoyAdvance
 * (the oracle of check/oracle/nba) with experiment ROMs; the code here is
 * relift's own.
 */
#include "gba_int.h"

enum {
    LINE = RL_GBA_LINE_CYCLES,
    BG_END = RL_GBA_LINE_CYCLES,
    BG_DRAW_END = 1007,
    MAP_LAST = 1004,
    AFFINE_START = 32,
    MERGE_START = 46,
    MERGE_END = 1006,
    OBJ_START = 40,
    OBJ_HBLANK_FREE_END = 964,
    OBJ_MOSAIC_STEP = 1192,
};

static unsigned io16(const gba_contend *c, unsigned off) { return (unsigned)(c->io[off] | c->io[off + 1] << 8); }

static unsigned rd16(const uint8_t *m, uint32_t a) { return (unsigned)(m[a] | m[a + 1] << 8); }

static uint32_t rd32(const uint8_t *m, uint32_t a) { return (uint32_t)rd16(m, a) | (uint32_t)rd16(m, a + 2) << 16; }

static uint32_t obj_base(const rl_gba *g) { return (g->dispcnt & 7) >= 3 ? 0x14000u : 0x10000u; }

static bool blank(const rl_gba *g) { return ((g->ct.latch[0] | g->dispcnt) & 0x80) != 0; }

static bool latch_line(unsigned line) { return line <= 160 || line >= 225; }

static uint64_t next_latch(const rl_gba *g, uint64_t t)
{
    uint64_t p = gba_lcd_pos(g, t);
    unsigned line = (unsigned)(p / LINE), dot = (unsigned)(p % LINE);
    uint64_t base = t - dot;
    if (dot > OBJ_START) {
        line = (line + 1) % RL_GBA_LINES;
        base += LINE;
    }
    while (!latch_line(line)) {
        line = (line + 1) % RL_GBA_LINES;
        base += LINE;
    }
    return base + OBJ_START;
}

static void latch_to(rl_gba *g, uint64_t now)
{
    gba_contend *c = &g->ct;
    for (unsigned n = 0; c->latch_next <= now; n++) {
        if (n == 3) {
            c->latch[0] = c->latch[1] = c->latch[2] = g->dispcnt;
            c->latch_next = next_latch(g, now + 1);
            return;
        }
        c->latch[0] = c->latch[1];
        c->latch[1] = c->latch[2];
        c->latch[2] = g->dispcnt;
        c->latch_next = next_latch(g, c->latch_next + 1);
    }
}

static unsigned bg_read(rl_gba *g, uint64_t slot, uint32_t addr)
{
    if (blank(g))
        return 0;
    if (addr >= obj_base(g))
        return g->ct.bg_latch;
    g->ct.bg_latch = (uint16_t)rd16(g->vram, addr & ~1u);
    g->ct.bg_slot = slot;
    return g->ct.bg_latch;
}

static void text_cycle(rl_gba *g, unsigned id, unsigned cycle, uint64_t slot)
{
    gba_contend *c = &g->ct;
    gba_ct_text *t = &c->text[id];
    unsigned cnt = io16(c, 0x08 + 2 * id);
    bool c256 = (cnt & 0x80) != 0;
    if (t->fetches > 0 && t->left == 0) {
        bg_read(g, slot, t->addr);
        t->addr += t->flip ? (uint32_t)-2 : 2u;
        t->left = 4;
        t->fetches--;
    }
    t->left -= c256 ? 2 : 1;
    unsigned hofs = io16(c, 0x10 + 4 * id) & 0x1FFu;
    unsigned step = (cycle >> 2) + (hofs & 7u);
    if (cycle >= BG_DRAW_END || step < 8 || (step & 7u))
        return;
    unsigned line = c->bg_y + (io16(c, 0x12 + 4 * id) & 0x1FFu);
    if (cnt & 0x40)
        line -= c->mos_bg;
    unsigned gx = (hofs >> 3) + (step >> 3) - 1u, gy = line >> 3;
    unsigned block = (cnt >> 8) & 0x1Fu;
    switch (cnt >> 14) {
    case 1:
        block += (gx >> 5) & 1u;
        break;
    case 2:
        block += (gy >> 5) & 1u;
        break;
    case 3:
        block += ((gx >> 5) & 1u) + (((gy >> 5) & 1u) << 1);
        break;
    default:
        break;
    }
    uint32_t map = (block << 11) + ((gy & 31u) << 6) + ((gx & 31u) << 1);
    unsigned entry = bg_read(g, slot, map);
    if (cycle >= MAP_LAST)
        return;
    unsigned row = (entry & 0x800) ? 7u - (line & 7u) : line & 7u;
    uint32_t chars = ((cnt >> 2) & 3u) << 14;
    t->flip = (entry & 0x400) != 0;
    if (c256) {
        t->addr = chars + ((entry & 0x3FFu) << 6) + (row << 3) + (t->flip ? 6u : 0u);
        t->fetches = 4;
    } else {
        t->addr = chars + ((entry & 0x3FFu) << 5) + (row << 2) + (t->flip ? 2u : 0u);
        t->fetches = 2;
    }
    t->left = 0;
}

static void bitmap_cycle(rl_gba *g, unsigned mode, uint64_t slot)
{
    gba_contend *c = &g->ct;
    int32_t x = c->aff[0][0] >> 8, y = c->aff[0][1] >> 8;
    uint32_t a;
    if (mode == 3)
        a = ((uint32_t)y * 240u + (uint32_t)x) * 2u;
    else if (mode == 4)
        a = ((g->dispcnt & 0x10) ? 0xA000u : 0u) + (uint32_t)y * 240u + (uint32_t)x;
    else
        a = ((g->dispcnt & 0x10) ? 0xA000u : 0u) + ((uint32_t)y * 160u + (uint32_t)x) * 2u;
    bg_read(g, slot, a & 0x1FFFFu);
    c->aff[0][0] += (int16_t)io16(c, 0x20);
    c->aff[0][1] += (int16_t)io16(c, 0x24);
}

static void affine_cycle(rl_gba *g, unsigned id, unsigned cycle, uint64_t slot)
{
    gba_contend *c = &g->ct;
    if (cycle & 1u) {
        bg_read(g, slot, c->aff_tile[id]);
        return;
    }
    unsigned cnt = io16(c, 0x0C + 2 * id), log = cnt >> 14;
    int32_t x = c->aff[id][0] >> 8, y = c->aff[id][1] >> 8;
    c->aff[id][0] += (int16_t)io16(c, 0x20 + 0x10 * id);
    c->aff[id][1] += (int16_t)io16(c, 0x24 + 0x10 * id);
    if (cnt & 0x2000) {
        x &= (128 << log) - 1;
        y &= (128 << log) - 1;
    }
    uint32_t map = (uint16_t)((((cnt >> 8) & 0x1Fu) << 11) + ((uint32_t)(y >> 3) << (4 + log)) + (uint32_t)(x >> 3));
    unsigned tile = (bg_read(g, slot, map) >> (8 * (map & 1u))) & 0xFFu;
    c->aff_tile[id] = (((cnt >> 2) & 3u) << 14) + (tile << 6) + ((uint32_t)(y & 7) << 3) + (uint32_t)(x & 7);
}

static void bg_run(rl_gba *g, unsigned end)
{
    gba_contend *c = &g->ct;
    if (end > BG_END)
        end = BG_END;
    if (c->bg_k >= end)
        return;
    unsigned mode = g->dispcnt & 7u, on = c->latch[0] & g->dispcnt;
    for (; c->bg_k < end; c->bg_k++) {
        unsigned cycle = c->bg_k + 1u;
        uint64_t slot = c->bg_t0 + c->bg_k;
        if (mode <= 1) {
            unsigned id = cycle & 3u;
            if ((id <= 1 || mode == 0) && (on & (0x100u << id)))
                text_cycle(g, id, cycle, slot);
        }
        if (cycle >= BG_DRAW_END || cycle < AFFINE_START)
            continue;
        if (mode == 1 || mode == 2) {
            unsigned id = ((cycle >> 1) & 1u) ^ 1u;
            if ((id == 0 || mode == 2) && (on & (0x400u << id)))
                affine_cycle(g, id, cycle, slot);
        } else if (mode >= 3 && mode <= 5 && (on & 0x400u) && (cycle & 3u) == 3u) {
            bitmap_cycle(g, mode, slot);
        }
    }
}

static void merge_run(rl_gba *g, unsigned end)
{
    gba_contend *c = &g->ct;
    if (end > MERGE_END)
        end = MERGE_END;
    if (c->mg_k >= end)
        return;
    const gba_ppu *p = &g->ppu;
    bool drawn = p->pram_y == c->bg_y;
    for (; c->mg_k < end; c->mg_k++) {
        if (c->mg_k < MERGE_START)
            continue;
        unsigned k = c->mg_k - MERGE_START, x = k >> 2;
        unsigned use = drawn ? p->pram_use[x] : 1u;
        if ((k & 3u) == 0) {
            c->mg_blank = blank(g);
            if (!c->mg_blank && (use & 1u))
                c->pram_slot = c->bg_t0 + c->mg_k;
        } else if ((k & 3u) == 2 && !c->mg_blank && (use & 2u)) {
            c->pram_slot = c->bg_t0 + c->mg_k;
        }
    }
}

static void ppu_to(rl_gba *g, uint64_t t)
{
    if (g->slot[RL_GBA_SLOT_PPU] == &g->d_ppu.dev)
        gba_ppu_sync(g, t);
}

static void bg_init(rl_gba *g, uint64_t t0, unsigned line)
{
    gba_contend *c = &g->ct;
    c->bg_t0 = t0;
    c->bg_k = 0;
    c->mg_k = 0;
    c->bg_y = line;
    c->bg_on = line < GBA_PPU_H;
    c->mg_blank = false;
    for (unsigned i = 0; i < 4; i++)
        c->text[i].fetches = 0;
    if (!c->bg_on)
        return;
    for (unsigned i = 0; i < 2; i++)
        for (unsigned a = 0; a < 2; a++)
            if (c->ref_written[i][a] || line == 0) {
                c->ref_cur[i][a] = c->ref_init[i][a];
                c->ref_written[i][a] = false;
            }
    for (unsigned i = 0; i < 2; i++) {
        c->aff[i][0] = c->ref_cur[i][0];
        c->aff[i][1] = c->ref_cur[i][1];
    }
}

static void bg_line_end(rl_gba *g)
{
    gba_contend *c = &g->ct;
    unsigned size = (c->io[0x4C] >> 4) + 1u;
    if (c->bg_y < GBA_PPU_H - 1) {
        if (++c->mos_bg == size)
            c->mos_bg = 0;
        else
            c->mos_bg &= 15;
    } else {
        c->mos_bg = 0;
    }
    unsigned mode = g->dispcnt & 7u, on = c->latch[0] & g->dispcnt;
    for (unsigned i = 0; i < 2; i++) {
        bool used = i == 0 ? mode >= 1 && mode <= 5 : mode == 2;
        if (!used || !(on & (0x400u << i)))
            continue;
        int32_t pb = (int16_t)io16(c, 0x22 + 0x10 * i), pd = (int16_t)io16(c, 0x26 + 0x10 * i);
        if (c->io[0x0C + 2 * i] & 0x40) {
            if (c->mos_bg == 0) {
                c->ref_cur[i][0] += (int32_t)size * pb;
                c->ref_cur[i][1] += (int32_t)size * pd;
            }
        } else {
            c->ref_cur[i][0] += pb;
            c->ref_cur[i][1] += pd;
        }
    }
}

static const uint8_t obj_size[4][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
    {{8, 8}, {8, 8}, {8, 8}, {8, 8}},
};

static void obj_vram(rl_gba *g, uint64_t slot)
{
    gba_contend *c = &g->ct;
    if (!c->drawing)
        return;
    gba_ct_obj *o = &c->st[c->ob_rd];
    bool map1d = (g->dispcnt & 0x40) != 0;
    int tx, ty;
    if (o->affine) {
        if (c->walk.delay)
            return;
        tx = o->tx >> 8;
        ty = o->ty >> 8;
        o->tx += o->m[0];
        o->ty += o->m[2];
        if (--o->left == 0)
            c->drawing = false;
        if (tx < 0 || tx >= o->w || ty < 0 || ty >= o->h)
            return;
    } else {
        tx = o->tx ^ (o->hflip ? o->w - 1 : 0);
        ty = o->ty;
        o->tx += 2;
        o->left -= 2;
        if (o->left == 0)
            c->drawing = false;
    }
    unsigned bx = (unsigned)tx >> 3, by = (unsigned)ty >> 3, px = (unsigned)tx & 7u, py = (unsigned)ty & 7u;
    if (!o->affine)
        px &= ~1u;
    unsigned tile;
    uint32_t a;
    if (o->c256) {
        if (map1d)
            tile = (o->tile + by * ((unsigned)o->w >> 2) + (bx << 1)) & 0x3FFu;
        else
            tile = ((o->tile + (by << 5)) & 0x3E0u) | (((o->tile & ~1u) + (bx << 1)) & 0x1Fu);
        a = 0x10000u + (tile << 5) + (py << 3) + px;
    } else {
        if (map1d)
            tile = (o->tile + by * ((unsigned)o->w >> 3) + bx) & 0x3FFu;
        else
            tile = ((o->tile + (by << 5)) & 0x3E0u) | ((o->tile + bx) & 0x1Fu);
        a = 0x10000u + (tile << 5) + (py << 2) + (px >> 1);
    }
    if (a >= obj_base(g))
        c->obj_slot = slot;
}

static void obj_submit(gba_contend *c)
{
    c->ob_rd ^= 1u;
    c->drawing = true;
    c->walk.index++;
    c->walk.step = 0;
    c->walk.wait = c->walk.pending;
    c->walk.delay = true;
}

static bool obj_attr01(gba_contend *c, uint32_t a)
{
    gba_ct_walk *w = &c->walk;
    gba_ct_obj *o = &c->st[c->ob_rd ^ 1u];
    unsigned mode = (a >> 10) & 3u;
    if ((a & 0x300u) == 0x200u || mode == 3)
        return false;
    int x = (int)((a >> 16) & 0x1FFu), y = (int)(a & 0xFFu);
    if (x >= 240)
        x -= 512;
    int wd = obj_size[(a >> 14) & 3u][a >> 30][0], ht = obj_size[(a >> 14) & 3u][a >> 30][1];
    int hw = wd >> 1, hh = ht >> 1;
    bool affine = (a & 0x100u) != 0;
    if (affine && (a & 0x200u)) {
        hw *= 2;
        hh *= 2;
    }
    int v = (int)c->ob_y;
    if (((v - y) & 255) >= hh * 2)
        return false;
    bool mosaic = (a & 0x1000u) && mode != 2;
    o->w = wd;
    o->h = ht;
    o->affine = affine;
    o->left = hw << 1;
    o->c256 = (a >> 13) & 1u;
    int ly = (v - y) & 255;
    if (mosaic) {
        ly -= c->ob_mosaic;
        if (ly < 0)
            ly = 0;
    }
    if (!affine) {
        o->hflip = (a >> 28) & 1u;
        o->tx = 0;
        o->ty = (a & (1u << 29)) ? ly ^ (ht - 1) : ly;
        w->pending = hw - 2;
    } else {
        w->x0 = -hw;
        w->y0 = ly - hh;
        w->pending = hw * 2 - 1;
        w->matrix = ((a >> 25) & 31u) * 32u + 6u;
    }
    if (x < 0) {
        int clip = -x & (affine ? ~0 : ~1);
        o->left -= clip;
        if (affine) {
            w->pending -= clip;
            w->x0 += clip;
        } else {
            w->pending -= clip >> 1;
            o->tx += clip;
        }
        if (o->left <= 0)
            return false;
    }
    return true;
}

static void obj_oam(rl_gba *g, uint64_t slot)
{
    gba_contend *c = &g->ct;
    gba_ct_walk *w = &c->walk;
    if (w->wait > 0 && !w->delay) {
        w->wait--;
        return;
    }
    w->delay = false;
    gba_ct_obj *o = &c->st[c->ob_rd ^ 1u];
    switch (w->step) {
    case 0:
        if (w->index == 128) {
            w->step = 6;
            return;
        }
        c->oam_slot = slot;
        if (obj_attr01(c, rd32(g->oam, w->index * 8u)))
            w->step = 1;
        else
            w->index++;
        return;
    case 1:
        c->oam_slot = slot;
        o->tile = rd16(g->oam, w->index * 8u + 4u) & 0x3FFu;
        if (o->affine)
            w->step = 2;
        else
            obj_submit(c);
        return;
    case 2:
    case 3:
    case 4:
    case 5:
        c->oam_slot = slot;
        o->m[w->step - 2] = (int16_t)rd16(g->oam, w->matrix);
        w->matrix += 8u;
        if (++w->step == 6) {
            o->tx = o->m[0] * w->x0 + o->m[1] * w->y0 + (o->w << 7);
            o->ty = o->m[2] * w->x0 + o->m[3] * w->y0 + (o->h << 7);
            obj_submit(c);
        }
        return;
    default:
        return;
    }
}

static void obj_run(rl_gba *g, uint64_t end)
{
    gba_contend *c = &g->ct;
    if (end > c->ob_end)
        end = c->ob_end;
    bool on = (g->dispcnt & 0x1000) != 0;
    for (; c->ob_k < end; c->ob_k++) {
        if (on && !(c->ob_k & 1u)) {
            uint64_t slot = c->ob_t0 + c->ob_k;
            obj_vram(g, slot);
            obj_oam(g, slot);
        }
        if (c->ob_k == OBJ_MOSAIC_STEP) {
            unsigned size = (c->io[0x4D] >> 4) + 1u;
            if (c->ob_y < GBA_PPU_H - 1) {
                if (++c->mos_obj == size)
                    c->mos_obj = 0;
                else
                    c->mos_obj &= 15;
            } else {
                c->mos_obj = 0;
            }
        }
    }
}

static void obj_init(rl_gba *g, uint64_t t0, unsigned line)
{
    gba_contend *c = &g->ct;
    c->ob_t0 = t0;
    c->ob_k = 0;
    c->ob_on = line < GBA_PPU_H - 1 || line == RL_GBA_LINES - 1;
    c->ob_y = (line + 1) % RL_GBA_LINES;
    c->ob_end = (g->dispcnt & 0x20) ? OBJ_HBLANK_FREE_END : LINE;
    c->ob_mosaic = c->mos_obj;
    c->walk = (gba_ct_walk){0};
    c->drawing = false;
    c->ob_rd = 0;
}

static void bg_to(rl_gba *g, uint64_t t0, unsigned line)
{
    gba_contend *c = &g->ct;
    if (c->bg_t0 == GBA_NONE || c->bg_t0 > t0 || t0 - c->bg_t0 > RL_GBA_FRAME_CYCLES) {
        bg_init(g, t0, line);
        return;
    }
    while (c->bg_t0 < t0) {
        latch_to(g, c->bg_t0 + LINE);
        if (c->bg_on) {
            bg_run(g, BG_END);
            bg_line_end(g);
        }
        bg_init(g, c->bg_t0 + LINE, (c->bg_y + 1) % RL_GBA_LINES);
    }
}

static void obj_to(rl_gba *g, uint64_t s0, unsigned line)
{
    gba_contend *c = &g->ct;
    if (c->ob_t0 == GBA_NONE || c->ob_t0 > s0 || s0 - c->ob_t0 > RL_GBA_FRAME_CYCLES) {
        obj_init(g, s0, line);
        return;
    }
    while (c->ob_t0 < s0) {
        if (c->ob_on)
            obj_run(g, LINE);
        obj_init(g, c->ob_t0 + LINE, c->ob_y);
    }
}

static void sync_to(rl_gba *g, uint64_t now)
{
    gba_contend *c = &g->ct;
    if (!g->sched)
        return;
    uint64_t p = gba_lcd_pos(g, now);
    unsigned line = (unsigned)(p / LINE), dot = (unsigned)(p % LINE);
    uint64_t t0 = now - dot;
    bg_to(g, t0, line);
    latch_to(g, now);
    if (c->bg_on) {
        bg_run(g, dot);
        if (dot > MERGE_START && g->ppu.pram_y != line)
            ppu_to(g, now);
        merge_run(g, dot);
    }
    uint64_t s0 = dot >= OBJ_START ? t0 + OBJ_START : t0 + OBJ_START - LINE;
    unsigned sl = dot >= OBJ_START ? line : (line + RL_GBA_LINES - 1) % RL_GBA_LINES;
    obj_to(g, s0, sl);
    if (c->ob_on)
        obj_run(g, now - s0);
}

void gba_contend_resync(rl_gba *g)
{
    gba_contend *c = &g->ct;
    c->bg_t0 = GBA_NONE;
    c->ob_t0 = GBA_NONE;
    c->bg_on = false;
    c->ob_on = false;
    c->latch_next = g->sched ? next_latch(g, gba_now(g)) : GBA_NONE;
}

void gba_contend_reset(rl_gba *g)
{
    gba_contend *c = &g->ct;
    *c = (gba_contend){0};
    c->io[0x20] = 0x00;
    c->io[0x21] = 0x01;
    c->io[0x26] = 0x00;
    c->io[0x27] = 0x01;
    c->bg_slot = c->obj_slot = c->oam_slot = c->pram_slot = GBA_NONE;
    gba_contend_resync(g);
}

void gba_contend_reg(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now)
{
    uint32_t off = addr - 0x04000000u;
    if (off >= sizeof g->ct.io)
        return;
    sync_to(g, now);
    gba_contend *c = &g->ct;
    c->io[off] = v;
    if ((off >= 0x28 && off < 0x30) || (off >= 0x38 && off < 0x40)) {
        unsigned i = off >= 0x38, a = (off >> 2) & 1u, b = off & 3u;
        uint32_t r = (uint32_t)c->ref_init[i][a];
        r = (r & ~(0xFFu << (8 * b)) & 0x0FFFFFFFu) | (uint32_t)v << (8 * b);
        r &= 0x0FFFFFFFu;
        if (r & (1u << 27))
            r |= 0xF0000000u;
        c->ref_init[i][a] = (int32_t)r;
        c->ref_written[i][a] = true;
    }
}

bool gba_contend_busy(rl_gba *g, uint32_t addr, uint64_t now)
{
    gba_contend *c = &g->ct;
    sync_to(g, now);
    uint64_t s = now - 1;
    switch (addr >> 24) {
    case 0x5:
        return c->pram_slot == s;
    case 0x6:
        return (addr & 0x1FFFFu) >= obj_base(g) ? c->obj_slot == s : c->bg_slot == s;
    default:
        return c->oam_slot == s;
    }
}
