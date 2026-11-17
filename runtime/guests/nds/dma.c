/**
 * @file dma.c
 * @brief The four DMA channels of each CPU: the registers (read back as
 * written; the address and count masks apply when a transfer starts), the
 * ARM9's fill registers, the start conditions (immediate
 * after a start delay, or a trigger from another device), priority by channel
 * number, repeat and reload, the end-of-transfer IRQ, and the time a transfer
 * takes on the shared bus. A CPU that touches the bus while one of its
 * channels may run waits until the transfer is done; an ARM9 that runs from
 * its TCMs does not.
 *
 * Fresh C from GBATEK ("DS DMA Transfers", "DS Memory Timings"). The costs of
 * a unit are the ARM7's code-fetch costs of the two regions (what a bus
 * master without a CPU's penalties pays), a nonsequential first unit, no
 * sequential accesses when both sides are in main RAM, and one more bus
 * cycle when both sides are in one memory other than main RAM; the numbers
 * agree with the oracle's per-unit costs (tests/test_dma.c).
 */
#include "nds_int.h"

enum {
    /** @brief Ticks of a transfer that run in one slice before other events may run (a few dozen units). */
    SLICE = 64,
    /** @brief Ticks from the enabling write of an immediate transfer to its first access. */
    START_DELAY = 2,
    /** @brief Units a GXFIFO start moves (GBATEK \"GXFIFO Access via DMA\"). */
    GX_BURST = 112,
};

static const uint32_t len_mask9 = 0x1FFFFFu;

static uint32_t len_mask(unsigned cpu, unsigned i)
{
    if (cpu == RL_NDS_ARM9)
        return len_mask9;
    return i == 3 ? 0xFFFFu : 0x3FFFu;
}

static uint32_t src_mask(unsigned cpu, unsigned i) { return cpu == RL_NDS_ARM7 && i == 0 ? 0x07FFFFFFu : 0x0FFFFFFFu; }
static uint32_t dst_mask(unsigned cpu, unsigned i) { return cpu == RL_NDS_ARM7 && i != 3 ? 0x07FFFFFFu : 0x0FFFFFFFu; }

static unsigned timing_of(unsigned cpu, unsigned i, uint32_t cnt)
{
    if (cpu == RL_NDS_ARM9)
        return (cnt >> 27) & 7u;
    unsigned mode = (cnt >> 28) & 3u;
    return mode == 3 && (i & 1u) ? NDS_DMA7_GBA_SLOT : mode;
}

void nds_dma_reset(rl_nds *m)
{
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        nds_dma *d = &m->dma[cpu];
        *d = (nds_dma){.m = m, .cpu = (uint8_t)cpu};
    }
}

static void slice(rl_nds *m, nds_dma *d, uint64_t t);

static void fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    nds_dma *d = e->opaque;
    slice(d->m, d, when > d->at ? when : d->at);
}

void nds_dma_attach(rl_nds *m)
{
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        nds_dma *d = &m->dma[cpu];
        d->ev = (rl_event){.fire = fire, .opaque = d};
        d->at = nds_now(m, cpu);
    }
}

/** @brief Queues the engine's next slice at the earliest tick an armed channel can run, or cancels it. */
static void repost(rl_nds *m, nds_dma *d)
{
    if (!m->sched)
        return;
    uint64_t next = NDS_NEVER;
    for (unsigned i = 0; i < 4; i++) {
        const nds_dma_ch *c = &d->ch[i];
        if (!c->active)
            continue;
        uint64_t at = c->ready > d->at ? c->ready : d->at;
        next = at < next ? at : next;
    }
    if (next == NDS_NEVER)
        rl_sched_cancel(m->sched, &d->ev);
    else if (!rl_event_queued(&d->ev) || d->ev.when != next)
        rl_sched_post(m->sched, &d->ev, next);
}

static void arm(rl_nds *m, nds_dma *d, unsigned i, uint64_t ready)
{
    nds_dma_ch *c = &d->ch[i];
    c->active = true;
    c->first = true;
    c->ready = ready;
    c->burst = GX_BURST;
    repost(m, d);
}

static int pick(const nds_dma *d, uint64_t t)
{
    for (int i = 0; i < 4; i++)
        if (d->ch[i].active && d->ch[i].ready <= t)
            return i;
    return -1;
}

/** @brief One access's end tick: from t, at the cost of the region, with main RAM's busy time; extra ticks are added to the cost. */
static uint64_t access(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, bool seq, bool main, uint64_t t, unsigned extra)
{
    unsigned r = addr >> 24;
    unsigned c = r >= 8 && r <= 0xA ? nds_cost(m, cpu, RL_NDS_DATA, addr, size, seq)
                                    : nds_cost(m, RL_NDS_ARM7, RL_NDS_CODE, addr, size, seq);
    c += extra;
    if (main) {
        uint64_t start = t > m->ram_free ? t : m->ram_free;
        m->ram_free = start + c;
        return m->ram_free;
    }
    return t + c;
}

static void finish(rl_nds *m, nds_dma *d, unsigned i, uint64_t t)
{
    nds_dma_ch *c = &d->ch[i];
    c->active = false;
    unsigned timing = timing_of(d->cpu, i, c->cnt);
    if ((c->cnt & 0x02000000u) && timing != 0) {
        c->len = c->cnt & len_mask(d->cpu, i);
        if (c->len == 0)
            c->len = len_mask(d->cpu, i) + 1;
        if (((c->cnt >> 21) & 3u) == 3)
            c->dst = c->dad & dst_mask(d->cpu, i);
    } else {
        c->cnt &= 0x7FFFFFFFu;
    }
    if (c->cnt & 0x40000000u)
        nds_irq_set(m, d->cpu, RL_NDS_IRQ_DMA0 << i, t);
    if ((c->cnt & 0x80000000u) && timing == (d->cpu == RL_NDS_ARM9 ? NDS_DMA9_CARD : NDS_DMA7_CARD))
        nds_cart_poke(m, t);
}

/** @brief One unit of channel i from tick t; returns the tick it ends. */
static uint64_t unit(rl_nds *m, nds_dma *d, unsigned i, uint64_t t)
{
    nds_dma_ch *c = &d->ch[i];
    unsigned cpu = d->cpu, size = (c->cnt & 0x04000000u) ? 4 : 2;
    uint32_t s = c->src & ~(size - 1), a = c->dst & ~(size - 1);
    bool smain, dmain;
    uint32_t v = nds_bus_load(m, cpu, s, size, t, &smain);
    nds_bus_store(m, cpu, a, size, v, t, &dmain);
    uint64_t wait = cpu == RL_NDS_ARM9 ? nds_gx_stall(m) : 0;
    bool seq = !c->first && !(smain && dmain);
    unsigned rsize = size, rextra = 0;
    if (smain && !dmain) {
        rsize = 2;
        if (seq && c->after_first)
            rextra = 2;
    }
    t = access(m, cpu, s, rsize, seq, smain, t, rextra);
    t = access(m, cpu, a, size, seq, dmain, t, 0);
    if ((s >> 24) == (a >> 24) && !smain)
        t += 2;
    if (wait > t)
        t = wait;
    c->after_first = c->first;
    c->first = false;
    switch ((c->cnt >> 23) & 3u) {
    case 1:
        c->src -= size;
        break;
    case 2:
        break;
    default:
        c->src += size;
        break;
    }
    switch ((c->cnt >> 21) & 3u) {
    case 1:
        c->dst -= size;
        break;
    case 2:
        break;
    default:
        c->dst += size;
        break;
    }
    if (--c->len == 0) {
        finish(m, d, i, t);
    } else if (cpu == RL_NDS_ARM9 && timing_of(cpu, i, c->cnt) == NDS_DMA9_GXFIFO && --c->burst == 0) {
        c->active = false;
        nds_gx_dma_check(m, t);
    }
    return t;
}

static void slice(rl_nds *m, nds_dma *d, uint64_t t)
{
    uint64_t limit = t + SLICE;
    t = (t + 1) & ~(uint64_t)1;
    for (;;) {
        int i = pick(d, t);
        if (i < 0) {
            uint64_t soon = NDS_NEVER;
            for (unsigned k = 0; k < 4; k++)
                if (d->ch[k].active && d->ch[k].ready < soon)
                    soon = d->ch[k].ready;
            if (soon >= limit)
                break;
            t = (soon + 1) & ~(uint64_t)1;
            continue;
        }
        t = unit(m, d, (unsigned)i, t);
        if (t >= limit)
            break;
    }
    d->at = t;
    repost(m, d);
}

void nds_dma_wait(rl_nds *m, unsigned cpu)
{
    nds_dma *d = &m->dma[cpu];
    for (;;) {
        uint64_t now = nds_now(m, cpu), ready = NDS_NEVER;
        for (unsigned i = 0; i < 4; i++)
            if (d->ch[i].active && d->ch[i].ready < ready)
                ready = d->ch[i].ready;
        if (ready > now)
            return;
        if (d->at > now)
            nds_charge(m, cpu, d->at - now);
        nds_order(m, cpu);
        now = nds_now(m, cpu);
        slice(m, d, now > ready ? now : ready);
        if (d->at > nds_now(m, cpu))
            nds_charge(m, cpu, d->at - nds_now(m, cpu));
    }
}

void nds_dma_start(rl_nds *m, unsigned cpu, unsigned timing, uint64_t tick)
{
    nds_dma *d = &m->dma[cpu];
    for (unsigned i = 0; i < 4; i++) {
        nds_dma_ch *c = &d->ch[i];
        if ((c->cnt & 0x80000000u) && !c->active && timing != 0 && timing_of(cpu, i, c->cnt) == timing)
            arm(m, d, i, tick);
    }
}

static unsigned reg_channel(uint32_t off) { return (off - 0xB0u) / 12u; }

uint32_t nds_dma_read(rl_nds *m, unsigned cpu, uint32_t off)
{
    nds_dma *d = &m->dma[cpu];
    if (off >= 0xE0)
        return cpu == RL_NDS_ARM9 ? d->fill[(off - 0xE0u) >> 2] : 0;
    const nds_dma_ch *c = &d->ch[reg_channel(off)];
    switch ((off - 0xB0u) % 12u) {
    case 0:
        return c->sad;
    case 4:
        return c->dad;
    default:
        return c->cnt;
    }
}

static uint32_t merge(uint32_t old, uint32_t v, uint32_t mask) { return (old & ~mask) | (v & mask); }

void nds_dma_write(rl_nds *m, unsigned cpu, uint32_t off, uint32_t v, uint32_t mask, uint64_t now)
{
    nds_dma *d = &m->dma[cpu];
    if (off >= 0xE0) {
        if (cpu == RL_NDS_ARM9)
            d->fill[(off - 0xE0u) >> 2] = merge(d->fill[(off - 0xE0u) >> 2], v, mask);
        return;
    }
    unsigned i = reg_channel(off);
    nds_dma_ch *c = &d->ch[i];
    switch ((off - 0xB0u) % 12u) {
    case 0:
        c->sad = merge(c->sad, v, mask);
        return;
    case 4:
        c->dad = merge(c->dad, v, mask);
        return;
    default:
        break;
    }
    bool was = (c->cnt & 0x80000000u) != 0;
    c->cnt = merge(c->cnt, v, mask);
    bool on = (c->cnt & 0x80000000u) != 0;
    if (was && !on) {
        c->active = false;
        repost(m, d);
    } else if (!was && on) {
        c->src = c->sad & src_mask(cpu, i);
        c->dst = c->dad & dst_mask(cpu, i);
        c->len = c->cnt & len_mask(cpu, i);
        if (c->len == 0)
            c->len = len_mask(cpu, i) + 1;
        if (timing_of(cpu, i, c->cnt) == 0)
            arm(m, d, i, now + START_DELAY);
        else if (cpu == RL_NDS_ARM9 && timing_of(cpu, i, c->cnt) == NDS_DMA9_GXFIFO)
            nds_gx_dma_check(m, now);
    }
}
