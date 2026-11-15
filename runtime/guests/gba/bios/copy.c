/**
 * @file copy.c
 * @brief CpuSet and CpuFastSet, with the BIOS's check that keeps them from
 * reading the BIOS region.
 */
#include "bios_int.h"

static bool source_blocked(uint32_t src, uint32_t bytes)
{
    if (bytes == 0)
        return true;
    uint32_t end = src + (bytes & 0x01FFFFFFu);
    return (src & 0x0E000000u) == 0 || (end & 0x0E000000u) == 0;
}

void bios_cpu_set(bios_call *c)
{
    bios_save(c, 4, 5, true);
    uint32_t src = c->r[0], dst = c->r[1], ctl = c->r[2];
    uint32_t bytes = (ctl << 11) >> 9;
    bool fill = (ctl >> 24) & 1u;
    bios_tick(c, 14);
    c->r[3] = RL_GBA_BIOS_SWI_RETURN;
    if (source_blocked(src, bytes)) {
        bios_tick(c, 6);
        return;
    }
    if ((ctl >> 26) & 1u) {
        int32_t end = (int32_t)(dst + bytes);
        if (fill) {
            uint32_t v = bios_ldm(c, src, false);
            src += 4;
            for (bool seq = false; (int32_t)dst < end; seq = true, dst += 4) {
                bios_st32(c, dst, v, seq);
                bios_tick(c, 5);
            }
        } else {
            for (bool seq = false; (int32_t)dst < end; seq = true, dst += 4, src += 4) {
                uint32_t v = bios_ldm(c, src, seq);
                bios_st32(c, dst, v, seq);
                bios_tick(c, 5);
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
            bios_st16(c, dst + (uint32_t)i, v, i != 0);
            bios_tick(c, 5);
        }
    } else {
        for (int32_t i = 0; i < len; i += 2) {
            uint32_t v = bios_ld16(c, src + (uint32_t)i, i != 0);
            bios_st16(c, dst + (uint32_t)i, v, i != 0);
            bios_tick(c, 5);
        }
    }
}

void bios_cpu_fast_set(bios_call *c)
{
    bios_save(c, 4, 10, true);
    uint32_t src = c->r[0], dst = c->r[1], ctl = c->r[2];
    uint32_t bytes = (ctl << 11) >> 9;
    bios_tick(c, 14);
    if (source_blocked(src, bytes)) {
        bios_tick(c, 4);
        return;
    }
    int32_t end = (int32_t)(dst + bytes);
    uint32_t w[8];
    if ((ctl >> 24) & 1u) {
        uint32_t v = bios_ld32(c, src, false);
        for (unsigned i = 0; i < 8; i++)
            w[i] = v;
        while ((int32_t)dst < end) {
            for (unsigned i = 0; i < 8; i++)
                bios_st32(c, dst + 4 * i, v, i != 0);
            dst += 32;
            bios_tick(c, 3);
        }
    } else {
        while ((int32_t)dst < end) {
            for (unsigned i = 0; i < 8; i++)
                w[i] = bios_ldm(c, src + 4 * i, i != 0);
            for (unsigned i = 0; i < 8; i++)
                bios_st32(c, dst + 4 * i, w[i], i != 0);
            src += 32;
            dst += 32;
            bios_tick(c, 4);
        }
        if (src == c->r[0])
            return;
    }
    c->r[0] = src;
    c->r[1] = dst;
    c->r[3] = w[1];
}
