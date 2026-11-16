/**
 * @file lcd.c
 * @brief The display timing of both CPUs: VCOUNT and each CPU's DISPSTAT
 * flags as functions of the tick (355 dots of 12 ticks per line, 263 lines),
 * and the line edges as one scheduler event: the VBlank, HBlank and VCOUNT
 * match interrupts of each CPU. Only edges that request an interrupt are
 * posted.
 *
 * Fresh C from GBATEK ("DS Video Stuff", DISPSTAT/VCOUNT), shaped like the
 * GBA layer's lcd.c. Measured on the oracle with relift's timing ROM: the
 * display starts at line 0 dot 0 when both CPUs start the cartridge, the
 * HBlank flag and interrupt come 1,584 bus cycles after the line starts
 * (GBATEK gives 1,606 for the flag), VBlank and VCOUNT match at the line
 * start.
 */
#include "nds_int.h"

enum { E_LINE = 0, E_HBLANK = NDS_HBLANK_TICKS };

uint64_t nds_lcd_pos(const rl_nds *m, uint64_t t)
{
    uint64_t o = m->lcd.origin % RL_NDS_FRAME_TICKS;
    return (t + RL_NDS_FRAME_TICKS - o) % RL_NDS_FRAME_TICKS;
}

/** @brief The 9-bit LYC of cpu's DISPSTAT. */
static unsigned lyc(const nds_lcd *l, unsigned cpu) { return (unsigned)(l->stat[cpu] >> 8) | (unsigned)(l->stat[cpu] & 0x80) << 1; }

/** @brief The interrupts cpu's edge (line, dot) requests. */
static uint32_t requests(const rl_nds *m, unsigned cpu, unsigned line, unsigned dot)
{
    const nds_lcd *l = &m->lcd;
    uint16_t s = l->stat[cpu];
    uint32_t r = 0;
    if (dot == E_LINE) {
        if (line == NDS_VISIBLE && (s & 0x08))
            r |= RL_NDS_IRQ_VBLANK;
        if ((s & 0x20) && line == lyc(l, cpu))
            r |= RL_NDS_IRQ_VCOUNT;
    } else if (s & 0x10) {
        r |= RL_NDS_IRQ_HBLANK;
    }
    return r;
}

static void repost_from(rl_nds *m, uint64_t now)
{
    if (!m->sched)
        return;
    static const unsigned edges[2] = {E_LINE, E_HBLANK};
    uint64_t p = nds_lcd_pos(m, now);
    unsigned line = (unsigned)(p / RL_NDS_LINE_TICKS), dot = (unsigned)(p % RL_NDS_LINE_TICKS);
    for (unsigned k = 0; k <= RL_NDS_LINES; k++) {
        unsigned ln = (line + k) % RL_NDS_LINES;
        for (unsigned e = 0; e < 2; e++) {
            if (k == 0 && edges[e] <= dot)
                continue;
            if (requests(m, RL_NDS_ARM9, ln, edges[e]) || requests(m, RL_NDS_ARM7, ln, edges[e])) {
                uint64_t when = now + (uint64_t)k * RL_NDS_LINE_TICKS + edges[e] - dot;
                if (!rl_event_queued(&m->lcd.ev) || m->lcd.ev.when != when)
                    rl_sched_post(m->sched, &m->lcd.ev, when);
                return;
            }
        }
    }
    rl_sched_cancel(m->sched, &m->lcd.ev);
}

static void fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    rl_nds *m = e->opaque;
    uint64_t p = nds_lcd_pos(m, when);
    unsigned line = (unsigned)(p / RL_NDS_LINE_TICKS), dot = (unsigned)(p % RL_NDS_LINE_TICKS);
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        uint32_t r = requests(m, cpu, line, dot);
        if (r)
            nds_irq_set(m, cpu, r, when);
    }
    repost_from(m, when);
}

void nds_lcd_reset(rl_nds *m)
{
    nds_lcd *l = &m->lcd;
    l->origin = 0;
    l->stat[0] = l->stat[1] = 0;
    l->ev = (rl_event){.fire = fire, .opaque = m};
}

void nds_lcd_attach(rl_nds *m)
{
    m->lcd.origin = nds_now(m, RL_NDS_ARM9);
    repost_from(m, m->lcd.origin);
}

uint32_t nds_lcd_read(const rl_nds *m, unsigned cpu, uint64_t now)
{
    uint64_t p = nds_lcd_pos(m, now);
    unsigned line = (unsigned)(p / RL_NDS_LINE_TICKS), dot = (unsigned)(p % RL_NDS_LINE_TICKS);
    uint32_t v = m->lcd.stat[cpu] & 0xFFB8u;
    if (line >= NDS_VISIBLE && line < RL_NDS_LINES - 1)
        v |= 1;
    if (dot >= NDS_HBLANK_TICKS)
        v |= 2;
    if (line == lyc(&m->lcd, cpu))
        v |= 4;
    return v | (uint32_t)line << 16;
}

void nds_lcd_write(rl_nds *m, unsigned cpu, uint32_t v, uint32_t mask, uint64_t now)
{
    nds_lcd *l = &m->lcd;
    uint16_t keep = (uint16_t)~(mask & 0xFFB8u);
    l->stat[cpu] = (uint16_t)((l->stat[cpu] & keep) | (v & mask & 0xFFB8u));
    if (mask & 0x01FF0000u) {
        uint64_t p = nds_lcd_pos(m, now);
        unsigned line = (unsigned)(p / RL_NDS_LINE_TICKS);
        unsigned want = (unsigned)(((line & ~(mask >> 16)) | ((v & mask) >> 16)) & 0x1FF);
        if (want < RL_NDS_LINES && want != line) {
            uint64_t q = (uint64_t)want * RL_NDS_LINE_TICKS + p % RL_NDS_LINE_TICKS;
            l->origin = (now + RL_NDS_FRAME_TICKS - q % RL_NDS_FRAME_TICKS) % RL_NDS_FRAME_TICKS;
        }
    }
    repost_from(m, now);
}

unsigned rl_nds_vcount(const rl_nds *m) { return (unsigned)(nds_lcd_pos(m, nds_now(m, RL_NDS_ARM9)) / RL_NDS_LINE_TICKS); }
