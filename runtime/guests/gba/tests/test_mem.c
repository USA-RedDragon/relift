/**
 * @file test_mem.c
 * @brief The address map: mirrors, the 8-bit write rules, the Game Pak ROM
 * and SRAM, open bus and the I/O map.
 */
#include "mach.h"

TEST(work_ram_palette_vram_and_oam_mirror)
{
    mach m = mach_new();
    CHECK(m.g != NULL);
    rl_gba_write(m.g, 0x02000010u, 4, 0x11223344u);
    CHECK_EQ(rl_gba_read(m.g, 0x02040010u, 4), 0x11223344u);
    CHECK_EQ(rl_gba_read(m.g, 0x02FC0010u, 2), 0x3344u);
    rl_gba_write(m.g, 0x03007FFCu, 4, 0xCAFEF00Du);
    CHECK_EQ(rl_gba_read(m.g, 0x03FFFFFCu, 4), 0xCAFEF00Du);
    rl_gba_write(m.g, 0x05000402u, 2, 0x7FFFu);
    CHECK_EQ(rl_gba_read(m.g, 0x05000002u, 2), 0x7FFFu);
    rl_gba_write(m.g, 0x06010000u, 2, 0xBEEFu);
    CHECK_EQ(rl_gba_read(m.g, 0x06018000u, 2), 0xBEEFu);
    CHECK_EQ(rl_gba_read(m.g, 0x06030000u, 2), 0xBEEFu);
    rl_gba_write(m.g, 0x07000404u, 4, 0x01020304u);
    CHECK_EQ(rl_gba_read(m.g, 0x07000004u, 4), 0x01020304u);
    mach_free(&m);
}

TEST(byte_writes_follow_each_regions_rule)
{
    mach m = mach_new();
    rl_gba_write(m.g, 0x05000011u, 1, 0xA5u);
    CHECK_EQ(rl_gba_read(m.g, 0x05000010u, 2), 0xA5A5u);
    rl_gba_write(m.g, 0x06000003u, 1, 0x5Au);
    CHECK_EQ(rl_gba_read(m.g, 0x06000002u, 2), 0x5A5Au);
    rl_gba_write(m.g, 0x06010000u, 1, 0x77u);
    CHECK_EQ(rl_gba_read(m.g, 0x06010000u, 2), 0u);
    rl_gba_write(m.g, 0x07000000u, 2, 0x1234u);
    rl_gba_write(m.g, 0x07000000u, 1, 0xFFu);
    CHECK_EQ(rl_gba_read(m.g, 0x07000000u, 2), 0x1234u);
    rl_gba_write(m.g, 0x02000001u, 1, 0x42u);
    CHECK_EQ(rl_gba_read(m.g, 0x02000000u, 2), 0x4200u);
    mach_free(&m);
}

TEST(bitmap_modes_move_the_object_area_and_drop_the_vram_mirror)
{
    mach m = mach_new();
    rl_gba_write(m.g, 0x04000000u, 2, 0x0003u);
    rl_gba_write(m.g, 0x06010000u, 1, 0x77u);
    CHECK_EQ(rl_gba_read(m.g, 0x06010000u, 2), 0x7777u);
    rl_gba_write(m.g, 0x06014000u, 1, 0x66u);
    CHECK_EQ(rl_gba_read(m.g, 0x06014000u, 2), 0u);
    rl_gba_write(m.g, 0x06014000u, 2, 0x1234u);
    CHECK_EQ(rl_gba_read(m.g, 0x0601C000u, 2), 0x1234u);
    CHECK_EQ(rl_gba_read(m.g, 0x06018000u, 2), 0u);
    rl_gba_write(m.g, 0x06018000u, 2, 0x9999u);
    CHECK_EQ(rl_gba_read(m.g, 0x06010000u, 2), 0x7777u);
    mach_free(&m);
}

TEST(rom_reads_and_the_pattern_past_its_end)
{
    mach m = mach_new();
    CHECK_EQ(rl_gba_read(m.g, 0x08000000u, 2), 0x1000u);
    CHECK_EQ(rl_gba_read(m.g, 0x08000010u, 4), 0x10091008u);
    CHECK_EQ(rl_gba_read(m.g, 0x0A000010u, 2), 0x1008u);
    CHECK_EQ(rl_gba_read(m.g, 0x0C000013u, 1), 0x10u);
    CHECK_EQ(rl_gba_read(m.g, 0x08100000u, 2), 0x0000u);
    CHECK_EQ(rl_gba_read(m.g, 0x08100006u, 4), 0x00030002u);
    CHECK_EQ(rl_gba_read(m.g, 0x09FFFFFEu, 2), 0xFFFFu);
    rl_gba_write(m.g, 0x08000000u, 2, 0xFFFFu);
    CHECK_EQ(rl_gba_read(m.g, 0x08000000u, 2), 0x1000u);
    mach_free(&m);
}

TEST(sram_is_eight_bits_wide_and_mirrored)
{
    uint8_t sram[0x8000];
    for (unsigned i = 0; i < sizeof sram; i++)
        sram[i] = 0xFF;
    mach m = mach_new_sram(sram, sizeof sram);
    rl_gba_write(m.g, 0x0E000005u, 1, 0x3Cu);
    rl_gba_write(m.g, 0x0E000004u, 1, 0x3Cu);
    CHECK_EQ(sram[5], 0x3Cu);
    CHECK_EQ(rl_gba_read(m.g, 0x0E008005u, 1), 0x3Cu);
    CHECK_EQ(rl_gba_read(m.g, 0x0E000008u, 4), 0xFFFFFFFFu);
    CHECK_EQ(rl_gba_read(m.g, 0x0E000004u, 2), 0x3C3Cu);
    CHECK_EQ(rl_gba_read(m.g, 0x0F000005u, 1), 0x3Cu);
    mach_free(&m);
    mach n = mach_new();
    CHECK_EQ(rl_gba_read(n.g, 0x0E000000u, 1), 0xFFu);
    mach_free(&n);
}

TEST(unmapped_addresses_read_the_last_bus_value)
{
    mach m = mach_new();
    CHECK_EQ(rl_gba_read(m.g, 0x00000000u, 4), RL_GBA_BIOS_OPEN_BOOT);
    rl_gba_write(m.g, 0x03000000u, 4, 0x89ABCDEFu);
    CHECK_EQ(rl_gba_read(m.g, 0x03000000u, 4), 0x89ABCDEFu);
    CHECK_EQ(rl_gba_read(m.g, 0x10000000u, 4), 0x89ABCDEFu);
    CHECK_EQ(rl_gba_read(m.g, 0x00004000u, 4), 0x89ABCDEFu);
    CHECK_EQ(rl_gba_read(m.g, 0x04000400u, 4), 0x89ABCDEFu);
    CHECK_EQ(rl_gba_read(m.g, 0x040000E2u, 2), 0x89ABu);
    CHECK_EQ(rl_gba_read(m.g, 0x040000B0u, 4), 0x89AB89ABu);
    rl_gba_write(m.g, 0x05000000u, 2, 0x1357u);
    CHECK_EQ(rl_gba_read(m.g, 0x05000000u, 2), 0x1357u);
    CHECK_EQ(rl_gba_read(m.g, 0x11000000u, 4), 0x13571357u);
    CHECK_EQ(rl_gba_peek(m.g, 0x04000004u, 2), 0x1357u);
    mach_free(&m);
}

TEST(memory_control_is_mirrored_and_disables_external_work_ram)
{
    mach m = mach_new();
    CHECK_EQ(rl_gba_read(m.g, 0x04000800u, 4), 0x0D000020u);
    CHECK_EQ(rl_gba_read(m.g, 0x04FF0800u, 4), 0x0D000020u);
    rl_gba_write(m.g, 0x03000004u, 4, 0x55u);
    rl_gba_write(m.g, 0x04AB0800u, 4, 0x0E000000u);
    CHECK_EQ(rl_gba_read(m.g, 0x04000800u, 4), 0x0E000000u);
    CHECK_EQ(rl_gba_read(m.g, 0x02000004u, 4), 0x55u);
    CHECK_EQ(rl_gba_wait(rl_gba_wait_table(m.g), 0x02000000u, 2, false), 1u);
    rl_gba_write(m.g, 0x04000800u, 4, 0x0E000020u);
    CHECK_EQ(rl_gba_wait(rl_gba_wait_table(m.g), 0x02000000u, 2, false), 2u);
    CHECK_EQ(rl_gba_wait(rl_gba_wait_table(m.g), 0x02000000u, 4, true), 4u);
    mach_free(&m);
}

TEST(system_registers_read_back_their_bits)
{
    mach m = mach_new();
    mach_wr(&m, 10, 0x04000204u, 2, 0xFFFFu);
    CHECK_EQ(mach_rd(&m, 12, 0x04000204u, 2), 0x5FFFu);
    mach_wr(&m, 20, 0x04000200u, 2, 0xFFFFu);
    CHECK_EQ(mach_rd(&m, 22, 0x04000200u, 2), 0x3FFFu);
    mach_wr(&m, 30, 0x04000208u, 4, 0xFFFFFFFFu);
    CHECK_EQ(mach_rd(&m, 32, 0x04000208u, 4), 1u);
    CHECK_EQ(mach_rd(&m, 33, 0x04000206u, 2), 0u);
    mach_wr(&m, 40, 0x04000300u, 1, 1u);
    CHECK_EQ(mach_rd(&m, 41, 0x04000300u, 1), 1u);
    CHECK_EQ(mach_rd(&m, 42, 0x04000130u, 2), 0x03FFu);
    mach_wr(&m, 43, 0x04000130u, 2, 0u);
    CHECK_EQ(mach_rd(&m, 44, 0x04000130u, 2), 0x03FFu);
    CHECK_EQ(mach_rd(&m, 45, 0x04000088u, 2), 0x0200u);
    mach_wr(&m, 46, 0x04000008u, 2, 0x0123u);
    CHECK_EQ(mach_rd(&m, 47, 0x04000008u, 2), 0x0123u);
    mach_free(&m);
}

TEST(the_bios_bus_reaches_the_map)
{
    mach m = mach_new();
    const rl_gba_bus *b = rl_gba_bios_bus(m.g);
    b->write(b->user, 0x02000100u, 4, 0xA1B2C3D4u);
    CHECK_EQ(b->read(b->user, 0x02000100u, 4), 0xA1B2C3D4u);
    CHECK_EQ(b->cycles(b->user, 0x02000100u, 4, false), 6u);
    CHECK_EQ(b->cycles(b->user, 0x03000100u, 4, false), 1u);
    CHECK_EQ(b->cycles(b->user, 0x08000000u, 2, false), 5u);
    rl_gba_bios_boot_io(rl_gba_bios_of(m.g), b);
    CHECK_EQ(rl_gba_read(m.g, 0x04000300u, 1), 1u);
    mach_free(&m);
}
