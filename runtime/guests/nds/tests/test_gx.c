/**
 * @file test_gx.c
 * @brief The geometry engine from GBATEK ("DS 3D Geometry Commands", "DS 3D
 * Matrix Stack", "DS 3D Status", "DS 3D Tests"): packed commands, the
 * matrix stacks and their error flag, the test commands and their results,
 * polygon and vertex RAM counts with strip sharing and culling, the swap at
 * VBlank, and the GXFIFO interrupt.
 */
#include "ndst.h"

#define POWCNT1 0x04000304u
#define GXFIFO 0x04000400u
#define GXSTAT 0x04000600u
#define RAM_COUNT 0x04000604u
#define POS_RESULT 0x04000620u
#define VEC_RESULT 0x04000630u
#define CLIPMTX 0x04000640u
#define IF9 0x04000214u
#define CMD(c) (0x04000400u + 4u * (c))

enum {
    MTX_MODE = 0x10,
    MTX_PUSH = 0x11,
    MTX_POP = 0x12,
    MTX_IDENTITY = 0x15,
    MTX_LOAD_4X4 = 0x16,
    MTX_TRANS = 0x1C,
    COLOR = 0x20,
    VTX_16 = 0x23,
    POLYGON_ATTR = 0x29,
    BEGIN_VTXS = 0x40,
    END_VTXS = 0x41,
    SWAP_BUFFERS = 0x50,
    BOX_TEST = 0x70,
    POS_TEST = 0x71,
    VEC_TEST = 0x72,
};

/** @brief A machine with the 3D engines powered, the ARM9 clock at the start of the frame. */
static ndst *gx_new(void)
{
    ndst *t = ndst_new();
    w9(t, POWCNT1, 4, 0x820F);
    return t;
}

/** @brief Moves both clocks on by n ticks (the engine runs its queue). */
static void run_for(ndst *t, uint64_t n) { ndst_both(t, rl_sched_now(t->k[RL_NDS_ARM9]) + n); }

static void cmd(ndst *t, unsigned c, uint32_t v) { w9(t, CMD(c), 4, v); }

static void vtx(ndst *t, int16_t x, int16_t y, int16_t z)
{
    cmd(t, VTX_16, (uint16_t)x | (uint32_t)(uint16_t)y << 16);
    cmd(t, VTX_16, (uint16_t)z);
}

TEST(gx_packed_commands_run_and_the_stack_pointer_shows)
{
    ndst *t = gx_new();
    w9(t, GXFIFO, 4, MTX_PUSH | MTX_MODE << 8 | MTX_PUSH << 16);
    w9(t, GXFIFO, 4, 1);
    run_for(t, 1000);
    CHECK_EQ((r9(t, GXSTAT, 4) >> 8) & 31, 1);
    CHECK(r9(t, GXSTAT, 4) & (1u << 13));
    CHECK_EQ(r9(t, GXSTAT, 4) & (1u << 27), 0);
}

TEST(gx_position_stack_overflow_sets_and_clears_the_error_flag)
{
    ndst *t = gx_new();
    cmd(t, MTX_MODE, 1);
    for (unsigned i = 0; i < 31; i++)
        cmd(t, MTX_PUSH, 0);
    run_for(t, 4000);
    CHECK_EQ(r9(t, GXSTAT, 4) & 0x8000, 0);
    cmd(t, MTX_PUSH, 0);
    run_for(t, 1000);
    CHECK(r9(t, GXSTAT, 4) & 0x8000);
    w9(t, GXSTAT, 4, 0x8000);
    CHECK_EQ(r9(t, GXSTAT, 4) & 0x8000, 0);
}

TEST(gx_position_and_vector_tests_return_their_results)
{
    ndst *t = gx_new();
    cmd(t, MTX_MODE, 2);
    cmd(t, MTX_IDENTITY, 0);
    cmd(t, MTX_TRANS, 0x1000);
    cmd(t, MTX_TRANS, 0x2000);
    cmd(t, MTX_TRANS, 0x3000);
    cmd(t, POS_TEST, 0x00000800);
    cmd(t, POS_TEST, 0);
    cmd(t, VEC_TEST, 0x100);
    run_for(t, 2000);
    CHECK_EQ(r9(t, POS_RESULT, 4), 0x1800);
    CHECK_EQ(r9(t, POS_RESULT + 4, 4), 0x2000);
    CHECK_EQ(r9(t, POS_RESULT + 8, 4), 0x3000);
    CHECK_EQ(r9(t, POS_RESULT + 12, 4), 0x1000);
    CHECK_EQ(r9(t, VEC_RESULT, 2), 0x800);
    CHECK_EQ(r9(t, CLIPMTX + 48, 4), 0x1000);
    CHECK_EQ(r9(t, CLIPMTX + 52, 4), 0x2000);
}

TEST(gx_box_test_sees_boxes_inside_the_view_volume)
{
    ndst *t = gx_new();
    cmd(t, BOX_TEST, (uint16_t)-0x400 | (uint32_t)(uint16_t)-0x400 << 16);
    cmd(t, BOX_TEST, (uint16_t)-0x400 | 0x800u << 16);
    cmd(t, BOX_TEST, 0x800 | 0x800u << 16);
    run_for(t, 1000);
    CHECK(r9(t, GXSTAT, 4) & 2);
    cmd(t, BOX_TEST, 0x2000 | 0x2000u << 16);
    cmd(t, BOX_TEST, 0x2000 | 0x400u << 16);
    cmd(t, BOX_TEST, 0x400 | 0x400u << 16);
    run_for(t, 1000);
    CHECK_EQ(r9(t, GXSTAT, 4) & 2, 0);
}

TEST(gx_strips_share_vertices_and_culled_polygons_are_not_stored)
{
    ndst *t = gx_new();
    cmd(t, POLYGON_ATTR, 0x001F00C0);
    cmd(t, BEGIN_VTXS, 2);
    vtx(t, -0x800, 0x800, 0);
    vtx(t, -0x800, -0x800, 0);
    vtx(t, 0, 0x800, 0);
    vtx(t, 0, -0x800, 0);
    cmd(t, END_VTXS, 0);
    run_for(t, 4000);
    CHECK_EQ(r9(t, RAM_COUNT, 4), 2u | 4u << 16);
    cmd(t, POLYGON_ATTR, 0x001F0000);
    cmd(t, BEGIN_VTXS, 0);
    vtx(t, -0x800, 0x800, 0);
    vtx(t, -0x800, -0x800, 0);
    vtx(t, 0, 0x800, 0);
    cmd(t, END_VTXS, 0);
    run_for(t, 4000);
    CHECK_EQ(r9(t, RAM_COUNT, 4), 2u | 4u << 16);
}

TEST(gx_swap_waits_for_vblank_and_empties_the_lists)
{
    ndst *t = gx_new();
    while (rl_nds_vcount(t->m) != 10)
        run_for(t, RL_NDS_LINE_TICKS);
    cmd(t, POLYGON_ATTR, 0x001F00C0);
    cmd(t, BEGIN_VTXS, 0);
    vtx(t, -0x800, 0x800, 0);
    vtx(t, -0x800, -0x800, 0);
    vtx(t, 0, 0x800, 0);
    cmd(t, SWAP_BUFFERS, 0);
    run_for(t, 4000);
    CHECK_EQ(r9(t, RAM_COUNT, 4), 1u | 3u << 16);
    CHECK(r9(t, GXSTAT, 4) & (1u << 27));
    while (rl_nds_vcount(t->m) != 193)
        run_for(t, RL_NDS_LINE_TICKS);
    CHECK_EQ(r9(t, RAM_COUNT, 4), 0);
    CHECK_EQ(r9(t, GXSTAT, 4) & (1u << 27), 0);
}

TEST(gx_fifo_interrupt_holds_while_the_fifo_is_empty)
{
    ndst *t = gx_new();
    w9(t, GXSTAT, 4, 2u << 30);
    run_for(t, 10);
    CHECK(r9(t, IF9, 4) & RL_NDS_IRQ_GXFIFO);
    w9(t, IF9, 4, RL_NDS_IRQ_GXFIFO);
    CHECK(r9(t, IF9, 4) & RL_NDS_IRQ_GXFIFO);
    w9(t, GXSTAT, 4, 0);
    w9(t, IF9, 4, RL_NDS_IRQ_GXFIFO);
    CHECK_EQ(r9(t, IF9, 4) & RL_NDS_IRQ_GXFIFO, 0);
}
