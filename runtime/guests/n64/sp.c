/**
 * @file sp.c
 * @brief The SP interface and the DP command registers: the SP registers
 * as the CPU and the RSP (COP0 0 to 7) see them, SP DMA with its duration,
 * the status bits, the semaphore and the signals, the RSP's start, halt and
 * BREAK, the SP interrupt, the DP registers (COP0 8 to 15) and the running
 * of the RSP in slices interleaved with the CPU.
 *
 * Time on the SP side is counted in RSP cycles (62.5 MHz, two thirds of a
 * CPU tick). The RSP runs behind the CPU and is brought up to the CPU's tick
 * before every CPU access outside RDRAM and at a slice event while it runs,
 * so every access to the SP, DP and MI registers and to DMEM and IMEM, from
 * either side, happens in time order (within one block of the RSP). The
 * machine knows no RSP code: the run layer's rl_n64_rsp_ops runs it.
 *
 * The DMA rules (the pending and the current transfer, the full and busy
 * flags, a row per (length + 8) / 8 RSP cycles, the data moved at the end
 * of each row, the registers reading the current transfer), the status
 * bits and their write pairs, BREAK and the semaphore, and the DP start,
 * end and status rules are ported from ares (n64/rsp/dma.cpp, io.cpp,
 * interpreter-ipu.cpp and n64/rdp/io.cpp), which carries this notice:
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
#include "n64_int.h"

/** @brief RSP cycles of a slice: the most the RSP runs before the CPU's next look at the SP side. */
#define SLICE 512u

/** @brief The RSP cycle at CPU tick t. */
static uint64_t rsp_at(uint64_t t) { return t * 2u / 3u; }
/** @brief The first CPU tick at or after RSP cycle c. */
static uint64_t tick_at(uint64_t c) { return (c * 3u + 1u) / 2u; }

/** @brief Copies one row of the current transfer and moves its addresses on. */
static void dma_row(rl_n64 *m)
{
    n64_sp_dma *d = &m->sp.current;
    uint32_t bank = d->mem & 0x1000u, first = d->mem & 0xFFFu, dram0 = d->dram;
    uint32_t n = d->length + 8u;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t s = bank | ((d->mem + k) & 0xFFFu);
        uint32_t r = (d->dram + k) & (RL_N64_RDRAM_SIZE - 1u);
        if (m->sp.busy_wr)
            rl_n64_wr8(m, r, m->spmem[s]);
        else
            m->spmem[s] = rl_n64_rd8(m, r);
    }
    d->mem = bank | ((d->mem + n) & 0xFFFu);
    d->dram = (d->dram + n) & 0xFFFFF8u;
    if (m->sp.busy_wr) {
        rl_n64_code_written(m, dram0 & (RL_N64_RDRAM_SIZE - 1u), n);
        return;
    }
    rl_n64_code_sp_written(m, bank | first, n);
    if (bank && m->sp.ops && m->sp.ops->imem_written)
        m->sp.ops->imem_written(m->sp.user, first, n);
}

static void post_dma(rl_n64 *m)
{
    if (m->s)
        rl_sched_post(m->s, &m->sp.dma_event, tick_at(m->sp.row_end));
}

/** @brief Starts the pending transfer at RSP cycle at when none is busy. */
static void dma_start(rl_n64 *m, uint64_t at)
{
    if (m->sp.busy_rd || m->sp.busy_wr || !(m->sp.full_rd || m->sp.full_wr))
        return;
    m->sp.current = m->sp.pending;
    m->sp.busy_rd = m->sp.full_rd;
    m->sp.busy_wr = m->sp.full_wr;
    m->sp.full_rd = m->sp.full_wr = false;
    m->sp.row_end = at + (m->sp.current.length + 8u) / 8u;
    m->stats.sp_dmas++;
    post_dma(m);
}

/** @brief Moves the transfers on to RSP cycle at: every row that ends by then is copied. */
static void dma_advance(rl_n64 *m, uint64_t at)
{
    while ((m->sp.busy_rd || m->sp.busy_wr) && m->sp.row_end <= at) {
        dma_row(m);
        uint64_t end = m->sp.row_end;
        n64_sp_dma *d = &m->sp.current;
        if (d->count) {
            d->count--;
            d->dram = (d->dram + d->skip) & 0xFFFFF8u;
            m->sp.row_end = end + (d->length + 8u) / 8u;
            post_dma(m);
        } else {
            m->sp.busy_rd = m->sp.busy_wr = false;
            d->length = 0xFF8u;
            dma_start(m, end);
        }
    }
}

/** @brief The SP side's time for an access at RSP cycle at: never before an earlier access. */
static uint64_t sp_time(rl_n64 *m, uint64_t at)
{
    if (at > m->sp.last)
        m->sp.last = at;
    dma_advance(m, m->sp.last);
    return m->sp.last;
}

static void post_slice(rl_n64 *m)
{
    if (m->s && !m->sp.halted)
        rl_sched_post(m->s, &m->sp.slice, tick_at(m->sp.cycle + SLICE));
}

void rl_n64_sp_catch_up(rl_n64 *m, uint64_t tick)
{
    if (m->sp.in_rsp)
        return;
    uint64_t to = rsp_at(tick);
    if (!m->sp.halted && m->sp.ops && to > m->sp.cycle) {
        m->sp.in_rsp = true;
        m->sp.cycle = m->sp.ops->run(m->sp.user, &m->sp.pc, m->sp.cycle, to);
        m->sp.in_rsp = false;
    }
    if (m->sp.halted && m->sp.cycle < to)
        m->sp.cycle = to;
    sp_time(m, to);
}

static void slice_fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    rl_n64 *m = e->opaque;
    rl_n64_sp_catch_up(m, when);
    post_slice(m);
}

static void dma_fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    rl_n64_sp_catch_up(e->opaque, when);
}

void rl_n64_sp_attach(rl_n64 *m)
{
    m->sp.slice.fire = slice_fire;
    m->sp.slice.opaque = m;
    m->sp.dma_event.fire = dma_fire;
    m->sp.dma_event.opaque = m;
}

void rl_n64_sp_init(rl_n64 *m)
{
    m->sp.halted = true;
    m->sp.current.length = 0;
    m->dp.ready = true;
}

void rl_n64_set_rsp(rl_n64 *m, const rl_n64_rsp_ops *ops, void *user)
{
    m->sp.ops = ops;
    m->sp.user = user;
}

bool rl_n64_rsp_halted(const rl_n64 *m) { return m->sp.halted; }
uint32_t rl_n64_sp_pc(const rl_n64 *m) { return m->sp.pc; }

static uint32_t sp_status(const rl_n64 *m)
{
    uint32_t v = 0;
    v |= m->sp.halted ? 0x1u : 0u;
    v |= m->sp.broken ? 0x2u : 0u;
    v |= (m->sp.busy_rd || m->sp.busy_wr) ? 0x4u : 0u;
    v |= (m->sp.full_rd || m->sp.full_wr) ? 0x8u : 0u;
    v |= m->sp.io_full ? 0x10u : 0u;
    v |= m->sp.sstep ? 0x20u : 0u;
    v |= m->sp.intbreak ? 0x40u : 0u;
    v |= (uint32_t)m->sp.signals << 7;
    return v;
}

/** @brief SP register i (0 to 7) read at RSP cycle at. */
static uint32_t sp_reg_value(rl_n64 *m, unsigned i, bool peek)
{
    const n64_sp_dma *d = &m->sp.current;
    switch (i & 7u) {
    case 0:
        return d->mem & 0x1FFFu;
    case 1:
        return d->dram & 0xFFFFFFu;
    case 2:
    case 3:
        return (d->length & 0xFFFu) | (d->count & 0xFFu) << 12 | (d->skip & 0xFFFu) << 20;
    case 4:
        return sp_status(m);
    case 5:
        return (m->sp.full_rd || m->sp.full_wr) ? 1u : 0u;
    case 6:
        return (m->sp.busy_rd || m->sp.busy_wr) ? 1u : 0u;
    default: {
        uint32_t v = m->sp.semaphore;
        if (!peek)
            m->sp.semaphore = 1;
        return v;
    }
    }
}

static uint32_t sp_reg_read(rl_n64 *m, unsigned i, uint64_t at)
{
    sp_time(m, at);
    return sp_reg_value(m, i, false);
}

static void start(rl_n64 *m, uint64_t at)
{
    m->stats.rsp_starts++;
    if (!m->sp.ops) {
        rl_report_unknown(0x4E36345350ull, "n64", "the program starts the RSP, which this runtime cannot run; it halts at once");
        m->sp.halted = true;
        m->sp.broken = true;
        if (m->sp.intbreak)
            rl_n64_mi_raise(m, RL_N64_IRQ_SP);
        return;
    }
    if (m->sp.cycle < at)
        m->sp.cycle = at;
    post_slice(m);
}

static void status_write(rl_n64 *m, uint32_t v, uint64_t at)
{
    bool was = m->sp.halted;
#define PAIR(clr, set, field)                                                                                          \
    do {                                                                                                               \
        if ((v & (1u << (clr))) && !(v & (1u << (set))))                                                               \
            field = false;                                                                                             \
        if ((v & (1u << (set))) && !(v & (1u << (clr))))                                                               \
            field = true;                                                                                              \
    } while (0)
    PAIR(0, 1, m->sp.halted);
    if (v & 0x4u)
        m->sp.broken = false;
    if ((v & 0x8u) && !(v & 0x10u))
        rl_n64_mi_lower(m, RL_N64_IRQ_SP);
    if ((v & 0x10u) && !(v & 0x8u))
        rl_n64_mi_raise(m, RL_N64_IRQ_SP);
    PAIR(5, 6, m->sp.sstep);
    PAIR(7, 8, m->sp.intbreak);
#undef PAIR
    for (unsigned b = 0; b < 8; b++) {
        uint32_t clr = 1u << (9 + 2 * b), set = 1u << (10 + 2 * b);
        if ((v & clr) && !(v & set))
            m->sp.signals &= (uint8_t)~(1u << b);
        if ((v & set) && !(v & clr))
            m->sp.signals |= (uint8_t)(1u << b);
    }
    if (was && !m->sp.halted)
        start(m, at);
    if (m->sp.halted && m->s)
        rl_sched_cancel(m->s, &m->sp.slice);
}

/** @brief SP register i written at RSP cycle at. */
static void sp_reg_write(rl_n64 *m, unsigned i, uint32_t v, uint64_t at)
{
    at = sp_time(m, at);
    n64_sp_dma *p = &m->sp.pending;
    switch (i & 7u) {
    case 0:
        p->mem = v & 0x1FF8u;
        break;
    case 1:
        p->dram = v & 0xFFFFF8u;
        break;
    case 2:
    case 3:
        p->length = v & 0xFF8u;
        p->count = v >> 12 & 0xFFu;
        p->skip = v >> 20 & 0xFF8u;
        m->sp.full_rd = (i & 7u) == 2;
        m->sp.full_wr = (i & 7u) == 3;
        dma_start(m, at);
        break;
    case 4:
        status_write(m, v, at);
        break;
    case 7:
        m->sp.semaphore = 0;
        break;
    default:
        break;
    }
}

/** @brief The 64-bit words of each command by its code, as ares's parallel-rdp path counts them. */
static const uint8_t dp_length[64] = {
    1, 1, 1, 1, 1, 1, 1, 1, 4, 6, 12, 14, 12, 14, 20, 22, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

/** @brief The DP's Sync_Full: everything drawn, the DP interrupt raised. */
static void dp_sync_full(rl_n64 *m)
{
    if (!m->dp.crashed) {
        rl_n64_mi_raise(m, RL_N64_IRQ_DP);
        m->dp.buffer_busy = false;
        m->dp.pipe_busy = false;
    }
    m->dp.start_gclk = false;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/** @brief Takes the commands [current, end) into the queue and runs every whole one. */
static void dp_render(rl_n64 *m)
{
    uint32_t current = m->dp.current & ~7u, end = m->dp.end & ~7u;
    if (current >= end)
        return;
    uint32_t length = (end - current) / 8u;
    if (m->dp.queue_size + length >= RL_N64_DP_QUEUE)
        return;
    uint32_t *q = m->dp.queue;
    for (; length; length--, current += 8u) {
        uint32_t i = m->dp.queue_size++ * 2u;
        if (m->dp.xbus) {
            q[i] = be32(m->spmem + (current & 0xFFCu));
            q[i + 1] = be32(m->spmem + ((current + 4u) & 0xFFCu));
        } else {
            q[i] = rl_n64_rd32(m, current);
            q[i + 1] = rl_n64_rd32(m, current + 4u);
        }
    }
    const rl_n64_rdp_ops *ops = m->dp.ops;
    while (m->dp.queue_offset < m->dp.queue_size) {
        const uint32_t *w = q + m->dp.queue_offset * 2u;
        uint32_t code = w[0] >> 24 & 63u, n = dp_length[code];
        if (m->dp.queue_offset + n > m->dp.queue_size) {
            m->dp.start = m->dp.current = m->dp.end;
            return;
        }
        if (code >= 8u && ops && ops->command)
            ops->command(m->dp.user, w, n * 2u);
        if (code == 0x29u) {
            if (ops && ops->sync_full)
                ops->sync_full(m->dp.user);
            dp_sync_full(m);
        }
        m->dp.queue_offset += n;
    }
    m->dp.queue_offset = 0;
    m->dp.queue_size = 0;
    m->dp.current = m->dp.end;
}

/** @brief Hands the command list [current, end) to the RDP, as ares's flushCommands. */
static void dp_flush(rl_n64 *m)
{
    if (m->dp.freeze || m->dp.crashed || m->dp.end <= m->dp.current)
        return;
    m->stats.dp_submits++;
    m->dp.buffer_busy = true;
    m->dp.pipe_busy = true;
    m->dp.start_gclk = true;
    dp_render(m);
    if (m->dp.ops && m->dp.ops->crashed && m->dp.ops->crashed(m->dp.user)) {
        m->dp.crashed = true;
        m->dp.pipe_busy = true;
    }
    m->dp.buffer_busy = false;
    m->dp.ready = true;
}

void rl_n64_set_rdp(rl_n64 *m, const rl_n64_rdp_ops *ops, void *user)
{
    m->dp.ops = ops;
    m->dp.user = user;
}

/** @brief DP register i (0 to 7) read at RSP cycle at. */
static uint32_t dp_reg_read(rl_n64 *m, unsigned i, uint64_t at)
{
    switch (i & 7u) {
    case 0:
        return m->dp.start;
    case 1:
        return m->dp.end;
    case 2:
        return m->dp.current;
    case 3:
        return (m->dp.xbus ? 0x1u : 0u) | (m->dp.freeze || m->dp.crashed ? 0x2u : 0u) |
               (m->dp.flush ? 0x4u : 0u) | (m->dp.start_gclk ? 0x8u : 0u) | (m->dp.tmem_busy ? 0x10u : 0u) |
               (m->dp.pipe_busy ? 0x20u : 0u) | (m->dp.buffer_busy ? 0x40u : 0u) |
               (m->dp.ready ? 0x80u : 0u) | (m->dp.end_valid ? 0x200u : 0u) |
               (m->dp.start_valid ? 0x400u : 0u);
    case 4:
        return (uint32_t)(at - m->dp.clock_base) & 0xFFFFFFu;
    case 5:
        return m->dp.buffer_busy ? 1u : 0u;
    case 6:
        return m->dp.pipe_busy ? 1u : 0u;
    default:
        return 0;
    }
}

/** @brief DP register i written at RSP cycle at. */
static void dp_reg_write(rl_n64 *m, unsigned i, uint32_t v, uint64_t at)
{
    switch (i & 7u) {
    case 0:
        if (!m->dp.start_valid)
            m->dp.start = v & 0xFFFFF8u;
        m->dp.start_valid = true;
        break;
    case 1:
        m->dp.end = v & 0xFFFFF8u;
        if (m->dp.start_valid) {
            m->dp.current = m->dp.start;
            m->dp.start_valid = false;
        }
        dp_flush(m);
        break;
    case 3:
        if (v & 0x1u)
            m->dp.xbus = false;
        if (v & 0x2u)
            m->dp.xbus = true;
        if (v & 0x4u) {
            m->dp.freeze = false;
            dp_flush(m);
        }
        if (v & 0x8u)
            m->dp.freeze = true;
        if (v & 0x10u)
            m->dp.flush = false;
        if (v & 0x20u)
            m->dp.flush = true;
        if ((v & 0x40u) && !m->dp.crashed)
            m->dp.tmem_busy = false;
        if ((v & 0x80u) && !m->dp.crashed)
            m->dp.pipe_busy = false;
        if ((v & 0x100u) && !m->dp.crashed)
            m->dp.buffer_busy = false;
        if (v & 0x200u)
            m->dp.clock_base = at;
        break;
    default:
        break;
    }
}

uint32_t rl_n64_sp_read(rl_n64 *m, uint32_t a)
{
    uint64_t at = rsp_at(rl_n64_now(m));
    if (a == 0x04080000u)
        return m->sp.pc;
    if ((a & 0xFFFE0u) == 0x40000u)
        return sp_reg_read(m, (a >> 2) & 7u, at);
    return 0;
}

void rl_n64_sp_write(rl_n64 *m, uint32_t a, uint32_t v)
{
    uint64_t at = rsp_at(rl_n64_now(m));
    if (a == 0x04080000u)
        m->sp.pc = v & 0xFFCu;
    else if ((a & 0xFFFE0u) == 0x40000u)
        sp_reg_write(m, (a >> 2) & 7u, v, at);
}

uint32_t rl_n64_sp_peek(rl_n64 *m, uint32_t a)
{
    if (a == 0x04080000u)
        return m->sp.pc;
    if ((a & 0xFFFE0u) == 0x40000u)
        return sp_reg_value(m, (a >> 2) & 7u, true);
    return 0;
}

uint32_t rl_n64_dp_peek(rl_n64 *m, unsigned i) { return dp_reg_read(m, i, m->sp.last); }

uint32_t rl_n64_dp_read(rl_n64 *m, unsigned i) { return dp_reg_read(m, i, sp_time(m, rsp_at(rl_n64_now(m)))); }

void rl_n64_dp_write(rl_n64 *m, unsigned i, uint32_t v) { dp_reg_write(m, i, v, sp_time(m, rsp_at(rl_n64_now(m)))); }

uint32_t rl_n64_rsp_mfc0(rl_n64 *m, uint32_t rd, uint64_t cycle)
{
    if (rd & 8u)
        return dp_reg_read(m, rd & 7u, sp_time(m, cycle));
    return sp_reg_read(m, rd & 7u, cycle);
}

void rl_n64_rsp_mtc0(rl_n64 *m, uint32_t rd, uint32_t value, uint64_t cycle)
{
    if (rd & 8u)
        dp_reg_write(m, rd & 7u, value, sp_time(m, cycle));
    else
        sp_reg_write(m, rd & 7u, value, cycle);
}

void rl_n64_rsp_break(rl_n64 *m, uint64_t cycle)
{
    sp_time(m, cycle);
    m->stats.rsp_breaks++;
    m->sp.halted = true;
    m->sp.broken = true;
    if (m->sp.intbreak)
        rl_n64_mi_raise(m, RL_N64_IRQ_SP);
    if (m->s)
        rl_sched_cancel(m->s, &m->sp.slice);
}

void rl_n64_imem_written(rl_n64 *m, uint32_t offset, uint32_t len)
{
    if (m->sp.ops && m->sp.ops->imem_written)
        m->sp.ops->imem_written(m->sp.user, offset, len);
}
