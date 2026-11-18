/**
 * @file gpu2d.c
 * @brief A 2D engine's registers and line merge: the windows (WIN0, WIN1,
 * the OBJ window), the layer priorities, alpha blending, semi-transparent
 * and bitmap OBJs, the 3D layer's own alpha, brightness up and down, and
 * master brightness. Colors are mixed with 6 bits per channel, a 5-bit
 * channel entering as twice its value.
 *
 * Fresh C from GBATEK ("LCD I/O Window Feature", "LCD I/O Mosaic
 * Function", "LCD I/O Color Special Effects", "DS Video Stuff"). The
 * rounding of the effects and of master brightness is the oracle's, from
 * relift's color probe ROM: a blend or a BLDY step rounds to the nearest
 * (+8 before the shift, +7 for a darken step), master brightness
 * truncates.
 */
#include "nds_int.h"

#include <string.h>

void nds_eng_reset(nds_eng *e, unsigned id)
{
    memset(e, 0, sizeof *e);
    e->id = id;
    e->pa[0] = e->pa[1] = e->pd[0] = e->pd[1] = 0x100;
}

/** @brief A 28-bit signed reference point from its register value. */
static int32_t s28(uint32_t v) { return (int32_t)(v << 4) >> 4; }

static uint16_t half(const uint8_t *r, uint32_t off) { return (uint16_t)(r[off] | r[off + 1] << 8); }

/** @brief Decodes the register word at off from the raw bytes after a write. */
static void decode(nds_eng *e, uint32_t off, bool ref_written)
{
    const uint8_t *r = e->raw;
    switch (off) {
    case 0x00:
        e->dispcnt = (uint32_t)half(r, 0) | (uint32_t)half(r, 2) << 16;
        return;
    case 0x08:
    case 0x0C:
        e->bgcnt[(off - 8) / 2] = half(r, off);
        e->bgcnt[(off - 8) / 2 + 1] = half(r, off + 2);
        return;
    case 0x10:
    case 0x14:
    case 0x18:
    case 0x1C: {
        unsigned n = (off - 0x10) / 4;
        e->hofs[n] = half(r, off) & 0x1FFu;
        e->vofs[n] = half(r, off + 2) & 0x1FFu;
        return;
    }
    case 0x20:
    case 0x24:
    case 0x30:
    case 0x34: {
        unsigned a = off >= 0x30;
        uint32_t b = a ? 0x30 : 0x20;
        e->pa[a] = (int16_t)half(r, b);
        e->pb[a] = (int16_t)half(r, b + 2);
        e->pc[a] = (int16_t)half(r, b + 4);
        e->pd[a] = (int16_t)half(r, b + 6);
        return;
    }
    case 0x28:
    case 0x2C:
    case 0x38:
    case 0x3C: {
        unsigned a = off >= 0x30;
        uint32_t v = (uint32_t)half(r, off) | (uint32_t)half(r, off + 2) << 16;
        if ((off & 4) == 0) {
            e->refx[a] = s28(v);
            if (ref_written)
                e->curx[a] = e->mosx[a] = e->refx[a];
        } else {
            e->refy[a] = s28(v);
            if (ref_written)
                e->cury[a] = e->mosy[a] = e->refy[a];
        }
        return;
    }
    case 0x40:
        e->winh[0] = half(r, 0x40);
        e->winh[1] = half(r, 0x42);
        return;
    case 0x44:
        e->winv[0] = half(r, 0x44);
        e->winv[1] = half(r, 0x46);
        return;
    case 0x48:
        e->winin = half(r, 0x48) & 0x3F3Fu;
        e->winout = half(r, 0x4A) & 0x3F3Fu;
        return;
    case 0x4C:
        e->mosaic = half(r, 0x4C);
        return;
    case 0x50:
        e->bldcnt = half(r, 0x50) & 0x3FFFu;
        e->bldalpha = half(r, 0x52) & 0x1F1Fu;
        return;
    case 0x54:
        e->bldy = half(r, 0x54) & 0x1Fu;
        return;
    case 0x6C:
        e->bright = half(r, 0x6C) & 0xC01Fu;
        return;
    default:
        return;
    }
}

uint32_t nds_eng_read(const nds_eng *e, uint32_t off)
{
    const uint8_t *r = e->raw;
    switch (off) {
    case 0x00:
        return e->dispcnt;
    case 0x08:
    case 0x0C:
        return (uint32_t)e->bgcnt[(off - 8) / 2] | (uint32_t)e->bgcnt[(off - 8) / 2 + 1] << 16;
    case 0x48:
        return (uint32_t)e->winin | (uint32_t)e->winout << 16;
    case 0x50:
        return (uint32_t)e->bldcnt | (uint32_t)e->bldalpha << 16;
    case 0x6C:
        return half(r, 0x6C) & 0xC01Fu;
    default:
        return 0;
    }
}

void nds_eng_write(nds_eng *e, uint32_t off, uint32_t v, uint32_t mask)
{
    if (off >= sizeof e->raw)
        return;
    for (unsigned i = 0; i < 4; i++)
        if (mask >> (8 * i) & 0xFF)
            e->raw[off + i] = (uint8_t)(v >> (8 * i));
    decode(e, off, true);
}

void nds_eng_frame(nds_eng *e)
{
    for (unsigned a = 0; a < 2; a++) {
        e->curx[a] = e->mosx[a] = e->refx[a];
        e->cury[a] = e->mosy[a] = e->refy[a];
    }
    e->bg_mos_cnt = e->obj_mos_cnt = 0;
    e->bg_mos_y = e->obj_mos_y = 0;
}

void nds_eng_line_end(nds_eng *e, unsigned y)
{
    for (unsigned a = 0; a < 2; a++) {
        e->curx[a] += e->pb[a];
        e->cury[a] += e->pd[a];
    }
    if (++e->bg_mos_cnt > ((e->mosaic >> 4) & 15u)) {
        e->bg_mos_cnt = 0;
        e->bg_mos_y = y + 1;
        for (unsigned a = 0; a < 2; a++) {
            e->mosx[a] = e->curx[a];
            e->mosy[a] = e->cury[a];
        }
    }
    if (++e->obj_mos_cnt > ((e->mosaic >> 12) & 15u)) {
        e->obj_mos_cnt = 0;
        e->obj_mos_y = y + 1;
    }
}

/** @brief The window control of each pixel of the line (bits 0-3 BG, 4 OBJ, 5 effects). */
static void windows(nds_eng *e, unsigned y, uint8_t *ctl)
{
    unsigned on = (e->dispcnt >> 13) & 7u;
    if (!on) {
        memset(ctl, 0x3F, NDS_W);
        return;
    }
    for (unsigned w = 0; w < 2; w++) {
        unsigned y1 = e->winv[w] >> 8, y2 = e->winv[w] & 0xFFu;
        if ((y & 0xFFu) == y1)
            e->winy[w] = true;
        if ((y & 0xFFu) == y2)
            e->winy[w] = false;
    }
    uint8_t out = (uint8_t)(e->winout & 0x3F), objin = (uint8_t)((e->winout >> 8) & 0x3F);
    for (unsigned x = 0; x < NDS_W; x++)
        ctl[x] = ((on & 4) && e->objwin[x]) ? objin : out;
    for (int w = 1; w >= 0; w--) {
        if (!(on & (1u << w)) || !e->winy[w])
            continue;
        unsigned x1 = e->winh[w] >> 8, x2 = e->winh[w] & 0xFFu;
        uint8_t in = (uint8_t)((e->winin >> (8 * w)) & 0x3F);
        bool h = e->winh_on[w];
        for (unsigned x = 0; x < NDS_W; x++) {
            if (x == x1)
                h = true;
            if (x == x2)
                h = false;
            if (h)
                ctl[x] = in;
        }
        e->winh_on[w] = h;
    }
}

static unsigned min_u(unsigned a, unsigned b) { return a < b ? a : b; }

static uint32_t blend(uint32_t a, unsigned eva, uint32_t b, unsigned evb)
{
    uint32_t r = 0;
    for (unsigned s = 0; s < 18; s += 6)
        r |= min_u(63, ((a >> s & 63u) * eva + (b >> s & 63u) * evb + 8) >> 4) << s;
    return r;
}

static uint32_t brighten(uint32_t a, unsigned evy)
{
    uint32_t r = 0;
    for (unsigned s = 0; s < 18; s += 6) {
        unsigned c = a >> s & 63u;
        r |= (c + (((63 - c) * evy + 8) >> 4)) << s;
    }
    return r;
}

static uint32_t darken(uint32_t a, unsigned evy)
{
    uint32_t r = 0;
    for (unsigned s = 0; s < 18; s += 6) {
        unsigned c = a >> s & 63u;
        r |= (c - ((c * evy + 7) >> 4)) << s;
    }
    return r;
}

uint32_t nds_eng_bright(const nds_eng *e, uint32_t c)
{
    unsigned f = min_u(16, e->bright & 31u), mode = e->bright >> 14;
    if (!f || mode == 0 || mode == 3)
        return c;
    uint32_t r = 0;
    for (unsigned s = 0; s < 18; s += 6) {
        unsigned v = c >> s & 63u;
        v = mode == 1 ? v + (((63 - v) * f) >> 4) : (v * (16 - f)) >> 4;
        r |= v << s;
    }
    return r;
}

void nds_eng_line(rl_nds *m, nds_eng *e, unsigned y, const uint32_t *line3d)
{
    for (unsigned n = 0; n < 4; n++)
        nds_eng_bg(m, e, n, y, line3d);
    nds_eng_obj(m, e, y);
    uint8_t ctl[NDS_W];
    windows(e, y, ctl);
    const uint8_t *pr = &m->pal[e->id * 0x400u];
    nds_px bd = {.c = nds_c18((uint32_t)(pr[0] | pr[1] << 8)), .prio = 4, .flags = NDS_PX_ON};
    unsigned mode = (e->bldcnt >> 6) & 3u, eva = min_u(16, e->bldalpha & 31u), evb = min_u(16, (e->bldalpha >> 8) & 31u);
    unsigned evy = min_u(16, e->bldy & 31u);
    unsigned order[4], nbg = 0;
    for (unsigned p = 0; p < 4; p++)
        for (unsigned k = 0; k < 4; k++)
            if ((e->dispcnt & (0x100u << k)) && (e->bgcnt[k] & 3u) == p)
                order[nbg++] = k;
    for (unsigned x = 0; x < NDS_W; x++) {
        const nds_px *top = &bd, *second = &bd;
        unsigned tl = 5, sl = 5;
        const nds_px *obj = &e->obj[x];
        bool obj_in = (obj->flags & NDS_PX_ON) && obj->prio < 4 && (ctl[x] >> 4 & 1);
        unsigned i = 0;
        while (sl == 5) {
            const nds_px *px;
            unsigned k;
            if (obj_in && (i == nbg || obj->prio <= (e->bgcnt[order[i]] & 3u))) {
                px = obj;
                k = 4;
                obj_in = false;
            } else if (i < nbg) {
                k = order[i++];
                px = &e->bg[k][x];
                if (!(px->flags & NDS_PX_ON) || !(ctl[x] >> k & 1))
                    continue;
            } else {
                break;
            }
            if (tl == 5) {
                top = px;
                tl = k;
            } else {
                second = px;
                sl = k;
            }
        }
        uint32_t c = top->c;
        bool fx = ctl[x] & 0x20, below = e->bldcnt >> (8 + sl) & 1;
        if ((top->flags & NDS_PX_3D) && top->alpha < 31 && below) {
            unsigned a = (top->alpha + 1u) / 2u;
            c = blend(c, a, second->c, 16 - a);
        } else if ((top->flags & NDS_PX_BITMAP) && below) {
            c = blend(c, top->alpha + 1u, second->c, 15u - top->alpha);
        } else if ((top->flags & NDS_PX_SEMI) && below) {
            c = blend(c, eva, second->c, evb);
        } else if (fx && (e->bldcnt >> tl & 1)) {
            if (mode == 1 && below && !(top->flags & NDS_PX_3D))
                c = blend(c, eva, second->c, evb);
            else if (mode == 2)
                c = brighten(c, evy);
            else if (mode == 3)
                c = darken(c, evy);
        }
        e->out[x] = c;
    }
}
