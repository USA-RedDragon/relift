/**
 * @file sysreg.c
 * @brief The ARM7's small system registers: POWCNT2 (speakers and Wifi
 * power), WIFIWAITCNT (16 bits, reachable only while Wifi is powered; it holds the 0x30 the oracle shows) and BIOSPROT (reads
 * the value the boot code leaves, ignores writes). The sound controller's
 * registers beside them go to sound.c.
 *
 * Fresh C from GBATEK ("DS Power Control", "DS Memory Control - BIOS").
 */
#include "nds_int.h"

enum { BIOSPROT_VALUE = 0x1204 };

void nds_sys_reset(rl_nds *m)
{
    m->powcnt2 = 1;
    m->wifiwait = 0x30;
}

bool nds_sys_has(uint32_t off)
{
    return off == 0x304 || off == 0x308 || (off >= 0x400 && off < 0x520);
}

uint32_t nds_sys_read(rl_nds *m, uint32_t off, uint64_t now)
{
    if (off == 0x304)
        return m->powcnt2;
    if (off == 0x308)
        return BIOSPROT_VALUE;
    return nds_snd_read(m, off, now);
}

void nds_sys_write(rl_nds *m, uint32_t off, uint32_t v, uint32_t mask, uint64_t now)
{
    if (off == 0x304) {
        nds_snd_run(m, now);
        m->powcnt2 = (uint8_t)(((m->powcnt2 & ~mask) | (v & mask)) & 3);
        return;
    }
    if (off == 0x308)
        return;
    nds_snd_write(m, off, v, mask, now);
}
