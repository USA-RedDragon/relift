/**
 * @file ndst.c
 * @brief The test fixture of the NDS machine.
 */
#include "ndst.h"

#include <stdlib.h>

ndst *ndst_new(void)
{
    static ndst t;
    rl_sched_config cfg = {.ordered = true};
    t.s = rl_sched_create(&cfg);
    t.k[RL_NDS_ARM9] = rl_sched_add_cpu(t.s, 1);
    t.k[RL_NDS_ARM7] = rl_sched_add_cpu(t.s, 2);
    t.m = rl_nds_create();
    if (!t.m || rl_nds_attach(t.m, t.s, t.k[RL_NDS_ARM9], t.k[RL_NDS_ARM7]) != 0) {
        fprintf(stderr, "ndst: cannot build the machine\n");
        abort();
    }
    return &t;
}

void ndst_to(ndst *t, unsigned cpu, uint64_t tick)
{
    uint64_t now = rl_sched_now(t->k[cpu]);
    if (tick <= now)
        return;
    uint64_t ratio = cpu == RL_NDS_ARM9 ? 1 : 2;
    rl_sched_stall(t->k[cpu], (tick - now + ratio - 1) / ratio);
}

void ndst_both(ndst *t, uint64_t tick)
{
    ndst_to(t, RL_NDS_ARM9, tick);
    ndst_to(t, RL_NDS_ARM7, tick);
}
