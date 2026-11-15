/**
 * @file lcd.c
 * @brief The display timing: VCOUNT and the DISPSTAT flags as functions of
 * the tick (1,232 cycles per line, 228 lines), and the line edges as
 * scheduler events: the VBlank, HBlank and VCOUNT match interrupts, the
 * VBlank and HBlank DMA starts and video capture DMA. Only edges that do
 * something are posted.
 *
 * Ported from ares (gba/display/display.cpp and io.cpp), reshaped from a
 * per-line thread into a position computed from the tick, which carries
 * this notice:
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
#include "gba_int.h"

enum { E_VBLANK = 1, E_VCOUNT = 2, E_CAPTURE = 5, E_HBLANK = 1008, E_HDMA = 1009 };

static const unsigned edges[] = {E_VBLANK, E_VCOUNT, E_CAPTURE, E_HBLANK, E_HDMA};

uint64_t gba_lcd_pos(const rl_gba *g, uint64_t t)
{
    uint64_t o = g->lcd.origin % RL_GBA_FRAME_CYCLES;
    return (t + RL_GBA_FRAME_CYCLES - o) % RL_GBA_FRAME_CYCLES;
}

void gba_lcd_reset(rl_gba *g)
{
    g->lcd.origin = 0;
    g->lcd.stat = 0;
    g->lcd.vcompare = 0;
    g->lcd.capture = false;
}

static bool has_dma(const rl_gba *g, unsigned t)
{
    for (unsigned i = 0; i < 4; i++)
        if ((g->dma[i].cnt_h & 0x8000) && ((g->dma[i].cnt_h >> 12) & 3) == t)
            return true;
    return false;
}

static bool relevant(const rl_gba *g, unsigned line, unsigned dot)
{
    const gba_lcd *l = &g->lcd;
    switch (dot) {
    case E_VBLANK:
        return line == 160 && ((l->stat & 0x08) || g->vblank_fn);
    case E_VCOUNT:
        return (line == 160 && has_dma(g, 1)) || ((l->stat & 0x20) && line == l->vcompare);
    case E_CAPTURE:
        if (line == 162)
            return l->capture || ((g->dma[3].cnt_h & 0xB000) == 0xB000);
        return l->capture && line >= 2 && line < 162;
    case E_HBLANK:
        return (l->stat & 0x10) != 0;
    default:
        return line < 160 && has_dma(g, 2);
    }
}

static void repost_from(rl_gba *g, uint64_t now)
{
    if (!g->sched)
        return;
    uint64_t p = gba_lcd_pos(g, now);
    unsigned line = (unsigned)(p / RL_GBA_LINE_CYCLES), dot = (unsigned)(p % RL_GBA_LINE_CYCLES);
    for (unsigned k = 0; k <= RL_GBA_LINES; k++) {
        unsigned ln = (line + k) % RL_GBA_LINES;
        for (unsigned e = 0; e < sizeof edges / sizeof edges[0]; e++) {
            if (k == 0 && edges[e] <= dot)
                continue;
            if (relevant(g, ln, edges[e])) {
                uint64_t when = now + (uint64_t)k * RL_GBA_LINE_CYCLES + edges[e] - dot;
                if (!rl_event_queued(&g->lcd.ev) || g->lcd.ev.when != when)
                    rl_sched_post(g->sched, &g->lcd.ev, when);
                return;
            }
        }
    }
    rl_sched_cancel(g->sched, &g->lcd.ev);
}

void gba_lcd_repost(rl_gba *g) { repost_from(g, gba_now(g)); }

static void fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    rl_gba *g = e->opaque;
    gba_lcd *l = &g->lcd;
    uint64_t p = gba_lcd_pos(g, when);
    unsigned line = (unsigned)(p / RL_GBA_LINE_CYCLES), dot = (unsigned)(p % RL_GBA_LINE_CYCLES);
    switch (dot) {
    case E_VBLANK:
        if (line == 160 && (l->stat & 0x08))
            gba_irq_set(g, RL_GBA_IRQ_VBLANK, when);
        if (line == 160 && g->vblank_fn)
            g->vblank_fn(g->vblank_user, line, when);
        break;
    case E_VCOUNT:
        if ((l->stat & 0x20) && line == l->vcompare)
            gba_irq_set(g, RL_GBA_IRQ_VCOUNT, when);
        if (line == 160)
            gba_dma_trigger(g, 1, when);
        break;
    case E_CAPTURE:
        if (line == 162) {
            if (l->capture)
                g->dma[3].cnt_h &= 0x7FFF;
            l->capture = !l->capture && (g->dma[3].cnt_h & 0xB000) == 0xB000;
        }
        if (line >= 2 && line < 162 && l->capture)
            gba_dma_hdma(g, when);
        break;
    case E_HBLANK:
        if (l->stat & 0x10)
            gba_irq_set(g, RL_GBA_IRQ_HBLANK, when);
        break;
    case E_HDMA:
        if (line < 160)
            gba_dma_trigger(g, 2, when);
        break;
    default:
        break;
    }
    repost_from(g, when);
}

void gba_lcd_attach(rl_gba *g)
{
    g->lcd.ev = (rl_event){.fire = fire, .opaque = g};
    g->lcd.origin = gba_now(g);
    gba_lcd_repost(g);
}

void rl_gba_lcd_set(rl_gba *g, unsigned line, unsigned dot)
{
    uint64_t now = gba_now(g);
    uint64_t p = ((uint64_t)(line % RL_GBA_LINES) * RL_GBA_LINE_CYCLES + dot % RL_GBA_LINE_CYCLES);
    g->lcd.origin = (now + RL_GBA_FRAME_CYCLES - p % RL_GBA_FRAME_CYCLES) % RL_GBA_FRAME_CYCLES;
    gba_lcd_repost(g);
    gba_ppu_dev_resync(g);
}

unsigned rl_gba_vcount(const rl_gba *g) { return (unsigned)(gba_lcd_pos(g, gba_now(g)) / RL_GBA_LINE_CYCLES); }

uint8_t gba_lcd_read(rl_gba *g, uint32_t addr, uint64_t now)
{
    uint64_t p = gba_lcd_pos(g, now);
    unsigned line = (unsigned)(p / RL_GBA_LINE_CYCLES), dot = (unsigned)(p % RL_GBA_LINE_CYCLES);
    switch (addr & 3) {
    case 0: {
        unsigned cmp = dot >= 1 ? line : (line + RL_GBA_LINES - 1) % RL_GBA_LINES;
        uint8_t v = g->lcd.stat & 0x38;
        if (line >= 160 && line <= 226)
            v |= 1;
        if (dot >= RL_GBA_HBLANK_DOT)
            v |= 2;
        if (cmp == g->lcd.vcompare)
            v |= 4;
        return v;
    }
    case 1:
        return g->lcd.vcompare;
    case 2:
        return (uint8_t)line;
    default:
        return 0;
    }
}

void gba_lcd_write(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now)
{
    (void)now;
    switch (addr & 3) {
    case 0:
        g->lcd.stat = v & 0x38;
        break;
    case 1:
        g->lcd.vcompare = v;
        break;
    default:
        return;
    }
    gba_lcd_repost(g);
}
