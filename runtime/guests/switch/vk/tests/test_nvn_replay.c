/**
 * @file test_nvn_replay.c
 * @brief One recorded NVN2 command at a time through the command-buffer
 * replay (nvn_draw_cmd), for the commands that change only the replay's
 * state and make no Vulkan call.
 */
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "runner.h"

#include <string.h>

/** @brief Replays e against a zeroed replay and state; returns its result. */
static int replay_one(nvn_gfx *st, const nvn_cmd *e)
{
    replay r;
    memset(&r, 0, sizeof r);
    r.st = st;
    return nvn_draw_cmd(&r, e);
}

TEST(nvn_replay_reset_counter_is_kept_in_the_state)
{
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_cmd e = { .op = NVN_CMD_RESET_COUNTER, .x = { 0, 0 } };
    CHECK_EQ(replay_one(&st, &e), RL_OK);
    CHECK_EQ(st.counters_reset, 1u);
    e.x[1] = 15;
    CHECK_EQ(replay_one(&st, &e), RL_OK);
    e.x[1] = 3;
    CHECK_EQ(replay_one(&st, &e), RL_OK);
    CHECK_EQ(st.counters_reset, 1u | 1u << 3 | 1u << 15);
}
