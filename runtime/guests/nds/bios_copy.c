/**
 * @file bios_copy.c
 * @brief CpuSet, CpuFastSet, BitUnPack and the difference unfilters.
 *
 * Fresh C from GBATEK ("BIOS Memory Copy", "BIOS Decompression Functions");
 * the structure and the refusal rules follow relift's GBA BIOS
 * (runtime/guests/gba/bios/copy.c, decomp.c), whose routines the DS BIOS
 * shares, except that CpuFastSet takes any word count (GBATEK: 4-byte blocks)
 * and, from its programming bug, runs without sequential accesses. The
 * ARM7's routines refuse a source in the BIOS region, the ARM9's
 * do not (GBATEK "BIOS Memory Copy").
 */
#include "bios_int.h"

/** @brief True when cpu's call refuses a source at src: the ARM7's BIOS never reads its own region for the caller. */
static bool refuses(const nds_bios_env *e, uint32_t src)
{
    return e->cpu == RL_NDS_ARM7 && (src >> 25) == 0;
}

/** @brief CpuSet/CpuFastSet's check on the ARM7: a zero length, or a start or end in the BIOS region, does nothing. */
static bool source_blocked(const nds_bios_env *e, uint32_t src, uint32_t bytes)
{
    if (bytes == 0)
        return true;
    if (e->cpu != RL_NDS_ARM7)
        return false;
    uint32_t end = src + (bytes & 0x01FFFFFFu);
    return (src & 0x0E000000u) == 0 || (end & 0x0E000000u) == 0;
}

void nds_bios_cpu_set(const nds_bios_env *e, uint32_t r[4])
{
    uint32_t src = r[0], dst = r[1], ctl = r[2];
    uint32_t bytes = (ctl << 11) >> 9;
    bool fill = (ctl >> 24) & 1u;
    if (source_blocked(e, src, bytes))
        return;
    if ((ctl >> 26) & 1u) {
        int32_t end = (int32_t)(dst + bytes);
        uint32_t v = fill ? nds_bios_ld(e, src, 4, false) : 0;
        for (; (int32_t)dst < end; dst += 4, src += 4) {
            if (!fill)
                v = nds_bios_ld(e, src, 4, false);
            nds_bios_st(e, dst, 4, v, false);
        }
        return;
    }
    int32_t len = (int32_t)(bytes >> 1);
    uint32_t v = fill ? nds_bios_ld(e, src, 2, false) : 0;
    for (int32_t i = 0; i < len; i += 2) {
        if (!fill)
            v = nds_bios_ld(e, src + (uint32_t)i, 2, false);
        nds_bios_st(e, dst + (uint32_t)i, 2, v, false);
    }
}

void nds_bios_cpu_fast_set(const nds_bios_env *e, uint32_t r[4])
{
    uint32_t src = r[0], dst = r[1], ctl = r[2];
    uint32_t words = ctl & 0x1FFFFFu;
    bool fill = (ctl >> 24) & 1u;
    if (source_blocked(e, src, words * 4u))
        return;
    uint32_t v = fill ? nds_bios_ld(e, src, 4, false) : 0;
    for (uint32_t i = 0; i < words; i++) {
        if (!fill)
            v = nds_bios_ld(e, src + 4u * i, 4, false);
        nds_bios_st(e, dst + 4u * i, 4, v, false);
    }
}

void nds_bios_bit_unpack(const nds_bios_env *e, uint32_t r[4])
{
    uint32_t src = r[0], dst = r[1], info = r[2];
    int32_t len = (int32_t)nds_bios_ld(e, info, 2, false);
    uint32_t sw = nds_bios_ld(e, info + 2, 1, false), dw = nds_bios_ld(e, info + 3, 1, true);
    uint32_t off = nds_bios_ld(e, info + 4, 4, true);
    if (len == 0 || refuses(e, src))
        return;
    bool zero = off >> 31;
    off &= 0x7FFFFFFFu;
    uint32_t shift = (8u - sw) & 0xFFu;
    uint32_t mask = shift >= 32 ? 0 : 0xFFu >> shift, buf = 0, bits = 0;
    while (--len >= 0) {
        uint32_t data = nds_bios_ld(e, src++, 1, false);
        for (uint32_t sb = 0; sb < 8; sb += sw) {
            uint32_t v = (data >> sb) & mask;
            if (v != 0 || zero)
                v += off;
            buf |= v << bits;
            bits = (bits + dw) & 31u;
            if (bits == 0) {
                nds_bios_st(e, dst, 4, buf, false);
                dst += 4;
                buf = 0;
            }
        }
    }
}

void nds_bios_diff8(const nds_bios_env *e, uint32_t r[4])
{
    uint32_t src = r[0], dst = r[1];
    int32_t len = (int32_t)(nds_bios_ld(e, src, 4, false) >> 8);
    src += 4;
    if (len == 0 || refuses(e, src))
        return;
    uint32_t v = nds_bios_ld(e, src++, 1, false);
    for (;;) {
        nds_bios_st(e, dst++, 1, v, false);
        if (--len <= 0)
            break;
        v = (v + nds_bios_ld(e, src++, 1, false)) & 0xFFu;
    }
}

void nds_bios_diff16(const nds_bios_env *e, uint32_t r[4])
{
    uint32_t src = r[0], dst = r[1];
    int32_t len = (int32_t)(nds_bios_ld(e, src, 4, false) >> 8);
    src += 4;
    if (len == 0 || refuses(e, src))
        return;
    uint32_t v = nds_bios_ld(e, src, 2, false);
    src += 2;
    for (;;) {
        nds_bios_st(e, dst, 2, v, false);
        dst += 2;
        len -= 2;
        if (len <= 0)
            break;
        v = (v + nds_bios_ld(e, src, 2, false)) & 0xFFFFu;
        src += 2;
    }
}
