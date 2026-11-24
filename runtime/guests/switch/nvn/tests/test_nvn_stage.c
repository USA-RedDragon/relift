/**
 * @file test_nvn_stage.c
 * @brief A replayed command's uploads all land in the batch it is recorded
 * into: when the arena fills partway, the command stages its data again
 * after the submit instead of keeping offsets the submit dropped.
 */
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "runner.h"

#include <string.h>

/** @brief Two uploads a command needs together. */
typedef struct two_uploads {
    uint8_t a[64], b[64];
    uint64_t at_a, at_b;
    unsigned runs;
} two_uploads;

static int stage_two(replay *r, void *arg)
{
    two_uploads *u = arg;
    u->runs++;
    if (!nvn_vk_upload_bytes(r, u->a, sizeof u->a, 0, 16, &u->at_a) ||
        !nvn_vk_upload_bytes(r, u->b, sizeof u->b, 0, 16, &u->at_b))
        return RL_PENDING;
    return RL_OK;
}

TEST(nvn_a_command_restages_what_a_submit_partway_dropped)
{
    static rl_ctx c;
    staging saved = g.arena;
    uint64_t saved_used = g.arena_used;
    uint8_t arena[256];
    memset(arena, 0, sizeof arena);
    g.arena = (staging){ .map = arena, .size = sizeof arena };
    g.arena_used = 160;
    replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    two_uploads u;
    memset(&u, 0, sizeof u);
    memset(u.a, 0xaa, sizeof u.a);
    memset(u.b, 0xbb, sizeof u.b);
    CHECK_EQ(nvn_vk_stage(&r, stage_two, &u, "a test command"), RL_OK);
    CHECK_EQ(u.runs, 2u);
    CHECK(u.at_a + sizeof u.a <= g.arena_used && u.at_b + sizeof u.b <= g.arena_used);
    CHECK(!memcmp(arena + u.at_a, u.a, sizeof u.a));
    CHECK(!memcmp(arena + u.at_b, u.b, sizeof u.b));
    g.arena = saved;
    g.arena_used = saved_used;
}
