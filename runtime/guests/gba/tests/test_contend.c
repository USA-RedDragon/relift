/**
 * @file test_contend.c
 * @brief Video memory contention: a CPU access to video RAM, palette RAM or
 * OAM waits a cycle when the picture unit reads that memory in the same
 * cycle. The expected cycles are the ones NanoBoyAdvance (the oracle of
 * check/oracle/nba) charges.
 */
#include "mach.h"

#define LINE 1232u

/** @brief Cycles a store of size bytes to addr takes when issued at dot d of line 12 (DISPCNT set three latches before). */
static unsigned store_at(uint16_t dispcnt, uint32_t addr, unsigned size, unsigned d)
{
    mach m = mach_new();
    rl_gba_write(m.g, 0x04000008u, 2, 0x0000);
    rl_gba_write(m.g, 0x04000000u, 2, dispcnt);
    rl_gba_lcd_set(m.g, 9, 0);
    uint64_t t0 = mach_now(&m) + 3 * LINE;
    mach_to(&m, t0 + d);
    uint64_t before = mach_now(&m);
    rl_gba_store(m.g, addr, size, 0, false);
    unsigned n = (unsigned)(mach_now(&m) - before);
    mach_free(&m);
    return n;
}

TEST(a_text_background_map_read_delays_a_vram_store)
{
    CHECK_EQ(store_at(0x0100, 0x06000000u, 2, 30), 1u);
    CHECK_EQ(store_at(0x0100, 0x06000000u, 2, 31), 2u);
    CHECK_EQ(store_at(0x0100, 0x06000000u, 2, 32), 1u);
    CHECK_EQ(store_at(0x0100, 0x06000000u, 2, 35), 2u);
    CHECK_EQ(store_at(0x0100, 0x06000000u, 2, 63), 2u);
}

TEST(a_word_store_pays_for_each_halfword)
{
    CHECK_EQ(store_at(0x0100, 0x06000000u, 4, 30), 3u);
    CHECK_EQ(store_at(0x0100, 0x06000000u, 4, 31), 3u);
    CHECK_EQ(store_at(0x0100, 0x06000000u, 4, 29), 2u);
}

TEST(forced_blank_and_disabled_layers_read_nothing)
{
    CHECK_EQ(store_at(0x0180, 0x06000000u, 2, 31), 1u);
    CHECK_EQ(store_at(0x0000, 0x06000000u, 2, 31), 1u);
}

TEST(background_reads_do_not_delay_object_vram)
{
    CHECK_EQ(store_at(0x0100, 0x06010000u, 2, 31), 1u);
}

TEST(the_pixel_merge_reads_palette_ram_from_dot_46)
{
    CHECK_EQ(store_at(0x0000, 0x05000000u, 2, 44), 1u);
    CHECK_EQ(store_at(0x0000, 0x05000000u, 2, 46), 2u);
    CHECK_EQ(store_at(0x0000, 0x05000000u, 2, 47), 1u);
    CHECK_EQ(store_at(0x0000, 0x05000000u, 2, 50), 2u);
}

TEST(the_sprite_unit_walks_oam_from_dot_40)
{
    CHECK_EQ(store_at(0x1000, 0x07000000u, 2, 38), 1u);
    CHECK_EQ(store_at(0x1000, 0x07000000u, 2, 40), 2u);
    CHECK_EQ(store_at(0x1000, 0x07000000u, 2, 41), 1u);
    CHECK_EQ(store_at(0x0000, 0x07000000u, 2, 40), 1u);
}
