/**
 * @file si.c
 * @brief SI: the registers, the 64-byte DMA between RDRAM and PIF RAM with
 * the time the joybus block takes, the status bits, the interrupt at the
 * end, and a CPU word access to the PIF's address space (busy for a while
 * after a write).
 *
 * A PIF to RDRAM DMA runs the joybus commands at its end, so the
 * controller state the game reads is the state at that moment; an RDRAM
 * to PIF DMA copies RDRAM at its end.
 *
 * The register rules, the DMA durations and the bus write window are
 * ported from ares (n64/si/io.cpp and dma.cpp), which carries this notice:
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

enum {
    DMA_READ_STATE = 1,
    DMA_WRITE_STATE = 4,
    BUS_WRITE_STATE = 9,
    DMA_READ_PCH = 4,
    DMA_WRITE_PCH = 1,
    BUS_WRITE_PCH = 0xB,
    DMA_WRITE_CYCLES = 4065,
    BUS_WRITE_CYCLES = 2150,
};

/** @brief RCP cycles of ares' clock (three per RCP cycle at 187.5 MHz) to CPU ticks (93.75 MHz). */
static uint64_t ticks(uint32_t cycles) { return (uint64_t)cycles * 3u / 2u; }

static void finished(rl_n64 *m)
{
    m->si.dma_busy = false;
    m->si.pch_state = 0;
    m->si.dma_state = 0;
    m->si.interrupt = true;
    rl_n64_mi_raise(m, RL_N64_IRQ_SI);
}

static void dma_read_done(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    (void)when;
    rl_n64 *m = e->opaque;
    rl_n64_pif_dma_read(m, m->si.rd_addr, m->si.dram);
    finished(m);
}

static void dma_write_done(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    (void)when;
    rl_n64 *m = e->opaque;
    rl_n64_pif_dma_write(m, m->si.wr_addr, m->si.dram);
    finished(m);
}

static void bus_write_done(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    (void)when;
    rl_n64 *m = e->opaque;
    m->si.io_busy = false;
    finished(m);
}

void rl_n64_si_attach(rl_n64 *m)
{
    m->si.dma_read.fire = dma_read_done;
    m->si.dma_read.opaque = m;
    m->si.dma_write.fire = dma_write_done;
    m->si.dma_write.opaque = m;
    m->si.bus_write.fire = bus_write_done;
    m->si.bus_write.opaque = m;
}

void rl_n64_si_detach(rl_n64 *m)
{
    if (!m->s)
        return;
    rl_sched_cancel(m->s, &m->si.dma_read);
    rl_sched_cancel(m->s, &m->si.dma_write);
    rl_sched_cancel(m->s, &m->si.bus_write);
}

uint32_t rl_n64_si_read(rl_n64 *m, uint32_t a)
{
    switch ((a & 0x1Fu) >> 2) {
    case 0:
        return m->si.dram;
    case 1:
        return m->si.rd_addr;
    case 4:
        return m->si.wr_addr;
    case 6:
        return (uint32_t)m->si.dma_busy | (uint32_t)m->si.io_busy << 1 | (uint32_t)m->si.pch_state << 4 |
               (uint32_t)m->si.dma_state << 8 | (uint32_t)m->si.interrupt << 12;
    default:
        return 0;
    }
}

void rl_n64_si_write(rl_n64 *m, uint32_t a, uint32_t v)
{
    switch ((a & 0x1Fu) >> 2) {
    case 0:
        m->si.dram = v & 0xFFFFF8u;
        break;
    case 1:
        m->si.rd_addr = v & ~1u;
        m->si.dma_busy = true;
        m->si.dma_state = DMA_READ_STATE;
        m->si.pch_state = DMA_READ_PCH;
        rl_sched_post(m->s, &m->si.dma_read, rl_n64_now(m) + ticks(rl_n64_pif_estimate(m)));
        break;
    case 4:
        m->si.wr_addr = v & ~1u;
        m->si.dma_busy = true;
        m->si.dma_state = DMA_WRITE_STATE;
        m->si.pch_state = DMA_WRITE_PCH;
        rl_sched_post(m->s, &m->si.dma_write, rl_n64_now(m) + ticks(DMA_WRITE_CYCLES));
        break;
    case 6:
        m->si.interrupt = false;
        rl_n64_mi_lower(m, RL_N64_IRQ_SI);
        break;
    default:
        break;
    }
}

uint32_t rl_n64_si_bus_read(rl_n64 *m, uint32_t a)
{
    if (m->si.io_busy) {
        rl_sched_cancel(m->s, &m->si.bus_write);
        m->si.io_busy = false;
        return m->si.latch;
    }
    return rl_n64_pif_read(m, a);
}

void rl_n64_si_bus_write(rl_n64 *m, uint32_t a, uint32_t v)
{
    if (m->si.io_busy)
        return;
    m->si.io_busy = true;
    m->si.dma_busy = true;
    m->si.pch_state = BUS_WRITE_PCH;
    m->si.dma_state = BUS_WRITE_STATE;
    m->si.latch = v;
    rl_sched_post(m->s, &m->si.bus_write, rl_n64_now(m) + ticks(BUS_WRITE_CYCLES));
    rl_n64_pif_write(m, a, v);
}
