/**
 * @file keypad.c
 * @brief KEYINPUT and KEYCNT of both CPUs (KEYCNT reads back all 16 bits as
 * written, as the oracle's does; only bits 0 to 9, 14 and 15 act), the keypad
 * interrupt condition (any or all selected keys), EXTKEYIN of the ARM7 (X, Y,
 * pen down, hinge) and the host's input hook.
 *
 * Fresh C from GBATEK ("DS Keypad", "GBA Keypad Input"); the readback of
 * KEYCNT and EXTKEYIN's idle bits were read from the oracle.
 */
#include "nds_int.h"

enum { KEY_MASK = 0x3FF, CNT_IRQ = 0x4000, CNT_AND = 0x8000 };

static uint16_t released(const rl_nds *m) { return (uint16_t)(~m->pad.in.keys & KEY_MASK); }

static bool condition(const rl_nds *m, unsigned cpu)
{
    uint16_t cnt = m->pad.cnt[cpu], mask = cnt & KEY_MASK, down = m->pad.in.keys & KEY_MASK;
    if (!(cnt & CNT_IRQ))
        return false;
    if (cnt & CNT_AND)
        return (down & mask) == mask;
    return (down & mask) != 0;
}

static void evaluate(rl_nds *m, unsigned cpu, uint64_t tick)
{
    if (condition(m, cpu))
        nds_irq_set(m, cpu, RL_NDS_IRQ_KEYPAD, tick);
}

uint16_t nds_extkeyin(const rl_nds *m)
{
    const rl_nds_input *in = &m->pad.in;
    uint16_t v = 0x7F;
    if (in->x)
        v &= (uint16_t)~1u;
    if (in->y)
        v &= (uint16_t)~2u;
    if (in->touch)
        v &= (uint16_t)~0x40u;
    if (in->lid_closed)
        v |= 0x80;
    return v;
}

uint32_t nds_keypad_read(rl_nds *m, unsigned cpu, uint32_t off)
{
    if (off == 0x130)
        return released(m) | (uint32_t)m->pad.cnt[cpu] << 16;
    return cpu == RL_NDS_ARM7 ? (uint32_t)m->rcnt | (uint32_t)nds_extkeyin(m) << 16 : 0;
}

void nds_keypad_write(rl_nds *m, unsigned cpu, uint32_t v, uint32_t mask, uint64_t now)
{
    uint32_t mk = mask >> 16;
    m->pad.cnt[cpu] = (uint16_t)((m->pad.cnt[cpu] & ~mk) | ((v >> 16) & mk));
    evaluate(m, cpu, now);
}

void rl_nds_set_input(rl_nds *m, const rl_nds_input *in)
{
    uint16_t before = m->pad.in.keys;
    m->pad.in = *in;
    m->pad.in.keys &= KEY_MASK;
    if (m->pad.in.keys == before)
        return;
    for (unsigned cpu = 0; cpu < 2; cpu++)
        evaluate(m, cpu, nds_now(m, cpu));
}
