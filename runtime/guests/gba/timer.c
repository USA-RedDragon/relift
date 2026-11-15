/**
 * @file timer.c
 * @brief The four timers: prescalers aligned to the system clock, count-up
 * (cascade), overflow reload and IRQ, the one-cycle latch of control and
 * reload writes, and the reload one cycle after a start.
 *
 * Ported from ares (gba/cpu/timer.cpp and the timer part of gba/cpu/io.cpp
 * and cpu.cpp), reshaped from a per-cycle step into a closed form: the
 * counter is a function of the tick, and each overflow is a scheduler
 * event. It carries this notice:
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

static const unsigned shifts[4] = {0, 6, 8, 10};

static bool counts(const gba_timer *t) { return (t->ctl & 0x80) && !(t->ctl & 0x04); }

static uint64_t ticks(uint64_t a, uint64_t b, unsigned shift)
{
    if (b <= a)
        return 0;
    uint64_t m = (1ull << shift) - 1;
    return ((b + m) >> shift) - ((a + m) >> shift);
}

static uint64_t kth(uint64_t a, uint64_t k, unsigned shift)
{
    uint64_t m = (1ull << shift) - 1;
    return ((a + m) & ~m) + ((k - 1) << shift);
}

static uint32_t wrap(const gba_timer *t, uint64_t v)
{
    if (v <= 0xFFFF)
        return (uint32_t)v;
    uint64_t period = 0x10000u - t->reload;
    return (uint32_t)(t->reload + (v - 0x10000u) % period);
}

static uint32_t value_at(const gba_timer *t, uint64_t x)
{
    unsigned sh = shifts[t->ctl & 3];
    bool run = counts(t);
    if (t->load != GBA_NONE && x > t->load)
        return wrap(t, t->reload + (run ? ticks(t->load + 1, x, sh) : 0));
    return wrap(t, t->val + (run ? ticks(t->at, x, sh) : 0));
}

static void settle(gba_timer *t, uint64_t x)
{
    if (x <= t->at)
        return;
    t->val = value_at(t, x);
    t->at = x;
    if (t->load != GBA_NONE && t->load < x)
        t->load = GBA_NONE;
}

static uint64_t next_overflow(const gba_timer *t)
{
    if (!counts(t))
        return GBA_NONE;
    unsigned sh = shifts[t->ctl & 3];
    uint64_t k = 0x10000u - t->val;
    if (t->load != GBA_NONE) {
        if (ticks(t->at, t->load + 1, sh) >= k)
            return kth(t->at, k, sh);
        return kth(t->load + 1, 0x10000u - t->reload, sh);
    }
    return kth(t->at, k, sh);
}

static void post(rl_gba *g, unsigned n)
{
    gba_timer *t = &g->tm[n];
    if (!g->sched)
        return;
    uint64_t c = next_overflow(t);
    if (c == GBA_NONE)
        rl_sched_cancel(g->sched, &t->ev);
    else if (!rl_event_queued(&t->ev) || t->ev.when != c + 1)
        rl_sched_post(g->sched, &t->ev, c + 1);
}

static void overflow(rl_gba *g, unsigned n, uint64_t c);

static void cascade(rl_gba *g, unsigned n, uint64_t c)
{
    gba_timer *u = &g->tm[n];
    if ((u->ctl & 0x84) != 0x84)
        return;
    if (u->load != GBA_NONE && u->load < c) {
        u->val = u->reload;
        u->load = GBA_NONE;
    }
    u->at = c + 1;
    if (++u->val == 0x10000u)
        overflow(g, n, c);
    if (u->load == c) {
        u->val = u->reload;
        u->load = GBA_NONE;
    }
}

static void overflow(rl_gba *g, unsigned n, uint64_t c)
{
    gba_timer *t = &g->tm[n];
    t->val = t->reload;
    t->at = c + 1;
    if (t->load != GBA_NONE && t->load <= c)
        t->load = GBA_NONE;
    if (t->ctl & 0x40)
        gba_irq_set(g, (uint16_t)(RL_GBA_IRQ_TIMER0 << n), c + 1);
    if (g->timer_fn)
        g->timer_fn(g->timer_user, n, c + 1);
    if (n < 3)
        cascade(g, n + 1, c);
    post(g, n);
}

static void fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    rl_gba *g = e->opaque;
    unsigned n;
    for (n = 0; n < 3; n++)
        if (&g->tm[n].ev == e)
            break;
    overflow(g, n, when - 1);
}

static void flush(rl_gba *g, uint64_t now)
{
    for (unsigned n = 0; n < 4; n++) {
        gba_timer *t = &g->tm[n];
        while (rl_event_queued(&t->ev) && t->ev.when <= now + 1) {
            uint64_t when = t->ev.when;
            rl_sched_cancel(g->sched, &t->ev);
            overflow(g, n, when - 1);
        }
    }
}

void gba_timer_reset(rl_gba *g)
{
    for (unsigned n = 0; n < 4; n++) {
        gba_timer *t = &g->tm[n];
        t->val = 0;
        t->at = 0;
        t->load = GBA_NONE;
        t->reload = 0;
        t->ctl = 0;
    }
}

void gba_timer_attach(rl_gba *g)
{
    uint64_t now = gba_now(g);
    for (unsigned n = 0; n < 4; n++) {
        g->tm[n].ev = (rl_event){.fire = fire, .dev = &g->d_timer.dev, .opaque = g};
        g->tm[n].at = now;
    }
}

uint8_t gba_timer_read(rl_gba *g, uint32_t addr, uint64_t now)
{
    gba_timer *t = &g->tm[(addr >> 2) & 3];
    switch (addr & 3) {
    case 0:
        return (uint8_t)value_at(t, now);
    case 1:
        return (uint8_t)(value_at(t, now) >> 8);
    case 2:
        return t->ctl & 0xC7;
    default:
        return 0;
    }
}

void gba_timer_write(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now)
{
    unsigned n = (addr >> 2) & 3;
    gba_timer *t = &g->tm[n];
    flush(g, now);
    settle(t, now + 1);
    switch (addr & 3) {
    case 0:
        t->reload = (uint16_t)((t->reload & 0xFF00) | v);
        break;
    case 1:
        t->reload = (uint16_t)((t->reload & 0x00FF) | v << 8);
        break;
    case 2: {
        bool was = t->ctl & 0x80;
        t->ctl = (uint8_t)(v & (n ? 0xC7 : 0xC3));
        if (!was && (t->ctl & 0x80))
            t->load = now + 1;
        break;
    }
    default:
        return;
    }
    post(g, n);
}
