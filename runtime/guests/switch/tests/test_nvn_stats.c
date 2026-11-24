/**
 * @file test_nvn_stats.c
 * @brief The NVN use histogram (RELIFT_NVN_STATS): recorded commands, replay outcomes, proc lookups and calls, and
 * the file it writes.
 */
#include "nvn/nvn_priv.h"
#include "t.h"
#include "tc.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define RET 0x7000000ull

/** @brief The text of path, NUL-terminated (static buffer). */
static const char *slurp(const char *path)
{
    static char buf[1 << 16];
    FILE *f = fopen(path, "r");
    if (!f)
        return "";
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

/** @brief A path in the test's temporary folder. */
static const char *temp_path(const char *name)
{
    static char p[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(p, sizeof p, "%s/relift-nvn-stats-%d-%s", tmp && *tmp ? tmp : "/tmp", (int)getpid(), name);
    return p;
}

TEST(nvn_stats_counts_recorded_commands)
{
    nvn_stats_reset(NULL, true);
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t cb = heap + 0x400;
    c->x[0] = cb;
    c->x[1] = heap;
    c->x[2] = 0;
    c->x[3] = 0;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferInitialize(c, t_mem()), RET);
    c->x[0] = cb;
    c->x[1] = heap;
    c->x[3] = 0x4000;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferAddCommandMemory(c, t_mem()), RET);
    c->x[0] = cb;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferBeginRecording(c, t_mem()), RET);
    for (unsigned i = 0; i < 3; i++) {
        c->x[0] = cb;
        c->x[1] = 3;
        c->x[2] = 0x80;
        c->x[30] = RET;
        CHECK_EQ(nvnh_nvnCommandBufferSetStencilRef(c, t_mem()), RET);
    }
    c->x[0] = cb;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferEndRecording(c, t_mem()), RET);
    CHECK_EQ(nvn_stats_count(NVN_CMD_SET_STENCIL_REF, NVN_STATS_REC), 3);
    CHECK_EQ(nvn_stats_count(NVN_CMD_SET_STENCIL_MASK, NVN_STATS_REC), 0);
    nvn_stats_reset(NULL, false);
}

TEST(nvn_stats_splits_replays_by_outcome)
{
    nvn_stats_reset(NULL, true);
    nvn_stats_begin();
    nvn_stats_end(NVN_CMD_SET_STENCIL_REF, RL_OK, false);
    nvn_stats_begin();
    CHECK_EQ(nvn_stats_mark(NVN_STATS_IGNORED), RL_OK);
    nvn_stats_end(NVN_CMD_SET_PATCH_SIZE, RL_OK, false);
    nvn_stats_begin();
    nvn_stats_mark(NVN_STATS_PARTIAL);
    nvn_stats_mark(NVN_STATS_IGNORED);
    nvn_stats_end(NVN_CMD_SET_LINE_WIDTH, RL_OK, false);
    nvn_stats_begin();
    nvn_stats_end(NVN_CMD_SET_DEPTH_BOUNDS, -2, false);
    nvn_stats_begin();
    nvn_stats_end(NVN_CMD_CALL_COMMANDS, -2, true);
    nvn_stats_begin();
    nvn_stats_end(NVN_CMD_CLEAR_COLOR, RL_PENDING, false);
    CHECK_EQ(nvn_stats_count(NVN_CMD_SET_STENCIL_REF, NVN_STATS_OK), 1);
    CHECK_EQ(nvn_stats_count(NVN_CMD_SET_PATCH_SIZE, NVN_STATS_IGNORED), 1);
    CHECK_EQ(nvn_stats_count(NVN_CMD_SET_LINE_WIDTH, NVN_STATS_PARTIAL), 1);
    CHECK_EQ(nvn_stats_count(NVN_CMD_SET_LINE_WIDTH, NVN_STATS_IGNORED), 0);
    CHECK_EQ(nvn_stats_count(NVN_CMD_SET_DEPTH_BOUNDS, NVN_STATS_STOPPED), 1);
    CHECK_EQ(nvn_stats_count(NVN_CMD_CALL_COMMANDS, NVN_STATS_UNHANDLED), 1);
    CHECK_EQ(nvn_stats_count(NVN_CMD_CLEAR_COLOR, NVN_STATS_OK), 0);
    nvn_stats_reset(NULL, false);
}

TEST(nvn_stats_writes_procs_and_commands_with_gap_flags)
{
    const char *path = temp_path("w.tsv");
    nvn_stats_reset(path, true);
    nvn_stats_lookup("nvnQueueSubmitCommands", NVN_STATS_IMPL, 7);
    nvn_stats_call(7);
    nvn_stats_call(7);
    nvn_stats_lookup("nvnDeviceFoo", NVN_STATS_MISSING, 900);
    nvn_stats_lookup("nvnBlendStateSetDefaults", NVN_STATS_DRIVER, UINT32_MAX);
    nvn_stats_lookup("nvnBlendStateSetDefaults", NVN_STATS_DRIVER, UINT32_MAX);
    nvn_stats_cmd(NVN_CMD_SET_STENCIL_REF, NVN_STATS_REC);
    nvn_stats_cmd(NVN_CMD_SET_STENCIL_REF, NVN_STATS_OK);
    nvn_stats_cmd(NVN_CMD_SET_PATCH_SIZE, NVN_STATS_REC);
    nvn_stats_cmd(NVN_CMD_SET_PATCH_SIZE, NVN_STATS_IGNORED);
    CHECK(nvn_stats_write());
    const char *t = slurp(path);
    CHECK(strstr(t, "proc\tnvnQueueSubmitCommands\timpl\t1\t2\t\n") != NULL);
    CHECK(strstr(t, "proc\tnvnDeviceFoo\tmissing\t1\t0\tgap\n") != NULL);
    CHECK(strstr(t, "proc\tnvnBlendStateSetDefaults\tdriver\t2\t-\t\n") != NULL);
    CHECK(strstr(t, "cmd\tnvnCommandBufferSetStencilRef\t1\t1\t0\t0\t0\t0\t\n") != NULL);
    CHECK(strstr(t, "cmd\tnvnCommandBufferSetPatchSize\t1\t0\t1\t0\t0\t0\tgap\n") != NULL);
    CHECK(strstr(t, "nvnCommandBufferSetStencilMask") == NULL);
    remove(path);
    nvn_stats_reset(NULL, false);
}

TEST(nvn_stats_off_counts_nothing)
{
    nvn_stats_reset(NULL, false);
    nvn_stats_cmd(NVN_CMD_SET_STENCIL_REF, NVN_STATS_REC);
    nvn_stats_begin();
    nvn_stats_end(NVN_CMD_SET_STENCIL_REF, RL_OK, false);
    CHECK_EQ(nvn_stats_count(NVN_CMD_SET_STENCIL_REF, NVN_STATS_REC), 0);
    CHECK_EQ(nvn_stats_count(NVN_CMD_SET_STENCIL_REF, NVN_STATS_OK), 0);
    CHECK(!nvn_stats_write());
}
