/**
 * @file dma.c
 * @brief The four DMA channels: registers and their latches, immediate,
 * VBlank, HBlank and special starts (sound FIFO, video capture), the
 * two-cycle start delay, priority by channel number, the Game Pak burst of
 * one channel's transfer, repeat and reload, the DMA's own open-bus latch
 * and the end-of-transfer IRQ. The CPU waits while a channel runs: the
 * transfer's accesses are charged to its clock.
 *
 * Ported from ares (gba/cpu/dma.cpp and the DMA part of gba/cpu/io.cpp and
 * timer.cpp), which carries this notice:
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

static const uint32_t src_mask[4] = {0x07FFFFFFu, 0x0FFFFFFFu, 0x0FFFFFFFu, 0x0FFFFFFFu};
static const uint32_t dst_mask[4] = {0x07FFFFFFu, 0x07FFFFFFu, 0x07FFFFFFu, 0x0FFFFFFFu};
static const uint32_t len_mask[4] = {0x3FFFu, 0x3FFFu, 0x3FFFu, 0xFFFFu};

static unsigned dmode(const gba_dma_ch *c) { return (c->cnt_h >> 5) & 3; }
static unsigned smode(const gba_dma_ch *c) { return (c->cnt_h >> 7) & 3; }
static unsigned timing(const gba_dma_ch *c) { return (c->cnt_h >> 12) & 3; }
static bool enabled(const gba_dma_ch *c) { return (c->cnt_h & 0x8000) != 0; }

void gba_dma_reset(rl_gba *g)
{
    for (unsigned i = 0; i < 4; i++) {
        gba_dma_ch *c = &g->dma[i];
        c->sad = c->dad = 0;
        c->cnt_l = c->cnt_h = 0;
        c->src = c->dst = c->len = c->data = 0;
        c->active = false;
        c->ready = 0;
    }
    g->dma_rom_burst = false;
    g->dma_last = 0;
}

static void ready_fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)when;
    rl_sched_raise(s, RL_GBA_LINE_DMA);
    (void)e;
}

void gba_dma_attach(rl_gba *g)
{
    for (unsigned i = 0; i < 4; i++)
        g->dma[i].ev = (rl_event){.fire = ready_fire, .opaque = g};
}

static void arm(rl_gba *g, unsigned i, uint64_t ready)
{
    gba_dma_ch *c = &g->dma[i];
    c->active = true;
    c->ready = ready;
    if (!g->sched)
        return;
    if (ready <= gba_now(g)) {
        rl_sched_cancel(g->sched, &c->ev);
        rl_sched_raise(g->sched, RL_GBA_LINE_DMA);
    } else {
        rl_sched_post(g->sched, &c->ev, ready);
    }
}

bool gba_dma_quiet(const rl_gba *g, unsigned cycles)
{
    for (unsigned i = 0; i < 4; i++)
        if (g->dma[i].active)
            return false;
    return !g->cpu || rl_sched_clock(g->cpu)->budget > (int64_t)cycles;
}

bool gba_dma_due(const rl_gba *g)
{
    uint64_t now = gba_now(g);
    for (unsigned i = 0; i < 4; i++)
        if (g->dma[i].active && g->dma[i].ready <= now)
            return true;
    return false;
}

static int pick(const rl_gba *g)
{
    uint64_t now = gba_now(g);
    for (int i = 0; i < 4; i++)
        if (g->dma[i].active && g->dma[i].ready <= now)
            return i;
    return -1;
}

static void unit(rl_gba *g, unsigned i)
{
    gba_dma_ch *c = &g->dma[i];
    bool word = (c->cnt_h & 0x0400) != 0;
    unsigned seek = word ? 4 : 2, size = seek;
    if (g->dma_last != (int)i) {
        g->dma_rom_burst = false;
        g->dma_last = (int)i;
    }
    uint32_t s = c->src & src_mask[i];
    if (s < 0x02000000u) {
        gba_pf_step(g, 1);
    } else {
        uint32_t v = gba_bus_read(g, GBA_DMA, s & ~(size - 1), size, true);
        c->data = word ? v : (v & 0xFFFFu) * 0x10001u;
    }
    switch (smode(c)) {
    case 0:
        c->src += seek;
        break;
    case 1:
        c->src -= seek;
        break;
    default:
        break;
    }
    uint32_t d = c->dst & dst_mask[i];
    if (d < 0x02000000u) {
        gba_pf_step(g, 1);
    } else {
        uint32_t a = d & ~(size - 1);
        gba_bus_write(g, GBA_DMA, a, size, word ? c->data : c->data >> ((a & 2) * 8), true);
    }
    switch (dmode(c)) {
    case 0:
    case 3:
        c->dst += seek;
        break;
    case 1:
        c->dst -= seek;
        break;
    default:
        break;
    }
    c->len--;
    if ((c->len & len_mask[i]) == 0) {
        c->active = false;
        if (dmode(c) == 3)
            c->dst = c->dad;
        if (!(c->cnt_h & 0x0200) || timing(c) == 0)
            c->cnt_h &= 0x7FFF;
        else
            c->len = c->cnt_l;
        uint64_t now = gba_now(g);
        if (c->cnt_h & 0x4000)
            gba_irq_set(g, (uint16_t)(RL_GBA_IRQ_DMA0 << i), now);
        if (i == 3 && (c->cnt_h & 0x0800))
            gba_irq_set(g, RL_GBA_IRQ_CART, now);
        if (timing(c) == 1 || timing(c) == 2)
            gba_lcd_repost(g);
    }
}

void gba_dma_run(rl_gba *g)
{
    if (g->dma_running || !g->cpu)
        return;
    if (rl_sched_clock(g->cpu)->budget <= 0)
        rl_sched_run(g->cpu);
    int i = pick(g);
    if (i < 0)
        return;
    g->dma_running = true;
    gba_pf_step(g, 1);
    while (i >= 0) {
        unit(g, (unsigned)i);
        rl_sched_run(g->cpu);
        i = pick(g);
    }
    gba_pf_step(g, 1);
    g->dma_rom_burst = false;
    g->rom_burst = false;
    g->dma_last = 0;
    g->dma_running = false;
    rl_sched_lower(g->sched, RL_GBA_LINE_DMA);
    for (unsigned k = 0; k < 4; k++)
        if (g->dma[k].active && !rl_event_queued(&g->dma[k].ev))
            arm(g, k, g->dma[k].ready);
}

void gba_dma_trigger(rl_gba *g, unsigned t, uint64_t now)
{
    for (unsigned i = 0; i < 4; i++)
        if (enabled(&g->dma[i]) && timing(&g->dma[i]) == t)
            arm(g, i, now);
}

void gba_dma_hdma(rl_gba *g, uint64_t now)
{
    gba_dma_ch *c = &g->dma[3];
    if (enabled(c) && timing(c) == 3)
        arm(g, 3, now);
}

void gba_dma_fifo_at(rl_gba *g, unsigned fifo, uint64_t when)
{
    if (fifo > 1)
        return;
    unsigned i = 1 + fifo;
    gba_dma_ch *c = &g->dma[i];
    if (!enabled(c) || timing(c) != 3)
        return;
    c->cnt_h = (uint16_t)((c->cnt_h & ~0x0060u) | 0x0040u | 0x0400u);
    c->len = 4;
    arm(g, i, when + 2);
}

void rl_gba_dma_fifo(rl_gba *g, unsigned fifo) { gba_dma_fifo_at(g, fifo, gba_now(g)); }

uint8_t gba_dma_read(rl_gba *g, uint32_t addr)
{
    uint32_t off = (addr & 0xFFu) - 0xB0u;
    gba_dma_ch *c = &g->dma[off / 12];
    switch (off % 12) {
    case 8:
    case 9:
        return 0;
    case 10:
        return (uint8_t)(c->cnt_h & 0xE0);
    case 11:
        return (uint8_t)(c->cnt_h >> 8);
    default:
        return gba_open_byte(g, addr);
    }
}

void gba_dma_write(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now)
{
    uint32_t off = (addr & 0xFFu) - 0xB0u;
    unsigned i = off / 12, r = off % 12;
    gba_dma_ch *c = &g->dma[i];
    if (r < 4) {
        c->sad = (c->sad & ~(0xFFu << (8 * r))) | (uint32_t)v << (8 * r);
        return;
    }
    if (r < 8) {
        c->dad = (c->dad & ~(0xFFu << (8 * (r - 4)))) | (uint32_t)v << (8 * (r - 4));
        return;
    }
    if (r == 8) {
        c->cnt_l = (uint16_t)((c->cnt_l & 0xFF00) | v);
        return;
    }
    if (r == 9) {
        c->cnt_l = (uint16_t)((c->cnt_l & 0x00FF) | v << 8);
        return;
    }
    if (r == 10) {
        c->cnt_h = (uint16_t)((c->cnt_h & 0xFF00) | (v & 0xE0));
        return;
    }
    bool was = enabled(c);
    if (i != 3)
        v &= 0xF7;
    c->cnt_h = (uint16_t)((c->cnt_h & 0x00FF) | v << 8);
    if (!was && enabled(c)) {
        c->src = c->sad & src_mask[i];
        c->dst = c->dad & dst_mask[i];
        c->len = c->cnt_l & len_mask[i];
        if (i == 3)
            gba_eeprom_hint(g, c->dst, c->len ? c->len : len_mask[i] + 1);
        if (timing(c) == 0)
            arm(g, i, now + 2);
    } else if (!enabled(c)) {
        c->active = false;
        if (g->sched) {
            rl_sched_cancel(g->sched, &c->ev);
            if (!gba_dma_due(g))
                rl_sched_lower(g->sched, RL_GBA_LINE_DMA);
        }
    }
    gba_lcd_repost(g);
}
