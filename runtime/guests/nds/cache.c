/**
 * @file cache.c
 * @brief The ARM9's caches and write buffer as costs: the tags of the 8 KiB
 * instruction cache and the 4 KiB data cache (four ways, 32-byte lines,
 * round-robin or pseudo-random replacement), line fills, write-back of
 * dirty lines, the eight-entry write buffer, the protection unit's
 * cacheable and bufferable bits per region, and the CP15 c7 cache
 * operations.
 *
 * Only timing is modeled: the bytes stay in memory, so a line the CPU did
 * not clean before another bus master wrote memory under it reads the new
 * bytes, where the console would read the stale line. Lockdown (c9, c0) is
 * kept as a register and does not pin lines.
 *
 * Fresh C from GBATEK ("DS Memory Timings", "ARM CP15 Cache Control") and
 * the ARM946E-S Technical Reference Manual (cache organization, write
 * buffer, the Ctt and Btt table of region attributes).
 */
#include "nds_int.h"

#include <string.h>

/** @brief Bytes per line, and the line of an address. */
#define LINE 32u
#define WAYS 4u
/** @brief Ticks of a hit (half a bus cycle, GBATEK). */
#define HIT_TICKS 1u
/**
 * @brief Ticks main RAM is busy with a line fill, and the fill from the BIOS
 * (GBATEK: 23 and 11 bus cycles). The ARM9 waits 41 ticks after its own cycle
 * for a load that misses (a loop of one such LDR, SUBS and BNE takes 46, the
 * access rounded to the bus clock) and 39 for code: straight code takes 38
 * more per line than when it hits (4 to 16 KiB loops right after an
 * invalidate).
 */
#define FILL_MAIN_TICKS 46u
#define FILL_DATA_MAIN_TICKS 41u
#define FILL_CODE_MAIN_TICKS 39u
#define FILL_BIOS_TICKS 22u

/** @brief The instruction or the data cache. */
static nds_cache_side *side(rl_nds *m, bool code) { return code ? &m->cache.i : &m->cache.d; }

static unsigned sets_of(bool code) { return code ? NDS_ICACHE_SETS : NDS_DCACHE_SETS; }

void nds_cache_reset(rl_nds *m)
{
    memset(&m->cache, 0, sizeof m->cache);
    m->cache.lfsr = 1;
    nds_cache_mpu(m);
}

void nds_cache_mpu(rl_nds *m)
{
    const rl_arm32_cp15 *p = &m->cp15;
    memset(m->cache.attr, 0, sizeof m->cache.attr);
    for (unsigned r = 0; r < 8; r++) {
        uint32_t reg = p->region[r];
        if (!(reg & 1))
            continue;
        unsigned n = (reg >> 1) & 0x1F;
        uint64_t size = n < 11 ? 0x1000u : (uint64_t)1 << (n + 1);
        uint64_t base = reg & 0xFFFFF000u & ~(uint32_t)(size - 1);
        uint8_t a = (uint8_t)(((p->icache_bits >> r) & 1u ? NDS_ATTR_IC : 0) | ((p->dcache_bits >> r) & 1u ? NDS_ATTR_DC : 0) |
                              ((p->write_buffer >> r) & 1u ? NDS_ATTR_WB : 0));
        uint64_t end = base + size > 0x100000000ull ? 0x100000000ull : base + size;
        memset(m->cache.attr + (base >> 12), a, (size_t)((end - base) >> 12));
    }
}

unsigned nds_cache_attr(const rl_nds *m, uint32_t addr)
{
    uint32_t c = m->cp15.control;
    if (!(c & RL_CP15_MPU))
        return 0;
    unsigned a = m->cache.attr[addr >> 12];
    if (!(c & RL_CP15_ICACHE))
        a &= ~(unsigned)NDS_ATTR_IC;
    if (!(c & RL_CP15_DCACHE))
        a &= ~(unsigned)NDS_ATTR_DC;
    return a;
}

/** @brief The way of set s that holds tag t, or -1. */
static int find(const nds_cache_side *c, unsigned s, uint32_t t)
{
    for (unsigned w = 0; w < WAYS; w++)
        if (c->valid[s] >> w & 1u && c->tag[s][w] == t)
            return (int)w;
    return -1;
}

/**
 * @brief The way a fill of set s replaces. Round robin (control bit 14): one
 * counter for the whole cache, stepped by every fill, picks the way even when
 * another way of the set is invalid, so lines 64 sets apart replace each
 * other in a loop over more than the cache's 64 sets right after an
 * invalidate. The console's cold loops over 4 to 16 KiB miss every line, every
 * pass; the TRM leaves the counter's scope open. Otherwise a pseudo-random way,
 * after any invalid one.
 */
static unsigned victim(rl_nds *m, nds_cache_side *c, unsigned s)
{
    if (m->cp15.control & RL_CP15_ROUND_ROBIN) {
        unsigned w = c->next;
        c->next = (uint8_t)((w + 1) % WAYS);
        return w;
    }
    for (unsigned w = 0; w < WAYS; w++)
        if (!(c->valid[s] >> w & 1u))
            return w;
    uint32_t x = m->cache.lfsr;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m->cache.lfsr = x;
    return x % WAYS;
}

/** @brief Ticks of one bus burst of eight words at addr (a line fill or a write-back). */
static unsigned burst(const rl_nds *m, uint32_t addr)
{
    unsigned r = addr >> 24;
    if (r == 0x02)
        return FILL_MAIN_TICKS;
    if (r == 0xFF)
        return FILL_BIOS_TICKS;
    return nds_cost(m, RL_NDS_ARM9, RL_NDS_DATA, addr, 4, false) + 7u * nds_cost(m, RL_NDS_ARM9, RL_NDS_DATA, addr, 4, true);
}

/** @brief Moves the ARM9 to the bus clock edge and waits until the write buffer is empty. */
static void drain(rl_nds *m)
{
    uint64_t now = nds_now(m, RL_NDS_ARM9);
    if (now & 1)
        now++;
    uint64_t done = m->cache.wb_last > now ? m->cache.wb_last : now;
    nds_charge(m, RL_NDS_ARM9, done - nds_now(m, RL_NDS_ARM9));
    m->cache.wb_n = 0;
}

/** @brief Retires the write buffer entries done by tick now. */
static void retire(rl_nds *m, uint64_t now)
{
    nds_cache_state *c = &m->cache;
    while (c->wb_n && c->wb_done[c->wb_head] <= now) {
        c->wb_head = (uint8_t)((c->wb_head + 1) % NDS_WB_ENTRIES);
        c->wb_n--;
    }
}

void nds_cache_buffer(rl_nds *m, uint32_t addr, unsigned ticks)
{
    nds_cache_state *c = &m->cache;
    uint64_t now = nds_now(m, RL_NDS_ARM9);
    retire(m, now);
    if (c->wb_n == NDS_WB_ENTRIES) {
        nds_charge(m, RL_NDS_ARM9, c->wb_done[c->wb_head] - now);
        now = nds_now(m, RL_NDS_ARM9);
        retire(m, now);
    }
    uint64_t start = c->wb_last > now ? c->wb_last : now;
    if (start & 1)
        start++;
    if (addr >> 24 == 0x02 && nds_ram_busy(m) > start)
        start = nds_ram_busy(m);
    uint64_t done = start + ticks;
    if (addr >> 24 == 0x02) {
        m->ram_free = done;
        m->ram_side = NDS_RAM_CPU9;
    }
    c->wb_done[(c->wb_head + c->wb_n) % NDS_WB_ENTRIES] = done;
    c->wb_n++;
    c->wb_last = done;
}

void nds_cache_before_bus(rl_nds *m)
{
    if (m->cache.wb_n || m->cache.wb_last > nds_now(m, RL_NDS_ARM9))
        drain(m);
}

void nds_cache_read(rl_nds *m, uint32_t addr, bool code, bool seq)
{
    nds_cache_side *c = side(m, code);
    unsigned s = (addr / LINE) % sets_of(code);
    uint32_t t = addr / LINE / sets_of(code);
    if (find(c, s, t) >= 0) {
        if (code || seq)
            nds_charge(m, RL_NDS_ARM9, HIT_TICKS);
        return;
    }
    unsigned w = victim(m, c, s);
    if (!code && c->dirty[s] >> w & 1u) {
        uint32_t old = (c->tag[s][w] * sets_of(code) + s) * LINE;
        nds_cache_buffer(m, old, burst(m, old));
    }
    drain(m);
    if (addr >> 24 == 0x02) {
        nds_order(m, RL_NDS_ARM9);
        nds_ram_wait(m, RL_NDS_ARM9);
    }
    uint64_t now = nds_now(m, RL_NDS_ARM9);
    unsigned ticks = burst(m, addr);
    if (addr >> 24 == 0x02) {
        ticks = code ? FILL_CODE_MAIN_TICKS : FILL_DATA_MAIN_TICKS;
        uint64_t busy = nds_ram_busy(m);
        uint64_t start = now > busy ? now : busy;
        m->ram_free = start + FILL_MAIN_TICKS;
        m->ram_side = NDS_RAM_CPU9;
        nds_charge(m, RL_NDS_ARM9, start + ticks - now - (code ? 0u : 1u));
    } else {
        nds_charge(m, RL_NDS_ARM9, ticks);
    }
    c->tag[s][w] = t;
    c->valid[s] |= (uint8_t)(1u << w);
    c->dirty[s] &= (uint8_t)~(1u << w);
}

bool nds_cache_write_hit(rl_nds *m, uint32_t addr, bool write_back, bool seq)
{
    nds_cache_side *c = &m->cache.d;
    unsigned s = (addr / LINE) % NDS_DCACHE_SETS;
    int w = find(c, s, addr / LINE / NDS_DCACHE_SETS);
    if (w < 0)
        return false;
    if (!write_back)
        return false;
    c->dirty[s] |= (uint8_t)(1u << w);
    if (seq)
        nds_charge(m, RL_NDS_ARM9, HIT_TICKS);
    return true;
}

/** @brief Drops the line of addr from cache side c, writing it back first when clean is set and it is dirty. */
static void line_op(rl_nds *m, bool code, uint32_t addr, bool clean, bool invalidate)
{
    nds_cache_side *c = side(m, code);
    unsigned s = (addr / LINE) % sets_of(code);
    int w = find(c, s, addr / LINE / sets_of(code));
    if (w < 0)
        return;
    if (clean && c->dirty[s] >> w & 1u) {
        nds_cache_buffer(m, addr & ~(LINE - 1), burst(m, addr));
        c->dirty[s] &= (uint8_t)~(1u << w);
    }
    if (invalidate)
        c->valid[s] &= (uint8_t)~(1u << w);
}

/** @brief The same by set and way (the index form: way in bits 30 to 31, set from bit 5). */
static void index_op(rl_nds *m, uint32_t v, bool invalidate)
{
    nds_cache_side *c = &m->cache.d;
    unsigned s = (v / LINE) % NDS_DCACHE_SETS, w = v >> 30;
    if (c->valid[s] >> w & 1u && c->dirty[s] >> w & 1u) {
        uint32_t old = (c->tag[s][w] * NDS_DCACHE_SETS + s) * LINE;
        nds_cache_buffer(m, old, burst(m, old));
        c->dirty[s] &= (uint8_t)~(1u << w);
    }
    if (invalidate)
        c->valid[s] &= (uint8_t)~(1u << w);
}

void nds_cache_op(rl_nds *m, uint32_t key, uint32_t v)
{
    uint32_t crn = key >> 7 & 15, crm = key >> 3 & 15, opc2 = key & 7;
    if (crn != 7 || (key >> 11 & 7) != 0)
        return;
    switch (crm << 3 | opc2) {
    case 5 << 3 | 0:
        memset(&m->cache.i, 0, sizeof m->cache.i);
        break;
    case 5 << 3 | 1:
        line_op(m, true, v, false, true);
        break;
    case 6 << 3 | 0:
        memset(&m->cache.d, 0, sizeof m->cache.d);
        break;
    case 6 << 3 | 1:
        line_op(m, false, v, false, true);
        break;
    case 10 << 3 | 1:
        line_op(m, false, v, true, false);
        break;
    case 10 << 3 | 2:
        index_op(m, v, false);
        break;
    case 10 << 3 | 4:
        nds_cache_before_bus(m);
        break;
    case 14 << 3 | 1:
        line_op(m, false, v, true, true);
        break;
    case 14 << 3 | 2:
        index_op(m, v, true);
        break;
    default:
        break;
    }
}
