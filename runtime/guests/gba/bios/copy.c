/**
 * @file copy.c
 * @brief CpuSet and CpuFastSet, with the BIOS's check that keeps them from
 * reading the BIOS region.
 */
#include "bios_int.h"

static bool source_blocked(bios_call *c, uint32_t src, uint32_t bytes)
{
    if (bytes == 0) {
        bios_ev(c, BE_ZERO, 1);
        return true;
    }
    uint32_t end = src + (bytes & 0x01FFFFFFu);
    bool blocked = (src & 0x0E000000u) == 0 || (end & 0x0E000000u) == 0;
    if (blocked)
        bios_ev(c, BE_REFUSED, 1);
    return blocked;
}

void bios_cpu_set(bios_call *c)
{
    bios_save(c, 4, 5, true);
    uint32_t src = c->r[0], dst = c->r[1], ctl = c->r[2];
    uint32_t bytes = (ctl << 11) >> 9;
    bool fill = (ctl >> 24) & 1u;
    c->r[3] = RL_GBA_BIOS_SWI_RETURN;
    if (source_blocked(c, src, bytes)) {
        return;
    }
    bios_ev(c, (ctl >> 26) & 1u ? BE_SET_W_CALL : BE_SET_H_CALL, 1);
    if ((ctl >> 26) & 1u) {
        int32_t end = (int32_t)(dst + bytes);
        if (fill) {
            uint32_t v = bios_ldm(c, src, false);
            src += 4;
            for (; (int32_t)dst < end; dst += 4) {
                bios_st32(c, dst, v, false);
                bios_ev(c, BE_SET_W_FILL, 1);
            }
        } else {
            for (; (int32_t)dst < end; dst += 4, src += 4) {
                uint32_t v = bios_ldm(c, src, false);
                bios_st32(c, dst, v, false);
                bios_ev(c, BE_SET_W_COPY, 1);
            }
        }
        c->r[0] = src;
        c->r[1] = dst;
        return;
    }
    int32_t len = (int32_t)(bytes >> 1);
    if (fill) {
        uint32_t v = bios_ld16(c, src, false);
        for (int32_t i = 0; i < len; i += 2) {
            bios_st16(c, dst + (uint32_t)i, v, false);
            bios_ev(c, BE_SET_H_FILL, 1);
        }
    } else {
        for (int32_t i = 0; i < len; i += 2) {
            uint32_t v = bios_ld16(c, src + (uint32_t)i, false);
            bios_st16(c, dst + (uint32_t)i, v, false);
            bios_ev(c, BE_SET_H_COPY, 1);
        }
    }
}

void bios_cpu_fast_set(bios_call *c)
{
    bios_save(c, 4, 10, true);
    uint32_t src = c->r[0], dst = c->r[1], ctl = c->r[2];
    uint32_t bytes = (ctl << 11) >> 9;
    if (source_blocked(c, src, bytes)) {
        return;
    }
    bios_ev(c, BE_FAST_CALL, 1);
    int32_t end = (int32_t)(dst + bytes);
    uint32_t w[8];
    if ((ctl >> 24) & 1u) {
        uint32_t v = bios_ld32(c, src, false);
        bios_ev(c, BE_FAST_FILL_START, 1);
        for (unsigned i = 0; i < 8; i++)
            w[i] = v;
        while ((int32_t)dst < end) {
            for (unsigned i = 0; i < 8; i++)
                bios_st32(c, dst + 4 * i, v, i != 0);
            dst += 32;
            bios_ev(c, BE_FAST_FILL, 1);
        }
    } else {
        while ((int32_t)dst < end) {
            for (unsigned i = 0; i < 8; i++)
                w[i] = bios_ldm(c, src + 4 * i, i != 0);
            for (unsigned i = 0; i < 8; i++)
                bios_st32(c, dst + 4 * i, w[i], i != 0);
            src += 32;
            dst += 32;
            bios_ev(c, BE_FAST_COPY, 1);
        }
        if (src == c->r[0])
            return;
    }
    c->r[0] = src;
    c->r[1] = dst;
    c->r[3] = w[1];
}
