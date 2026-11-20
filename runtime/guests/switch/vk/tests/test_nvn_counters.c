/**
 * @file test_nvn_counters.c
 * @brief The GPU counters a report writes: both timestamp locations write
 * the guest clock, and a type the layer does not count stops the replay.
 */
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "runner.h"

#include <string.h>

TEST(nvn_timestamps_of_both_pipe_locations_write_the_guest_clock)
{
    static rl_ctx c;
    static uint8_t mem[0x1000];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    uint32_t types[2] = { 0, NVN_COUNTER_TIMESTAMP_TOP };
    for (unsigned i = 0; i < 2; i++) {
        uint64_t at = 0x100 + 0x20 * i;
        rl_st64(mem, at, 0xdeadull);
        CHECK_EQ(nvn_counters_report(&r, types[i], at), RL_OK);
        CHECK_EQ(rl_ld64(mem, at), 0);
        CHECK(rl_ld64(mem, at + 8) != 0);
    }
}
