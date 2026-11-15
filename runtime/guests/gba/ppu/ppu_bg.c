/**
 * @file ppu_bg.c
 * @brief The background layers: text, affine and bitmap, with the mosaic and the VRAM reads they make.
 * The affine reference points advance as NanoBoyAdvance measured the console,
 * not as ares: only while the layer is enabled (and in a mode that uses it),
 * and by the whole mosaic height once per mosaic block. A read of video RAM
 * above the background limit returns the last halfword the layer fetched
 * (the bus latch), not zero; the map address of an affine layer is 16 bits wide.
 *
 * Ported from ares (gba/ppu/background.cpp and memory.cpp), which carries this notice:
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

bool gba_ppu_blank(const gba_ppu *p) { return p->force_blank[0] || p->stopped; }

uint32_t gba_ppu_vram_bg(gba_ppu *p, unsigned size, uint32_t addr)
{
    uint32_t half;
    if (addr >= (p->mode < 3 ? 0x10000u : 0x14000u)) {
        half = p->vram_bg_latch;
    } else {
        uint32_t a = addr & 0x1FFFFu;
        a &= (a & 0x10000) ? 0x17FFEu : 0x0FFFEu;
        half = (uint32_t)(p->vram[a] | p->vram[a + 1] << 8);
        p->vram_bg_latch = (uint16_t)half;
    }
    if (size == RD_BYTE)
        return (addr & 1) ? half >> 8 : half & 0xFF;
    return half;
}

uint32_t gba_ppu_pram(const gba_ppu *p, unsigned index) { return ppu_h16(p->pram, index & 511); }

void gba_ppu_bg_reset(gba_ppu_bg *b, unsigned id)
{
    memset(b, 0, sizeof *b);
    b->id = id;
}

void gba_ppu_bg_set_enable(gba_ppu_bg *b, bool status)
{
    b->enable[3] = status;
    for (unsigned i = 0; i < 4; i++)
        b->enable[i] &= status;
}

void gba_ppu_bg_scanline(gba_ppu *p, gba_ppu_bg *b, unsigned y)
{
    b->mosaic_offset = 0;
    memset(b->output, 0, sizeof b->output);
    b->latch.active = 0;
    unsigned rows = b->mosaic ? 1u + p->bg_mosaic_h : 1u;
    if (rows == 1 || (y % rows) == 0) {
        b->vmosaic = y;
        b->affine.hmosaic = (uint32_t)b->lx;
        b->affine.vmosaic = (uint32_t)b->ly;
        bool advances = b->enable[0] && (b->id == 2 ? p->mode >= 1 && p->mode <= 5 : b->id == 3 && p->mode == 2);
        if (advances) {
            b->lx = ppu_s28(b->lx + (int32_t)rows * b->pb);
            b->ly = ppu_s28(b->ly + (int32_t)rows * b->pd);
        }
    }
}

void gba_ppu_bg_output(gba_ppu *p, gba_ppu_bg *b, unsigned x)
{
    if (!b->mosaic_offset) {
        b->mosaic_offset = 1u + p->bg_mosaic_w;
        b->mosaic_latch = b->output[x];
    } else if (!b->mosaic) {
        b->mosaic_latch = b->output[x];
    }
    b->mosaic_offset--;
}

static void linear_map(gba_ppu *p, gba_ppu_bg *b, int x)
{
    uint32_t tx = (uint32_t)((x + (int32_t)b->hoff) >> 3) & 63u;
    uint32_t ty = (uint32_t)(((int32_t)b->vmosaic + (int32_t)b->voff) >> 3) & 63u;
    uint32_t offset = (ty & 31) << 5 | (tx & 31);
    if ((b->screen_size & 1) && (tx & 32))
        offset += 32u << 5;
    if ((b->screen_size & 2) && (ty & 32))
        offset += 32u << (5 + (b->screen_size & 1));
    offset = ((uint32_t)b->screen_base << 11) + (offset << 1);
    uint32_t t = gba_ppu_vram_bg(p, RD_HALF, offset);
    b->latch.character = t & 0x3FF;
    b->latch.hflip = (t >> 10) & 1;
    b->latch.vflip = (t >> 11) & 1;
    b->latch.palette = (t >> 12) & 15;
}

static void linear_tile(gba_ppu *p, gba_ppu_bg *b, int x0)
{
    unsigned py = ((unsigned)b->vmosaic + b->voff) & 7;
    if (b->latch.vflip)
        py = ~py & 7;
    uint32_t base = ((uint32_t)b->char_base << 14) + ((((uint32_t)b->latch.character << 5) + (py << 2)) << b->color_mode);
    unsigned cm = b->color_mode;
    for (unsigned i = 0; i < 8; i++) {
        int x = x0 + (int)i;
        if (x >= 240)
            return;
        unsigned sub = b->latch.hflip ? 7 - i : i;
        if (((i << cm) & 3) == 0)
            b->latch.data = (uint16_t)gba_ppu_vram_bg(p, RD_HALF, base + (sub >> (1 - cm)));
        uint32_t color = b->latch.data;
        if (cm == 0) {
            color >>= (sub & 3) * 4;
            color &= 15;
        } else {
            color >>= (sub & 1) * 8;
            color &= 255;
        }
        if (color && x >= 0) {
            if (cm == 0)
                color |= (uint32_t)b->latch.palette << 4;
            gba_pixel *o = &b->output[x];
            o->flags |= PX_ON;
            o->priority = b->priority;
            o->color = (uint16_t)color;
        }
    }
}

static void affine_run(gba_ppu *p, gba_ppu_bg *b)
{
    b->fx = (int32_t)b->affine.hmosaic;
    b->fy = (int32_t)b->affine.vmosaic;
    b->fx = ppu_s28(b->fx);
    b->fy = ppu_s28(b->fy);
    uint32_t size = 16u << b->screen_size;
    uint32_t wrap = (1u << (b->wrap ? 7u + b->screen_size : 20u)) - 1u;
    for (unsigned x = 0; x < GBA_PPU_W; x++) {
        uint32_t cx = (uint32_t)(b->fx >> 8) & wrap;
        uint32_t cy = (uint32_t)(b->fy >> 8) & wrap;
        uint32_t tx = cx >> 3, ty = cy >> 3;
        uint32_t character = gba_ppu_vram_bg(p, RD_BYTE, (((uint32_t)b->screen_base << 11) + ty * size + tx) & 0xFFFFu) & 0xFF;
        uint32_t color = gba_ppu_vram_bg(p, RD_BYTE, ((uint32_t)b->char_base << 14) + (character << 6) + ((cy & 7) << 3) + (cx & 7)) & 0xFF;
        if (tx < size && ty < size && color) {
            gba_pixel *o = &b->output[x];
            o->flags |= PX_ON;
            o->priority = b->priority;
            o->color = (uint16_t)color;
        }
        b->fx = ppu_s28(b->fx + b->pa);
        b->fy = ppu_s28(b->fy + b->pc);
    }
}

static void bitmap_run(gba_ppu *p, gba_ppu_bg *b)
{
    b->fx = ppu_s28((int32_t)b->affine.hmosaic);
    b->fy = ppu_s28((int32_t)b->affine.vmosaic);
    unsigned depth = p->mode != 4;
    uint32_t width = p->mode == 5 ? 160 : 240;
    uint32_t height = p->mode == 5 ? 128 : 160;
    uint32_t base = p->mode == 3 ? 0 : 0xA000u * p->frame;
    for (unsigned x = 0; x < GBA_PPU_W; x++) {
        uint32_t px = (uint32_t)(b->fx >> 8);
        uint32_t py = (uint32_t)(b->fy >> 8);
        uint32_t offset = py * width + px;
        uint32_t color = gba_ppu_vram_bg(p, depth ? RD_HALF : RD_BYTE, base + (offset << depth));
        if (px < width && py < height && (depth || color)) {
            gba_pixel *o = &b->output[x];
            if (depth)
                o->flags |= PX_DIRECT;
            o->flags |= PX_ON;
            o->priority = b->priority;
            o->color = (uint16_t)color;
        }
        b->fx = ppu_s28(b->fx + b->pa);
        b->fy = ppu_s28(b->fy + b->pc);
    }
}

void gba_ppu_bg_run_line(gba_ppu *p, gba_ppu_bg *b)
{
    if (gba_ppu_blank(p) || !b->enable[0])
        return;
    switch (b->id) {
    case 0:
    case 1:
        if (p->mode > 1)
            return;
        break;
    case 2:
        if (p->mode >= 3) {
            if (p->mode <= 5)
                bitmap_run(p, b);
            return;
        }
        if (p->mode >= 1) {
            affine_run(p, b);
            return;
        }
        break;
    default:
        if (p->mode == 2)
            affine_run(p, b);
        if (p->mode != 0)
            return;
        break;
    }
    for (int x = -(int)(b->hoff & 7); x < 240; x += 8) {
        linear_map(p, b, x);
        linear_tile(p, b, x);
    }
}
