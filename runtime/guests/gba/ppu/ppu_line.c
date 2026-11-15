/**
 * @file ppu_line.c
 * @brief One line: the line start (enable pipeline, reference points, mosaic rows) and the render in the order the fast path of ares draws it.
 *
 * Ported from ares (gba/ppu/ppu.cpp), which carries this notice:
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

void gba_ppu_line_start(gba_ppu *p, unsigned y)
{
    if (y == 0) {
        p->frames++;
        for (unsigned i = 2; i < 4; i++) {
            p->bg[i].lx = p->bg[i].x;
            p->bg[i].ly = p->bg[i].y;
        }
    }
    memmove(p->force_blank, p->force_blank + 1, sizeof p->force_blank - 1);
    for (unsigned i = 0; i < 4; i++)
        memmove(p->bg[i].enable, p->bg[i].enable + 1, sizeof p->bg[i].enable - 1);
    memmove(p->obj.enable, p->obj.enable + 1, sizeof p->obj.enable - 1);
    for (unsigned i = 0; i < 4; i++)
        gba_ppu_bg_scanline(p, &p->bg[i], y);
    gba_ppu_win_scanline(&p->win[W_IN0], y);
    gba_ppu_win_scanline(&p->win[W_IN1], y);
    p->line = y < GBA_PPU_H ? p->pixels + (size_t)y * GBA_PPU_W : p->pixels;
}

void gba_ppu_render(gba_ppu *p, unsigned y)
{
    gba_ppu_obj_render_scanline(p, (y + 1) % GBA_PPU_LINES);
    if (y >= GBA_PPU_H)
        return;
    for (unsigned i = 0; i < 4; i++)
        gba_ppu_bg_run_line(p, &p->bg[i]);
    for (unsigned x = 0; x < 256; x++) {
        gba_ppu_win_run(&p->win[W_IN0], x);
        gba_ppu_win_run(&p->win[W_IN1], x);
    }
    p->obj.hmosaic_offset = p->obj.mosaic_w;
    for (unsigned x = 0; x < GBA_PPU_W; x++) {
        for (unsigned i = 0; i < 4; i++)
            gba_ppu_bg_output(p, &p->bg[i], x);
        gba_ppu_obj_output(p, x, y);
        p->win[W_IN2].output[x] = (p->obj.output.flags & PX_WIN) != 0;
        gba_ppu_dac_upper(p, x);
        gba_ppu_dac_lower(p, x);
    }
}
