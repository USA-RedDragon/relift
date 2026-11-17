/**
 * @file test_timing.c
 * @brief CPU timing against console measurements (DS mode on a 3DS and on a
 * DSi): the ARM9 caches' replacement.
 */
#include "ndst.h"

/** @brief The ARM9 with the protection unit on, main RAM instruction cacheable, round-robin replacement. */
static ndst *cached(void)
{
    ndst *t = ndst_new();
    rl_nds_cp15_write(t->m, RL_CP15_KEY(0, 6, 1, 0), 0x02000000u | 21u << 1 | 1u);
    rl_nds_cp15_write(t->m, RL_CP15_KEY(0, 2, 0, 1), 1u << 1);
    rl_nds_cp15_write(t->m, RL_CP15_KEY(0, 1, 0, 0),
                      rl_nds_cp15(t->m)->control | RL_CP15_MPU | RL_CP15_ICACHE | RL_CP15_ROUND_ROBIN);
    return t;
}

/** @brief Ticks of one pass over lines code lines from base, one fetch per line. */
static uint64_t pass(ndst *t, uint32_t base, unsigned lines)
{
    rl_sched_cpu *k = t->k[RL_NDS_ARM9];
    uint64_t t0 = rl_sched_now(k);
    for (unsigned l = 0; l < lines; l++)
        rl_nds_code_cost(t->m, base + 32u * l, 4, false);
    return rl_sched_now(k) - t0;
}

/*
 * The console's straight-line loops of 2 to 16 KiB, run right after an
 * instruction cache invalidate (the hardware results' big2k..big16k cold
 * cases): 2 KiB (64 lines) hits from the second pass on; every larger loop
 * misses every line on every pass, 38 cycles per line on both consoles,
 * though 128 lines fit the 256-line cache.
 */
TEST(round_robin_replaces_lines_64_sets_apart_in_cold_loops)
{
    static const unsigned kib[] = {2, 4, 6, 7, 8, 9, 12, 16};
    for (unsigned i = 0; i < sizeof kib / sizeof kib[0]; i++) {
        ndst *t = cached();
        unsigned lines = kib[i] * 32u;
        rl_nds_cp15_write(t->m, RL_CP15_KEY(0, 7, 5, 0), 0);
        pass(t, 0x02100000u, lines);
        uint64_t hit = pass(t, 0x02100000u, lines);
        uint64_t again = pass(t, 0x02100000u, lines);
        CHECK_EQ(hit, again);
        if (lines == 64)
            CHECK_EQ(hit, (uint64_t)lines);
        else
            CHECK(hit >= (uint64_t)lines * 38u);
    }
}
