/**
 * @file vdp.c
 * @brief The Master System and Game Gear VDP, with the TMS9918 modes, in
 * catch-up form.
 *
 * Ported from ares (ms/vdp/vdp.cpp, io.cpp, irq.cpp, background.cpp,
 * sprite.cpp, dac.cpp), with the per-clock thread replaced by a loop over
 * the same positions: a line start, then one pixel every two counts of the
 * first 512 on a picture line, then the counter step. It carries this
 * notice:
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
#include "vdp.h"

#include <string.h>

#define PIXELS_END 512u

typedef struct bg_out {
    uint8_t color;
    uint8_t palette;
    uint8_t priority;
} bg_out;

static uint8_t vram_at(const sms_vdp *v, uint32_t a) { return v->vram[a & 0x3FFFu]; }

uint32_t sms_vdp_lines(const sms_vdp *v) { return v->pal ? 313u : 262u; }

uint32_t sms_vdp_vlines(const sms_vdp *v)
{
    if (v->revision == 1)
        return 192;
    switch (v->video_mode) {
    case 0xB:
        return 224;
    case 0xE:
        return 240;
    default:
        return 192;
    }
}

static void poll(sms_vdp *v)
{
    bool level = (v->line_pending && v->line_enable) || (v->frame_pending && v->frame_enable);
    if (level != v->irq_level) {
        v->irq_level = level;
        if (v->on_irq)
            v->on_irq(v->user, level);
    }
}

void sms_vdp_reset(sms_vdp *v, sms_vdp_mode mode, uint8_t revision, bool pal)
{
    sms_vdp_frame_fn *f = v->on_frame;
    sms_vdp_irq_fn *i = v->on_irq;
    void *u = v->user;
    memset(v, 0, sizeof *v);
    v->on_frame = f;
    v->on_irq = i;
    v->user = u;
    v->mode = mode;
    v->revision = revision;
    v->pal = pal;
    v->line_coincidence = 0xFF;
    v->overflow_index = 0x1F;
    for (unsigned k = 0; k < 8; k++)
        v->objects[k].y = 0xD0;
}

/* Background: the TMS9918 graphics modes 1 and 2, and mode 4. */

static uint8_t tms_pixel(const sms_vdp *v, uint32_t pattern_addr, uint32_t color_addr, unsigned hoffset)
{
    uint8_t color = vram_at(v, color_addr);
    unsigned index = (hoffset ^ 7u) & 7u;
    return (vram_at(v, pattern_addr) >> index) & 1u ? (uint8_t)(color >> 4) : (uint8_t)(color & 15u);
}

static uint32_t tms_name(const sms_vdp *v, unsigned hoffset, unsigned voffset)
{
    return ((hoffset >> 3) & 31u) | ((voffset >> 3) & 31u) << 5 | (uint32_t)(v->name_table & 15u) << 10;
}

static void graphics1(const sms_vdp *v, unsigned hoffset, unsigned voffset, bg_out *o)
{
    uint8_t pattern = vram_at(v, tms_name(v, hoffset, voffset));
    uint32_t pa = (voffset & 7u) | (uint32_t)pattern << 3 | (uint32_t)(v->pattern_table & 7u) << 11;
    uint32_t ca = (uint32_t)(pattern >> 3) | (uint32_t)v->color_table << 6;
    o->color = tms_pixel(v, pa, ca, hoffset);
}

static void graphics2(const sms_vdp *v, unsigned hoffset, unsigned voffset, bg_out *o)
{
    uint8_t pattern = vram_at(v, tms_name(v, hoffset, voffset));
    uint32_t pa = (voffset & 7u) | (uint32_t)pattern << 3;
    if (voffset >= 64 && voffset <= 127)
        pa = (pa & ~(1u << 11)) | (uint32_t)(v->pattern_table & 1u) << 11;
    if (voffset >= 128 && voffset <= 191)
        pa = (pa & ~(1u << 12)) | (uint32_t)((v->pattern_table >> 1) & 1u) << 12;
    uint32_t ca = pa | (uint32_t)((v->color_table >> 7) & 1u) << 13;
    pa |= (uint32_t)((v->pattern_table >> 2) & 1u) << 13;
    o->color = tms_pixel(v, pa, ca, hoffset);
}

static void graphics3(const sms_vdp *v, unsigned hoffset, unsigned voffset, unsigned vlines, bg_out *o)
{
    if (hoffset < (v->latch_hscroll & 7u))
        return;
    if (!v->hscroll_lock || voffset >= 16)
        hoffset = (hoffset - v->latch_hscroll) & 0xFFu;
    if (!v->vscroll_lock || hoffset <= 191)
        voffset = (voffset + v->latch_vscroll) & 0x1FFu;
    uint32_t nta;
    if (vlines == 192) {
        voffset %= 224;
        nta = (uint32_t)(v->name_table >> 1) << 11;
        nta += (voffset >> 3) << 6;
        nta += (hoffset >> 3) << 1;
        nta &= 0x3FFFu;
        if (v->revision == 1 && !(v->name_table & 1u))
            nta &= ~(1u << 10);
    } else {
        voffset %= 256;
        nta = (uint32_t)(v->name_table >> 2) << 12 | 0x0700u;
        nta += (voffset >> 3) << 6;
        nta += (hoffset >> 3) << 1;
        nta &= 0x3FFFu;
    }
    uint32_t pattern = vram_at(v, nta | 0u) | (uint32_t)vram_at(v, nta | 1u) << 8;
    if (pattern & 0x200u)
        hoffset ^= 7u;
    if (pattern & 0x400u)
        voffset ^= 7u;
    o->palette = (pattern >> 11) & 1u;
    o->priority = (pattern >> 12) & 1u;
    uint32_t pa = ((voffset & 7u) << 2 | (pattern & 0x1FFu) << 5) & 0x3FFFu;
    unsigned index = (hoffset ^ 7u) & 7u;
    uint8_t c = 0;
    for (unsigned k = 0; k < 4; k++)
        c |= (uint8_t)(((vram_at(v, pa | k) >> index) & 1u) << k);
    o->color = c;
    if (!c)
        o->priority = 0;
}

static void background(const sms_vdp *v, unsigned x, unsigned y, bg_out *o)
{
    memset(o, 0, sizeof *o);
    switch (v->video_mode) {
    case 0x0:
        graphics1(v, x, y, o);
        return;
    case 0x2:
        graphics2(v, x, y, o);
        return;
    case 0x8:
    case 0xA:
    case 0xC:
    case 0xF:
        graphics3(v, x, y, 192, o);
        return;
    case 0xB:
        graphics3(v, x, y, 224, o);
        return;
    case 0xE:
        graphics3(v, x, y, 240, o);
        return;
    default:
        return;
    }
}

/* Sprites. */

static void sprite_setup(sms_vdp *v, unsigned voffset)
{
    unsigned valid = 0;
    int vlimit = (int)(((8u << v->sprite_zoom) << v->sprite_size) - 1u) & 31;
    for (unsigned k = 0; k < 8; k++)
        v->objects[k].y = 0xD0;
    if (!(v->video_mode & 8u)) {
        uint32_t attr = (uint32_t)(v->sprite_attr & 0x7Fu) << 7;
        for (unsigned index = 0; index < 32; index++) {
            int y = vram_at(v, attr++);
            if (y == 0xD0)
                break;
            if (y >= 0xE0)
                y -= 0x100;
            int x = vram_at(v, attr++);
            uint8_t pattern = vram_at(v, attr++);
            uint8_t extra = vram_at(v, attr++);
            if (extra & 0x80u)
                x -= 32;
            y += 1;
            if ((int)voffset < y || (int)voffset > y + vlimit)
                continue;
            if (vlimit == (v->sprite_zoom ? 31 : 15))
                pattern &= (uint8_t)~3u;
            if (valid == 4) {
                v->overflow = true;
                v->overflow_index = (uint8_t)index;
                break;
            }
            v->objects[valid++] = (sms_vdp_object){(int16_t)x, (int16_t)y, pattern, (uint8_t)(extra & 15u)};
        }
        return;
    }
    uint32_t attr = (uint32_t)((v->sprite_attr >> 1) & 0x3Fu) << 8;
    for (unsigned index = 0; index < 64; index++) {
        int y = vram_at(v, attr + index);
        if (sms_vdp_vlines(v) == 192 && y == 0xD0)
            break;
        if (y >= 0xF0)
            y -= 0x100;
        int x = vram_at(v, attr + 0x80u + (index << 1));
        uint8_t pattern = vram_at(v, attr + 0x81u + (index << 1));
        if (v->sprite_shift)
            x -= 8;
        y += 1;
        if ((int)voffset < y || (int)voffset > y + vlimit)
            continue;
        if (vlimit == (v->sprite_zoom ? 31 : 15))
            pattern &= (uint8_t)~1u;
        if (valid == 8) {
            v->overflow = true;
            v->overflow_index = (uint8_t)index;
            break;
        }
        v->objects[valid++] = (sms_vdp_object){(int16_t)x, (int16_t)y, pattern, 0};
    }
}

static uint8_t sprite_tms(sms_vdp *v, unsigned hoffset, unsigned voffset)
{
    uint8_t out = 0;
    int limit = (int)(((8u << v->sprite_zoom) << v->sprite_size) - 1u) & 31;
    for (unsigned k = 0; k < 8; k++) {
        const sms_vdp_object *o = &v->objects[k];
        if (o->y == 0xD0 || (int)hoffset < o->x || (int)hoffset > o->x + limit)
            continue;
        unsigned x = (unsigned)((int)hoffset - o->x) >> v->sprite_zoom;
        unsigned y = (unsigned)((int)voffset - o->y) >> v->sprite_zoom;
        uint32_t a = (((uint32_t)o->pattern << 3) + (x >> 3 << 4) + (y & (unsigned)limit)) & 0x7FFu;
        a |= (uint32_t)(v->sprite_pattern & 7u) << 11;
        if ((vram_at(v, a) >> ((x ^ 7u) & 7u)) & 1u) {
            if (out && v->display) {
                v->collision = true;
                break;
            }
            out = o->color;
        }
    }
    return out;
}

static uint8_t sprite_m4(sms_vdp *v, unsigned hoffset, unsigned voffset)
{
    uint8_t out = 0;
    int hlimit = (int)((8u << v->sprite_zoom) - 1u) & 15;
    unsigned vlimit = (((8u << v->sprite_zoom) << v->sprite_size) - 1u) & 31u;
    for (unsigned k = 0; k < 8; k++) {
        const sms_vdp_object *o = &v->objects[k];
        if (o->y == 0xD0 || (int)hoffset < o->x || (int)hoffset > o->x + hlimit)
            continue;
        unsigned x = (unsigned)((int)hoffset - o->x) >> v->sprite_zoom;
        unsigned y = (unsigned)((int)voffset - o->y) >> v->sprite_zoom;
        uint32_t a = ((((uint32_t)o->pattern << 3) + (y & vlimit)) & 0x7FFu) << 2;
        a |= (uint32_t)((v->sprite_pattern >> 2) & 1u) << 13;
        unsigned index = (x ^ 7u) & 7u;
        uint8_t c = 0;
        for (unsigned b = 0; b < 4; b++)
            c |= (uint8_t)(((vram_at(v, a | b) >> index) & 1u) << b);
        if (!c)
            continue;
        if (out && v->display) {
            v->collision = true;
            break;
        }
        out = c;
    }
    return out;
}

static uint8_t sprite(sms_vdp *v, unsigned x, unsigned y)
{
    switch (v->video_mode) {
    case 0x0:
    case 0x2:
        return sprite_tms(v, x, y);
    case 0x8:
    case 0xA:
    case 0xB:
    case 0xC:
    case 0xE:
    case 0xF:
        return sprite_m4(v, x, y);
    default:
        return 0;
    }
}

/* Colour output. */

static const uint8_t tms_colors[16] = {0x00, 0x00, 0x08, 0x0C, 0x10, 0x30, 0x01, 0x3C,
                                       0x02, 0x03, 0x05, 0x0F, 0x04, 0x33, 0x15, 0x3F};

static uint16_t widen(uint8_t c)
{
    uint16_t r = (uint16_t)((c & 3u) | (c & 3u) << 2);
    uint16_t g = (uint16_t)(((c >> 2) & 3u) | ((c >> 2) & 3u) << 2);
    uint16_t b = (uint16_t)(((c >> 4) & 3u) | ((c >> 4) & 3u) << 2);
    return (uint16_t)(r | g << 4 | b << 8);
}

uint16_t sms_vdp_palette(const sms_vdp *v, unsigned index)
{
    index &= 31u;
    bool m4 = v->video_mode & 8u;
    switch (v->mode) {
    case SMS_VDP_MS:
        return m4 ? (uint16_t)(v->cram[index] & 0x3Fu) : tms_colors[index & 15u];
    case SMS_VDP_GG_MS:
        return widen(m4 ? (uint8_t)(v->cram[index] & 0x3Fu) : tms_colors[index & 15u]);
    case SMS_VDP_GG:
        return m4 ? (uint16_t)(v->cram[index] & 0xFFFu) : widen(tms_colors[index & 15u]);
    }
    return 0;
}

static void pixel(sms_vdp *v, unsigned x, unsigned y)
{
    bg_out bg;
    background(v, x, y, &bg);
    uint8_t sp = sprite(v, x, y);
    uint16_t color = sms_vdp_palette(v, 16u | v->backdrop);
    if (v->display && (!v->left_clip || x >= 8)) {
        if (bg.priority || !sp)
            color = sms_vdp_palette(v, (unsigned)bg.palette << 4 | bg.color);
        else
            color = sms_vdp_palette(v, 16u | sp);
    }
    if (y < SMS_VDP_H)
        v->pixels[y * SMS_VDP_W + x] = color;
}

/* Line timing. */

static void line_start(sms_vdp *v)
{
    uint32_t vc = v->vcounter;
    if (v->pos) {
        if (vc == 240) {
            v->frames++;
            if (v->on_frame)
                v->on_frame(v->user);
        }
        if (vc < (v->pal ? 311u : 260u))
            v->ccounter = (uint16_t)((v->ccounter + 1u) & 0xFFFu);
    }
    uint32_t vl = sms_vdp_vlines(v);
    if (vc <= vl) {
        if (v->line_counter-- == 0) {
            v->line_counter = v->line_coincidence;
            v->line_pending = true;
            poll(v);
        }
    } else {
        v->line_counter = v->line_coincidence;
    }
    if (vc == vl + 1) {
        v->frame_pending = true;
        poll(v);
    }
    if (vc < vl) {
        v->latch_hscroll = v->hscroll;
        v->latch_vscroll = v->vscroll;
        sprite_setup(v, vc);
    }
}

void sms_vdp_run(sms_vdp *v, uint64_t last)
{
    while (v->pos <= last) {
        if (v->hcounter == 0) {
            line_start(v);
            v->line_draws = v->vcounter < sms_vdp_vlines(v);
            if (!v->line_draws && v->vcounter < SMS_VDP_H)
                memset(&v->pixels[v->vcounter * SMS_VDP_W], 0, SMS_VDP_W * sizeof v->pixels[0]);
        }
        uint64_t left = last - v->pos + 1;
        if (v->line_draws && v->hcounter < PIXELS_END) {
            uint32_t n = PIXELS_END - v->hcounter;
            if (left < n)
                n = (uint32_t)left;
            for (uint32_t h = v->hcounter; h < v->hcounter + n; h++)
                if (!(h & 1u))
                    pixel(v, h >> 1, v->vcounter);
            v->pos += n;
            v->hcounter += n;
            continue;
        }
        uint32_t n = SMS_VDP_LINE - v->hcounter;
        if (left < n)
            n = (uint32_t)left;
        v->pos += n;
        v->hcounter += n;
        if (v->hcounter == SMS_VDP_LINE) {
            v->hcounter = 0;
            if (++v->vcounter == sms_vdp_lines(v))
                v->vcounter = 0;
        }
    }
}

static uint64_t next_line_start(const sms_vdp *v, uint32_t *line)
{
    if (v->hcounter == 0) {
        *line = v->vcounter;
        return v->pos;
    }
    *line = v->vcounter + 1 == sms_vdp_lines(v) ? 0 : v->vcounter + 1;
    return v->pos + (SMS_VDP_LINE - v->hcounter);
}

uint64_t sms_vdp_next_irq(const sms_vdp *v)
{
    if (v->irq_level)
        return UINT64_MAX;
    uint32_t line;
    uint64_t at = next_line_start(v, &line);
    uint32_t lines = sms_vdp_lines(v), vl = sms_vdp_vlines(v);
    uint8_t counter = v->line_counter;
    for (uint32_t k = 0; k < 2 * lines + 1; k++, at += SMS_VDP_LINE) {
        if (line <= vl) {
            if (counter-- == 0) {
                counter = v->line_coincidence;
                if (v->line_enable)
                    return at;
            }
        } else {
            counter = v->line_coincidence;
        }
        if (line == vl + 1 && v->frame_enable)
            return at;
        if (++line == lines)
            line = 0;
    }
    return UINT64_MAX;
}

uint64_t sms_vdp_frame_edge(const sms_vdp *v)
{
    uint32_t lines = sms_vdp_lines(v);
    uint32_t ahead = (240u + lines - v->vcounter) % lines;
    uint64_t start = v->pos - v->hcounter + (uint64_t)ahead * SMS_VDP_LINE;
    if (start < v->pos)
        start += (uint64_t)lines * SMS_VDP_LINE;
    return start;
}

/* Ports. */

uint8_t sms_vdp_vcounter(const sms_vdp *v)
{
    uint32_t vc = v->vcounter;
    uint8_t mode = v->video_mode;
    if (!v->pal) {
        switch (mode) {
        case 0xB:
            return (uint8_t)(vc <= 234 ? vc : vc - 6);
        case 0xE:
            return (uint8_t)vc;
        default:
            return (uint8_t)(vc <= 218 ? vc : vc - 6);
        }
    }
    switch (mode) {
    case 0xB:
        return (uint8_t)(vc <= 258 ? vc : vc - 57);
    case 0xE:
        return (uint8_t)(vc <= 266 ? vc : vc - 56);
    default:
        return (uint8_t)(vc <= 242 ? vc : vc - 57);
    }
}

uint8_t sms_vdp_hcounter(const sms_vdp *v)
{
    uint32_t h = v->latch_hcounter;
    if (h >= 592)
        h += 340;
    return (uint8_t)(h >> 2);
}

void sms_vdp_latch_h(sms_vdp *v) { v->latch_hcounter = v->hcounter; }

uint16_t sms_vdp_ccounter(const sms_vdp *v) { return v->ccounter; }

uint8_t sms_vdp_data_read(sms_vdp *v)
{
    v->latch_control = false;
    uint8_t data = v->latch_vram;
    v->latch_vram = vram_at(v, v->address);
    v->address = (uint16_t)((v->address + 1u) & 0x3FFFu);
    return data;
}

uint8_t sms_vdp_status(sms_vdp *v)
{
    v->latch_control = false;
    uint8_t data = (uint8_t)((v->overflow_index & 0x1Fu) | (v->collision ? 0x20u : 0) | (v->overflow ? 0x40u : 0) |
                             (v->frame_pending ? 0x80u : 0));
    v->overflow_index = 0x1F;
    v->collision = false;
    v->overflow = false;
    v->frame_pending = false;
    v->line_pending = false;
    poll(v);
    return data;
}

void sms_vdp_data_write(sms_vdp *v, uint8_t data)
{
    v->latch_control = false;
    v->latch_vram = data;
    if (v->code <= 2) {
        v->vram[v->address & 0x3FFFu] = data;
    } else if (v->mode == SMS_VDP_GG) {
        if (!(v->address & 1u))
            v->latch_cram = data;
        else
            v->cram[(v->address >> 1) & 31u] = (uint16_t)((data & 15u) << 8 | v->latch_cram);
    } else {
        v->cram[v->address & 31u] = data & 0x3Fu;
    }
    v->address = (uint16_t)((v->address + 1u) & 0x3FFFu);
}

static void register_write(sms_vdp *v, unsigned reg, uint8_t data)
{
    switch (reg) {
    case 0x0:
        v->external_sync = data & 1u;
        v->video_mode = (uint8_t)((v->video_mode & ~0x0Au) | (data & 2u) | ((data >> 2) & 1u) << 3);
        v->sprite_shift = (data >> 3) & 1u;
        v->line_enable = (data >> 4) & 1u;
        v->left_clip = (data >> 5) & 1u;
        v->hscroll_lock = (data >> 6) & 1u;
        v->vscroll_lock = (data >> 7) & 1u;
        v->line_pending = v->line_pending && v->line_enable;
        poll(v);
        return;
    case 0x1:
        v->sprite_zoom = data & 1u;
        v->sprite_size = (data >> 1) & 1u;
        v->video_mode = (uint8_t)((v->video_mode & ~0x05u) | ((data >> 3) & 1u) << 2 | ((data >> 4) & 1u));
        v->frame_enable = (data >> 5) & 1u;
        v->display = (data >> 6) & 1u;
        v->frame_pending = v->frame_pending && v->frame_enable;
        poll(v);
        return;
    case 0x2:
        v->name_table = data & 15u;
        return;
    case 0x3:
        v->color_table = data;
        return;
    case 0x4:
        v->pattern_table = data & 7u;
        return;
    case 0x5:
        v->sprite_attr = data & 0x7Fu;
        return;
    case 0x6:
        v->sprite_pattern = data & 7u;
        return;
    case 0x7:
        v->backdrop = data & 15u;
        return;
    case 0x8:
        v->hscroll = data;
        return;
    case 0x9:
        v->vscroll = data;
        return;
    case 0xA:
        v->line_coincidence = data;
        return;
    default:
        return;
    }
}

void sms_vdp_control(sms_vdp *v, uint8_t data)
{
    if (!v->latch_control) {
        v->latch_control = true;
        v->address = (uint16_t)((v->address & 0x3F00u) | data);
        return;
    }
    v->latch_control = false;
    v->address = (uint16_t)((v->address & 0xFFu) | (uint16_t)(data & 0x3Fu) << 8);
    v->code = (uint8_t)(data >> 6);
    if (v->code == 0) {
        v->latch_vram = vram_at(v, v->address);
        v->address = (uint16_t)((v->address + 1u) & 0x3FFFu);
    }
    if (v->code == 2)
        register_write(v, (v->address >> 8) & 15u, (uint8_t)v->address);
}
