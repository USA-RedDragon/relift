/**
 * @file gpu2d_obj.c
 * @brief The OBJ layer of a 2D engine: 128 OBJs of 16 or 256 colors (with
 * the OBJ extended palette) and bitmap OBJs, plain or affine, double size,
 * 1D and 2D tile and bitmap mappings with their boundaries, semi-transparent
 * OBJs, the OBJ window and mosaic. Vertical mosaic is per OBJ. Horizontal
 * mosaic works on the finished OBJ line, as console captures show: a mosaic
 * OBJ marks every pixel of its span that no OBJ drew yet, its transparent
 * texels too, any other OBJ clears the mark on the pixels of its span that
 * are still undrawn or that it draws, and
 * a marked pixel that does not start a mosaic block takes the pixel to its
 * left (drawn or transparent) when that one is marked as well. A bitmap OBJ of alpha 0 is not drawn
 * (the oracle's answer). At one pixel the OBJ of the lowest
 * priority value wins, the lower OAM index at equal priority.
 *
 * Fresh C from GBATEK ("LCD OBJ - OAM Attributes", "LCD OBJ - OAM
 * Rotation/Scaling Parameters", "LCD OBJ - VRAM Character (Tile) Mapping",
 * "DS Video OBJs", "DS Video Extended Palettes").
 */
#include "nds_int.h"

#include <string.h>

/** @brief Width and height of each shape (attr 0 bits 14-15) and size (attr 1 bits 14-15). */
static const uint8_t dims[4][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
    {{8, 8}, {8, 8}, {8, 8}, {8, 8}},
};

/** @brief The 15-bit color of texel (tx, ty) of an OBJ, or -1 for a transparent one. */
static int32_t texel(const rl_nds *m, const nds_eng *e, uint16_t a0, uint16_t a2, unsigned w, unsigned tx, unsigned ty)
{
    unsigned view = e->id ? NDS_V_OBJB : NDS_V_OBJA;
    uint32_t cnt = e->dispcnt;
    unsigned tile = a2 & 0x3FFu;
    if (((a0 >> 10) & 3) == 3) {
        uint32_t addr;
        if (cnt & 0x40) {
            if (cnt & 0x20)
                return -1;
            addr = tile * ((cnt & 0x400000u) && e->id == 0 ? 256u : 128u) + (ty * w + tx) * 2u;
        } else {
            unsigned maskx = (cnt & 0x20) ? 0x1Fu : 0x0Fu, stride = (cnt & 0x20) ? 512u : 256u;
            addr = (tile & maskx) * 16u + (tile & ~maskx) * 128u + ty * stride + tx * 2u;
        }
        uint32_t c = nds_vram_rd(m, view, addr, 2);
        return (c & 0x8000) ? (int32_t)(c & 0x7FFF) : -1;
    }
    bool c256 = a0 & 0x2000;
    unsigned bytes = c256 ? 64u : 32u;
    uint32_t addr;
    if (cnt & 0x10) {
        unsigned bound = 32u << ((cnt >> 20) & 3);
        addr = tile * bound + ((ty >> 3) * (w >> 3) + (tx >> 3)) * bytes;
    } else {
        addr = tile * 32u + (ty >> 3) * 1024u + (tx >> 3) * bytes;
    }
    unsigned px = tx & 7u, py = ty & 7u;
    if (c256) {
        unsigned idx = nds_vram_rd(m, view, addr + py * 8u + px, 1);
        if (!idx)
            return -1;
        if (cnt & 0x80000000u)
            return (int32_t)nds_vram_rd(m, e->id ? NDS_V_OBJEXTB : NDS_V_OBJEXTA, ((a2 >> 12) * 256u + idx) * 2u, 2);
        const uint8_t *pr = &m->pal[e->id * 0x400u + 0x200u];
        return (int32_t)(pr[idx * 2] | pr[idx * 2 + 1] << 8);
    }
    unsigned b = nds_vram_rd(m, view, addr + py * 4u + px / 2u, 1);
    unsigned idx = (px & 1) ? b >> 4 : b & 15u;
    if (!idx)
        return -1;
    unsigned i = (a2 >> 12) * 16u + idx;
    const uint8_t *pr = &m->pal[e->id * 0x400u + 0x200u];
    return (int32_t)(pr[i * 2] | pr[i * 2 + 1] << 8);
}

void nds_eng_obj(rl_nds *m, nds_eng *e, unsigned y)
{
    memset(e->obj, 0, sizeof e->obj);
    memset(e->objwin, 0, sizeof e->objwin);
    if (!(e->dispcnt & 0x1000))
        return;
    const uint8_t *oam = &m->oam[e->id * 0x400u];
    unsigned mw = ((e->mosaic >> 8) & 15u) + 1u;
    for (unsigned i = 0; i < 128; i++) {
        const uint8_t *o = &oam[i * 8];
        uint16_t a0 = (uint16_t)(o[0] | o[1] << 8), a1 = (uint16_t)(o[2] | o[3] << 8), a2 = (uint16_t)(o[4] | o[5] << 8);
        bool aff = a0 & 0x100;
        if (!aff && (a0 & 0x200))
            continue;
        unsigned mode = (a0 >> 10) & 3;
        if (mode == 3 && !(a2 >> 12))
            continue;
        unsigned w = dims[a0 >> 14][a1 >> 14][0], h = dims[a0 >> 14][a1 >> 14][1];
        unsigned bw = w, bh = h;
        if (aff && (a0 & 0x200)) {
            bw *= 2;
            bh *= 2;
        }
        bool mos = (a0 & 0x1000) && mode != 2;
        unsigned oy = (y - (a0 & 0xFFu)) & 0xFFu;
        if (oy >= bh)
            continue;
        if (mos) {
            int32_t row = (int32_t)oy - (int32_t)(y - e->obj_mos_y);
            oy = row > 0 ? (unsigned)row : 0;
        }
        int32_t ox = a1 & 0x1FF;
        if (ox >= 256)
            ox -= 512;
        unsigned prio = (a2 >> 10) & 3u;
        int16_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (aff) {
            const uint8_t *pp = &oam[((a1 >> 9) & 31u) * 32u];
            pa = (int16_t)(pp[6] | pp[7] << 8);
            pb = (int16_t)(pp[14] | pp[15] << 8);
            pc = (int16_t)(pp[22] | pp[23] << 8);
            pd = (int16_t)(pp[30] | pp[31] << 8);
        }
        for (unsigned bx = 0; bx < bw; bx++) {
            int32_t sx = ox + (int32_t)bx;
            if (sx < 0 || sx >= (int32_t)NDS_W)
                continue;
            unsigned col = bx;
            unsigned tx, ty;
            if (aff) {
                int32_t dx = (int32_t)col - (int32_t)(bw / 2), dy = (int32_t)oy - (int32_t)(bh / 2);
                int32_t u = ((pa * dx + pb * dy) >> 8) + (int32_t)(w / 2), v = ((pc * dx + pd * dy) >> 8) + (int32_t)(h / 2);
                if (u < 0 || v < 0 || u >= (int32_t)w || v >= (int32_t)h)
                    continue;
                tx = (unsigned)u;
                ty = (unsigned)v;
            } else {
                if (col >= w)
                    continue;
                tx = (a1 & 0x1000) ? w - 1u - col : col;
                ty = (a1 & 0x2000) ? h - 1u - oy : oy;
            }
            int32_t c = texel(m, e, a0, a2, w, tx, ty);
            nds_px *q = &e->obj[sx];
            if (mode != 2 && !(q->flags & NDS_PX_ON))
                q->flags = mos ? NDS_PX_MOSAIC : 0;
            if (c < 0)
                continue;
            if (mode == 2) {
                e->objwin[sx] = 1;
                continue;
            }
            nds_px *p = &e->obj[sx];
            if ((p->flags & NDS_PX_ON) && p->prio <= prio)
                continue;
            p->c = nds_c18((uint32_t)c);
            p->prio = (uint8_t)prio;
            p->flags = (uint8_t)(NDS_PX_ON | (mode == 1 ? NDS_PX_SEMI : 0) | (mode == 3 ? NDS_PX_BITMAP : 0) | (mos ? NDS_PX_MOSAIC : 0));
            p->alpha = (uint8_t)(a2 >> 12);
        }
    }
    for (unsigned x = 1; x < NDS_W; x++)
        if ((e->obj[x].flags & NDS_PX_MOSAIC) && x % mw && (e->obj[x - 1].flags & NDS_PX_MOSAIC))
            e->obj[x] = e->obj[x - 1];
}
