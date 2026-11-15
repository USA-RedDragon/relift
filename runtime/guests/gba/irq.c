/**
 * @file irq.c
 * @brief The interrupt controller: IE, IF and IME with the console's
 * pipeline (a written or requested value reaches the registers' read side
 * one cycle later and the CPU's IRQ input two cycles later), the halt wake
 * line, and HALTCNT's halt and stop.
 *
 * Ported from ares (gba/cpu/cpu.cpp: stepIRQ, setInterruptFlag and the halt
 * loop of main; gba/cpu/io.cpp: IE, IF and IME), reshaped from a per-cycle
 * step into a log of register states with the tick each became current,
 * which carries this notice:
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

#include <string.h>

#define STOP_SOURCES (RL_GBA_IRQ_SERIAL | RL_GBA_IRQ_KEYPAD | RL_GBA_IRQ_CART)

void gba_irq_reset(rl_gba *g)
{
    memset(&g->irq.log, 0, sizeof g->irq.log);
    g->irq.n = 1;
    g->irq.line = false;
    g->irq.wake = false;
    g->irq.wake_mask = 0x3FFF;
}

gba_irq_state gba_irq_at(const rl_gba *g, uint64_t t)
{
    const gba_irq *q = &g->irq;
    unsigned i = q->n - 1;
    while (i > 0 && q->log[i].tau > t)
        i--;
    return q->log[i];
}

static bool line_of(gba_irq_state s) { return s.ime && (s.ie & s.iflag & 0x3FFF); }

static void prune(gba_irq *q, uint64_t now)
{
    unsigned keep = 0;
    while (keep + 1 < q->n && q->log[keep + 1].tau + 2 <= now)
        keep++;
    if (keep) {
        memmove(&q->log[0], &q->log[keep], (q->n - keep) * sizeof q->log[0]);
        q->n -= keep;
    }
}

static void set_line(rl_gba *g, unsigned line, bool on)
{
    if (!g->sched)
        return;
    if (on)
        rl_sched_raise(g->sched, line);
    else
        rl_sched_lower(g->sched, line);
}

static void repost(rl_gba *g, uint64_t now)
{
    gba_irq *q = &g->irq;
    if (!g->sched)
        return;
    uint64_t next = UINT64_MAX;
    for (unsigned i = 0; i < q->n; i++) {
        uint64_t t = q->log[i].tau;
        if (t + 1 > now && t + 1 < next)
            next = t + 1;
        else if (t + 2 > now && t + 2 < next)
            next = t + 2;
    }
    if (next == UINT64_MAX)
        rl_sched_cancel(g->sched, &q->ev);
    else if (!rl_event_queued(&q->ev) || q->ev.when != next)
        rl_sched_post(g->sched, &q->ev, next);
}

static void update(rl_gba *g, uint64_t x)
{
    gba_irq *q = &g->irq;
    bool line = x >= 2 && line_of(gba_irq_at(g, x - 2));
    gba_irq_state w = gba_irq_at(g, x ? x - 1 : 0);
    bool wake = (w.ie & w.iflag & q->wake_mask) != 0;
    if (line != q->line) {
        q->line = line;
        if (line)
            q->rise = x;
        set_line(g, RL_GBA_LINE_IRQ, line);
    }
    if (wake != q->wake) {
        q->wake = wake;
        set_line(g, RL_GBA_LINE_WAKE, wake);
    }
}

static void fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    rl_gba *g = e->opaque;
    update(g, when);
    g->irq.done = when;
    prune(&g->irq, when);
    repost(g, when);
}

typedef enum op_kind { OP_SET, OP_ACK, OP_IE, OP_IME } op_kind;

static void apply(rl_gba *g, op_kind op, uint16_t v, uint64_t tau)
{
    gba_irq *q = &g->irq;
    prune(q, q->done);
    unsigned i = q->n;
    while (i > 0 && q->log[i - 1].tau > tau)
        i--;
    if (i == 0 || q->log[i - 1].tau != tau) {
        if (q->n == GBA_IRQ_LOG) {
            memmove(&q->log[0], &q->log[1], (q->n - 1) * sizeof q->log[0]);
            q->n--;
            if (i)
                i--;
        }
        memmove(&q->log[i + 1], &q->log[i], (q->n - i) * sizeof q->log[0]);
        q->log[i] = q->log[i ? i - 1 : 1];
        q->log[i].tau = tau;
        q->n++;
    } else {
        i--;
    }
    for (unsigned k = i; k < q->n; k++) {
        gba_irq_state *s = &q->log[k];
        switch (op) {
        case OP_SET:
            s->iflag |= v;
            break;
        case OP_ACK:
            s->iflag &= (uint16_t)~v;
            break;
        case OP_IE:
            if (k == i)
                s->ie = v;
            break;
        case OP_IME:
            if (k == i)
                s->ime = v != 0;
            break;
        }
    }
    repost(g, g->irq.done);
}

void gba_irq_set(rl_gba *g, uint16_t bits, uint64_t tau) { apply(g, OP_SET, bits & 0x3FFF, tau); }
void gba_irq_ack(rl_gba *g, uint16_t bits, uint64_t tau) { apply(g, OP_ACK, bits, tau); }
void gba_irq_ie(rl_gba *g, uint16_t ie, uint64_t tau) { apply(g, OP_IE, ie, tau); }
void gba_irq_ime(rl_gba *g, bool ime, uint64_t tau) { apply(g, OP_IME, ime, tau); }

bool gba_irq_wants(const rl_gba *g)
{
    const gba_irq *q = &g->irq;
    return line_of(q->log[q->n - 1]);
}

void gba_irq_attach(rl_gba *g)
{
    g->irq.ev = (rl_event){.fire = fire, .opaque = g};
    g->irq.done = gba_now(g);
    repost(g, g->irq.done);
}

bool rl_gba_irq_line(const rl_gba *g) { return g->irq.line; }

bool rl_gba_irq_sample(rl_gba *g)
{
    if (g->cpu)
        rl_sched_run(g->cpu);
    return g->irq.line && g->irq.rise <= g->acc_start;
}

void rl_gba_request_irq(rl_gba *g, uint16_t sources) { gba_irq_set(g, sources, gba_now(g)); }

void gba_halt(rl_gba *g)
{
    bool stop = g->stop_req;
    g->halt_req = false;
    g->stop_req = false;
    rl_sched_cpu *c = g->cpu;
    if (!c)
        return;
    gba_irq *q = &g->irq;
    q->wake_mask = stop ? STOP_SOURCES : 0x3FFF;
    update(g, rl_sched_now(c));
    repost(g, g->irq.done);
    g->halted = true;
    rl_sched_set_mask(c, (1ull << RL_GBA_LINE_WAKE) | (1ull << RL_GBA_LINE_DMA));
    for (;;) {
        if (q->wake)
            break;
        if (gba_dma_due(g)) {
            gba_dma_run(g);
            continue;
        }
        if (!rl_sched_halt(c))
            break;
    }
    g->halted = false;
    q->wake_mask = 0x3FFF;
    rl_sched_set_mask(c, (1ull << RL_GBA_LINE_IRQ) | (1ull << RL_GBA_LINE_DMA));
    if (q->wake)
        gba_charge(g, 2);
}
