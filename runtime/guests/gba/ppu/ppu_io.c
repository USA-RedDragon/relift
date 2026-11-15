/**
 * @file ppu_io.c
 * @brief The picture unit's registers (0x04000000 to 0x04000055) byte by byte, and power on.
 *
 * Ported from ares (gba/ppu/io.cpp), which carries this notice:
 *
 * Copyright (c) 2004-2025 ares team, Near et al
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "ppu_int.h"

#include <string.h>

static void set_win_active(gba_ppu_window *w, uint8_t v)
{
    w->active[L_BG0] = v & 1;
    w->active[L_BG1] = (v >> 1) & 1;
    w->active[L_BG2] = (v >> 2) & 1;
    w->active[L_BG3] = (v >> 3) & 1;
    w->active[L_OBJ] = (v >> 4) & 1;
    w->active[L_SFX] = (v >> 5) & 1;
}

static uint8_t win_byte(const gba_ppu_window *w)
{
    return (uint8_t)(w->active[L_BG0] | w->active[L_BG1] << 1 | w->active[L_BG2] << 2 | w->active[L_BG3] << 3 | w->active[L_OBJ] << 4 |
                     w->active[L_SFX] << 5);
}

static uint8_t bg_lo(const gba_ppu_bg *b) { return (uint8_t)(b->priority | b->char_base << 2 | b->unused << 4 | b->mosaic << 6 | b->color_mode << 7); }

static uint8_t bg_hi(const gba_ppu_bg *b) { return (uint8_t)(b->screen_base | b->wrap << 5 | b->screen_size << 6); }

static uint8_t layer_bits(const uint8_t *f)
{
    return (uint8_t)(f[L_BG0] | f[L_BG1] << 1 | f[L_BG2] << 2 | f[L_BG3] << 3 | f[L_OBJ] << 4 | f[L_SFX] << 5);
}

static void set_layer_bits(uint8_t *f, uint8_t v)
{
    f[L_BG0] = v & 1;
    f[L_BG1] = (v >> 1) & 1;
    f[L_BG2] = (v >> 2) & 1;
    f[L_BG3] = (v >> 3) & 1;
    f[L_OBJ] = (v >> 4) & 1;
    f[L_SFX] = (v >> 5) & 1;
}

int gba_ppu_read(const gba_ppu *p, uint32_t addr)
{
    const gba_ppu_bg *b;
    switch (addr) {
    case 0x04000000:
        return p->mode | p->gbc_mode << 3 | p->frame << 4 | p->obj.hblank << 5 | p->obj.mapping << 6 | p->force_blank[3] << 7;
    case 0x04000001:
        return p->bg[0].enable[3] | p->bg[1].enable[3] << 1 | p->bg[2].enable[3] << 2 | p->bg[3].enable[3] << 3 | p->obj.enable[3] << 4 |
               p->win[W_IN0].enable << 5 | p->win[W_IN1].enable << 6 | p->win[W_IN2].enable << 7;
    case 0x04000002:
        return p->green_swap;
    case 0x04000003:
        return 0;
    case 0x04000008:
    case 0x0400000A:
    case 0x0400000C:
    case 0x0400000E:
        b = &p->bg[(addr - 0x04000008) >> 1];
        return bg_lo(b);
    case 0x04000009:
    case 0x0400000B:
    case 0x0400000D:
    case 0x0400000F:
        b = &p->bg[(addr - 0x04000009) >> 1];
        return bg_hi(b);
    case 0x04000048:
        return win_byte(&p->win[W_IN0]);
    case 0x04000049:
        return win_byte(&p->win[W_IN1]);
    case 0x0400004A:
        return win_byte(&p->win[W_OUT]);
    case 0x0400004B:
        return win_byte(&p->win[W_IN2]);
    case 0x04000050:
        return layer_bits(p->dac.blend_above) | p->dac.blend_mode << 6;
    case 0x04000051:
        return layer_bits(p->dac.blend_below);
    case 0x04000052:
        return p->dac.eva;
    case 0x04000053:
        return p->dac.evb;
    default:
        return -1;
    }
}

static void bg_cnt_lo(gba_ppu_bg *b, uint8_t v)
{
    b->priority = v & 3;
    b->char_base = (v >> 2) & 3;
    b->unused = (v >> 4) & 3;
    b->mosaic = (v >> 6) & 1;
    b->color_mode = (v >> 7) & 1;
}

static void bg_cnt_hi(gba_ppu_bg *b, uint8_t v, bool affine)
{
    b->screen_base = v & 31;
    b->wrap = affine ? (v >> 5) & 1 : 0;
    b->screen_size = (v >> 6) & 3;
}

static void bg_offset(gba_ppu_bg *b, unsigned reg, uint8_t v)
{
    uint16_t *o = (reg & 2) ? &b->voff : &b->hoff;
    if (reg & 1)
        *o = (uint16_t)((*o & 0xFF) | (v & 1) << 8);
    else
        *o = (uint16_t)((*o & 0x100) | v);
}

static void bg_param(gba_ppu_bg *b, unsigned reg, uint8_t v)
{
    int16_t *q = reg < 2 ? &b->pa : reg < 4 ? &b->pb : reg < 6 ? &b->pc : &b->pd;
    uint16_t u = (uint16_t)*q;
    u = (reg & 1) ? (uint16_t)((u & 0xFF) | v << 8) : (uint16_t)((u & 0xFF00) | v);
    *q = (int16_t)u;
}

static void bg_ref(int32_t *r, int32_t *l, unsigned byte, uint8_t v)
{
    uint32_t u = (uint32_t)*r & 0x0FFFFFFFu;
    uint32_t m = byte == 3 ? 0x0Fu : 0xFFu;
    u = (u & ~(0xFFu << (8 * byte))) | ((uint32_t)(v & m) << (8 * byte));
    *r = ppu_s28((int32_t)u);
    *l = *r;
}

void gba_ppu_write(gba_ppu *p, uint32_t addr, uint8_t v)
{
    gba_ppu_bg *b;
    unsigned off = addr - 0x04000000u;
    if (off >= 0x10 && off < 0x40) {
        if (off < 0x20) {
            bg_offset(&p->bg[(off - 0x10) >> 2], off & 3, v);
        } else {
            b = &p->bg[off < 0x30 ? 2 : 3];
            unsigned r = off & 15;
            if (r < 8)
                bg_param(b, r, v);
            else if (r < 12)
                bg_ref(&b->x, &b->lx, r - 8, v);
            else
                bg_ref(&b->y, &b->ly, r - 12, v);
        }
        return;
    }
    switch (off) {
    case 0x00:
        p->mode = v & 7;
        p->gbc_mode = (v >> 3) & 1;
        p->frame = (v >> 4) & 1;
        p->obj.hblank = (v >> 5) & 1;
        p->obj.mapping = (v >> 6) & 1;
        p->force_blank[3] = (v >> 7) & 1;
        for (unsigned i = 0; i < 4; i++)
            p->force_blank[i] |= (v >> 7) & 1;
        return;
    case 0x01:
        for (unsigned i = 0; i < 4; i++)
            gba_ppu_bg_set_enable(&p->bg[i], (v >> i) & 1);
        gba_ppu_obj_set_enable(p, (v >> 4) & 1);
        p->win[W_IN0].enable = (v >> 5) & 1;
        p->win[W_IN1].enable = (v >> 6) & 1;
        p->win[W_IN2].enable = (v >> 7) & 1;
        p->win[W_OUT].enable = (v >> 5) != 0;
        return;
    case 0x02:
        p->green_swap = v & 1;
        return;
    case 0x08:
    case 0x0A:
    case 0x0C:
    case 0x0E:
        bg_cnt_lo(&p->bg[(off - 8) >> 1], v);
        return;
    case 0x09:
    case 0x0B:
    case 0x0D:
    case 0x0F:
        bg_cnt_hi(&p->bg[(off - 9) >> 1], v, off >= 0x0D);
        return;
    case 0x40:
        p->win[W_IN0].x2 = v;
        return;
    case 0x41:
        p->win[W_IN0].x1 = v;
        return;
    case 0x42:
        p->win[W_IN1].x2 = v;
        return;
    case 0x43:
        p->win[W_IN1].x1 = v;
        return;
    case 0x44:
        p->win[W_IN0].y2 = v;
        return;
    case 0x45:
        p->win[W_IN0].y1 = v;
        return;
    case 0x46:
        p->win[W_IN1].y2 = v;
        return;
    case 0x47:
        p->win[W_IN1].y1 = v;
        return;
    case 0x48:
        set_win_active(&p->win[W_IN0], v);
        return;
    case 0x49:
        set_win_active(&p->win[W_IN1], v);
        return;
    case 0x4A:
        set_win_active(&p->win[W_OUT], v);
        return;
    case 0x4B:
        set_win_active(&p->win[W_IN2], v);
        return;
    case 0x4C:
        p->bg_mosaic_w = v & 15;
        p->bg_mosaic_h = v >> 4;
        return;
    case 0x4D:
        p->obj.mosaic_w = v & 15;
        p->obj.mosaic_h = v >> 4;
        return;
    case 0x50:
        set_layer_bits(p->dac.blend_above, v);
        p->dac.blend_mode = v >> 6;
        return;
    case 0x51:
        set_layer_bits(p->dac.blend_below, v);
        return;
    case 0x52:
        p->dac.eva = v & 31;
        return;
    case 0x53:
        p->dac.evb = v & 31;
        return;
    case 0x54:
        p->dac.evy = v & 31;
        return;
    default:
        return;
    }
}

void gba_ppu_reset(gba_ppu *p, const uint8_t *pram, const uint8_t *vram, const uint8_t *oam)
{
    memset(p, 0, sizeof *p);
    p->pram = pram;
    p->vram = vram;
    p->oam = oam;
    for (unsigned i = 0; i < 4; i++)
        gba_ppu_bg_reset(&p->bg[i], i);
    gba_ppu_obj_reset(p);
    p->line = p->pixels;
    p->pram_y = ~0u;
}

uint64_t gba_ppu_hash(const gba_ppu *p)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned i = 0; i < GBA_PPU_W * GBA_PPU_H; i++) {
        h = (h ^ (p->pixels[i] & 0xFFu)) * 0x100000001B3ull;
        h = (h ^ (p->pixels[i] >> 8)) * 0x100000001B3ull;
    }
    return h;
}
