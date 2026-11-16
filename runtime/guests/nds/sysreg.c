/**
 * @file sysreg.c
 * @brief The ARM7's small system registers: POWCNT2 (speakers and Wifi
 * power), WIFIWAITCNT (16 bits, reachable only while Wifi is powered; it holds the 0x30 the oracle shows), BIOSPROT (reads
 * the value the boot code leaves, ignores writes) and the sound controller's
 * registers as a register file with the readback masks of GBATEK (channel
 * control, SOUNDCNT, SOUNDBIAS, capture control and destination; the address,
 * timer, loop point and length registers read zero). No audio is made here.
 *
 * Fresh C from GBATEK ("DS Power Control", "DS Sound Control Registers",
 * "DS Sound Capture", "DS Memory Control - BIOS"); the readback masks agree
 * with what the oracle returns for all-ones writes.
 */
#include "nds_int.h"

#include <string.h>

enum {
    BIOSPROT_VALUE = 0x1204,
    CH_CNT_MASK = 0xFF7F837Fu,
    SAD_MASK = 0x07FFFFFCu,
    LEN_MASK = 0x3FFFFFu,
    CAP_CNT_MASK = 0x8F,
    CAP_DAD_MASK = 0x07FFFFFCu,
    SOUNDCNT_MASK = 0xBF7F,
    BIAS_MASK = 0x3FF,
};

void nds_sys_reset(rl_nds *m)
{
    memset(&m->snd, 0, sizeof m->snd);
    m->snd.bias = 0x200;
    m->powcnt2 = 1;
    m->wifiwait = 0x30;
}

bool nds_sys_has(uint32_t off)
{
    return off == 0x304 || off == 0x308 || (off >= 0x400 && off < 0x520);
}

static uint32_t merge(uint32_t old, uint32_t v, uint32_t mask) { return (old & ~mask) | (v & mask); }

uint32_t nds_sys_read(rl_nds *m, uint32_t off)
{
    const nds_sound *s = &m->snd;
    if (off == 0x304)
        return m->powcnt2;
    if (off == 0x308)
        return BIOSPROT_VALUE;
    if (off < 0x500)
        return (off & 0xF) == 0 ? s->ch[(off - 0x400) >> 4].cnt : 0;
    switch (off) {
    case 0x500:
        return s->soundcnt;
    case 0x504:
        return s->bias;
    case 0x508:
        return (uint32_t)s->capcnt[0] | (uint32_t)s->capcnt[1] << 8;
    case 0x510:
        return s->capdad[0];
    case 0x518:
        return s->capdad[1];
    default:
        return 0;
    }
}

void nds_sys_write(rl_nds *m, uint32_t off, uint32_t v, uint32_t mask)
{
    nds_sound *s = &m->snd;
    if (off == 0x304) {
        m->powcnt2 = (uint8_t)(merge(m->powcnt2, v, mask) & 3);
        return;
    }
    if (off == 0x308)
        return;
    if (off < 0x500) {
        nds_snd_ch *c = &s->ch[(off - 0x400) >> 4];
        switch (off & 0xF) {
        case 0:
            c->cnt = merge(c->cnt, v, mask) & CH_CNT_MASK;
            break;
        case 4:
            c->sad = merge(c->sad, v, mask) & SAD_MASK;
            break;
        case 8:
            c->tmr = (uint16_t)merge(c->tmr, v, mask);
            c->pnt = (uint16_t)merge(c->pnt, v >> 16, mask >> 16);
            break;
        default:
            c->len = merge(c->len, v, mask) & LEN_MASK;
            break;
        }
        return;
    }
    switch (off) {
    case 0x500:
        s->soundcnt = (uint16_t)(merge(s->soundcnt, v, mask) & SOUNDCNT_MASK);
        break;
    case 0x504:
        s->bias = (uint16_t)(merge(s->bias, v, mask) & BIAS_MASK);
        break;
    case 0x508:
        s->capcnt[0] = (uint8_t)(merge(s->capcnt[0], v, mask) & CAP_CNT_MASK);
        s->capcnt[1] = (uint8_t)(merge(s->capcnt[1], v >> 8, mask >> 8) & CAP_CNT_MASK);
        break;
    case 0x510:
        s->capdad[0] = merge(s->capdad[0], v, mask) & CAP_DAD_MASK;
        break;
    case 0x514:
        s->caplen[0] = merge(s->caplen[0], v, mask) & 0xFFFF;
        break;
    case 0x518:
        s->capdad[1] = merge(s->capdad[1], v, mask) & CAP_DAD_MASK;
        break;
    case 0x51C:
        s->caplen[1] = merge(s->caplen[1], v, mask) & 0xFFFF;
        break;
    default:
        break;
    }
}
