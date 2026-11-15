/**
 * @file test_keypad.c
 * @brief KEYINPUT, KEYCNT and the keypad interrupt: from a pinned mask, from
 * the p1 port of the native port table, and against the NanoBoyAdvance
 * oracle with keys held.
 */
#include "oracle.h"

#define KEYINPUT 0x04000130u
#define KEYCNT 0x04000132u
#define IE_ 0x04000200u
#define IF_ 0x04000202u
#define IME_ 0x04000208u

enum { KA = 1, KB = 2, KSELECT = 4, KSTART = 8, KRIGHT = 16, KLEFT = 32, KUP = 64, KDOWN = 128, KR = 256, KL = 512 };

static void press_key(rl_input_snapshot *s, unsigned usage) { s->keys[usage >> 6] |= 1ull << (usage & 63); }

TEST(keyinput_reads_all_keys_released_by_default)
{
    mach m = mach_new();
    CHECK_EQ(mach_rd(&m, 0, KEYINPUT, 2), 0x03FFu);
    CHECK_EQ(mach_rd(&m, 0, KEYCNT, 2), 0u);
    mach_free(&m);
}

TEST(a_pinned_mask_reads_active_low)
{
    mach m = mach_new();
    rl_gba_keypad_set(m.g, KA | KSTART | KL);
    CHECK_EQ(mach_rd(&m, 10, KEYINPUT, 2), 0x03FFu & ~(uint32_t)(KA | KSTART | KL));
    CHECK_EQ(mach_rd(&m, 11, KEYINPUT, 1), 0xF6u);
    CHECK_EQ(mach_rd(&m, 12, KEYINPUT + 1, 1), 0x01u);
    rl_gba_keypad_set(m.g, 0xFFFFu);
    CHECK_EQ(mach_rd(&m, 13, KEYINPUT, 4) & 0xFFFFu, 0u);
    mach_free(&m);
}

TEST(keyinput_is_read_only_and_keycnt_keeps_its_bits)
{
    mach m = mach_new();
    mach_wr(&m, 5, KEYINPUT, 2, 0);
    CHECK_EQ(mach_rd(&m, 6, KEYINPUT, 2), 0x03FFu);
    mach_wr(&m, 7, KEYCNT, 2, 0xFFFFu);
    CHECK_EQ(mach_rd(&m, 8, KEYCNT, 2), 0xC3FFu);
    mach_wr(&m, 9, KEYCNT, 1, 0x5Au);
    CHECK_EQ(mach_rd(&m, 10, KEYCNT, 2), 0xC35Au);
    mach_free(&m);
}

TEST(the_interrupt_in_or_mode_fires_on_any_selected_key)
{
    mach m = mach_new();
    mach_wr(&m, 1, KEYCNT, 2, 0x4000u | KA | KB);
    rl_gba_keypad_set(m.g, KUP);
    CHECK_EQ(mach_if(&m, 20) & RL_GBA_IRQ_KEYPAD, 0u);
    rl_gba_keypad_set(m.g, KUP | KB);
    CHECK_EQ(mach_if(&m, 30) & RL_GBA_IRQ_KEYPAD, RL_GBA_IRQ_KEYPAD);
    mach_free(&m);
}

TEST(the_interrupt_in_and_mode_needs_every_selected_key)
{
    mach m = mach_new();
    mach_wr(&m, 1, KEYCNT, 2, 0xC000u | KA | KB);
    rl_gba_keypad_set(m.g, KA);
    CHECK_EQ(mach_if(&m, 20) & RL_GBA_IRQ_KEYPAD, 0u);
    rl_gba_keypad_set(m.g, KA | KB | KL);
    CHECK_EQ(mach_if(&m, 30) & RL_GBA_IRQ_KEYPAD, RL_GBA_IRQ_KEYPAD);
    mach_free(&m);
}

TEST(a_disabled_interrupt_never_fires)
{
    mach m = mach_new();
    mach_wr(&m, 1, KEYCNT, 2, KA);
    rl_gba_keypad_set(m.g, KA);
    CHECK_EQ(mach_if(&m, 50) & RL_GBA_IRQ_KEYPAD, 0u);
    mach_free(&m);
}

TEST(writing_keycnt_while_the_condition_holds_raises_the_interrupt)
{
    mach m = mach_new();
    rl_gba_keypad_set(m.g, KA);
    mach_wr(&m, 10, KEYCNT, 2, 0x4000u | KA);
    CHECK_EQ(mach_if(&m, 30) & RL_GBA_IRQ_KEYPAD, RL_GBA_IRQ_KEYPAD);
    mach_free(&m);
}

TEST(the_interrupt_wakes_through_ie_and_ime)
{
    mach m = mach_new();
    mach_wr(&m, 1, IE_, 2, RL_GBA_IRQ_KEYPAD);
    mach_wr(&m, 2, IME_, 2, 1);
    mach_wr(&m, 3, KEYCNT, 2, 0x4000u | KSTART);
    CHECK(!rl_gba_irq_line(m.g));
    rl_gba_keypad_set(m.g, KSTART);
    mach_to(&m, 40);
    CHECK(rl_gba_irq_line(m.g));
    mach_free(&m);
}

static void declare_ports(void)
{
    rl_ports_reset();
    CHECK_EQ(rl_ports_declare(rl_gba_ports, rl_gba_ports_count), true);
    rl_ports_bind_defaults();
}

TEST(the_p1_port_drives_keyinput)
{
    declare_ports();
    mach m = mach_new();
    CHECK_EQ(mach_rd(&m, 0, KEYINPUT, 2), 0x03FFu);
    rl_input_snapshot s = {0};
    s.pads[0].connected = true;
    press_key(&s, 0x1B);
    press_key(&s, 0x28);
    press_key(&s, 0x52);
    s.pads[0].buttons = 1u << RL_BTN_RIGHT_SHOULDER;
    rl_input_publish(&s);
    CHECK_EQ(mach_rd(&m, 5000, KEYINPUT, 2), 0x03FFu & ~(uint32_t)(KA | KSTART | KUP | KR));
    mach_free(&m);
}

TEST(a_rebound_control_follows_its_binding)
{
    declare_ports();
    CHECK(rl_ports_bind("p1", "b", "key:space"));
    mach m = mach_new();
    rl_input_snapshot s = {0};
    s.pads[0].connected = true;
    press_key(&s, 0x2C);
    rl_input_publish(&s);
    CHECK_EQ(mach_rd(&m, 10, KEYINPUT, 2), 0x03FFu & ~(uint32_t)KB);
    mach_free(&m);
}

TEST(ports_are_sampled_at_most_once_per_scanline)
{
    declare_ports();
    mach m = mach_new();
    rl_input_snapshot s = {0};
    s.pads[0].connected = true;
    press_key(&s, 0x1B);
    rl_input_publish(&s);
    CHECK_EQ(mach_rd(&m, 10, KEYINPUT, 2) & KA, 0u);
    rl_input_snapshot none = {0};
    none.pads[0].connected = true;
    rl_input_publish(&none);
    CHECK_EQ(mach_rd(&m, 100, KEYINPUT, 2) & KA, 0u);
    CHECK_EQ(mach_rd(&m, 10 + RL_GBA_LINE_CYCLES, KEYINPUT, 2) & KA, KA);
    mach_free(&m);
}

TEST(the_interrupt_polls_the_ports_without_any_read)
{
    declare_ports();
    mach m = mach_new();
    mach_wr(&m, 1, KEYCNT, 2, 0x4000u | KA);
    rl_input_snapshot s = {0};
    s.pads[0].connected = true;
    press_key(&s, 0x1B);
    rl_input_publish(&s);
    mach_to(&m, 5 * RL_GBA_LINE_CYCLES);
    CHECK_EQ(mach_if(&m, 5 * RL_GBA_LINE_CYCLES + 2) & RL_GBA_IRQ_KEYPAD, RL_GBA_IRQ_KEYPAD);
    mach_free(&m);
}

TEST(pinning_the_keys_stops_the_ports_and_using_them_resumes)
{
    declare_ports();
    mach m = mach_new();
    rl_input_snapshot s = {0};
    s.pads[0].connected = true;
    press_key(&s, 0x1B);
    rl_input_publish(&s);
    rl_gba_keypad_set(m.g, KL);
    CHECK_EQ(mach_rd(&m, 5 * RL_GBA_LINE_CYCLES, KEYINPUT, 2), 0x03FFu & ~(uint32_t)KL);
    rl_gba_keypad_use_ports(m.g);
    CHECK_EQ(mach_rd(&m, 6 * RL_GBA_LINE_CYCLES, KEYINPUT, 2), 0x03FFu & ~(uint32_t)KA);
    mach_free(&m);
}

static const dop keys_read[] = {DR(2, KEYINPUT), DR(1, KEYINPUT), DR(1, KEYINPUT + 1), DR(4, KEYINPUT), DEND};
static const dop keycnt_and_if[] = {
    DW(2, KEYCNT, 0xC000u | KA | KB), DR(2, KEYCNT), DR(2, IF_), DL(20), DR(2, IF_), DEND,
};
static const dop keycnt_or_if[] = {
    DW(2, KEYCNT, 0x4000u | KL | KDOWN), DR(2, KEYCNT), DR(2, IF_), DL(20), DR(2, IF_), DEND,
};

static bool diff_keys(const char *name, const dop *ops, uint16_t keys)
{
    dsetup cfg = {.keys = keys};
    unsigned reads = 0, diffs = 0;
    for (unsigned pad = 0; pad < 4; pad++) {
        dres r = oracle_run_cfg(name, ops, pad, 0, 0, &cfg);
        if (r.skipped) {
            printf("  %s: oracle absent, skipped\n", name);
            return true;
        }
        if (r.broken)
            return false;
        reads += r.reads;
        diffs += r.diffs;
    }
    printf("  %s: %u reads, %u differ\n", name, reads, diffs);
    return diffs == 0;
}

TEST(diff_keyinput_with_keys_held)
{
    CHECK(diff_keys("keyinput none", keys_read, 0));
    CHECK(diff_keys("keyinput some", keys_read, KA | KSELECT | KUP | KL));
    CHECK(diff_keys("keyinput all", keys_read, 0x3FF));
}

TEST(diff_keypad_interrupt_flag)
{
    CHECK(diff_keys("keycnt and, both held", keycnt_and_if, KA | KB));
    CHECK(diff_keys("keycnt and, one held", keycnt_and_if, KA));
    CHECK(diff_keys("keycnt or, one held", keycnt_or_if, KDOWN));
    CHECK(diff_keys("keycnt or, none held", keycnt_or_if, KA));
}
