/**
 * @file order.c
 * @brief The two CPUs on one timeline (spike 20-nds q3, design E): every
 * shared access waits until the other CPU is past it in the order (tick,
 * rank, index), with EXMEMCNT bit 15 choosing which CPU wins a tie; HALT
 * takes a CPU out of the order until IE & IF wakes it at the later of its
 * own tick and the request's tick plus the IRQ delay; interrupts are
 * delivered at block boundaries.
 */
#include "nds_int.h"

void nds_order(rl_nds *m, unsigned cpu)
{
    if (m->sched)
        rl_sched_order(m->cpu[cpu]);
}

void nds_ranks(rl_nds *m)
{
    if (!m->sched)
        return;
    bool arm7_first = m->exmem[RL_NDS_ARM9] & 0x8000;
    rl_sched_set_rank(m->cpu[RL_NDS_ARM9], arm7_first ? 1 : 0);
    rl_sched_set_rank(m->cpu[RL_NDS_ARM7], arm7_first ? 0 : 1);
}

/** @brief Moves cpu's clock forward to tick (rounded up to its cycle). */
static void move_to(rl_nds *m, unsigned cpu, uint64_t tick)
{
    uint64_t now = nds_now(m, cpu);
    if (tick > now)
        nds_charge(m, cpu, tick - now);
}

void nds_wake_line(rl_nds *m, unsigned cpu)
{
    if (!m->sched)
        return;
    nds_irq *q = &m->irq[cpu];
    uint32_t line = cpu == RL_NDS_ARM9 ? RL_NDS_LINE_WAKE9 : RL_NDS_LINE_WAKE7;
    if (q->wake_at == NDS_NEVER) {
        rl_sched_lower(m->sched, line);
        return;
    }
    if (m->halted[cpu])
        move_to(m, cpu, q->wake_at);
    rl_sched_raise(m->sched, line);
}

int nds_order_attach(rl_nds *m)
{
    rl_sched_route(m->sched, RL_NDS_LINE_WAKE9, rl_sched_index(m->cpu[RL_NDS_ARM9]));
    rl_sched_route(m->sched, RL_NDS_LINE_WAKE7, rl_sched_index(m->cpu[RL_NDS_ARM7]));
    rl_sched_set_mask(m->cpu[RL_NDS_ARM9], 1ull << RL_NDS_LINE_WAKE9);
    rl_sched_set_mask(m->cpu[RL_NDS_ARM7], 1ull << RL_NDS_LINE_WAKE7);
    nds_ranks(m);
    return 0;
}

bool rl_nds_halt(rl_nds *m, unsigned cpu)
{
    if (!m->sched || cpu > 1)
        return false;
    nds_order(m, cpu);
    nds_irq *q = &m->irq[cpu];
    if (!q->ie)
        return false;
    if (q->wake_at == NDS_NEVER) {
        m->halted[cpu] = true;
        int got = rl_sched_halt(m->cpu[cpu]);
        m->halted[cpu] = false;
        if (!got)
            return false;
    }
    move_to(m, cpu, q->wake_at);
    return true;
}

bool rl_nds_irq_poll(rl_nds *m, unsigned cpu)
{
    nds_irq *q = &m->irq[cpu];
    if (!m->sched || !q->ime || !q->ie)
        return false;
    rl_sched_cpu *k = m->cpu[cpu];
    rl_sched_order(k);
    if (rl_sched_clock(k)->budget <= 0)
        rl_sched_run(k);
    return q->line_at <= rl_sched_now(k);
}
