/**
 * @file tlb.c
 * @brief The VR4300 TLB (32 entries, TLBR, TLBWI, TLBWR, TLBP), Random, and
 * address translation per segment and mode with its exceptions.
 */
#include "mips.h"

/** @brief VPN2 bits of EntryHi (39 to 13) and its R field (63 and 62). */
#define VPN2_MASK UINT64_C(0x000000FFFFFFE000)
#define REGION_MASK UINT64_C(0xC000000000000000)
#define ASID_MASK UINT64_C(0xFF)

/** @brief The cycle the CPU is at. */
static uint64_t cycle(const rl_ctx *c) { return c->time.deadline - (uint64_t)c->time.budget; }

uint64_t rl_mips_random(rl_ctx *c)
{
    uint64_t now = cycle(c);
    if ((uint32_t)c->cp0_wired != c->wired_seen) {
        c->wired_seen = (uint32_t)c->cp0_wired;
        c->wired_cycle = now;
    }
    uint64_t n = now - c->wired_cycle;
    uint64_t wired = c->cp0_wired & 63u;
    if (wired > 31u)
        return (31u - n) & 63u;
    return 31u - n % (32u - wired);
}

/** @brief The odd-page select bit and the offset mask of a page mask. */
static void page_bits(uint64_t page_mask, uint64_t *offset, uint64_t *select)
{
    *offset = (page_mask | 0x1FFFu) >> 1;
    *select = *offset + 1u;
}

/** @brief Whether entry e maps vaddr under ASID asid. */
static bool entry_matches(const rl_mips_tlb_entry *e, uint64_t vaddr, uint64_t asid)
{
    uint64_t hi_mask = VPN2_MASK & ~e->page_mask;
    if ((vaddr & hi_mask) != (e->entry_hi & hi_mask))
        return false;
    if ((vaddr & REGION_MASK) != (e->entry_hi & REGION_MASK))
        return false;
    bool global = (e->entry_lo[0] & 1u) != 0;
    return global || (e->entry_hi & ASID_MASK) == asid;
}

static rl_mips_xlat tlb_lookup(const rl_ctx *c, uint64_t vaddr, bool write, uint64_t *paddr)
{
    uint64_t asid = c->cp0_entry_hi & ASID_MASK;
    for (unsigned i = 0; i < RL_MIPS_TLB_ENTRIES; i++) {
        const rl_mips_tlb_entry *e = &c->tlb[i];
        if (!entry_matches(e, vaddr, asid))
            continue;
        uint64_t offset, select;
        page_bits(e->page_mask, &offset, &select);
        uint64_t lo = e->entry_lo[(vaddr & select) ? 1 : 0];
        if (!(lo & 2u))
            return RL_MIPS_XLAT_INVALID;
        if (write && !(lo & 4u))
            return RL_MIPS_XLAT_MODIFIED;
        *paddr = (((lo >> 6) & 0xFFFFFFu) << 12) + (vaddr & offset);
        *paddr &= 0xFFFFFFFFu;
        return RL_MIPS_XLAT_OK;
    }
    return RL_MIPS_XLAT_MISS;
}

/** @brief The privilege of the current mode: 0 kernel, 1 supervisor, 2 user. */
static unsigned mode_of(const rl_ctx *c)
{
    uint32_t st = c->cp0_status;
    if (st & (RL_MIPS_SR_EXL | RL_MIPS_SR_ERL))
        return 0;
    unsigned ksu = st >> 3 & 3u;
    return ksu > 2u ? 2u : ksu;
}

/** @brief Whether the current mode uses 64-bit addressing (KX, SX or UX). */
static bool wide_mode(const rl_ctx *c)
{
    static const uint32_t bit[3] = {RL_MIPS_SR_KX, RL_MIPS_SR_SX, RL_MIPS_SR_UX};
    return (c->cp0_status & bit[mode_of(c)]) != 0;
}

/** @brief The xkphys windows of ares's segment table: bits 63 to 59 select one of eight cache attributes. */
static bool xkphys(uint64_t vaddr, uint64_t *paddr)
{
    if ((vaddr >> 62) != 2u)
        return false;
    if (vaddr & UINT64_C(0x07FFFFFF00000000))
        return false;
    *paddr = vaddr & 0xFFFFFFFFu;
    return true;
}

rl_mips_xlat rl_mips_translate(const rl_ctx *c, uint64_t vaddr, bool write, uint64_t *paddr)
{
    unsigned mode = mode_of(c);
    if (!wide_mode(c)) {
        if ((uint64_t)(int64_t)(int32_t)(uint32_t)vaddr != vaddr)
            return RL_MIPS_XLAT_ADDRESS;
        uint32_t a = (uint32_t)vaddr;
        unsigned seg = a >> 29;
        if (mode == 0) {
            if (seg == 4 || seg == 5) {
                *paddr = a & 0x1FFFFFFFu;
                return RL_MIPS_XLAT_OK;
            }
            if (seg < 4 && (c->cp0_status & RL_MIPS_SR_ERL)) {
                *paddr = a;
                return RL_MIPS_XLAT_OK;
            }
            return tlb_lookup(c, vaddr, write, paddr);
        }
        if (seg < 4 || (mode == 1 && seg == 6))
            return tlb_lookup(c, vaddr, write, paddr);
        return RL_MIPS_XLAT_ADDRESS;
    }
    bool xuseg = vaddr <= UINT64_C(0x000000FFFFFFFFFF);
    bool xsseg = vaddr >= UINT64_C(0x4000000000000000) && vaddr <= UINT64_C(0x400000FFFFFFFFFF);
    bool csseg = vaddr >= UINT64_C(0xFFFFFFFFC0000000) && vaddr <= UINT64_C(0xFFFFFFFFDFFFFFFF);
    if (mode == 2)
        return xuseg ? tlb_lookup(c, vaddr, write, paddr) : RL_MIPS_XLAT_ADDRESS;
    if (mode == 1)
        return (xuseg || xsseg || csseg) ? tlb_lookup(c, vaddr, write, paddr) : RL_MIPS_XLAT_ADDRESS;
    if (xuseg && (c->cp0_status & RL_MIPS_SR_ERL) && vaddr <= 0x7FFFFFFFu) {
        *paddr = vaddr;
        return RL_MIPS_XLAT_OK;
    }
    if (xuseg || xsseg)
        return tlb_lookup(c, vaddr, write, paddr);
    if (xkphys(vaddr, paddr))
        return RL_MIPS_XLAT_OK;
    if (vaddr >= UINT64_C(0xC000000000000000) && vaddr <= UINT64_C(0xC00000FF7FFFFFFF))
        return tlb_lookup(c, vaddr, write, paddr);
    if (vaddr >= UINT64_C(0xFFFFFFFF80000000) && vaddr <= UINT64_C(0xFFFFFFFFBFFFFFFF)) {
        *paddr = vaddr & 0x1FFFFFFFu;
        return RL_MIPS_XLAT_OK;
    }
    if (vaddr >= UINT64_C(0xFFFFFFFFC0000000))
        return tlb_lookup(c, vaddr, write, paddr);
    return RL_MIPS_XLAT_ADDRESS;
}

uint64_t rl_mips_xlat_exception(rl_ctx *c, rl_mips_xlat why, bool write, uint64_t vaddr, uint64_t pc,
                                bool in_delay)
{
    c->cp0_bad_vaddr = vaddr;
    c->cp0_context = (c->cp0_context & ~UINT64_C(0x7FFFF0)) | ((vaddr >> 13) & 0x7FFFFu) << 4;
    c->cp0_xcontext = (c->cp0_xcontext & ~UINT64_C(0x1FFFFFFF0)) | ((vaddr >> 13) & 0x7FFFFFFu) << 4 |
                      ((vaddr >> 62) & 3u) << 31;
    c->cp0_entry_hi = (c->cp0_entry_hi & ASID_MASK) | (vaddr & (VPN2_MASK | REGION_MASK));
    uint32_t code;
    switch (why) {
    case RL_MIPS_XLAT_ADDRESS:
        code = write ? RL_MIPS_EXC_ADES : RL_MIPS_EXC_ADEL;
        break;
    case RL_MIPS_XLAT_MODIFIED:
        code = RL_MIPS_EXC_MOD;
        break;
    default:
        code = write ? RL_MIPS_EXC_TLBS : RL_MIPS_EXC_TLBL;
        break;
    }
    bool refill = why == RL_MIPS_XLAT_MISS && !(c->cp0_status & RL_MIPS_SR_EXL);
    bool wide = wide_mode(c);
    uint64_t v = rl_mips_exception(c, code, pc, in_delay, 0);
    if (refill)
        v = (v & ~UINT64_C(0x1FF)) | (wide ? 0x080u : 0x000u);
    return v;
}

/** @brief Writes EntryHi, EntryLo0, EntryLo1 and PageMask into entry i (TLBWI, TLBWR). */
static void tlb_write(rl_ctx *c, unsigned i)
{
    rl_mips_tlb_entry *e = &c->tlb[i];
    uint64_t pm = c->cp0_page_mask & (UINT64_C(0xAAA) << 13);
    pm |= pm >> 1;
    uint64_t g = c->cp0_entry_lo0 & c->cp0_entry_lo1 & 1u;
    e->page_mask = pm;
    e->entry_hi = c->cp0_entry_hi & (REGION_MASK | (VPN2_MASK & ~pm) | ASID_MASK);
    e->entry_lo[0] = (c->cp0_entry_lo0 & ~UINT64_C(1)) | g;
    e->entry_lo[1] = (c->cp0_entry_lo1 & ~UINT64_C(1)) | g;
}

void rl_mips_tlb(rl_ctx *c, uint32_t op, uint32_t run)
{
    if (!(run & 1u))
        return;
    switch (op & 3u) {
    case 0: {
        unsigned i = (unsigned)(c->cp0_index & 63u);
        if (i >= RL_MIPS_TLB_ENTRIES)
            return;
        const rl_mips_tlb_entry *e = &c->tlb[i];
        c->cp0_page_mask = e->page_mask;
        c->cp0_entry_hi = e->entry_hi;
        c->cp0_entry_lo0 = e->entry_lo[0];
        c->cp0_entry_lo1 = e->entry_lo[1];
        break;
    }
    case 1: {
        unsigned i = (unsigned)(c->cp0_index & 63u);
        if (i < RL_MIPS_TLB_ENTRIES)
            tlb_write(c, i);
        break;
    }
    case 2: {
        unsigned i = (unsigned)rl_mips_random(c);
        if (i < RL_MIPS_TLB_ENTRIES)
            tlb_write(c, i);
        break;
    }
    default: {
        uint64_t asid = c->cp0_entry_hi & ASID_MASK;
        c->cp0_index = UINT64_C(0x80000000);
        for (unsigned i = 0; i < RL_MIPS_TLB_ENTRIES; i++) {
            const rl_mips_tlb_entry *e = &c->tlb[i];
            uint64_t m = VPN2_MASK & ~e->page_mask;
            if ((e->entry_hi & m) != (c->cp0_entry_hi & m))
                continue;
            if ((e->entry_hi & REGION_MASK) != (c->cp0_entry_hi & REGION_MASK))
                continue;
            if (!(e->entry_lo[0] & 1u) && (e->entry_hi & ASID_MASK) != asid)
                continue;
            c->cp0_index = i;
            break;
        }
        break;
    }
    }
}
