/**
 * @file test_input.c
 * @brief KEYINPUT, KEYCNT and EXTKEYIN of both CPUs and the host's input hook
 * (values read from the oracle with its KEYS and TOUCH commands).
 */
#include "ndst.h"

#define KEYINPUT 0x04000130u
#define KEYCNT 0x04000132u
#define RCNT 0x04000134u
#define EXTKEYIN 0x04000136u
#define IF 0x04000214u

TEST(keyinput_is_inverted_with_the_upper_bits_clear_and_keycnt_reads_back_everything)
{
    ndst *t = ndst_new();
    CHECK_EQ(r9(t, KEYINPUT, 2), 0x3FF);
    CHECK_EQ(r7(t, KEYINPUT, 2), 0x3FF);
    rl_nds_input in = {.keys = RL_NDS_KEY_A | RL_NDS_KEY_SELECT};
    rl_nds_set_input(t->m, &in);
    CHECK_EQ(r9(t, KEYINPUT, 2), 0x3FA);
    CHECK_EQ(r7(t, KEYINPUT, 2), 0x3FA);
    in.keys = 0x3FF;
    rl_nds_set_input(t->m, &in);
    CHECK_EQ(r9(t, KEYINPUT, 2), 0);
    w9(t, KEYCNT, 2, 0xFFFF);
    w7(t, KEYCNT, 2, 0xFFFF);
    CHECK_EQ(r9(t, KEYCNT, 2), 0xFFFF);
    CHECK_EQ(r7(t, KEYCNT, 2), 0xFFFF);
    CHECK_EQ(r9(t, KEYINPUT, 4), 0xFFFF0000u);
    w9(t, KEYCNT, 2, 0);
    CHECK_EQ(r7(t, KEYCNT, 2), 0xFFFF);
}

TEST(extkeyin_reports_x_y_pen_and_hinge_on_the_arm7_only)
{
    ndst *t = ndst_new();
    CHECK_EQ(r7(t, EXTKEYIN, 2), 0x7F);
    CHECK_EQ(r9(t, EXTKEYIN, 2), 0);
    rl_nds_input in = {.x = true, .touch = true, .touch_x = 100, .touch_y = 80};
    rl_nds_set_input(t->m, &in);
    CHECK_EQ(r7(t, EXTKEYIN, 2), 0x3E);
    in = (rl_nds_input){.y = true, .lid_closed = true};
    rl_nds_set_input(t->m, &in);
    CHECK_EQ(r7(t, EXTKEYIN, 2), 0xFD);
    in = (rl_nds_input){0};
    rl_nds_set_input(t->m, &in);
    CHECK_EQ(r7(t, EXTKEYIN, 2), 0x7F);
}

TEST(rcnt_starts_at_8000_on_the_arm7)
{
    ndst *t = ndst_new();
    CHECK_EQ(r7(t, RCNT, 4), 0x007F8000u);
    w7(t, RCNT, 2, 0x8100);
    CHECK_EQ(r7(t, RCNT, 2), 0x8100);
    CHECK_EQ(r9(t, RCNT, 2), 0);
}

TEST(the_keypad_irq_follows_the_or_and_and_conditions)
{
    ndst *t = ndst_new();
    w9(t, KEYCNT, 2, 0x4000 | RL_NDS_KEY_A | RL_NDS_KEY_B);
    w7(t, KEYCNT, 2, 0xC000 | RL_NDS_KEY_A | RL_NDS_KEY_B);
    rl_nds_input in = {.keys = RL_NDS_KEY_A};
    rl_nds_set_input(t->m, &in);
    CHECK_EQ(r9(t, IF, 4), RL_NDS_IRQ_KEYPAD);
    CHECK_EQ(r7(t, IF, 4), 0);
    in.keys = RL_NDS_KEY_A | RL_NDS_KEY_B;
    rl_nds_set_input(t->m, &in);
    CHECK_EQ(r7(t, IF, 4), RL_NDS_IRQ_KEYPAD);
}
