/**
 * @file bus.c
 * @brief Bus timing: the wait states of every region from WAITCNT and the
 * memory control register, the Game Pak's sequential bursts and their
 * 128 KiB page rule, and the prefetch buffer; the CPU's bus calls.
 *
 * Ported from ares (gba/cpu/bus.cpp, prefetch.cpp, memory.cpp and
 * gba/cartridge/mrom.cpp), which carries this notice:
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

static bool is_rom(uint32_t addr) { return addr >= 0x08000000u && addr < 0x0E000000u; }

static unsigned wait_cart(const rl_gba *g, uint32_t addr, bool seq)
{
    static const unsigned nt[4] = {5, 4, 3, 9};
    unsigned w = g->waitcnt;
    unsigned n, s;
    switch (addr & 0x0E000000u) {
    case 0x08000000u:
        n = nt[(w >> 2) & 3];
        s = (w & 0x10) ? 2 : 3;
        break;
    case 0x0A000000u:
        n = nt[(w >> 5) & 3];
        s = (w & 0x80) ? 2 : 5;
        break;
    case 0x0C000000u:
        n = nt[(w >> 8) & 3];
        s = (w & 0x400) ? 2 : 9;
        break;
    default:
        n = nt[w & 3];
        s = n;
        break;
    }
    return seq ? s : n;
}

static unsigned ewram_wait(const rl_gba *g) { return 16u - ((g->memcnt >> 24) & 0xFu); }

void gba_waits_rebuild(rl_gba *g)
{
    for (unsigned r = 0; r < 16; r++)
        for (unsigned z = 0; z < 3; z++)
            for (unsigned q = 0; q < 2; q++) {
                uint32_t a = r << 24;
                unsigned c = 1;
                if (r == 0x2 && (g->memcnt & 0x20))
                    c = ewram_wait(g) << (z == 2);
                else if (r == 0x5 || r == 0x6)
                    c = 1u << (z == 2);
                else if (r >= 0x8 && r <= 0xD)
                    c = wait_cart(g, a, q) + (z == 2 ? wait_cart(g, a, true) : 0);
                else if (r >= 0xE)
                    c = wait_cart(g, a, false);
                g->waits.c[r][z][q] = (uint8_t)c;
            }
}

static bool pf_on(const rl_gba *g) { return (g->waitcnt & 0x4000) != 0; }
static bool pf_empty(const gba_prefetch *p) { return p->addr == p->load; }
static bool pf_full(const gba_prefetch *p) { return p->load - p->addr == 16; }

static void start_burst(rl_gba *g, uint32_t addr)
{
    g->page_addr = (uint16_t)(addr >> 1);
    g->rom_burst = true;
}

static uint16_t rom_burst_read(rl_gba *g, uint32_t addr)
{
    if ((addr & 0x1FFFEu) == 0x1FFFEu)
        g->rom_burst = false;
    uint32_t a = (addr & 0xFFE0000u) | ((uint32_t)g->page_addr << 1);
    uint16_t h = gba_rom_half(g, a);
    g->page_addr++;
    return h;
}

void gba_pf_step(rl_gba *g, unsigned clocks)
{
    gba_charge(g, clocks);
    gba_prefetch *p = &g->pf;
    if (!pf_on(g) || p->stopped)
        return;
    while (clocks--) {
        if (pf_full(p)) {
            p->stopped = true;
            p->ahead = false;
            g->rom_burst = false;
            break;
        }
        p->ahead = true;
        if (--p->wait)
            continue;
        if (p->load & 0x1FFFEu) {
            start_burst(g, p->load);
            p->slot[(p->load >> 1) & 7] = rom_burst_read(g, p->load);
            p->load += 2;
        }
        p->wait = (int32_t)wait_cart(g, p->load, true);
    }
}

static void pf_reset(rl_gba *g)
{
    if (g->pf.wait == 1)
        gba_charge(g, 1);
    g->pf.stopped = true;
    g->pf.ahead = false;
    g->pf.wait = 0;
    g->pf.addr = 0;
    g->pf.load = 0;
}

static void pf_sync(rl_gba *g, unsigned size, uint32_t addr)
{
    if (g->pf.wait == 1)
        gba_charge(g, 1);
    g->pf.stopped = false;
    g->pf.ahead = false;
    g->pf.addr = addr + size;
    g->pf.load = addr + size;
    g->pf.wait = (int32_t)wait_cart(g, g->pf.load, true);
}

static uint16_t pf_read(rl_gba *g)
{
    gba_prefetch *p = &g->pf;
    if (p->stopped && pf_empty(p)) {
        p->stopped = false;
        p->wait = (int32_t)wait_cart(g, p->load, false);
    }
    if (pf_empty(p))
        gba_pf_step(g, (unsigned)p->wait);
    uint16_t h = p->slot[(p->addr >> 1) & 7];
    p->addr += 2;
    return h;
}

static uint32_t rom_timed(rl_gba *g, uint32_t addr, unsigned size, bool burst)
{
    uint32_t a = size == 4 ? addr & ~3u : addr & ~1u;
    uint32_t v = 0;
    for (unsigned i = 0; i < (size == 4 ? 2u : 1u); i++, a += 2) {
        if (!burst)
            start_burst(g, a);
        gba_charge(g, wait_cart(g, a, burst));
        v |= (uint32_t)rom_burst_read(g, a) << (16 * i);
        burst = true;
    }
    if (size == 1)
        v = (uint8_t)(v >> (8 * (addr & 1)));
    return v;
}

static bool rom_seq(rl_gba *g, gba_who who, bool seq)
{
    if (!g->rom_burst)
        return false;
    return who == GBA_DMA ? g->dma_rom_burst : seq;
}

static void plain_cost(rl_gba *g, uint32_t addr, unsigned size)
{
    switch (addr >> 24) {
    case 0x2:
        if (g->memcnt & 0x20) {
            gba_pf_step(g, ewram_wait(g) << (size == 4));
            return;
        }
        gba_pf_step(g, 1);
        return;
    case 0x5:
    case 0x6:
        gba_pf_step(g, 1u << (size == 4));
        return;
    default:
        gba_pf_step(g, 1);
        return;
    }
}

static uint32_t dup(uint32_t v, unsigned size) { return size == 4 ? v : size == 2 ? v * 0x10001u : v * 0x01010101u; }

uint32_t gba_bus_read(rl_gba *g, gba_who who, uint32_t addr, unsigned size, bool seq)
{
    uint32_t v;
    if (is_rom(addr)) {
        bool burst = rom_seq(g, who, seq);
        if (who == GBA_DMA)
            g->dma_rom_burst = true;
        pf_reset(g);
        v = rom_timed(g, addr, size, burst);
        if (gba_eeprom_hit(g, addr))
            v = gba_eeprom_read(g, size);
    } else if (addr >= 0x0E000000u && addr < 0x10000000u) {
        pf_reset(g);
        gba_charge(g, wait_cart(g, addr, false));
        v = gba_mem_read(g, addr, size);
    } else {
        plain_cost(g, addr, size);
        v = gba_mem_read(g, addr, size);
    }
    g->mdr = dup(v, size);
    return v;
}

void gba_bus_write(rl_gba *g, gba_who who, uint32_t addr, unsigned size, uint32_t value, bool seq)
{
    if (is_rom(addr)) {
        bool burst = rom_seq(g, who, seq);
        if (who == GBA_DMA)
            g->dma_rom_burst = true;
        pf_reset(g);
        uint32_t a = size == 4 ? addr & ~3u : addr & ~1u;
        for (unsigned i = 0; i < (size == 4 ? 2u : 1u); i++, a += 2) {
            if (!burst)
                start_burst(g, a);
            gba_charge(g, wait_cart(g, a, burst));
            rom_burst_read(g, a);
            burst = true;
        }
        g->mdr = value;
        if (gba_eeprom_hit(g, addr))
            gba_eeprom_write(g, size, value);
        return;
    }
    if (addr >= 0x0E000000u && addr < 0x10000000u) {
        pf_reset(g);
        gba_charge(g, wait_cart(g, addr, false));
    } else {
        plain_cost(g, addr, size);
    }
    g->mdr = value;
    gba_mem_write(g, addr, size, value);
}

uint32_t rl_gba_fetch(rl_gba *g, uint32_t addr, unsigned size, bool seq)
{
    gba_dma_run(g);
    g->dma_overlap = 0;
    g->acc_start = gba_now(g);
    addr &= ~(size - 1);
    if (!is_rom(addr)) {
        uint32_t v = gba_bus_read(g, GBA_CPU, addr, size, seq);
        g->rom_burst = false;
        return v;
    }
    bool burst = g->rom_burst && seq;
    gba_prefetch *p = &g->pf;
    uint32_t v;
    if ((addr & 0x1FFFEu) && pf_on(g) && addr == p->addr && (!pf_empty(p) || p->ahead)) {
        gba_pf_step(g, 1);
        v = pf_read(g);
        v |= size == 4 ? (uint32_t)pf_read(g) << 16 : v << 16;
        g->mdr = v;
        return size == 4 ? v : v & 0xFFFFu;
    }
    pf_sync(g, size, addr);
    v = rom_timed(g, addr, size, burst);
    g->mdr = dup(v, size);
    return v;
}

void rl_gba_fetched(rl_gba *g, uint32_t addr, unsigned size)
{
    g->rom_burst = is_rom(addr);
    g->page_addr = (uint16_t)((addr + size) >> 1);
}

uint32_t rl_gba_load(rl_gba *g, uint32_t addr, unsigned size, bool seq)
{
    gba_dma_run(g);
    g->dma_overlap = 0;
    g->acc_start = gba_now(g);
    addr = gba_align(addr, size);
    uint32_t v = gba_bus_read(g, GBA_CPU, addr, size, seq);
    if (!is_rom(addr))
        g->rom_burst = false;
    return v;
}

void rl_gba_store(rl_gba *g, uint32_t addr, unsigned size, uint32_t value, bool seq)
{
    gba_dma_run(g);
    g->dma_overlap = 0;
    g->acc_start = gba_now(g);
    addr = gba_align(addr, size);
    gba_bus_write(g, GBA_CPU, addr, size, value, seq);
    if (!is_rom(addr))
        g->rom_burst = false;
    gba_after_write(g, addr, size);
}

void rl_gba_idle(rl_gba *g, unsigned cycles)
{
    while (cycles) {
        uint64_t t = gba_now(g);
        gba_dma_run(g);
        uint64_t dma = gba_now(g) - t;
        if (dma)
            g->dma_overlap = dma;
        if (g->dma_overlap) {
            g->dma_overlap--;
            g->acc_start = gba_now(g);
            cycles--;
            continue;
        }
        unsigned n = gba_dma_quiet(g, cycles) ? cycles : 1;
        g->acc_start = gba_now(g) + n - 1;
        gba_pf_step(g, n);
        cycles -= n;
    }
}

void rl_gba_service(rl_gba *g)
{
    gba_dma_run(g);
    if (g->halt_req || g->stop_req)
        gba_halt(g);
}
