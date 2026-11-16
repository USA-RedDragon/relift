/**
 * @file pi.c
 * @brief PI: the DMA registers, the two cartridge domains' timing, DMA
 * between RDRAM and the cartridge with the duration the bus takes, the
 * status and the interrupt at the end, and CPU reads of the cartridge bus
 * (the devices on it are cart.c's). A DMA from the cartridge tells the code binding which ROM bytes reached
 * RDRAM (rl_n64_code_loaded).
 *
 * The register rules, the DMA's block shape and its duration are ported
 * from ares (n64/pi/io.cpp, dma.cpp and bus.hpp), which carries this
 * notice:
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

/** @brief The domain of a cartridge bus address: 2 for 0x05xxxxxx and 0x08 to 0x0F, else 1. */
static n64_bsd *bsd_for(rl_n64 *m, uint32_t a)
{
    uint32_t hi = a >> 24;
    return (hi == 0x05u || (hi >= 0x08u && hi <= 0x0Fu)) ? &m->pi.bsd[1] : &m->pi.bsd[0];
}

/** @brief The CPU ticks of a DMA of len bytes (len already rounded) starting at cart address a: ares counts RCP cycles, 1.5 ticks each. */
static uint32_t duration(rl_n64 *m, uint32_t a, uint32_t len)
{
    n64_bsd *bsd = bsd_for(m, a);
    uint32_t shift = bsd->page_size + 2u;
    uint32_t page = 1u << shift, mask = page - 1u;
    uint32_t first = a, last = a + len - 2u;
    uint32_t fp = first >> shift, lp = last >> shift;
    uint32_t pages = lp - fp + 1u;
    uint32_t buffers = 0, partial = 0;
    if (fp == lp) {
        if (len == 128u)
            buffers = 1;
        else
            partial = len;
    } else {
        if ((first & mask) == 0)
            buffers++;
        else
            partial += page - (first & mask);
        if (((last + 2u) & mask) == 0)
            buffers++;
        else
            partial += (last & mask) + 2u;
        if (fp + 1u < lp)
            buffers += (pages - 2u) * page / 128u;
    }
    uint32_t cycles = (14u + bsd->latency + 1u) * pages;
    cycles += (bsd->pulse_width + 1u + bsd->release + 1u) * len / 2u;
    cycles += buffers * 28u + partial;
    return (cycles * 3u + 1u) / 2u;
}

/** @brief Cartridge to RDRAM, in ares' block shape: 128-byte blocks that stop at 2 KiB rows, the first block's short form. */
static void dma_write(rl_n64 *m)
{
    int32_t length = (int32_t)m->pi.wr_len + 1;
    int32_t max_block = 128;
    uint32_t dram0 = m->pi.dram, cart0 = m->pi.cart;
    uint32_t page_mask = (1u << (bsd_for(m, m->pi.cart)->page_size + 2u)) - 1u;
    bool selected = false;
    uint8_t buf[128];
    while (length > 0) {
        int32_t misalign = (int32_t)(m->pi.dram & 7u);
        int32_t row_end = 0x800 - (int32_t)(m->pi.dram & 0x7FFu);
        int32_t block = max_block - misalign < row_end ? max_block - misalign : row_end;
        int32_t cur = length < block ? length : block;
        for (int32_t i = 0; i < cur; i += 2) {
            if (!selected || (m->pi.cart & page_mask) == 0) {
                rl_n64_bus_select(m, m->pi.cart);
                selected = true;
            }
            uint16_t d = rl_n64_bus_read_half(m);
            buf[i] = (uint8_t)(d >> 8);
            buf[i + 1] = (uint8_t)d;
            m->pi.cart += 2;
            length -= 2;
        }
        for (int32_t i = 0; i < cur - misalign; i++)
            m->rdram[m->pi.dram++ & (RL_N64_RDRAM_SIZE - 1u)] = buf[i];
        m->pi.dram = (m->pi.dram + 7u) & ~7u;
        m->pi.wr_len = (uint32_t)(cur <= 8 ? 127 - misalign : 127);
        max_block = row_end < 8 ? 128 - misalign : 128;
    }
    uint32_t n = m->pi.dram - dram0;
    if (cart0 >= 0x10000000u && cart0 < 0x1FC00000u)
        rl_n64_code_loaded(m, 0, cart0 - 0x10000000u, dram0, n);
    else
        rl_n64_code_written(m, dram0, n);
}

/** @brief RDRAM to the cartridge: to the save memory in domain 2, and to nothing on the ROM. */
static void dma_read(rl_n64 *m)
{
    uint32_t len = (m->pi.rd_len | 1u) + 1u;
    uint32_t page_mask = (1u << (bsd_for(m, m->pi.cart)->page_size + 2u)) - 1u;
    rl_n64_bus_select(m, m->pi.cart);
    for (uint32_t a = 0; a < len; a += 2) {
        uint32_t cursor = m->pi.cart + a;
        if (a && (cursor & page_mask) == 0)
            rl_n64_bus_select(m, cursor);
        uint32_t d = m->pi.dram + a;
        uint16_t v = (uint16_t)(m->rdram[d & (RL_N64_RDRAM_SIZE - 1u)] << 8 | m->rdram[(d + 1u) & (RL_N64_RDRAM_SIZE - 1u)]);
        rl_n64_bus_write_half(m, v);
    }
}

static void dma_done(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    (void)when;
    rl_n64 *m = e->opaque;
    m->pi.busy = false;
    m->pi.interrupt = true;
    rl_n64_mi_raise(m, RL_N64_IRQ_PI);
}

void rl_n64_pi_attach(rl_n64 *m)
{
    m->pi.done.fire = dma_done;
    m->pi.done.opaque = m;
    rl_n64_cart_attach(m);
}

uint32_t rl_n64_pi_read(rl_n64 *m, unsigned i)
{
    switch (i) {
    case 0:
        return m->pi.dram;
    case 1:
        return m->pi.cart;
    case 2:
        return m->pi.rd_len;
    case 3:
        return m->pi.wr_len;
    case 4:
        return (uint32_t)m->pi.busy | (uint32_t)m->pi.io_busy << 1 | (uint32_t)m->pi.error << 2 |
               (uint32_t)m->pi.interrupt << 3;
    case 5:
    case 9:
        return m->pi.bsd[i == 9].latency;
    case 6:
    case 10:
        return m->pi.bsd[i == 10].pulse_width;
    case 7:
    case 11:
        return m->pi.bsd[i == 11].page_size;
    case 8:
    case 12:
        return m->pi.bsd[i == 12].release;
    case 13:
    case 14:
        return m->pi.latch;
    default:
        return 0;
    }
}

void rl_n64_pi_write(rl_n64 *m, unsigned i, uint32_t v)
{
    if (i != 4 && (m->pi.busy || m->pi.io_busy)) {
        m->pi.error = true;
        return;
    }
    switch (i) {
    case 0:
        m->pi.dram = v & 0xFFFFFEu;
        break;
    case 1:
        m->pi.cart = v & ~1u;
        break;
    case 2:
        m->pi.rd_len = v & 0xFFFFFFu;
        m->pi.busy = true;
        rl_sched_post(m->s, &m->pi.done, rl_n64_now(m) + duration(m, m->pi.cart, (m->pi.rd_len | 1u) + 1u));
        dma_read(m);
        break;
    case 3:
        m->pi.wr_len = v & 0xFFFFFFu;
        m->pi.busy = true;
        m->stats.pi_dmas++;
        rl_sched_post(m->s, &m->pi.done, rl_n64_now(m) + duration(m, m->pi.cart, (m->pi.wr_len | 1u) + 1u));
        dma_write(m);
        break;
    case 4:
        if (v & 1u) {
            m->pi.busy = false;
            m->pi.error = false;
            rl_sched_cancel(m->s, &m->pi.done);
        }
        if (v & 2u) {
            m->pi.interrupt = false;
            rl_n64_mi_lower(m, RL_N64_IRQ_PI);
        }
        break;
    case 5:
    case 9:
        m->pi.bsd[i == 9].latency = (uint8_t)v;
        break;
    case 6:
    case 10:
        m->pi.bsd[i == 10].pulse_width = (uint8_t)v;
        break;
    case 7:
        m->pi.bsd[0].page_size = (uint8_t)(v & 0xFu);
        break;
    case 11:
        m->pi.bsd[1].page_size = (uint8_t)v;
        break;
    case 8:
        m->pi.bsd[0].release = (uint8_t)(v & 3u);
        break;
    case 12:
        m->pi.bsd[1].release = (uint8_t)v;
        break;
    default:
        break;
    }
}
