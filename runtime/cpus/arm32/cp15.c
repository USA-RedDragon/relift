/**
 * @file cp15.c
 * @brief The ARM946E-S system control coprocessor: register reads and
 * writes by lifter key, the TCM areas and the protection regions.
 */
#include "cp15.h"

#include <string.h>

#define CONTROL_FIXED 0x00000078u
#define CONTROL_WRITABLE 0x000FF085u
#define CONTROL_TCM (RL_CP15_DTCM_ON | RL_CP15_DTCM_LOAD | RL_CP15_ITCM_ON | RL_CP15_ITCM_LOAD)
#define REGION_MASK 0xFFFFF03Fu
#define DTCM_MASK 0xFFFFF03Eu
#define ITCM_MASK 0x0000003Eu

void rl_arm32_cp15_reset(rl_arm32_cp15 *p)
{
    memset(p, 0, sizeof *p);
    p->control = CONTROL_FIXED;
}

/** @brief The two-bit permissions (c5, c0, 0 and 1) of the four-bit ones. */
static uint32_t perm_short(uint32_t ext)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 8; i++)
        v |= ((ext >> (4 * i)) & 3u) << (2 * i);
    return v;
}

static uint32_t perm_long(uint32_t v)
{
    uint32_t ext = 0;
    for (unsigned i = 0; i < 8; i++)
        ext |= ((v >> (2 * i)) & 3u) << (4 * i);
    return ext;
}

uint32_t rl_arm32_cp15_read(const rl_arm32_cp15 *p, uint32_t key)
{
    if (!(key >> 14 & 1))
        return 0;
    uint32_t opc1 = key >> 11 & 7, crn = key >> 7 & 15, crm = key >> 3 & 15, opc2 = key & 7;
    if (opc1 != 0)
        return 0;
    switch (crn) {
    case 0:
        if (crm != 0)
            return 0;
        switch (opc2) {
        case 1:
            return 0x0F0D2112u;
        case 2:
            return 0x00140180u;
        default:
            return 0x41059461u;
        }
    case 1:
        return crm == 0 && opc2 == 0 ? p->control : 0;
    case 2:
        if (crm != 0)
            return 0;
        return opc2 == 0 ? p->dcache_bits : opc2 == 1 ? p->icache_bits : 0;
    case 3:
        return crm == 0 && opc2 == 0 ? p->write_buffer : 0;
    case 5:
        if (crm != 0)
            return 0;
        switch (opc2) {
        case 0:
            return perm_short(p->data_perm);
        case 1:
            return perm_short(p->insn_perm);
        case 2:
            return p->data_perm;
        case 3:
            return p->insn_perm;
        default:
            return 0;
        }
    case 6:
        return opc2 <= 1 ? p->region[crm & 7] : 0;
    case 9:
        if (crm == 0)
            return opc2 == 0 ? p->dcache_lock : opc2 == 1 ? p->icache_lock : 0;
        if (crm == 1)
            return opc2 == 0 ? p->dtcm : opc2 == 1 ? p->itcm : 0;
        return 0;
    case 13:
        return opc2 == 1 ? p->trace_id : 0;
    default:
        return 0;
    }
}

unsigned rl_arm32_cp15_write(rl_arm32_cp15 *p, uint32_t key, uint32_t v)
{
    if (!(key >> 14 & 1))
        return RL_CP15_UNDEFINED;
    uint32_t opc1 = key >> 11 & 7, crn = key >> 7 & 15, crm = key >> 3 & 15, opc2 = key & 7;
    if (opc1 != 0)
        return 0;
    switch (crn) {
    case 1: {
        if (crm != 0 || opc2 != 0)
            return 0;
        uint32_t old = p->control;
        p->control = (v & CONTROL_WRITABLE) | CONTROL_FIXED;
        uint32_t d = old ^ p->control;
        unsigned r = 0;
        if (d & CONTROL_TCM)
            r |= RL_CP15_CHANGED_TCM;
        if (d & RL_CP15_HIGH_VECTORS)
            r |= RL_CP15_CHANGED_VECTORS;
        if (d & (RL_CP15_MPU | RL_CP15_DCACHE | RL_CP15_ICACHE))
            r |= RL_CP15_CHANGED_MPU;
        if (p->control & (RL_CP15_BIG_ENDIAN | RL_CP15_PRE_V5))
            r |= RL_CP15_UNSUPPORTED;
        return r;
    }
    case 2:
        if (crm != 0)
            return 0;
        if (opc2 == 0)
            p->dcache_bits = v & 0xFFu;
        else if (opc2 == 1)
            p->icache_bits = v & 0xFFu;
        else
            return 0;
        return RL_CP15_CHANGED_MPU;
    case 3:
        if (crm != 0 || opc2 != 0)
            return 0;
        p->write_buffer = v & 0xFFu;
        return RL_CP15_CHANGED_MPU;
    case 5:
        if (crm != 0)
            return 0;
        switch (opc2) {
        case 0:
            p->data_perm = perm_long(v);
            break;
        case 1:
            p->insn_perm = perm_long(v);
            break;
        case 2:
            p->data_perm = v;
            break;
        case 3:
            p->insn_perm = v;
            break;
        default:
            return 0;
        }
        return RL_CP15_CHANGED_MPU;
    case 6:
        if (opc2 > 1)
            return 0;
        p->region[crm & 7] = v & REGION_MASK;
        return RL_CP15_CHANGED_MPU;
    case 7:
        if ((crm == 0 && opc2 == 4) || (crm == 8 && opc2 == 2))
            return RL_CP15_WAIT;
        return 0;
    case 9:
        if (crm == 0) {
            if (opc2 == 0)
                p->dcache_lock = v;
            else if (opc2 == 1)
                p->icache_lock = v;
            return 0;
        }
        if (crm == 1) {
            if (opc2 == 0)
                p->dtcm = v & DTCM_MASK;
            else if (opc2 == 1)
                p->itcm = v & ITCM_MASK;
            else
                return 0;
            return RL_CP15_CHANGED_TCM;
        }
        return 0;
    case 13:
        if (opc2 == 1)
            p->trace_id = v;
        return 0;
    default:
        return 0;
    }
}

uint32_t rl_arm32_cp15_vectors(const rl_arm32_cp15 *p)
{
    return p->control & RL_CP15_HIGH_VECTORS ? 0xFFFF0000u : 0;
}

/** @brief 512 shifted left by the size field (bits 1 to 5), at most 4 GiB. */
static uint64_t tcm_size(uint32_t reg)
{
    unsigned n = reg >> 1 & 31u;
    return n > 23 ? (uint64_t)1 << 32 : (uint64_t)512 << n;
}

void rl_arm32_cp15_tcm(const rl_arm32_cp15 *p, rl_arm32_tcm *out)
{
    out->itcm_on = !!(p->control & RL_CP15_ITCM_ON);
    out->itcm_load = !!(p->control & RL_CP15_ITCM_LOAD);
    out->itcm_size = tcm_size(p->itcm);
    out->dtcm_on = !!(p->control & RL_CP15_DTCM_ON);
    out->dtcm_load = !!(p->control & RL_CP15_DTCM_LOAD);
    out->dtcm_size = tcm_size(p->dtcm);
    out->dtcm_base = (uint32_t)(p->dtcm & 0xFFFFF000u & ~(out->dtcm_size - 1));
}

int rl_arm32_cp15_region(const rl_arm32_cp15 *p, uint32_t addr)
{
    for (int i = 7; i >= 0; i--) {
        uint32_t r = p->region[i];
        if (!(r & 1))
            continue;
        uint64_t size = (uint64_t)2 << (r >> 1 & 31u);
        uint64_t base = r & 0xFFFFF000u & ~(size - 1);
        if ((uint64_t)addr - base < size)
            return i;
    }
    return -1;
}
