/**
 * @file gpu2d_bg.c
 * @brief The BG layers of a 2D engine: text BGs with 16 or 256 colors and
 * extended palettes, affine BGs with 8-bit maps, extended affine BGs (16-bit
 * maps, 256-color and direct color bitmaps), engine A's large bitmap and
 * its 3D layer, with mosaic.
 *
 * Fresh C from GBATEK ("LCD VRAM BG Screen Data Format", "LCD VRAM Bitmap BG
 * Modes", "DS Video BG Modes / Control", "DS Video Extended Palettes", "DS
 * 3D Final 2D Output").
 */
#include "nds_int.h"

#include <string.h>

/** @brief The kinds of BG a layer can be. */
enum { K_OFF, K_TEXT, K_AFFINE, K_EXT, K_LARGE, K_3D };

/** @brief What layer n is in the engine's BG mode. */
static unsigned kind_of(const nds_eng *e, unsigned n)
{
    unsigned mode = e->dispcnt & 7;
    if (n == 0 && e->id == 0 && (e->dispcnt & 8))
        return K_3D;
    switch (mode) {
    case 0:
        return K_TEXT;
    case 1:
        return n == 3 ? K_AFFINE : K_TEXT;
    case 2:
        return n >= 2 ? K_AFFINE : K_TEXT;
    case 3:
        return n == 3 ? K_EXT : K_TEXT;
    case 4:
        return n == 3 ? K_EXT : n == 2 ? K_AFFINE : K_TEXT;
    case 5:
        return n >= 2 ? K_EXT : K_TEXT;
    case 6:
        if (e->id != 0)
            return K_OFF;
        return n == 2 ? K_LARGE : n == 0 ? K_TEXT : K_OFF;
    default:
        return K_OFF;
    }
}

/** @brief The engine's palette RAM: BG (obj false) or OBJ. */
static const uint8_t *pal_of(const rl_nds *m, const nds_eng *e, bool obj)
{
    return &m->pal[e->id * 0x400u + (obj ? 0x200u : 0u)];
}

/** @brief Puts an opaque pixel of 15-bit color c. */
static void put(nds_px *p, uint32_t c15, unsigned prio)
{
    p->c = nds_c18(c15);
    p->prio = (uint8_t)prio;
    p->flags = NDS_PX_ON;
}

/** @brief A 256-color index idx of a tile of BG n with palette pal: from the extended palette when on, else the standard one. */
static void put256(const rl_nds *m, const nds_eng *e, nds_px *p, unsigned n, unsigned idx, unsigned pal, bool ext, unsigned prio)
{
    if (!idx)
        return;
    if (ext && (e->dispcnt & 0x40000000u)) {
        unsigned slot = n;
        if (n < 2 && (e->bgcnt[n] & 0x2000))
            slot += 2;
        uint32_t a = slot * 0x2000u + (pal * 256u + idx) * 2u;
        put(p, nds_vram_rd(m, e->id ? NDS_V_BGEXTB : NDS_V_BGEXTA, a, 2), prio);
        return;
    }
    const uint8_t *pr = pal_of(m, e, false);
    put(p, (uint32_t)(pr[idx * 2] | pr[idx * 2 + 1] << 8), prio);
}

static uint32_t char_base(const nds_eng *e, uint16_t cnt)
{
    uint32_t b = ((cnt >> 2) & 15u) * 0x4000u;
    return e->id == 0 ? b + ((e->dispcnt >> 24) & 7u) * 0x10000u : b;
}

static uint32_t screen_base(const nds_eng *e, uint16_t cnt)
{
    uint32_t b = ((cnt >> 8) & 31u) * 0x800u;
    return e->id == 0 ? b + ((e->dispcnt >> 27) & 7u) * 0x10000u : b;
}

/** @brief The horizontal mosaic: the pixel x shows, for a layer with mosaic on. */
static unsigned mos_x(const nds_eng *e, unsigned x) { return x - x % ((e->mosaic & 15u) + 1u); }

static void text(const rl_nds *m, nds_eng *e, unsigned n, unsigned y)
{
    uint16_t cnt = e->bgcnt[n];
    unsigned view = e->id ? NDS_V_BGB : NDS_V_BGA, prio = cnt & 3u;
    unsigned w = (cnt & 0x4000) ? 512u : 256u, h = (cnt & 0x8000) ? 512u : 256u;
    bool mos = cnt & 0x40, c256 = cnt & 0x80;
    unsigned yy = ((mos ? e->bg_mos_y : y) + e->vofs[n]) & (h - 1u);
    uint32_t cb = char_base(e, cnt), sb = screen_base(e, cnt) + ((yy >> 8) * (w >> 8)) * 0x800u + ((yy >> 3) & 31u) * 64u;
    const uint8_t *pr = pal_of(m, e, false);
    nds_px *out = e->bg[n];
    for (unsigned x = 0; x < NDS_W; x++) {
        unsigned xx = ((mos ? mos_x(e, x) : x) + e->hofs[n]) & (w - 1u);
        uint32_t me = nds_vram_rd(m, view, sb + (xx >> 8) * 0x800u + ((xx >> 3) & 31u) * 2u, 2);
        unsigned tx = xx & 7u, ty = yy & 7u;
        if (me & 0x400)
            tx ^= 7;
        if (me & 0x800)
            ty ^= 7;
        unsigned tile = me & 0x3FFu, pal = me >> 12;
        if (c256) {
            unsigned idx = nds_vram_rd(m, view, cb + tile * 64u + ty * 8u + tx, 1);
            put256(m, e, &out[x], n, idx, pal, true, prio);
        } else {
            unsigned b = nds_vram_rd(m, view, cb + tile * 32u + ty * 4u + tx / 2u, 1);
            unsigned idx = (tx & 1) ? b >> 4 : b & 15u;
            if (idx) {
                unsigned i = pal * 16u + idx;
                put(&out[x], (uint32_t)(pr[i * 2] | pr[i * 2 + 1] << 8), prio);
            }
        }
    }
}

/** @brief The texel coordinates of affine layer n at pixel x, and whether they fall inside w x h (wrapping when the BG wraps). */
static bool affine_at(const nds_eng *e, unsigned n, unsigned x, unsigned w, unsigned h, bool wrap, unsigned *tx, unsigned *ty)
{
    unsigned a = n - 2;
    bool mos = e->bgcnt[n] & 0x40;
    unsigned sx = mos ? mos_x(e, x) : x;
    int32_t cx = mos ? e->mosx[a] : e->curx[a], cy = mos ? e->mosy[a] : e->cury[a];
    int32_t px = (cx + e->pa[a] * (int32_t)sx) >> 8, py = (cy + e->pc[a] * (int32_t)sx) >> 8;
    if (wrap) {
        *tx = (unsigned)px & (w - 1u);
        *ty = (unsigned)py & (h - 1u);
        return true;
    }
    if (px < 0 || py < 0 || px >= (int32_t)w || py >= (int32_t)h)
        return false;
    *tx = (unsigned)px;
    *ty = (unsigned)py;
    return true;
}

static void affine(const rl_nds *m, nds_eng *e, unsigned n)
{
    uint16_t cnt = e->bgcnt[n];
    unsigned view = e->id ? NDS_V_BGB : NDS_V_BGA, prio = cnt & 3u, size = 128u << (cnt >> 14);
    bool wrap = cnt & 0x2000;
    uint32_t cb = char_base(e, cnt), sb = screen_base(e, cnt);
    nds_px *out = e->bg[n];
    for (unsigned x = 0; x < NDS_W; x++) {
        unsigned tx, ty;
        if (!affine_at(e, n, x, size, size, wrap, &tx, &ty))
            continue;
        unsigned tile = nds_vram_rd(m, view, sb + (ty >> 3) * (size >> 3) + (tx >> 3), 1);
        unsigned idx = nds_vram_rd(m, view, cb + tile * 64u + (ty & 7u) * 8u + (tx & 7u), 1);
        put256(m, e, &out[x], n, idx, 0, false, prio);
    }
}

static void ext_tiles(const rl_nds *m, nds_eng *e, unsigned n)
{
    uint16_t cnt = e->bgcnt[n];
    unsigned view = e->id ? NDS_V_BGB : NDS_V_BGA, prio = cnt & 3u, size = 128u << (cnt >> 14);
    bool wrap = cnt & 0x2000;
    uint32_t cb = char_base(e, cnt), sb = screen_base(e, cnt);
    nds_px *out = e->bg[n];
    for (unsigned x = 0; x < NDS_W; x++) {
        unsigned tx, ty;
        if (!affine_at(e, n, x, size, size, wrap, &tx, &ty))
            continue;
        uint32_t me = nds_vram_rd(m, view, sb + ((ty >> 3) * (size >> 3) + (tx >> 3)) * 2u, 2);
        unsigned px = tx & 7u, py = ty & 7u;
        if (me & 0x400)
            px ^= 7;
        if (me & 0x800)
            py ^= 7;
        unsigned idx = nds_vram_rd(m, view, cb + (me & 0x3FFu) * 64u + py * 8u + px, 1);
        put256(m, e, &out[x], n, idx, me >> 12, true, prio);
    }
}

static void bitmap(const rl_nds *m, nds_eng *e, unsigned n, bool large)
{
    static const uint16_t ws[4] = {128, 256, 512, 512}, hs[4] = {128, 256, 256, 512};
    uint16_t cnt = e->bgcnt[n];
    unsigned view = e->id ? NDS_V_BGB : NDS_V_BGA, prio = cnt & 3u, sz = cnt >> 14;
    unsigned w = ws[sz], h = hs[sz];
    uint32_t base = ((cnt >> 8) & 31u) * 0x4000u;
    bool direct = (cnt & 0x84) == 0x84, wrap = cnt & 0x2000;
    if (large) {
        w = (sz & 1) ? 1024u : 512u;
        h = (sz & 1) ? 512u : 1024u;
        base = 0;
        direct = false;
    }
    const uint8_t *pr = pal_of(m, e, false);
    nds_px *out = e->bg[n];
    for (unsigned x = 0; x < NDS_W; x++) {
        unsigned tx, ty;
        if (!affine_at(e, n, x, w, h, wrap, &tx, &ty))
            continue;
        if (direct) {
            uint32_t c = nds_vram_rd(m, view, base + (ty * w + tx) * 2u, 2);
            if (c & 0x8000)
                put(&out[x], c & 0x7FFFu, prio);
        } else {
            unsigned idx = nds_vram_rd(m, view, base + ty * w + tx, 1);
            if (idx)
                put(&out[x], (uint32_t)(pr[idx * 2] | pr[idx * 2 + 1] << 8), prio);
        }
    }
}

static void layer3d(nds_eng *e, const uint32_t *line3d)
{
    nds_px *out = e->bg[0];
    if (!line3d)
        return;
    unsigned prio = e->bgcnt[0] & 3u;
    for (unsigned x = 0; x < NDS_W; x++) {
        unsigned sx = (x + e->hofs[0]) & 511u;
        if (sx >= NDS_W)
            continue;
        uint32_t v = line3d[sx];
        unsigned alpha = (v >> 24) & 31u;
        if (!alpha)
            continue;
        out[x] = (nds_px){.c = v & 0x3FFFFu, .prio = (uint8_t)prio, .flags = NDS_PX_ON | NDS_PX_3D, .alpha = (uint8_t)alpha};
    }
}

void nds_eng_bg(rl_nds *m, nds_eng *e, unsigned n, unsigned y, const uint32_t *line3d)
{
    memset(e->bg[n], 0, sizeof e->bg[n]);
    if (!(e->dispcnt & (0x100u << n)))
        return;
    switch (kind_of(e, n)) {
    case K_TEXT:
        text(m, e, n, y);
        return;
    case K_AFFINE:
        affine(m, e, n);
        return;
    case K_EXT:
        if (e->bgcnt[n] & 0x80)
            bitmap(m, e, n, false);
        else
            ext_tiles(m, e, n);
        return;
    case K_LARGE:
        bitmap(m, e, n, true);
        return;
    case K_3D:
        layer3d(e, line3d);
        return;
    default:
        return;
    }
}
