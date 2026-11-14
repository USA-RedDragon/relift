/**
 * @file test_mbc.c
 * @brief Every memory bank controller: MBC1 (bank 0 remap, banking modes,
 * multicart), MBC2 (nibble RAM), MBC3 (banks, clock, latch) and MBC5 (9-bit
 * ROM bank, rumble).
 */
#include "gbt.h"

static void put(rl_gb *g, uint16_t addr, uint8_t v) { rl_gb_write(g, NULL, addr, v); }

TEST(mbc1_bank_zero_selects_bank_one)
{
    uint8_t *rom = gbt_rom(32, 0x01, 0, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 32);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 1);
    put(g, 0x2000, 0x00);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 1);
    put(g, 0x2000, 0x1F);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 31);
    put(g, 0x2000, 0x20);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 1);
    put(g, 0x2000, 0x05);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 5);
    CHECK_EQ(gbt_bank_at(g, 0x0000), 0);
    rl_gb_destroy(g);
    free(rom);
}

TEST(mbc1_bank_number_wraps_to_the_rom_size)
{
    uint8_t *rom = gbt_rom(4, 0x01, 0, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 4);
    put(g, 0x2000, 0x06);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 2);
    put(g, 0x2000, 0x04);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0);
    rl_gb_destroy(g);
    free(rom);
}

TEST(mbc1_upper_bits_and_the_remap_of_banks_20_40_60)
{
    uint8_t *rom = gbt_rom(128, 0x01, 0, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 128);
    put(g, 0x4000, 0x01);
    put(g, 0x2000, 0x00);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x21);
    CHECK_EQ(gbt_bank_at(g, 0x0000), 0);
    put(g, 0x4000, 0x02);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x41);
    put(g, 0x4000, 0x03);
    put(g, 0x2000, 0x1F);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x7F);
    put(g, 0x4000, 0xFD);
    put(g, 0x2000, 0x03);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x23);
    rl_gb_destroy(g);
    free(rom);
}

TEST(mbc1_mode_one_remaps_bank_zero_and_ram)
{
    uint8_t *rom = gbt_rom(128, 0x03, 0x03, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 128);
    put(g, 0x0000, 0x0A);
    for (unsigned b = 0; b < 4; b++) {
        put(g, 0x6000, 0x01);
        put(g, 0x4000, (uint8_t)b);
        put(g, 0xA000, (uint8_t)(0x40 + b));
    }
    put(g, 0x6000, 0x00);
    put(g, 0x4000, 0x02);
    CHECK_EQ(gbt_bank_at(g, 0x0000), 0);
    CHECK_EQ(rl_gb_ram_bank(g), 0);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0x40);
    put(g, 0x6000, 0x01);
    CHECK_EQ(gbt_bank_at(g, 0x0000), 0x40);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x41);
    CHECK_EQ(rl_gb_ram_bank(g), 2);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0x42);
    put(g, 0x4000, 0x03);
    CHECK_EQ(gbt_bank_at(g, 0x0000), 0x60);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0x43);
    rl_gb_destroy(g);
    free(rom);
}

TEST(mbc1_ram_needs_the_enable_value)
{
    uint8_t *rom = gbt_rom(4, 0x03, 0x02, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 4);
    CHECK(!rl_gb_ram_enabled(g));
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0xFF);
    put(g, 0xA000, 0x12);
    put(g, 0x0000, 0x0A);
    CHECK(rl_gb_ram_enabled(g));
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0x00);
    put(g, 0xA000, 0x12);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0x12);
    put(g, 0x1FFF, 0x3A);
    CHECK(rl_gb_ram_enabled(g));
    put(g, 0x0000, 0x0B);
    CHECK(!rl_gb_ram_enabled(g));
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0xFF);
    put(g, 0xA000, 0x99);
    put(g, 0x0000, 0x0A);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0x12);
    rl_gb_destroy(g);
    free(rom);
}

static uint8_t *multicart_rom(void)
{
    uint8_t *rom = gbt_rom(64, 0x01, 0, 0);
    for (unsigned b = 0; b < 64; b += 0x10)
        for (unsigned i = 0; i < 48; i++)
            rom[(size_t)b * 0x4000 + 0x104 + i] = (uint8_t)(0xCE + i);
    return rom;
}

TEST(mbc1_multicart_is_detected_and_uses_four_bank_bits)
{
    uint8_t *rom = multicart_rom();
    rl_gb_cart c;
    CHECK_EQ(rl_gb_cart_parse(rom, 64 * 0x4000, &c), 0);
    CHECK(c.multicart);
    rl_gb *g = rl_gb_create(RL_GB_DMG, &c, NULL);
    put(g, 0x4000, 0x01);
    put(g, 0x2000, 0x03);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x13);
    put(g, 0x2000, 0x1F);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x1F);
    put(g, 0x2000, 0x10);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x10);
    put(g, 0x2000, 0x00);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x11);
    put(g, 0x4000, 0x03);
    put(g, 0x2000, 0x02);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x32);
    CHECK_EQ(gbt_bank_at(g, 0x0000), 0);
    put(g, 0x6000, 0x01);
    CHECK_EQ(gbt_bank_at(g, 0x0000), 0x30);
    rl_gb_destroy(g);
    free(rom);
}

TEST(mbc1_one_megabyte_game_is_not_a_multicart)
{
    uint8_t *rom = gbt_rom(64, 0x01, 0, 0);
    rl_gb_cart c;
    CHECK_EQ(rl_gb_cart_parse(rom, 64 * 0x4000, &c), 0);
    CHECK(!c.multicart);
    free(rom);
}

TEST(mbc2_rom_bank_register_is_selected_by_address_bit_8)
{
    uint8_t *rom = gbt_rom(16, 0x06, 0, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 16);
    put(g, 0x2100, 0x07);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 7);
    put(g, 0x0000, 0x0F);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 7);
    CHECK(!rl_gb_ram_enabled(g));
    put(g, 0x2100, 0x00);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 1);
    put(g, 0x3F00, 0x13);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 3);
    put(g, 0x4100, 0x05);
    put(g, 0x6000, 0x05);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 3);
    rl_gb_destroy(g);
    free(rom);
}

TEST(mbc2_ram_holds_nibbles_and_mirrors)
{
    uint8_t *rom = gbt_rom(16, 0x06, 0, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 16);
    put(g, 0xA000, 0x05);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0xFF);
    put(g, 0x1E00, 0x0A);
    CHECK(rl_gb_ram_enabled(g));
    put(g, 0xA000, 0x35);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0xF5);
    put(g, 0xA1FF, 0xC9);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA1FF), 0xF9);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA200), 0xF5);
    CHECK_EQ(rl_gb_read(g, NULL, 0xBFFF), 0xF9);
    put(g, 0xA3FF, 0x02);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA1FF), 0xF2);
    size_t n = 0;
    CHECK(rl_gb_ram(g, &n) != NULL);
    CHECK_EQ(n, 512);
    put(g, 0x0000, 0x00);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0xFF);
    rl_gb_destroy(g);
    free(rom);
}

TEST(mbc3_rom_and_ram_banks)
{
    uint8_t *rom = gbt_rom(128, 0x13, 0x03, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 128);
    put(g, 0x2000, 0x00);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 1);
    put(g, 0x2000, 0x7F);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x7F);
    put(g, 0x2000, 0xFF);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x7F);
    put(g, 0x2000, 0x45);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x45);
    put(g, 0x0000, 0x0A);
    for (unsigned b = 0; b < 4; b++) {
        put(g, 0x4000, (uint8_t)b);
        put(g, 0xA000, (uint8_t)(0x70 + b));
    }
    for (unsigned b = 0; b < 4; b++) {
        put(g, 0x4000, (uint8_t)b);
        CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0x70 + b);
    }
    put(g, 0x4000, 0x05);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0xFF);
    rl_gb_destroy(g);
    free(rom);
}

typedef struct fake_time {
    uint64_t now;
} fake_time;

static uint64_t fake_now(void *ctx) { return ((fake_time *)ctx)->now; }

static rl_gb *clock_machine(uint8_t **rom, fake_time *t)
{
    *rom = gbt_rom(4, 0x10, 0x02, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, *rom, 4);
    rl_gb_set_time_source(g, fake_now, t);
    put(g, 0x0000, 0x0A);
    return g;
}

static uint8_t clock_reg(rl_gb *g, unsigned reg)
{
    put(g, 0x4000, (uint8_t)reg);
    return rl_gb_read(g, NULL, 0xA000);
}

static void latch(rl_gb *g)
{
    put(g, 0x6000, 0x00);
    put(g, 0x6000, 0x01);
}

TEST(mbc3_clock_counts_and_latches)
{
    uint8_t *rom;
    fake_time t = {1000};
    rl_gb *g = clock_machine(&rom, &t);
    latch(g);
    CHECK_EQ(clock_reg(g, 8), 0);
    t.now += 3 * 3600 + 25 * 60 + 41;
    CHECK_EQ(clock_reg(g, 8), 0);
    latch(g);
    CHECK_EQ(clock_reg(g, 8), 41);
    CHECK_EQ(clock_reg(g, 9), 25);
    CHECK_EQ(clock_reg(g, 10), 3);
    CHECK_EQ(clock_reg(g, 11), 0);
    CHECK_EQ(clock_reg(g, 12), 0);
    t.now += 20;
    CHECK_EQ(clock_reg(g, 8), 41);
    put(g, 0x6000, 0x01);
    put(g, 0x6000, 0x01);
    CHECK_EQ(clock_reg(g, 8), 41);
    latch(g);
    CHECK_EQ(clock_reg(g, 8), 1);
    CHECK_EQ(clock_reg(g, 9), 26);
    rl_gb_destroy(g);
    free(rom);
}

TEST(mbc3_clock_registers_are_writable)
{
    uint8_t *rom;
    fake_time t = {5000};
    rl_gb *g = clock_machine(&rom, &t);
    put(g, 0x4000, 0x08);
    put(g, 0xA000, 58);
    put(g, 0x4000, 0x09);
    put(g, 0xA000, 59);
    put(g, 0x4000, 0x0A);
    put(g, 0xA000, 23);
    put(g, 0x4000, 0x0B);
    put(g, 0xA000, 0xFF);
    t.now += 2;
    latch(g);
    CHECK_EQ(clock_reg(g, 8), 0);
    CHECK_EQ(clock_reg(g, 9), 0);
    CHECK_EQ(clock_reg(g, 10), 0);
    CHECK_EQ(clock_reg(g, 11), 0);
    CHECK_EQ(clock_reg(g, 12), 1);
    rl_gb_destroy(g);
    free(rom);
}

TEST(mbc3_clock_halts_and_carries)
{
    uint8_t *rom;
    fake_time t = {0};
    rl_gb *g = clock_machine(&rom, &t);
    put(g, 0x4000, 0x0C);
    put(g, 0xA000, 0x40);
    t.now += 1000;
    latch(g);
    CHECK_EQ(clock_reg(g, 8), 0);
    CHECK_EQ(clock_reg(g, 12), 0x40);
    put(g, 0x4000, 0x0C);
    put(g, 0xA000, 0x00);
    t.now += 70;
    latch(g);
    CHECK_EQ(clock_reg(g, 8), 10);
    CHECK_EQ(clock_reg(g, 9), 1);
    put(g, 0x4000, 0x0B);
    put(g, 0xA000, 0xFF);
    put(g, 0x4000, 0x0C);
    put(g, 0xA000, 0x01);
    t.now += 86400;
    latch(g);
    CHECK_EQ(clock_reg(g, 11), 0);
    CHECK_EQ(clock_reg(g, 12), 0x80);
    put(g, 0x4000, 0x0C);
    put(g, 0xA000, 0x00);
    latch(g);
    CHECK_EQ(clock_reg(g, 12), 0x00);
    rl_gb_destroy(g);
    free(rom);
}

TEST(mbc3_without_a_clock_ignores_the_clock_registers)
{
    uint8_t *rom = gbt_rom(4, 0x13, 0x03, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 4);
    put(g, 0x0000, 0x0A);
    put(g, 0x4000, 0x08);
    put(g, 0xA000, 0x33);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0xFF);
    put(g, 0x4000, 0x00);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0x00);
    rl_gb_destroy(g);
    free(rom);
}

TEST(mbc5_has_a_nine_bit_rom_bank)
{
    uint8_t *rom = gbt_rom(512, 0x19, 0, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 512);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 1);
    put(g, 0x2000, 0x00);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0);
    put(g, 0x2000, 0xFF);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0xFF);
    put(g, 0x3000, 0x01);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x1FF);
    put(g, 0x2000, 0x34);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x134);
    put(g, 0x3000, 0xFE);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x034);
    put(g, 0x2FFF, 0x12);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 0x012);
    CHECK_EQ(gbt_bank_at(g, 0x0000), 0);
    rl_gb_destroy(g);
    free(rom);
}

TEST(mbc5_ram_banks_and_enable)
{
    uint8_t *rom = gbt_rom(4, 0x1B, 0x04, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 4);
    put(g, 0x0000, 0x1A);
    CHECK(!rl_gb_ram_enabled(g));
    put(g, 0x0000, 0x0A);
    CHECK(rl_gb_ram_enabled(g));
    for (unsigned b = 0; b < 16; b++) {
        put(g, 0x4000, (uint8_t)b);
        put(g, 0xA000, (uint8_t)(0x80 + b));
    }
    for (unsigned b = 0; b < 16; b++) {
        put(g, 0x4000, (uint8_t)b);
        CHECK_EQ(rl_gb_ram_bank(g), b);
        CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0x80 + b);
    }
    rl_gb_destroy(g);
    free(rom);
}

TEST(mbc5_rumble_bit_is_not_part_of_the_ram_bank)
{
    uint8_t *rom = gbt_rom(4, 0x1E, 0x03, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 4);
    put(g, 0x0000, 0x0A);
    put(g, 0x4000, 0x03);
    put(g, 0xA000, 0x66);
    CHECK(!rl_gb_rumble(g));
    put(g, 0x4000, 0x0B);
    CHECK(rl_gb_rumble(g));
    CHECK_EQ(rl_gb_ram_bank(g), 3);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0x66);
    put(g, 0x4000, 0x02);
    CHECK(!rl_gb_rumble(g));
    rl_gb_destroy(g);
    free(rom);
    rom = gbt_rom(4, 0x1B, 0x03, 0);
    g = gbt_machine(RL_GB_DMG, rom, 4);
    put(g, 0x4000, 0x0B);
    CHECK(!rl_gb_rumble(g));
    CHECK_EQ(rl_gb_ram_bank(g), 11);
    rl_gb_destroy(g);
    free(rom);
}
