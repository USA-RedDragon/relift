/**
 * @file ndst.c
 * @brief The test fixture of the NDS machine.
 */
#include "ndst.h"
#include "scene.h"

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

uint32_t ndst_scene(ndst *t, const uint32_t *p, uint32_t *out, uint32_t max)
{
    uint32_t n = 0;
    for (;;) {
        uint32_t op = *p++;
        if (op == OP_END)
            return n;
        uint32_t a = *p++;
        switch (op) {
        case OP_W32:
        case OP_W16:
        case OP_W8:
            rl_nds_write(t->m, RL_NDS_ARM9, a, op == OP_W32 ? 4 : op == OP_W16 ? 2 : 1, *p++);
            break;
        case OP_FILL32:
        case OP_FILL16: {
            uint32_t c = *p++, v = *p++, size = op == OP_FILL32 ? 4 : 2;
            for (uint32_t i = 0; i < c; i++)
                rl_nds_write(t->m, RL_NDS_ARM9, a + size * i, size, v);
            break;
        }
        case OP_RAMP16: {
            uint32_t c = *p++, v = *p++, d = *p++;
            for (uint32_t i = 0; i < c; i++)
                rl_nds_write(t->m, RL_NDS_ARM9, a + 2 * i, 2, v + d * i);
            break;
        }
        case OP_COPY32: {
            uint32_t c = *p++;
            for (uint32_t i = 0; i < c; i++)
                rl_nds_write(t->m, RL_NDS_ARM9, a + 4 * i, 4, *p++);
            break;
        }
        case OP_R32:
        case OP_R16:
        case OP_R8: {
            uint32_t v = rl_nds_read(t->m, RL_NDS_ARM9, a, op == OP_R32 ? 4 : op == OP_R16 ? 2 : 1);
            if (n < max)
                out[n] = v;
            n++;
            break;
        }
        case OP_WAITLINE:
            while (rl_nds_vcount(t->m) != a) {
                uint64_t now = rl_sched_now(t->k[RL_NDS_ARM9]);
                ndst_both(t, now - now % RL_NDS_LINE_TICKS + RL_NDS_LINE_TICKS);
            }
            break;
        default:
            fprintf(stderr, "ndst: op %u is not known here\n", op);
            abort();
        }
    }
}
