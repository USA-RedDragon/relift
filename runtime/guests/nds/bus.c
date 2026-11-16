/**
 * @file bus.c
 * @brief Timed accesses of the CPUs: the TCM fast path, the bus clock edge,
 * main RAM contention between the two CPUs and the cost of each access.
 *
 * Fresh C from GBATEK ("DS Memory Timings": external accesses of the ARM9
 * are delayed to the bus clock; one CPU at a time is on the main memory
 * bus, EXMEMCNT bit 15 says which one wins a tie).
 */
#include "nds_int.h"

void nds_charge(rl_nds *m, unsigned cpu, uint64_t ticks)
{
    if (!m->cpu[cpu] || ticks == 0)
        return;
    rl_sched_stall(m->cpu[cpu], cpu == RL_NDS_ARM9 ? ticks : (ticks + 1) / 2);
}

static unsigned cost(const rl_nds *m, unsigned cpu, unsigned kind, uint32_t addr, unsigned size, bool seq)
{
    unsigned r = addr >> 24;
    return m->costs.t[cpu][kind][r > 14 ? 15 : r][size >> 1][seq];
}

/** @brief Moves cpu to the next bus cycle edge (an even tick). */
static void edge(rl_nds *m, unsigned cpu)
{
    if (cpu == RL_NDS_ARM9 && (nds_now(m, cpu) & 1))
        nds_charge(m, cpu, 1);
}

static void finish(rl_nds *m, unsigned cpu, bool main, unsigned ticks)
{
    if (!main) {
        nds_charge(m, cpu, ticks);
        return;
    }
    uint64_t now = nds_now(m, cpu);
    uint64_t start = now > m->ram_free ? now : m->ram_free;
    m->ram_free = start + ticks;
    nds_charge(m, cpu, m->ram_free - now);
}

static uint32_t bus_read(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, nds_where w)
{
    switch (w.kind) {
    case NDS_K_MEM:
        return nds_ld(w.p, size);
    case NDS_K_IO:
        if (m->sched)
            return (uint32_t)rl_sched_read(m->cpu[cpu], nds_io_key(cpu, addr), size);
        return nds_io_read(m, cpu, addr, size, 0);
    case NDS_K_SLOT:
        return nds_slot_read(m, cpu, addr, size);
    default:
        return 0;
    }
}

static uint32_t timed_read(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, bool seq, bool code)
{
    addr &= ~(size - 1);
    nds_where w = nds_decode(m, cpu, addr, code, false);
    if (w.tcm) {
        nds_charge(m, cpu, 1);
        return nds_ld(w.p, size);
    }
    edge(m, cpu);
    w = nds_decode(m, cpu, addr, code, false);
    uint32_t v = bus_read(m, cpu, addr, size, w);
    finish(m, cpu, w.main, cost(m, cpu, code ? RL_NDS_CODE : RL_NDS_DATA, addr, size, seq && !(code && cpu == RL_NDS_ARM9)));
    return v;
}

uint32_t rl_nds_fetch(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, bool seq)
{
    return timed_read(m, cpu, addr, size, seq, true);
}

uint32_t rl_nds_load(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, bool seq)
{
    return timed_read(m, cpu, addr, size, seq, false);
}

void rl_nds_store(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value, bool seq)
{
    addr &= ~(size - 1);
    value &= nds_mask(size);
    nds_where w = nds_decode(m, cpu, addr, false, true);
    if (w.tcm) {
        nds_charge(m, cpu, 1);
        nds_st(w.p, size, value);
        return;
    }
    edge(m, cpu);
    w = nds_decode(m, cpu, addr, false, true);
    if (w.kind == NDS_K_MEM && w.p)
        nds_st(w.p, size, value);
    else if (w.kind == NDS_K_IO) {
        if (m->sched)
            rl_sched_write(m->cpu[cpu], nds_io_key(cpu, addr), size, value);
        else
            nds_io_write(m, cpu, addr, size, value, 0);
    }
    finish(m, cpu, w.main, cost(m, cpu, RL_NDS_DATA, addr, size, seq));
}

void rl_nds_idle(rl_nds *m, unsigned cpu, unsigned ticks) { nds_charge(m, cpu, ticks); }
