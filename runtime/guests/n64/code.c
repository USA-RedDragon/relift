/**
 * @file code.c
 * @brief Binding lifted code to RDRAM: every code run of the program starts
 * unchecked, with the entry check (the run layer's stub) in each of its
 * dispatch slots. The first entry into a run compares the FNV-1a hash of
 * its bytes in RDRAM with the hash it was lifted from; on a match the run's
 * functions take its slots. Several runs may be linked at one address (code
 * a game loads in turn); the check binds the one whose bytes are there now.
 * A run may also be listed a second time without the words at its ends
 * that only the sweep found (data a loader may not bring along); only the
 * functions that start inside the run that matched take slots.
 * A DMA into RDRAM (rl_n64_code_loaded, rl_n64_code_written) or an
 * instruction cache invalidation puts the stub back over the runs it
 * touches, so the next entry checks again. Nothing is interpreted: an entry
 * that no run matches is missed code.
 */
#include "n64_int.h"

#include "crash.h"

#include <stdio.h>
#include <stdlib.h>

/** @brief The physical address of a kseg0 or kseg1 address. */
static uint32_t phys(uint64_t a) { return (uint32_t)a & 0x1FFFFFFFu; }

int rl_n64_code_init(rl_n64 *m, const rl_unit *const *units, uint32_t unit_count, const rl_n64_code *code,
                     uint32_t count)
{
    m->code.units = units;
    m->code.unit_count = unit_count;
    m->code.code = code;
    m->code.count = count;
    m->code.state = calloc(count ? count : 1, 1);
    if (!m->code.state)
        return -1;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (code[i].unit >= unit_count)
            return -1;
        if (code[i].link < lo)
            lo = code[i].link;
        if (code[i].link + code[i].len > hi)
            hi = code[i].link + code[i].len;
    }
    for (uint32_t k = 0; k < unit_count; k++) {
        const rl_unit *u = units[k];
        if (u->abi_version != RL_ABI_VERSION || u->ctx_size != rl_core_cpu()->ctx_size) {
            rl_log(RL_LOG_ERROR, "n64", "unit %s was built for another runtime", u->name ? u->name : "?");
            return -1;
        }
        if (k == 0)
            rl_dispatch_set_image(u->image_id);
        if (u->map_count && rl_fn_maps_register(u->maps, u->map_count) != 0)
            return -1;
    }
    if (hi > lo && rl_dispatch_reserve(lo, hi) != 0)
        return -1;
    return 0;
}

void rl_n64_code_set_stub(rl_n64 *m, rl_fn entry_stub)
{
    m->code.stub = entry_stub;
    for (uint32_t i = 0; i < m->code.count; i++) {
        const rl_n64_code *r = &m->code.code[i];
        for (uint32_t k = 0; k < r->len; k += 4)
            rl_dispatch_set(r->link + k, entry_stub);
        m->code.state[i] = CODE_OUT;
    }
}

static bool overlaps(const rl_n64_code *r, uint32_t pa, uint32_t len)
{
    uint32_t p = phys(r->link);
    return p < pa + len && pa < p + r->len;
}

/** @brief Puts the stub back over run i. */
static void unbind(rl_n64 *m, uint32_t i)
{
    const rl_n64_code *r = &m->code.code[i];
    if (m->code.state[i] == CODE_LIVE)
        m->stats.invalidations++;
    m->code.state[i] = CODE_OUT;
    if (!m->code.stub)
        return;
    for (uint32_t k = 0; k < r->len; k += 4)
        rl_dispatch_set(r->link + k, m->code.stub);
}

void rl_n64_code_written(rl_n64 *m, uint32_t paddr, uint32_t len)
{
    if (!len)
        return;
    for (uint32_t i = 0; i < m->code.count; i++)
        if (m->code.state[i] != CODE_OUT && overlaps(&m->code.code[i], paddr, len))
            unbind(m, i);
}

void rl_n64_code_loaded(rl_n64 *m, uint32_t source, uint32_t offset, uint32_t dram, uint32_t len)
{
    if (!len)
        return;
    rl_n64_code_written(m, dram, len);
    for (uint32_t i = 0; i < m->code.count; i++) {
        const rl_n64_code *r = &m->code.code[i];
        uint32_t p = phys(r->link);
        if (r->source == source && p >= dram && p + r->len <= dram + len && r->offset - offset == p - dram) {
            m->code.state[i] = CODE_LOADED;
            m->stats.binds++;
        }
    }
}

static uint64_t fnv(const uint8_t *b, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

/** @brief Whether run i's bytes are in RDRAM now. */
static bool matches(rl_n64 *m, uint32_t i)
{
    const rl_n64_code *r = &m->code.code[i];
    uint32_t p = phys(r->link);
    if (p + r->len > RL_N64_RDRAM_SIZE)
        return false;
    return fnv(m->rdram + p, r->len) == r->hash;
}

static void bind(rl_n64 *m, uint32_t i)
{
    const rl_n64_code *r = &m->code.code[i];
    const rl_unit *u = m->code.units[r->unit];
    for (uint32_t j = 0; j < m->code.count; j++)
        if (j != i && m->code.state[j] == CODE_LIVE && overlaps(&m->code.code[j], phys(r->link), r->len))
            m->code.state[j] = CODE_OUT;
    uint64_t lo = r->link, hi = r->link + r->len;
    for (uint64_t a = lo; a < hi; a += 4)
        rl_dispatch_set(a, NULL);
    for (uint32_t k = 0; k < u->fn_count; k++) {
        const rl_fn_entry *e = &u->fns[k];
        if (e->guest_addr < lo || e->guest_addr >= hi)
            continue;
        uint64_t end = e->guest_addr + ((e->flags & RL_FN_RANGE) ? e->guest_size : 4u);
        for (uint64_t a = e->guest_addr; a < end && a < hi; a += 4)
            if (!rl_dispatch_find(a))
                rl_dispatch_set(a, e->fn);
    }
    for (uint64_t a = lo; a < hi; a += 4)
        if (!rl_dispatch_find(a))
            rl_dispatch_set(a, m->code.stub);
    m->code.state[i] = CODE_LIVE;
}

rl_fn rl_n64_code_check(rl_n64 *m, uint64_t pc, char *why, size_t n)
{
    uint32_t p = phys(pc);
    int found = 0;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < m->code.count; i++) {
            const rl_n64_code *r = &m->code.code[i];
            if (pc < r->link || pc >= r->link + r->len)
                continue;
            if ((pass == 0) != (m->code.state[i] == CODE_LOADED))
                continue;
            found++;
            if (!matches(m, i))
                continue;
            m->stats.checks++;
            bind(m, i);
            return rl_dispatch_find(pc);
        }
    m->stats.mismatches++;
    if (found)
        snprintf(why, n, "the code at 0x%08x is not any of the %d lifted runs linked there (their hashes differ)", p,
                 found);
    else
        snprintf(why, n, "no lifted run is linked at 0x%08x", p);
    return NULL;
}
