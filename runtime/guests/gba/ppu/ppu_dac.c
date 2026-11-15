/**
 * @file ppu_dac.c
 * @brief The windows, the layer priorities and the blend, brighten and darken effects.
 * The effects follow the console as NanoBoyAdvance measured it, not ares:
 * green has six bits (bit 15 of a palette or bitmap color is its low bit),
 * and a blend or brighten result is rounded to the nearest step.
 *
 * Ported from ares (gba/ppu/window.cpp and dac.cpp), which carries this notice:
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

void gba_ppu_win_scanline(gba_ppu_window *w, unsigned y)
{
    if (y == w->y1)
        w->v = 1;
    if (y == w->y2)
        w->v = 0;
}

void gba_ppu_win_run(gba_ppu_window *w, unsigned x)
{
    if (x == w->x1)
        w->h = 1;
    if (x == w->x2)
        w->h = 0;
    w->output[x] = w->h && w->v;
}

static unsigned min_u(unsigned a, unsigned b) { return a < b ? a : b; }

static uint16_t lookup(const gba_ppu *p, const gba_pixel *l)
{
    if (l->flags & PX_DIRECT)
        return l->color;
    return (uint16_t)gba_ppu_pram(p, l->color);
}

static unsigned green6(uint16_t c) { return ((c >> 4) & 62u) | ((c >> 15) & 1u); }

static uint16_t pack(unsigned r, unsigned g6, unsigned b) { return (uint16_t)(r | (g6 >> 1) << 5 | b << 10); }

static uint16_t blend(uint16_t above, unsigned eva, uint16_t below, unsigned evb)
{
    unsigned r = min_u(31, ((above & 31u) * eva + (below & 31u) * evb + 8) >> 4);
    unsigned g = min_u(63, (green6(above) * eva + green6(below) * evb + 8) >> 4);
    unsigned b = min_u(31, (((above >> 10) & 31u) * eva + ((below >> 10) & 31u) * evb + 8) >> 4);
    return pack(r, g, b);
}

static uint16_t brighten(uint16_t c, unsigned evy)
{
    unsigned r = c & 31u, g = green6(c), b = (c >> 10) & 31u;
    return pack(r + (((31 - r) * evy + 8) >> 4), g + (((63 - g) * evy + 8) >> 4), b + (((31 - b) * evy + 8) >> 4));
}

static uint16_t darken(uint16_t c, unsigned evy)
{
    unsigned r = c & 31u, g = green6(c), b = (c >> 10) & 31u;
    return pack(r - ((r * evy + 7) >> 4), g - ((g * evy + 7) >> 4), b - ((b * evy + 7) >> 4));
}

void gba_ppu_dac_upper(gba_ppu *p, unsigned x)
{
    if (gba_ppu_blank(p)) {
        p->dac.color = 0x7FFF;
        p->dac.blending = false;
        p->pram_use[x] = 1;
        return;
    }

    static const uint8_t all_on[6] = {1, 1, 1, 1, 1, 1};
    const uint8_t *active = all_on;
    if (p->win[W_IN0].enable || p->win[W_IN1].enable || p->win[W_IN2].enable) {
        active = p->win[W_OUT].active;
        if (p->win[W_IN2].enable && p->win[W_IN2].output[x])
            active = p->win[W_IN2].active;
        if (p->win[W_IN1].enable && p->win[W_IN1].output[x])
            active = p->win[W_IN1].active;
        if (p->win[W_IN0].enable && p->win[W_IN0].output[x])
            active = p->win[W_IN0].active;
    }

    gba_pixel *layers = p->dac.layers;
    layers[L_OBJ] = p->obj.mosaic_latch;
    for (unsigned i = 0; i < 4; i++)
        layers[L_BG0 + i] = p->bg[i].mosaic_latch;
    layers[L_SFX] = (gba_pixel){.flags = PX_ON, .priority = 3, .color = 0};

    unsigned above_layer = 5, below_layer = 5, key_above = 3u * 8 + 5, key_below = 3u * 8 + 5;
    for (unsigned layer = 0; layer < 5; layer++) {
        if (!(layers[layer].flags & PX_ON) || !active[layer])
            continue;
        unsigned key = layers[layer].priority * 8u + layer;
        if (key < key_above) {
            key_below = key_above;
            below_layer = above_layer;
            key_above = key;
            above_layer = layer;
        } else if (key < key_below) {
            key_below = key;
            below_layer = layer;
        }
    }
    p->dac.above_layer = above_layer;
    p->dac.below_layer = below_layer;

    gba_pixel above = layers[above_layer];
    p->dac.color = lookup(p, &above);
    p->pram_use[x] = (above.flags & PX_DIRECT) ? 0 : 1;

    if ((above.flags & PX_TRANS) && p->dac.blend_below[below_layer]) {
        p->dac.blending = true;
        return;
    }
    if (active[L_SFX]) {
        unsigned evy = min_u(16, p->dac.evy);
        if (p->dac.blend_mode == 1 && p->dac.blend_above[above_layer] && p->dac.blend_below[below_layer]) {
            p->dac.blending = true;
            return;
        } else if (p->dac.blend_mode == 2 && p->dac.blend_above[above_layer]) {
            p->dac.color = brighten(p->dac.color, evy);
        } else if (p->dac.blend_mode == 3 && p->dac.blend_above[above_layer]) {
            p->dac.color = darken(p->dac.color, evy);
        }
    }
    p->dac.blending = false;
}

void gba_ppu_dac_lower(gba_ppu *p, unsigned x)
{
    if (p->dac.blending) {
        const gba_pixel *below = &p->dac.layers[p->dac.below_layer];
        if (!(below->flags & PX_DIRECT))
            p->pram_use[x] |= 2;
        p->dac.color = blend(p->dac.color, min_u(16, p->dac.eva), lookup(p, below), min_u(16, p->dac.evb));
    }
    p->line[x] = p->dac.color & 0x7FFFu;
}
