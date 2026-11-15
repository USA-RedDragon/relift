/**
 * @file ppu_dac.c
 * @brief The windows, the layer priorities and the blend, brighten and darken effects.
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

static uint16_t blend(uint16_t above, unsigned eva, uint16_t below, unsigned evb)
{
    unsigned ar = above & 31, ag = (above >> 5) & 31, ab = (above >> 10) & 31;
    unsigned br = below & 31, bg = (below >> 5) & 31, bb = (below >> 10) & 31;
    unsigned r = (ar * eva + br * evb) >> 4;
    unsigned g = (ag * eva + bg * evb) >> 4;
    unsigned b = (ab * eva + bb * evb) >> 4;
    return (uint16_t)(min_u(31, r) | min_u(31, g) << 5 | min_u(31, b) << 10);
}

void gba_ppu_dac_upper(gba_ppu *p, unsigned x)
{
    if (gba_ppu_blank(p)) {
        p->dac.color = 0x7FFF;
        p->dac.blending = false;
        return;
    }

    uint8_t active[6] = {1, 1, 1, 1, 1, 1};
    if (p->win[W_IN0].enable || p->win[W_IN1].enable || p->win[W_IN2].enable) {
        memcpy(active, p->win[W_OUT].active, sizeof active);
        if (p->win[W_IN2].enable && p->win[W_IN2].output[x])
            memcpy(active, p->win[W_IN2].active, sizeof active);
        if (p->win[W_IN1].enable && p->win[W_IN1].output[x])
            memcpy(active, p->win[W_IN1].active, sizeof active);
        if (p->win[W_IN0].enable && p->win[W_IN0].output[x])
            memcpy(active, p->win[W_IN0].active, sizeof active);
    }

    gba_pixel *layers = p->dac.layers;
    layers[L_OBJ] = p->obj.mosaic_latch;
    for (unsigned i = 0; i < 4; i++)
        layers[L_BG0 + i] = p->bg[i].mosaic_latch;
    layers[L_SFX] = (gba_pixel){.flags = PX_ON, .priority = 3, .color = 0};

    unsigned above_layer = 5, below_layer = 5;
    for (int priority = 3; priority >= 0; priority--)
        for (int layer = 5; layer >= 0; layer--)
            if ((layers[layer].flags & PX_ON) && layers[layer].priority == priority && active[layer]) {
                below_layer = above_layer;
                above_layer = (unsigned)layer;
            }
    p->dac.above_layer = above_layer;
    p->dac.below_layer = below_layer;

    gba_pixel above = layers[above_layer];
    p->dac.color = lookup(p, &above);

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
            p->dac.color = blend(p->dac.color, 16 - evy, 0x7FFF, evy);
        } else if (p->dac.blend_mode == 3 && p->dac.blend_above[above_layer]) {
            p->dac.color = blend(p->dac.color, 16 - evy, 0x0000, evy);
        }
    }
    p->dac.blending = false;
}

void gba_ppu_dac_lower(gba_ppu *p, unsigned x)
{
    if (p->dac.blending) {
        const gba_pixel *below = &p->dac.layers[p->dac.below_layer];
        p->dac.color = blend(p->dac.color, min_u(16, p->dac.eva), lookup(p, below), min_u(16, p->dac.evb));
    }
    p->line[x] = p->dac.color;
}
