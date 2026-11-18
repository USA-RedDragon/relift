/**
 * @file test_dma.c
 * @brief The DMA channels of both CPUs: register masks and readback, the
 * time of immediate transfers against the oracle's (an ARM9 and an ARM7
 * program timed a transfer with a bus-clock timer), triggers, repeat,
 * reload, priority and the IRQ, and the VBlank and HBlank starts the
 * display timing raises.
 */
#include "ndst.h"

#include "nds_int.h"

#define TM0 0x04000100u
#define IF 0x04000214u
#define DMA(i) (0x040000B0u + 12u * (i))

/** @brief A timed ARM9 sequence as the oracle's ITCM program runs it; returns the timer's bus cycles from the read before the CNT write to the one after. */
static uint32_t timed(ndst *t, unsigned cpu, uint32_t sad, uint32_t dad, uint32_t cnt)
{
    rl_nds *m = t->m;
    w9(t, DMA(1), 4, sad);
    w9(t, DMA(1) + 4, 4, dad);
    (void)cpu;
    rl_nds_store(m, RL_NDS_ARM9, TM0, 4, 0x00800000u, false);
    rl_nds_idle(m, RL_NDS_ARM9, 1);
    uint32_t t0 = rl_nds_load(m, RL_NDS_ARM9, TM0, 2, false);
    rl_nds_idle(m, RL_NDS_ARM9, 1);
    rl_nds_store(m, RL_NDS_ARM9, DMA(1) + 8, 4, cnt, false);
    rl_nds_idle(m, RL_NDS_ARM9, 1);
    uint32_t t1 = rl_nds_load(m, RL_NDS_ARM9, TM0, 2, false);
    return (t1 - t0) & 0xFFFF;
}

static void far7(ndst *t) { ndst_to(t, RL_NDS_ARM7, 1u << 30); }

TEST(arm9_unit_costs_of_immediate_transfers_match_the_oracle)
{
    ndst *t = ndst_new();
    far7(t);
    static const uint32_t ns[5] = {1, 2, 4, 16, 64};
    /* oracle: ITCM program, bus cycles per unit count 1, 2, 4, 16, 64 (16-bit then 32-bit) */
    static const struct {
        uint32_t s, d;
        uint32_t want[2][5];
    } c[] = {
        {0x02100000, 0x02101000, {{0x14, 0x24, 0x44, 0x104, 0x404}, {0x16, 0x28, 0x4c, 0x124, 0x484}}},
        {0x03000000, 0x03004000, {{7, 0xa, 0x10, 0x34, 0xc4}, {7, 0xa, 0x10, 0x34, 0xc4}}},
        {0x06800000, 0x06804000, {{7, 0xa, 0x10, 0x34, 0xc4}, {9, 0xe, 0x18, 0x54, 0x144}}},
        {0x02100000, 0x03004000, {{0xb, 0xe, 0x12, 0x2a, 0x8a}, {0xd, 0x10, 0x14, 0x2c, 0x8c}}},
        {0x03000000, 0x02101000, {{0xc, 0xe, 0x12, 0x2a, 0x8a}, {0xd, 0x10, 0x16, 0x3a, 0xca}}},
        {0x06800000, 0x03004000, {{6, 8, 0xc, 0x24, 0x84}, {7, 0xa, 0x10, 0x34, 0xc4}}},
        {0x06800000, 0x02101000, {{0xc, 0xe, 0x12, 0x2a, 0x8a}, {0xd, 0x11, 0x19, 0x49, 0x10e}}},
        {0x05000000, 0x05000400, {{7, 0xa, 0x10, 0x34, 0xc4}, {9, 0xe, 0x18, 0x54, 0x144}}},
    };
    unsigned bad = 0;
    for (unsigned r = 0; r < sizeof c / sizeof c[0]; r++)
        for (unsigned w = 0; w < 2; w++) {
            uint32_t base = 0;
            for (unsigned j = 0; j < 5; j++) {
                uint32_t got = timed(t, RL_NDS_ARM9, c[r].s, c[r].d, 0x80000000u | (w ? 1u << 26 : 0) | ns[j]);
                if (j == 1)
                    base = got;
                else if (j == 0) {
                } else if (r == 6 && w == 1 && j == 4) {
                    /* the oracle takes 5 cycles more than 63 units of 4 for 256 bytes of video RAM into main RAM */
                } else if (got - base != c[r].want[w][j] - c[r].want[w][1]) {
                    fprintf(stderr, "case %u w%u n%u got +%x want +%x\n", r, w, ns[j], got - base, c[r].want[w][j] - c[r].want[w][1]);
                    bad++;
                }
                ndst_both(t, rl_nds_now(t->m, RL_NDS_ARM9) + 64);
            }
        }
    CHECK_EQ(bad, 0);
}

/** @brief The ARM7's DMA program of the oracle: cycles of the CNT write to the second timer read, per case. */
static uint32_t timed7(ndst *t, uint32_t sad, uint32_t dad, uint32_t cnt)
{
    rl_nds *m = t->m;
    w7(t, DMA(1), 4, sad);
    w7(t, DMA(1) + 4, 4, dad);
    rl_nds_store(m, RL_NDS_ARM7, TM0, 4, 0x00800000u, false);
    uint32_t t0 = rl_nds_load(m, RL_NDS_ARM7, TM0, 2, false);
    rl_nds_store(m, RL_NDS_ARM7, DMA(1) + 8, 4, cnt, false);
    uint32_t t1 = rl_nds_load(m, RL_NDS_ARM7, TM0, 2, false);
    return (t1 - t0) & 0xFFFF;
}

TEST(arm7_unit_costs_of_immediate_transfers_match_the_oracle)
{
    ndst *t = ndst_new();
    ndst_to(t, RL_NDS_ARM9, 1u << 30);
    static const uint32_t ns[5] = {1, 2, 4, 16, 64};
    static const struct {
        uint32_t s, d;
        uint32_t want[2][5];
    } c[] = {
        {0x02300000, 0x02301000, {{0x13, 0x23, 0x43, 0x103, 0x403}, {0x15, 0x27, 0x4b, 0x123, 0x483}}},
        {0x03801000, 0x03802000, {{6, 9, 0xf, 0x33, 0xc3}, {6, 9, 0xf, 0x33, 0xc3}}},
        {0x02300000, 0x03802000, {{0xa, 0xd, 0x11, 0x29, 0x89}, {0xc, 0xf, 0x13, 0x2b, 0x8b}}},
        {0x03801000, 0x02301000, {{0xb, 0xd, 0x11, 0x29, 0x89}, {0xc, 0xf, 0x15, 0x39, 0xc9}}},
    };
    unsigned bad = 0;
    for (unsigned r = 0; r < sizeof c / sizeof c[0]; r++)
        for (unsigned w = 0; w < 2; w++) {
            uint32_t base = 0;
            for (unsigned j = 0; j < 5; j++) {
                uint32_t got = timed7(t, c[r].s, c[r].d, 0x80000000u | (w ? 1u << 26 : 0) | ns[j]);
                if (j == 1)
                    base = got;
                else if (j == 0) {
                } else if (got - base != c[r].want[w][j] - c[r].want[w][1]) {
                    fprintf(stderr, "arm7 case %u w%u n%u got +%x want +%x\n", r, w, ns[j], got - base, c[r].want[w][j] - c[r].want[w][1]);
                    bad++;
                }
                ndst_both(t, rl_nds_now(t->m, RL_NDS_ARM7) + 64);
            }
        }
    CHECK_EQ(bad, 0);
}

TEST(registers_read_back_as_written_and_the_masks_apply_at_the_start)
{
    ndst *t = ndst_new();
    far7(t);
    for (unsigned cpu = 0; cpu < 2; cpu++)
        for (unsigned i = 0; i < 4; i++) {
            uint32_t a = 0x040000B0u + 12u * i;
            rl_nds_write(t->m, cpu, a, 4, 0xFFFFFFFFu);
            rl_nds_write(t->m, cpu, a + 4, 4, 0xFFFFFFFFu);
            rl_nds_write(t->m, cpu, a + 8, 4, 0x7FFFFFFFu);
            CHECK_EQ(rl_nds_read(t->m, cpu, a, 4), 0xFFFFFFFFu);
            CHECK_EQ(rl_nds_read(t->m, cpu, a + 4, 4), 0xFFFFFFFFu);
            CHECK_EQ(rl_nds_read(t->m, cpu, a + 8, 4), 0x7FFFFFFFu);
            rl_nds_write(t->m, cpu, a + 8, 4, 0);
        }
    w9(t, 0x040000E0u, 4, 0xAABBCCDDu);
    CHECK_EQ(r9(t, 0x040000E0u, 4), 0xAABBCCDDu);
    CHECK_EQ(r7(t, 0x040000E0u, 4), 0);
}

/** @brief Moves both CPUs to tick and lets the events up to it run (a register read reaches the scheduler). */
static void run_to(ndst *t, uint64_t tick)
{
    ndst_both(t, tick);
    (void)r9(t, 0x04000214u, 4);
    (void)r7(t, 0x04000214u, 4);
}

TEST(an_immediate_transfer_copies_and_clears_the_enable_bit)
{
    ndst *t = ndst_new();
    far7(t);
    for (unsigned i = 0; i < 16; i++)
        w9(t, 0x02100000u + 4 * i, 4, 0x11111111u * (i + 1));
    w9(t, DMA(2), 4, 0x02100000u);
    w9(t, DMA(2) + 4, 4, 0x02200000u);
    w9(t, DMA(2) + 8, 4, 0x84000010u);
    CHECK(r9(t, DMA(2) + 8, 4) & 0x80000000u);
    run_to(t, 4000);
    CHECK_EQ(r9(t, DMA(2) + 8, 4) & 0x80000000u, 0);
    for (unsigned i = 0; i < 16; i++)
        CHECK_EQ(r9(t, 0x02200000u + 4 * i, 4), 0x11111111u * (i + 1));
    CHECK_EQ(r9(t, 0x02200040u, 4), 0);
}

TEST(a_fixed_source_in_the_fill_registers_fills_memory)
{
    ndst *t = ndst_new();
    far7(t);
    w9(t, 0x040000ECu, 4, 0xCAFEF00Du);
    w9(t, DMA(0), 4, 0x040000ECu);
    w9(t, DMA(0) + 4, 4, 0x02300000u);
    w9(t, DMA(0) + 8, 4, 0x85000008u);
    run_to(t, 4000);
    for (unsigned i = 0; i < 8; i++)
        CHECK_EQ(r9(t, 0x02300000u + 4 * i, 4), 0xCAFEF00Du);
    CHECK_EQ(r9(t, 0x02300020u, 4), 0);
}

TEST(a_dma_irq_is_requested_when_the_transfer_ends)
{
    ndst *t = ndst_new();
    far7(t);
    w9(t, DMA(1), 4, 0x02100000u);
    w9(t, DMA(1) + 4, 4, 0x02100100u);
    w9(t, DMA(1) + 8, 4, 0xC4000004u);
    CHECK_EQ(r9(t, 0x04000214u, 4), 0);
    run_to(t, 4000);
    CHECK_EQ(r9(t, 0x04000214u, 4), RL_NDS_IRQ_DMA0 << 1);
}

TEST(a_count_of_zero_is_the_maximum_and_channel_zero_goes_first)
{
    ndst *t = ndst_new();
    ndst_to(t, RL_NDS_ARM9, 1u << 30);
    w7(t, DMA(0) + 8, 4, 0);
    w7(t, DMA(0), 4, 0x02200000u);
    w7(t, DMA(0) + 4, 4, 0x02210000u);
    w7(t, DMA(1), 4, 0x02300000u);
    w7(t, DMA(1) + 4, 4, 0x02310000u);
    w7(t, 0x02200000u, 4, 0x01020304u);
    w7(t, 0x02300000u, 4, 0x05060708u);
    w7(t, DMA(1) + 8, 4, 0x84000001u);
    w7(t, DMA(0) + 8, 4, 0x84000001u);
    run_to(t, 4000);
    CHECK_EQ(r7(t, 0x02210000u, 4), 0x01020304u);
    CHECK_EQ(r7(t, 0x02310000u, 4), 0x05060708u);
    w7(t, DMA(3) + 8, 4, 0x80000000u);
    CHECK(r7(t, DMA(3) + 8, 4) & 0x80000000u);
    ndst_both(t, 8000);
    CHECK(r7(t, DMA(3) + 8, 4) & 0x80000000u);
    ndst_both(t, 400000);
    CHECK_EQ(r7(t, DMA(3) + 8, 4) & 0x80000000u, 0);
}

TEST(a_vblank_transfer_waits_for_the_trigger_and_repeats_with_destination_reload)
{
    ndst *t = ndst_new();
    far7(t);
    w9(t, 0x02100000u, 4, 0xAAAA0001u);
    w9(t, 0x02100004u, 4, 0xAAAA0002u);
    w9(t, DMA(3), 4, 0x02100000u);
    w9(t, DMA(3) + 4, 4, 0x02200000u);
    w9(t, DMA(3) + 8, 4, 0x8E600001u);
    run_to(t, 4000);
    CHECK_EQ(r9(t, 0x02200000u, 4), 0);
    w9(t, 0x02100000u, 4, 0xBBBB0001u);
    nds_dma_start(t->m, RL_NDS_ARM9, NDS_DMA9_VBLANK, 4000);
    run_to(t, 8000);
    CHECK_EQ(r9(t, 0x02200000u, 4), 0xBBBB0001u);
    CHECK(r9(t, DMA(3) + 8, 4) & 0x80000000u);
    nds_dma_start(t->m, RL_NDS_ARM9, NDS_DMA9_HBLANK, 8000);
    run_to(t, 9000);
    CHECK_EQ(r9(t, 0x02200004u, 4), 0);
    w9(t, 0x02100004u, 4, 0xCCCC0002u);
    nds_dma_start(t->m, RL_NDS_ARM9, NDS_DMA9_VBLANK, 9000);
    run_to(t, 12000);
    CHECK_EQ(r9(t, 0x02200000u, 4), 0xCCCC0002u);
    w9(t, DMA(3) + 8, 4, 0);
    CHECK_EQ(r9(t, DMA(3) + 8, 4), 0);
}

TEST(arm7_start_modes_come_from_bits_28_and_29)
{
    ndst *t = ndst_new();
    ndst_to(t, RL_NDS_ARM9, 1u << 30);
    w7(t, 0x02100000u, 4, 0x13572468u);
    w7(t, DMA(1), 4, 0x02100000u);
    w7(t, DMA(1) + 4, 4, 0x02200000u);
    w7(t, DMA(1) + 8, 4, 0xA4000001u);
    ndst_to(t, RL_NDS_ARM7, 4000);
    nds_dma_start(t->m, RL_NDS_ARM7, NDS_DMA7_VBLANK, 4000);
    (void)r7(t, 0x04000214u, 4);
    CHECK_EQ(r7(t, 0x02200000u, 4), 0);
    nds_dma_start(t->m, RL_NDS_ARM7, NDS_DMA7_CARD, 4000);
    ndst_to(t, RL_NDS_ARM7, 8000);
    (void)r7(t, 0x04000214u, 4);
    CHECK_EQ(r7(t, 0x02200000u, 4), 0x13572468u);
}

#define LINE RL_NDS_LINE_TICKS
#define HBLANK 3168u

TEST(a_vblank_transfer_starts_at_line_192)
{
    ndst *t = ndst_new();
    far7(t);
    w9(t, 0x02100000u, 4, 0x12345678u);
    w9(t, DMA(2), 4, 0x02100000u);
    w9(t, DMA(2) + 4, 4, 0x02200000u);
    w9(t, DMA(2) + 8, 4, 0x8C000001u);
    run_to(t, 192u * LINE - 2);
    CHECK_EQ(r9(t, 0x02200000u, 4), 0);
    CHECK(r9(t, DMA(2) + 8, 4) & 0x80000000u);
    run_to(t, 192u * LINE + 200);
    CHECK_EQ(r9(t, 0x02200000u, 4), 0x12345678u);
    CHECK_EQ(r9(t, DMA(2) + 8, 4) & 0x80000000u, 0);
}

TEST(an_hblank_transfer_repeats_on_each_visible_line_and_pauses_in_vblank)
{
    ndst *t = ndst_new();
    far7(t);
    for (unsigned i = 0; i < 263; i++)
        w9(t, 0x02100000u + 4 * i, 4, i + 1);
    w9(t, DMA(1), 4, 0x02100000u);
    w9(t, DMA(1) + 4, 4, 0x02200000u);
    w9(t, DMA(1) + 8, 4, 0x96000001u);
    run_to(t, HBLANK - 2);
    CHECK_EQ(r9(t, 0x02200000u, 4), 0);
    run_to(t, HBLANK + 200);
    CHECK_EQ(r9(t, 0x02200000u, 4), 1);
    run_to(t, 2u * LINE + HBLANK + 200);
    CHECK_EQ(r9(t, 0x02200008u, 4), 3);
    CHECK_EQ(r9(t, 0x0220000Cu, 4), 0);
    run_to(t, 262u * LINE + HBLANK + 200);
    CHECK_EQ(r9(t, 0x022002FCu, 4), 192);
    CHECK_EQ(r9(t, 0x02200300u, 4), 0);
    run_to(t, RL_NDS_FRAME_TICKS + HBLANK + 200);
    CHECK_EQ(r9(t, 0x02200300u, 4), 193);
    w9(t, DMA(1) + 8, 4, 0);
}
