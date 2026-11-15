/**
 * @file ppu_obj.c
 * @brief The sprites: the OAM walk and the cycle budget of a line, regular and affine sprites, the OBJ window and the sprite mosaic.
 *
 * Ported from ares (gba/ppu/object.cpp), which carries this notice:
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

enum { ST_A01, ST_A2, ST_PA, ST_PB, ST_PC, ST_PD };

void gba_ppu_obj_reset(gba_ppu *p) { memset(&p->obj, 0, sizeof p->obj); }

void gba_ppu_obj_set_enable(gba_ppu *p, bool status)
{
    p->obj.enable[3] = status;
    for (unsigned i = 0; i < 4; i++)
        p->obj.enable[i] &= status;
}

static void go_next(gba_ppu *p)
{
    p->obj.obj_index = (p->obj.obj_index + 1) & 0x7F;
    if (p->obj.obj_index == 0)
        p->obj.active = false;
}

static void read_a01(gba_ppu *p, unsigned y)
{
    static const uint8_t widths[] = {8, 16, 32, 64, 16, 32, 32, 64, 8, 8, 16, 32, 8, 8, 8, 8};
    static const uint8_t heights[] = {8, 16, 32, 64, 8, 8, 16, 32, 16, 32, 32, 64, 8, 8, 8, 8};
    gba_ppu_obj_latch *o = &p->obj.latch[1];
    uint32_t i = p->obj.obj_index;

    uint32_t a0 = ppu_h16(p->oam, i << 2 | 0);
    unsigned ypos = a0 & 0xFF;
    o->affine = (a0 >> 8) & 1;
    o->affine_size = (a0 >> 9) & 1;
    o->mode = (a0 >> 10) & 3;
    o->mosaic = (a0 >> 12) & 1;
    o->colors = (a0 >> 13) & 1;
    unsigned shape = (a0 >> 14) & 3;

    uint32_t a1 = ppu_h16(p->oam, i << 2 | 1);
    o->x = a1 & 0x1FF;
    o->affine_param = (a1 >> 9) & 31;
    o->hflip = (a1 >> 12) & 1;
    o->vflip = (a1 >> 13) & 1;
    unsigned size = (a1 >> 14) & 3;

    o->width = widths[shape * 4 + size];
    o->height = heights[shape * 4 + size];

    o->px = 0;
    if (o->x >= 240)
        o->px = (uint16_t)((o->px - o->x) & 0x1FF);
    if (!o->affine)
        o->px &= (uint16_t)~1u;

    o->py = (uint8_t)(y - ypos);
    if ((o->affine == 0 && o->affine_size == 1) || o->py >= (o->height << o->affine_size) || o->px >= (o->width << o->affine_size)) {
        go_next(p);
        p->obj.state = ST_A01;
    } else {
        if (o->mosaic) {
            int32_t d = p->obj.mosaic_y - (int32_t)ypos;
            o->py = (uint8_t)(ypos >= 160 || d >= 0 ? (uint32_t)d : 0u);
        }
        p->obj.state = ST_A2;
    }
}

static void read_a2(gba_ppu *p)
{
    p->obj.latch[0] = p->obj.latch[1];
    gba_ppu_obj_latch *o = &p->obj.latch[0];
    uint32_t a2 = ppu_h16(p->oam, (uint32_t)p->obj.obj_index << 2 | 2);
    o->character = a2 & 0x3FF;
    o->priority = (a2 >> 10) & 3;
    o->palette = (a2 >> 12) & 15;
    if (!o->affine) {
        go_next(p);
        p->obj.state = ST_A01;
        p->obj.vram_stage_ready = true;
    } else {
        p->obj.state = ST_PA;
    }
}

static unsigned obj_vram(const gba_ppu *p, uint32_t address)
{
    if (p->mode >= 3 && p->mode <= 5 && address <= 0x3FFF)
        return 0;
    return p->vram[0x10000 + (address & 0x7FFF)];
}

static void draw_object(gba_ppu *p, unsigned y)
{
    gba_pixel *buffer = p->obj.line_buffers[y & 1];
    gba_ppu_obj_latch *o = &p->obj.latch[0];

    int32_t center_x = (int32_t)(o->width >> 1);
    int32_t center_y = (int32_t)(o->height >> 1);
    int32_t current_x = -(center_x << o->affine_size) + (int32_t)o->px;
    int32_t current_y = -(center_y << o->affine_size) + (int32_t)o->py;
    int32_t fx = ppu_s28(current_x * o->pa + current_y * o->pb);
    int32_t fy = ppu_s28(current_x * o->pc + current_y * o->pd);

    uint32_t sx, sy;
    if (!o->affine) {
        sx = o->px ^ (o->hflip ? o->width - 1 : 0);
        sy = o->py ^ (o->vflip ? o->height - 1 : 0);
    } else {
        sx = (uint32_t)((fx >> 8) + center_x);
        sy = (uint32_t)((fy >> 8) + center_y);
    }
    uint32_t sub_tile = (((sy & 7) * 8 + (sx & 7)) >> !o->colors) & 63u;

    uint32_t tile;
    if (p->obj.mapping) {
        uint32_t offset = (sy >> 3) * (o->width >> 3) + (sx >> 3);
        tile = (o->character + (offset << o->colors)) & 0x3FFu;
    } else {
        uint32_t row = ((uint32_t)(o->character >> 5) + (sy >> 3)) & 31u;
        uint32_t entry = ((uint32_t)o->character + ((sx >> 3) << o->colors)) & 31u;
        tile = ((row << 5) + entry) & 0x3FFu;
    }

    uint32_t color = obj_vram(p, (tile << 5) + sub_tile);
    if (o->colors == 0)
        color = (sx & 1) ? color >> 4 : color & 15;
    unsigned bx = (o->x + o->px) & 0x1FFu;
    if (bx < 240 && sx < o->width && sy < o->height) {
        gba_pixel *b = &buffer[bx];
        if (o->mode & 2) {
            if (color)
                b->flags |= PX_WIN;
        } else if (!(b->flags & PX_ON) || o->priority < b->priority) {
            b->priority = o->priority;
            b->flags = (uint8_t)((b->flags & ~PX_MOSAIC) | (o->mosaic ? PX_MOSAIC : 0));
            if (color) {
                if (o->colors == 0)
                    color = (uint32_t)o->palette * 16 + color;
                b->flags |= PX_ON;
                b->color = (uint16_t)(256 + color);
                b->flags = (uint8_t)((b->flags & ~PX_TRANS) | (o->mode == 1 ? PX_TRANS : 0));
            }
        }
    }

    o->px = (o->px + 1) & 0x1FF;
    if (o->px == (o->width << o->affine_size))
        p->obj.vram_stage_active = false;
}

static void step_oam(gba_ppu *p)
{
    if (!p->obj.active)
        return;
    gba_ppu_obj_latch *o = &p->obj.latch[0];
    uint32_t g = (uint32_t)o->affine_param << 4;
    switch (p->obj.state) {
    case ST_A01:
        read_a01(p, p->obj.render_y);
        break;
    case ST_A2:
        read_a2(p);
        break;
    case ST_PA:
        o->pa = (int16_t)ppu_h16(p->oam, g | 3);
        p->obj.state = ST_PB;
        break;
    case ST_PB:
        o->pb = (int16_t)ppu_h16(p->oam, g | 7);
        p->obj.state = ST_PC;
        break;
    case ST_PC:
        o->pc = (int16_t)ppu_h16(p->oam, g | 11);
        p->obj.state = ST_PD;
        break;
    default:
        o->pd = (int16_t)ppu_h16(p->oam, g | 15);
        go_next(p);
        p->obj.vram_stage_ready = true;
        p->obj.state = ST_A01;
        break;
    }
}

static void obj_step(gba_ppu *p)
{
    gba_ppu_obj_latch *o = &p->obj.latch[0];
    if (p->obj.active_cycle) {
        if (p->obj.vram_stage_ready) {
            p->obj.vram_stage_ready = false;
            p->obj.vram_stage_active = true;
            if (!o->affine) {
                draw_object(p, p->obj.render_y);
                draw_object(p, p->obj.render_y);
            }
            step_oam(p);
        } else if (p->obj.vram_stage_active) {
            draw_object(p, p->obj.render_y);
            if (!o->affine)
                draw_object(p, p->obj.render_y);
            if (!p->obj.vram_stage_active)
                step_oam(p);
        } else {
            step_oam(p);
        }
    }
    p->obj.active_cycle = !p->obj.active_cycle;
}

static void obj_scanline(gba_ppu *p, unsigned y)
{
    if (y >= 160)
        return;
    p->obj.hmosaic_offset = p->obj.mosaic_w;
    if (y == 0 || p->obj.vmosaic_offset == p->obj.mosaic_h) {
        p->obj.vmosaic_offset = 0;
        p->obj.mosaic_y = (int32_t)y;
    } else {
        p->obj.vmosaic_offset++;
    }
    memset(p->obj.line_buffers[y & 1], 0, sizeof p->obj.line_buffers[0]);
    if (p->force_blank[1] || p->stopped || !p->obj.enable[1])
        return;
    p->obj.render_y = y;
    p->obj.obj_index = 0;
    p->obj.active = true;
    p->obj.active_cycle = true;
    p->obj.vram_stage_ready = false;
    p->obj.vram_stage_active = false;
    p->obj.state = ST_A01;
}

void gba_ppu_obj_render_scanline(gba_ppu *p, unsigned y)
{
    obj_scanline(p, y);
    unsigned steps = p->obj.hblank ? 964u : 1232u;
    for (unsigned i = 0; i < steps; i++) {
        if (!p->obj.active && !p->obj.vram_stage_ready && !p->obj.vram_stage_active)
            break;
        obj_step(p);
    }
    p->obj.active = false;
    p->obj.vram_stage_ready = false;
    p->obj.vram_stage_active = false;
}

void gba_ppu_obj_output(gba_ppu *p, unsigned x, unsigned y)
{
    memset(&p->obj.output, 0, sizeof p->obj.output);
    if (gba_ppu_blank(p) || !p->obj.enable[0]) {
        memset(&p->obj.mosaic_latch, 0, sizeof p->obj.mosaic_latch);
        return;
    }
    p->obj.output = p->obj.line_buffers[y & 1][x];
    if (p->obj.hmosaic_offset == p->obj.mosaic_w) {
        p->obj.hmosaic_offset = 0;
        p->obj.mosaic_latch = p->obj.output;
    } else {
        p->obj.hmosaic_offset++;
    }
    if (!(p->obj.mosaic_latch.flags & PX_MOSAIC) || !(p->obj.output.flags & PX_MOSAIC) || p->obj.output.priority < p->obj.mosaic_latch.priority)
        p->obj.mosaic_latch = p->obj.output;
}
