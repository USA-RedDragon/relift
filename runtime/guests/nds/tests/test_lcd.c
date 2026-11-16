/**
 * @file test_lcd.c
 * @brief The display timing: VCOUNT and DISPSTAT of both CPUs, the VBlank,
 * HBlank and VCOUNT match interrupts at the ticks the oracle showed (line
 * start, and 1,584 bus cycles into the line for HBlank).
 */
#include "ndst.h"

#define DISPSTAT 0x04000004u
#define VCOUNT 0x04000006u
#define IE 0x04000210u
#define IF 0x04000214u
#define LINE RL_NDS_LINE_TICKS
#define HB 3168u

TEST(vcount_and_the_flags_follow_the_tick)
{
    ndst *t = ndst_new();
    CHECK_EQ(r9(t, VCOUNT, 2), 0);
    CHECK_EQ(r9(t, DISPSTAT, 2), 4);
    ndst_both(t, HB - 2);
    CHECK_EQ(r9(t, DISPSTAT, 2) & 2, 0);
    ndst_both(t, HB);
    CHECK_EQ(r9(t, DISPSTAT, 2) & 2, 2);
    CHECK_EQ(r7(t, DISPSTAT, 2) & 2, 2);
    ndst_both(t, LINE);
    CHECK_EQ(r9(t, VCOUNT, 2), 1);
    CHECK_EQ(r9(t, DISPSTAT, 2), 0);
    ndst_both(t, 191ull * LINE + LINE - 2);
    CHECK_EQ(r9(t, DISPSTAT, 2) & 1, 0);
    ndst_both(t, 192ull * LINE);
    CHECK_EQ(r9(t, VCOUNT, 2), 192);
    CHECK_EQ(r7(t, DISPSTAT, 2) & 1, 1);
    ndst_both(t, 262ull * LINE - 2);
    CHECK_EQ(r9(t, DISPSTAT, 2) & 1, 1);
    ndst_both(t, 262ull * LINE);
    CHECK_EQ(r9(t, VCOUNT, 2), 262);
    CHECK_EQ(r9(t, DISPSTAT, 2) & 1, 0);
    ndst_both(t, RL_NDS_FRAME_TICKS);
    CHECK_EQ(r9(t, VCOUNT, 2), 0);
    CHECK_EQ(rl_nds_vcount(t->m), 0);
}

TEST(each_cpu_has_its_own_lyc_and_irq_enables)
{
    ndst *t = ndst_new();
    w9(t, DISPSTAT, 2, 0x0A28);
    w7(t, DISPSTAT, 2, 0x0BA0);
    CHECK_EQ(r9(t, DISPSTAT, 2), 0x0A28);
    CHECK_EQ(r7(t, DISPSTAT, 2), 0x0BA0);
    ndst_both(t, 10ull * LINE - 2);
    CHECK_EQ(r9(t, IF, 4), 0);
    ndst_both(t, 10ull * LINE);
    CHECK_EQ(r9(t, IF, 4), RL_NDS_IRQ_VCOUNT);
    CHECK_EQ(r9(t, DISPSTAT, 2) & 4, 4);
    CHECK_EQ(r7(t, IF, 4), 0);
    w9(t, IF, 4, RL_NDS_IRQ_VCOUNT);
    ndst_both(t, 192ull * LINE);
    CHECK_EQ(r9(t, IF, 4), RL_NDS_IRQ_VBLANK);
    CHECK_EQ(r7(t, IF, 4), 0);
    w9(t, IF, 4, RL_NDS_IRQ_VBLANK);
    ndst_both(t, (256ull + 11) * LINE);
    CHECK_EQ(r7(t, IF, 4), 0);
    CHECK_EQ(r7(t, DISPSTAT, 2) & 4, 0);
    w7(t, DISPSTAT, 2, 0x0B90);
    ndst_both(t, (256ull + 11) * LINE + HB);
    CHECK_EQ(r7(t, IF, 4), RL_NDS_IRQ_HBLANK);
    CHECK_EQ(r9(t, IF, 4), 0);
}

TEST(hblank_irq_reaches_the_input_at_the_flag)
{
    ndst *t = ndst_new();
    w9(t, DISPSTAT, 2, 0x10);
    w9(t, IE, 4, RL_NDS_IRQ_HBLANK);
    w9(t, 0x04000208u, 4, 1);
    ndst_both(t, 5ull * LINE + HB - 1);
    w9(t, IF, 4, RL_NDS_IRQ_HBLANK);
    CHECK_EQ(r9(t, IF, 4), 0);
    ndst_both(t, 5ull * LINE + HB);
    CHECK_EQ(r9(t, IF, 4), RL_NDS_IRQ_HBLANK);
    CHECK_EQ(rl_nds_irq_at(t->m, RL_NDS_ARM9), 5ull * LINE + HB + 2);
}

TEST(a_vcount_write_moves_the_line_and_keeps_the_dot)
{
    ndst *t = ndst_new();
    ndst_both(t, 202ull * LINE + 100);
    w9(t, VCOUNT, 2, 205);
    CHECK_EQ(r9(t, VCOUNT, 2), 205);
    ndst_both(t, 203ull * LINE);
    CHECK_EQ(r9(t, VCOUNT, 2), 206);
    CHECK_EQ(r7(t, VCOUNT, 2), 206);
}
