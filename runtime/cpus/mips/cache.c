/**
 * @file cache.c
 * @brief The VR4300's caches as far as time goes: the tags of the 16 KiB
 * instruction cache (32-byte lines) and the 8 KiB write-back data cache
 * (16-byte lines, with a dirty bit), the cycles of a line fill and a write
 * back, and CACHE on both. Lifted code reads and writes guest memory
 * directly, so memory holds the bytes the CPU sees, its lines' included;
 * every change of a data line goes to the line hook (rl_mips_set_dline_hook),
 * which keeps the bytes the rest of the machine sees under the lines that
 * differ: a line's fill keeps memory's bytes, a dirty line written back
 * leaves memory as it is, and a line dropped gives its kept bytes back.
 *
 * Only kseg0 and TLB-mapped addresses are cached; kseg1 is not. A fill
 * takes 48 cycles for an instruction line and 40 for a data line, a write
 * back 40, and a data cache hit one cycle on top of the instruction's
 * issue cycle.
 *
 * The line rules, the CACHE operations and the cycles are ported from ares
 * (n64/cpu/cpu.hpp, dcache.cpp and interpreter-ipu.cpp), which carries this
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
#include "mips.h"

enum {
    IFILL = 48,
    DFILL = 40,
    WRITEBACK = 40,
    TAG = 0xFFFFF000u,
};

static rl_mips_dline_hook *g_dline_hook;

void rl_mips_set_dline_hook(rl_mips_dline_hook *hook) { g_dline_hook = hook; }

static void charge(rl_ctx *c, uint32_t n) { c->time.budget -= (int64_t)n; }

/** @brief Sets the data line at line to now, telling the line hook when it changes. */
static void dset(rl_ctx *c, uint32_t *line, uint32_t now, bool written_back)
{
    uint32_t old = *line;
    if (old == now)
        return;
    *line = now;
    if (g_dline_hook)
        g_dline_hook(c, (uint32_t)(line - c->dcache), old, now, written_back);
}

/** @brief The physical address of a cached vaddr, or false for kseg1 and an address that does not translate. */
static bool cached(const rl_ctx *c, uint64_t vaddr, uint32_t *paddr)
{
    uint32_t a = (uint32_t)vaddr;
    if ((uint64_t)(int64_t)(int32_t)a == vaddr) {
        if ((a >> 29) == 4u) {
            *paddr = a & 0x1FFFFFFFu;
            return true;
        }
        if ((a >> 29) == 5u)
            return false;
    }
    uint64_t pa;
    if (rl_mips_translate(c, vaddr, false, &pa) != RL_MIPS_XLAT_OK)
        return false;
    *paddr = (uint32_t)pa;
    return true;
}

static uint32_t *iline(rl_ctx *c, uint64_t vaddr) { return &c->icache[(vaddr >> 5) & (RL_MIPS_ICACHE_LINES - 1u)]; }

static uint32_t *dline(rl_ctx *c, uint64_t vaddr) { return &c->dcache[(vaddr >> 4) & (RL_MIPS_DCACHE_LINES - 1u)]; }

static bool hit(uint32_t line, uint32_t paddr) { return (line & RL_MIPS_LINE_VALID) && (line & TAG) == (paddr & TAG); }

void rl_mips_icache_fill(rl_ctx *c, uint64_t pc)
{
    uint32_t pa;
    if (!cached(c, pc, &pa))
        return;
    uint32_t *line = iline(c, pc);
    if (hit(*line, pa))
        return;
    *line = (pa & TAG) | RL_MIPS_LINE_VALID;
    charge(c, IFILL);
    rl_mips_get_stats()->icache_misses++;
}

/** @brief Fills the data line of vaddr with paddr's, writing a dirty line back first. */
static void dfill(rl_ctx *c, uint64_t vaddr, uint32_t paddr, uint32_t write)
{
    uint32_t *line = dline(c, vaddr);
    if ((*line & (RL_MIPS_LINE_VALID | RL_MIPS_LINE_DIRTY)) == (RL_MIPS_LINE_VALID | RL_MIPS_LINE_DIRTY)) {
        charge(c, WRITEBACK);
        rl_mips_get_stats()->dcache_writebacks++;
    }
    dset(c, line, (paddr & TAG) | RL_MIPS_LINE_VALID | (write ? RL_MIPS_LINE_DIRTY : 0u), true);
    charge(c, DFILL);
    rl_mips_get_stats()->dcache_misses++;
}

void rl_mips_dcache_access(rl_ctx *c, uint64_t vaddr, uint32_t write)
{
    uint32_t pa;
    if (!cached(c, vaddr, &pa))
        return;
    uint32_t *line = dline(c, vaddr);
    if (!hit(*line, pa)) {
        dfill(c, vaddr, pa, write);
        return;
    }
    *line |= write ? RL_MIPS_LINE_DIRTY : 0u;
    charge(c, 1);
}

/** @brief TagLo of a line's tag word: PTagLo (bits 27 to 8) and PState (bits 7 and 6, state when valid). */
static uint64_t tag_lo(uint32_t line, uint32_t state)
{
    return (uint64_t)(line & TAG) >> 4 | ((line & RL_MIPS_LINE_VALID) ? state << 6 : 0u);
}

/** @brief The tag word TagLo stores: its address, valid when PState bit 7 is set. */
static uint32_t from_tag_lo(uint64_t t) { return (uint32_t)(t << 4) & TAG; }

static bool tag_lo_valid(uint64_t t) { return (t >> 7) & 1u; }

void rl_mips_cache_op(rl_ctx *c, uint32_t op, uint64_t vaddr)
{
    uint32_t pa;
    if (!cached(c, vaddr, &pa)) {
        uint32_t a = (uint32_t)vaddr;
        if ((uint64_t)(int64_t)(int32_t)a != vaddr || (a >> 29) != 5u)
            return;
        pa = a & 0x1FFFFFFFu;
    }
    uint32_t *il = iline(c, vaddr);
    uint32_t *dl = dline(c, vaddr);
    switch (op & 31u) {
    case 0x00:
        *il = pa & TAG;
        break;
    case 0x04:
        c->cp0_tag_lo = tag_lo(*il, 2u);
        break;
    case 0x08:
        *il = from_tag_lo(c->cp0_tag_lo) | (tag_lo_valid(c->cp0_tag_lo) ? RL_MIPS_LINE_VALID : 0u);
        break;
    case 0x10:
        if (hit(*il, pa))
            *il &= ~RL_MIPS_LINE_VALID;
        break;
    case 0x14:
        *il = (pa & TAG) | RL_MIPS_LINE_VALID;
        charge(c, IFILL);
        rl_mips_get_stats()->icache_misses++;
        break;
    case 0x18:
        if (hit(*il, pa))
            charge(c, IFILL);
        break;
    case 0x01:
        if ((*dl & RL_MIPS_LINE_VALID) && (*dl & RL_MIPS_LINE_DIRTY)) {
            charge(c, WRITEBACK);
            rl_mips_get_stats()->dcache_writebacks++;
        }
        dset(c, dl, ((*dl & RL_MIPS_LINE_VALID) ? pa & TAG : *dl) & ~(uint32_t)(RL_MIPS_LINE_VALID | RL_MIPS_LINE_DIRTY),
             true);
        break;
    case 0x05:
        c->cp0_tag_lo = tag_lo(*dl, 3u);
        break;
    case 0x09:
        dset(c, dl,
             from_tag_lo(c->cp0_tag_lo) | (*dl & RL_MIPS_LINE_DIRTY) |
                 (tag_lo_valid(c->cp0_tag_lo) ? RL_MIPS_LINE_VALID : 0u),
             false);
        break;
    case 0x0D: {
        uint32_t dirty = *dl & RL_MIPS_LINE_DIRTY;
        if (!hit(*dl, pa)) {
            if ((*dl & (RL_MIPS_LINE_VALID | RL_MIPS_LINE_DIRTY)) == (RL_MIPS_LINE_VALID | RL_MIPS_LINE_DIRTY)) {
                charge(c, WRITEBACK);
                rl_mips_get_stats()->dcache_writebacks++;
            }
            dirty = 0;
        }
        dset(c, dl, (pa & TAG) | dirty | RL_MIPS_LINE_VALID, true);
        break;
    }
    case 0x11:
        if (hit(*dl, pa))
            dset(c, dl, *dl & ~RL_MIPS_LINE_VALID, false);
        break;
    case 0x15:
        if (hit(*dl, pa)) {
            if (*dl & RL_MIPS_LINE_DIRTY) {
                charge(c, WRITEBACK);
                rl_mips_get_stats()->dcache_writebacks++;
            }
            dset(c, dl, *dl & ~RL_MIPS_LINE_VALID, true);
        }
        break;
    case 0x19:
        if (hit(*dl, pa) && (*dl & RL_MIPS_LINE_DIRTY)) {
            charge(c, WRITEBACK);
            rl_mips_get_stats()->dcache_writebacks++;
            dset(c, dl, *dl & ~RL_MIPS_LINE_DIRTY, true);
        }
        break;
    default:
        break;
    }
}
