/**
 * @file tms9918.c
 * @brief The TMS9918A of the SG-1000 in catch-up form.
 *
 * Ported from ares (component/video/tms9918: tms9918.cpp, io.cpp,
 * background.cpp, sprite.cpp, dac.cpp; sg/vdp/color.cpp), with the
 * per-clock thread replaced by a loop over the same positions: a line
 * start, one pixel every two counts of the first 512 on a picture line. It
 * carries this notice:
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
#include "tms9918.h"

#include <string.h>

static uint8_t vram_at(const sms_tms *t, uint32_t a) { return t->vram[a & 0x3FFFu]; }

static void poll(sms_tms *t) { t->irq_level = t->frame_pending && t->frame_enable; }

void sms_tms_reset(sms_tms *t)
{
    sms_tms_frame_fn *f = t->on_frame;
    void *u = t->user;
    memset(t, 0, sizeof *t);
    t->on_frame = f;
    t->user = u;
    t->overflow_index = 0x1F;
    for (unsigned k = 0; k < 4; k++)
        t->objects[k].y = 0xD0;
}

static uint8_t bg_pixel(const sms_tms *t, unsigned x, unsigned y)
{
    switch (t->video_mode) {
    case 0: {
        uint8_t pattern = vram_at(t, (x >> 3) | (y >> 3) << 5 | (uint32_t)(t->name_table & 15u) << 10);
        uint32_t pa = (y & 7u) | (uint32_t)pattern << 3 | (uint32_t)(t->pattern_table & 7u) << 11;
        uint8_t color = vram_at(t, (uint32_t)(pattern >> 3) | (uint32_t)t->color_table << 6);
        return (vram_at(t, pa) >> ((x ^ 7u) & 7u)) & 1u ? (uint8_t)(color >> 4) : (uint8_t)(color & 15u);
    }
    case 1: {
        uint32_t na = (((y >> 3) * 40u + x / 6u) & 0x3FFu) | (uint32_t)(t->name_table & 15u) << 10;
        uint8_t pattern = vram_at(t, na);
        uint32_t pa = (y & 7u) | (uint32_t)pattern << 3 | (uint32_t)(t->pattern_table & 7u) << 11;
        unsigned index = (7u - x % 6u) & 7u;
        return (vram_at(t, pa) >> index) & 1u ? t->foreground : 0;
    }
    case 2: {
        uint8_t pattern = vram_at(t, (x >> 3) | (y >> 3) << 5 | (uint32_t)(t->name_table & 15u) << 10);
        uint32_t pa = ((y >> 2) & 7u) | (uint32_t)pattern << 3 | (uint32_t)(t->pattern_table & 7u) << 11;
        uint8_t color = vram_at(t, pa);
        return (x >> 2) & 1u ? (uint8_t)(color & 15u) : (uint8_t)(color >> 4);
    }
    case 4: {
        uint8_t pattern = vram_at(t, (x >> 3) | (y >> 3) << 5 | (uint32_t)(t->name_table & 15u) << 10);
        uint32_t pa = (y & 7u) | (uint32_t)pattern << 3;
        if (y >= 64 && y <= 127)
            pa = (pa & ~(1u << 11)) | (uint32_t)(t->pattern_table & 1u) << 11;
        if (y >= 128 && y <= 191)
            pa = (pa & ~(1u << 12)) | (uint32_t)((t->pattern_table >> 1) & 1u) << 12;
        uint32_t ca = (pa & 0x1FFFu) | (uint32_t)((t->color_table >> 7) & 1u) << 13;
        pa = (pa & 0x1FFFu) | (uint32_t)((t->pattern_table >> 2) & 1u) << 13;
        uint8_t color = vram_at(t, ca);
        return (vram_at(t, pa) >> ((x ^ 7u) & 7u)) & 1u ? (uint8_t)(color >> 4) : (uint8_t)(color & 15u);
    }
    default:
        return 0;
    }
}

static void sprite_setup(sms_tms *t, unsigned voffset)
{
    unsigned valid = 0;
    int vlimit = (int)(((8u << t->sprite_zoom) << t->sprite_size) - 1u) & 31;
    for (unsigned k = 0; k < 4; k++)
        t->objects[k].y = 0xD0;
    uint32_t a = (uint32_t)(t->sprite_attr & 0x7Fu) << 7;
    for (unsigned index = 0; index < 32; index++) {
        int y = vram_at(t, a++);
        if (y == 0xD0)
            break;
        if (y >= 0xE0)
            y -= 0x100;
        int x = vram_at(t, a++);
        uint8_t pattern = vram_at(t, a++);
        uint8_t extra = vram_at(t, a++);
        if (extra & 0x80u)
            x -= 32;
        y += 1;
        if ((int)voffset < y || (int)voffset > y + vlimit)
            continue;
        if (vlimit == (t->sprite_zoom ? 31 : 15))
            pattern &= (uint8_t)~3u;
        if (valid == 4) {
            t->overflow = true;
            t->overflow_index = (uint8_t)index;
            break;
        }
        t->objects[valid++] = (sms_tms_object){(int16_t)x, (int16_t)y, pattern, (uint8_t)(extra & 15u)};
    }
}

static uint8_t sprite_pixel(sms_tms *t, unsigned x, unsigned y)
{
    if (t->video_mode == 1)
        return 0;
    uint8_t color = 0;
    int limit = (int)(((8u << t->sprite_zoom) << t->sprite_size) - 1u) & 31;
    for (unsigned k = 0; k < 4; k++) {
        const sms_tms_object *o = &t->objects[k];
        if (o->y == 0xD0 || (int)x < o->x || (int)x > o->x + limit)
            continue;
        unsigned sx = (unsigned)((int)x - o->x) >> t->sprite_zoom;
        unsigned sy = (unsigned)((int)y - o->y) >> t->sprite_zoom;
        uint32_t a = (((uint32_t)o->pattern << 3) + (sx >> 3 << 4) + (sy & (unsigned)limit)) & 0x7FFu;
        a |= (uint32_t)(t->sprite_pattern & 7u) << 11;
        if ((vram_at(t, a) >> ((sx ^ 7u) & 7u)) & 1u) {
            if (color) {
                t->collision = true;
                break;
            }
            color = o->color;
        }
    }
    return color;
}

static void pixel(sms_tms *t, unsigned x, unsigned y)
{
    uint8_t bg = bg_pixel(t, x, y);
    uint8_t sp = sprite_pixel(t, x, y);
    uint8_t c = t->background;
    if (t->display) {
        if (bg)
            c = bg;
        if (sp)
            c = sp;
    }
    t->pixels[y * SMS_TMS_W + x] = c;
}

static void line_start(sms_tms *t)
{
    if (t->vcounter == 192 && t->pos) {
        t->frame_pending = true;
        poll(t);
        t->frames++;
        if (t->on_frame)
            t->on_frame(t->user);
    }
    if (t->vcounter < 192)
        sprite_setup(t, t->vcounter);
}

void sms_tms_run(sms_tms *t, uint64_t last)
{
    while (t->pos <= last) {
        if (t->hcounter == 0)
            line_start(t);
        uint64_t left = last - t->pos + 1;
        if (t->vcounter < 192 && t->hcounter < 512) {
            uint32_t n = 512 - t->hcounter;
            if (left < n)
                n = (uint32_t)left;
            for (uint32_t h = t->hcounter; h < t->hcounter + n; h++)
                if (!(h & 1u))
                    pixel(t, h >> 1, t->vcounter);
            t->pos += n;
            t->hcounter += n;
            continue;
        }
        uint32_t n = SMS_TMS_LINE - t->hcounter;
        if (left < n)
            n = (uint32_t)left;
        t->pos += n;
        t->hcounter += n;
        if (t->hcounter == SMS_TMS_LINE) {
            t->hcounter = 0;
            if (++t->vcounter == SMS_TMS_LINES)
                t->vcounter = 0;
        }
    }
}

uint64_t sms_tms_frame_edge(const sms_tms *t)
{
    uint32_t ahead = (192u + SMS_TMS_LINES - t->vcounter) % SMS_TMS_LINES;
    uint64_t start = t->pos - t->hcounter + (uint64_t)ahead * SMS_TMS_LINE;
    if (start < t->pos)
        start += (uint64_t)SMS_TMS_LINES * SMS_TMS_LINE;
    return start;
}

uint64_t sms_tms_next_irq(const sms_tms *t)
{
    if (t->irq_level || !t->frame_enable)
        return UINT64_MAX;
    return sms_tms_frame_edge(t);
}

uint8_t sms_tms_status(sms_tms *t)
{
    t->control_latch = 0;
    uint8_t data = (uint8_t)((t->overflow_index & 0x1Fu) | (t->collision ? 0x20u : 0) | (t->overflow ? 0x40u : 0) |
                             (t->frame_pending ? 0x80u : 0));
    t->overflow_index = 0x1F;
    t->collision = false;
    t->overflow = false;
    t->frame_pending = false;
    poll(t);
    return data;
}

uint8_t sms_tms_data_read(sms_tms *t)
{
    t->control_latch = 0;
    uint16_t a = t->control & 0x3FFFu;
    t->control = (uint16_t)((t->control & 0xC000u) | ((a + 1u) & 0x3FFFu));
    uint8_t data = t->vram_latch;
    t->vram_latch = vram_at(t, a);
    return data;
}

void sms_tms_data_write(sms_tms *t, uint8_t data)
{
    t->control_latch = 0;
    uint16_t a = t->control & 0x3FFFu;
    t->control = (uint16_t)((t->control & 0xC000u) | ((a + 1u) & 0x3FFFu));
    t->vram[a] = data;
}

static void reg(sms_tms *t, unsigned r, uint8_t data)
{
    switch (r) {
    case 0:
        t->external_sync = data & 1u;
        t->video_mode = (uint8_t)((t->video_mode & ~4u) | ((data >> 1) & 1u) << 2);
        break;
    case 1:
        t->sprite_zoom = data & 1u;
        t->sprite_size = (data >> 1) & 1u;
        t->video_mode = (uint8_t)((t->video_mode & ~3u) | ((data >> 3) & 1u) << 1 | ((data >> 4) & 1u));
        t->frame_enable = (data >> 5) & 1u;
        t->display = (data >> 6) & 1u;
        t->vram_mode = (data >> 7) & 1u;
        poll(t);
        break;
    case 2:
        t->name_table = data & 15u;
        break;
    case 3:
        t->color_table = data;
        break;
    case 4:
        t->pattern_table = data & 7u;
        break;
    case 5:
        t->sprite_attr = data & 0x7Fu;
        break;
    case 6:
        t->sprite_pattern = data & 7u;
        break;
    default:
        t->background = data & 15u;
        t->foreground = data >> 4;
        break;
    }
}

void sms_tms_control(sms_tms *t, uint8_t data)
{
    if (t->control_latch == 0) {
        t->control = (uint16_t)((t->control & 0xFF00u) | data);
        t->control_latch = 1;
        return;
    }
    t->control = (uint16_t)((t->control & 0x00FFu) | (uint16_t)data << 8);
    t->control_latch = 0;
    if (t->control & 0x8000u) {
        reg(t, (t->control >> 8) & 7u, (uint8_t)t->control);
        return;
    }
    if (!(t->control & 0x4000u))
        (void)sms_tms_data_read(t);
}

void sms_tms_rgb(uint8_t c, uint8_t rgb[3])
{
    static const uint8_t pal[16][3] = {
        {0x00, 0x00, 0x00}, {0x00, 0x00, 0x00}, {0x21, 0xC8, 0x42}, {0x5E, 0xDC, 0x78}, {0x54, 0x55, 0xED}, {0x7D, 0x76, 0xFC},
        {0xD4, 0x52, 0x4D}, {0x42, 0xEB, 0xF5}, {0xFC, 0x55, 0x54}, {0xFF, 0x79, 0x78}, {0xD4, 0xC1, 0x54}, {0xE6, 0xCE, 0x80},
        {0x21, 0xB0, 0x3B}, {0xC9, 0x5B, 0xBA}, {0xCC, 0xCC, 0xCC}, {0xFF, 0xFF, 0xFF},
    };
    memcpy(rgb, pal[c & 15u], 3);
}
