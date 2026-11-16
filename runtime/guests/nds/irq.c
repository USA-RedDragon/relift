/**
 * @file irq.c
 * @brief The interrupt controller of each CPU: IME, IE and IF (acknowledged
 * by writing 1), the IRQ input that rises NDS_IRQ_DELAY ticks after a
 * request meets IME and IE, and the wake condition IE & IF (the scheduler
 * line that ends a halt, order.c).
 *
 * Fresh C from GBATEK ("DS Interrupts"). The delay of the IRQ input is the
 * spike's two ticks, not measured on a console yet.
 */
#include "nds_int.h"

void nds_irq_reset(rl_nds *m)
{
    for (unsigned cpu = 0; cpu < 2; cpu++)
        m->irq[cpu] = (nds_irq){.line_at = NDS_NEVER, .wake_at = NDS_NEVER};
}

void nds_irq_update(rl_nds *m, unsigned cpu, uint64_t tick)
{
    nds_irq *q = &m->irq[cpu];
    uint32_t hit = q->ie & q->iflag;
    bool woke = q->wake_at != NDS_NEVER;
    if (q->ime && hit) {
        if (q->line_at == NDS_NEVER)
            q->line_at = tick + NDS_IRQ_DELAY;
    } else {
        q->line_at = NDS_NEVER;
    }
    if (hit) {
        if (q->wake_at == NDS_NEVER)
            q->wake_at = tick + NDS_IRQ_DELAY;
    } else {
        q->wake_at = NDS_NEVER;
    }
    if (woke != (q->wake_at != NDS_NEVER))
        nds_wake_line(m, cpu);
}

void nds_irq_set(rl_nds *m, unsigned cpu, uint32_t bits, uint64_t tick)
{
    m->irq[cpu].iflag |= bits;
    nds_irq_update(m, cpu, tick);
}

uint32_t nds_irq_read(rl_nds *m, unsigned cpu, uint32_t off)
{
    nds_irq *q = &m->irq[cpu];
    switch (off) {
    case 0x208:
        return q->ime;
    case 0x210:
        return q->ie;
    case 0x214:
        return q->iflag;
    default:
        return 0;
    }
}

void nds_irq_write(rl_nds *m, unsigned cpu, uint32_t off, uint32_t v, uint32_t mask, uint64_t now)
{
    nds_irq *q = &m->irq[cpu];
    switch (off) {
    case 0x208:
        if (mask & 1)
            q->ime = v & 1;
        break;
    case 0x210:
        q->ie = (q->ie & ~mask) | (v & mask);
        break;
    case 0x214:
        q->iflag &= ~(v & mask);
        break;
    default:
        return;
    }
    nds_irq_update(m, cpu, now);
}

void rl_nds_request_irq(rl_nds *m, unsigned cpu, uint32_t sources)
{
    if (cpu < 2)
        nds_irq_set(m, cpu, sources, nds_now(m, cpu));
}

bool rl_nds_irq_line(const rl_nds *m, unsigned cpu)
{
    return cpu < 2 && m->irq[cpu].line_at <= nds_now(m, cpu);
}

uint64_t rl_nds_irq_at(const rl_nds *m, unsigned cpu) { return cpu < 2 ? m->irq[cpu].line_at : NDS_NEVER; }
