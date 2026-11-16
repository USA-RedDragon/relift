/**
 * @file timer.c
 * @brief The four timers of each CPU (GBATEK: the GBA's timers, clocked by
 * the 33.51 MHz bus): prescalers aligned to the bus clock, count-up
 * (cascade), overflow reload and IRQ, the one-cycle latch of control and
 * reload writes, and the reload one cycle after a start. The counters run
 * in bus cycles (two ticks).
 *
 * Ported from ares (gba/cpu/timer.cpp and the timer part of gba/cpu/io.cpp
 * and cpu.cpp) by way of relift's GBA layer (runtime/guests/gba/timer.c),
 * reshaped from a per-cycle step into a closed form: the counter is a
 * function of the bus cycle, and each overflow is a scheduler event. It
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
#include "nds_int.h"

static const unsigned shifts[4] = {0, 6, 8, 10};

static bool counts(const nds_timer *t) { return (t->ctl & 0x80) && !(t->ctl & 0x04); }

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

static uint32_t wrap(const nds_timer *t, uint64_t v)
{
    if (v <= 0xFFFF)
        return (uint32_t)v;
    uint64_t period = 0x10000u - t->reload;
    return (uint32_t)(t->reload + (v - 0x10000u) % period);
}

static uint32_t value_at(const nds_timer *t, uint64_t x)
{
    unsigned sh = shifts[t->ctl & 3];
    bool run = counts(t);
    if (t->load != NDS_NEVER && x > t->load)
        return wrap(t, t->reload + (run ? ticks(t->load + 1, x, sh) : 0));
    return wrap(t, t->val + (run ? ticks(t->at, x, sh) : 0));
}

static void settle(nds_timer *t, uint64_t x)
{
    if (x <= t->at)
        return;
    t->val = value_at(t, x);
    t->at = x;
    if (t->load != NDS_NEVER && t->load < x)
        t->load = NDS_NEVER;
}

static uint64_t next_overflow(const nds_timer *t)
{
    if (!counts(t))
        return NDS_NEVER;
    unsigned sh = shifts[t->ctl & 3];
    uint64_t k = 0x10000u - t->val;
    if (t->load != NDS_NEVER) {
        if (ticks(t->at, t->load + 1, sh) >= k)
            return kth(t->at, k, sh);
        return kth(t->load + 1, 0x10000u - t->reload, sh);
    }
    return kth(t->at, k, sh);
}

static void post(rl_nds *m, nds_timer *t)
{
    if (!m->sched)
        return;
    uint64_t c = next_overflow(t);
    if (c == NDS_NEVER)
        rl_sched_cancel(m->sched, &t->ev);
    else if (!rl_event_queued(&t->ev) || t->ev.when != 2 * (c + 1))
        rl_sched_post(m->sched, &t->ev, 2 * (c + 1));
}

static void overflow(rl_nds *m, nds_timer *t, uint64_t c);

static void cascade(rl_nds *m, nds_timer *u, uint64_t c)
{
    if ((u->ctl & 0x84) != 0x84)
        return;
    if (u->load != NDS_NEVER && u->load < c) {
        u->val = u->reload;
        u->load = NDS_NEVER;
    }
    u->at = c + 1;
    if (++u->val == 0x10000u)
        overflow(m, u, c);
    if (u->load == c) {
        u->val = u->reload;
        u->load = NDS_NEVER;
    }
}

static void overflow(rl_nds *m, nds_timer *t, uint64_t c)
{
    t->val = t->reload;
    t->at = c + 1;
    if (t->load != NDS_NEVER && t->load <= c)
        t->load = NDS_NEVER;
    if (t->ctl & 0x40)
        nds_irq_set(m, t->cpu, RL_NDS_IRQ_TIMER0 << t->n, 2 * (c + 1));
    if (t->n < 3)
        cascade(m, t + 1, c);
    post(m, t);
}

static void fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    nds_timer *t = e->opaque;
    overflow(t->m, t, when / 2 - 1);
}

static void flush(rl_nds *m, unsigned cpu, uint64_t now)
{
    for (unsigned n = 0; n < 4; n++) {
        nds_timer *t = &m->tm[cpu][n];
        while (rl_event_queued(&t->ev) && t->ev.when / 2 <= now + 1) {
            uint64_t when = t->ev.when;
            rl_sched_cancel(m->sched, &t->ev);
            overflow(m, t, when / 2 - 1);
        }
    }
}

void nds_timer_reset(rl_nds *m)
{
    for (unsigned cpu = 0; cpu < 2; cpu++)
        for (unsigned n = 0; n < 4; n++) {
            nds_timer *t = &m->tm[cpu][n];
            *t = (nds_timer){.load = NDS_NEVER, .m = m, .cpu = (uint8_t)cpu, .n = (uint8_t)n};
            t->ev = (rl_event){.fire = fire, .opaque = t};
        }
}

void nds_timer_attach(rl_nds *m)
{
    for (unsigned cpu = 0; cpu < 2; cpu++)
        for (unsigned n = 0; n < 4; n++)
            m->tm[cpu][n].at = nds_now(m, cpu) / 2;
}

uint32_t nds_timer_read(rl_nds *m, unsigned cpu, uint32_t off, uint64_t now)
{
    nds_timer *t = &m->tm[cpu][(off >> 2) & 3];
    return value_at(t, now / 2) | (uint32_t)(t->ctl & 0xC7) << 16;
}

static void write_byte(rl_nds *m, nds_timer *t, unsigned lane, uint8_t v, uint64_t now)
{
    if (m->sched)
        flush(m, t->cpu, now);
    settle(t, now + 1);
    switch (lane) {
    case 0:
        t->reload = (uint16_t)((t->reload & 0xFF00) | v);
        break;
    case 1:
        t->reload = (uint16_t)((t->reload & 0x00FF) | v << 8);
        break;
    case 2: {
        bool was = t->ctl & 0x80;
        t->ctl = (uint8_t)(v & (t->n ? 0xC7 : 0xC3));
        if (!was && (t->ctl & 0x80))
            t->load = now + 1;
        break;
    }
    default:
        return;
    }
    post(m, t);
}

void nds_timer_write(rl_nds *m, unsigned cpu, uint32_t off, uint32_t v, uint32_t mask, uint64_t now)
{
    nds_timer *t = &m->tm[cpu][(off >> 2) & 3];
    for (unsigned lane = 0; lane < 3; lane++)
        if (mask >> (8 * lane) & 0xFF)
            write_byte(m, t, lane, (uint8_t)(v >> (8 * lane)), now / 2);
}
