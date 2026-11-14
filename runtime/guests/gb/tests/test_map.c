/**
 * @file test_map.c
 * @brief The address map: cartridge parsing, ROM without a controller, video
 * RAM, work RAM and echo RAM, OAM, the unusable cells and high RAM, and the
 * color-hardware bank registers.
 */
#include "gbt.h"

TEST(cart_parse_reads_the_header)
{
    uint8_t *rom = gbt_rom(4, 0x1E, 0x03, 0x80);
    rl_gb_cart c;
    CHECK_EQ(rl_gb_cart_parse(rom, 4 * 0x4000, &c), 0);
    CHECK_EQ(c.mbc, RL_GB_MBC5);
    CHECK(c.battery && c.rumble && c.cgb && !c.rtc && !c.multicart);
    CHECK_EQ(c.ram_len, 0x8000);
    rom[0x147] = 0x04;
    CHECK_EQ(rl_gb_cart_parse(rom, 4 * 0x4000, &c), -1);
    rom[0x147] = 0x22;
    CHECK_EQ(rl_gb_cart_parse(rom, 4 * 0x4000, &c), 0);
    CHECK_EQ(c.mbc, RL_GB_MBC_UNSUPPORTED);
    CHECK_EQ(rl_gb_cart_parse(rom, 0x100, &c), -1);
    free(rom);
}

TEST(unsupported_controller_is_refused)
{
    uint8_t *rom = gbt_rom(2, 0xFC, 0x02, 0);
    rl_gb_cart c;
    CHECK_EQ(rl_gb_cart_parse(rom, 2 * 0x4000, &c), 0);
    rl_gb_status st = RL_GB_OK;
    CHECK(rl_gb_create(RL_GB_DMG, &c, &st) == NULL);
    CHECK_EQ(st, RL_GB_ERR_UNSUPPORTED);
    c.rom_len = 100;
    c.mbc = RL_GB_MBC_NONE;
    CHECK(rl_gb_create(RL_GB_DMG, &c, &st) == NULL);
    CHECK_EQ(st, RL_GB_ERR_ROM);
    free(rom);
}

TEST(rom_only_maps_both_halves_and_ignores_writes)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 2);
    CHECK(g != NULL);
    CHECK_EQ(gbt_bank_at(g, 0x0000), 0);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 1);
    CHECK_EQ(rl_gb_read(g, NULL, 0x3FFF), 0xA5);
    CHECK_EQ(rl_gb_read(g, NULL, 0x7FFF), 0xA5);
    rl_gb_write(g, NULL, 0x2000, 0x00);
    rl_gb_write(g, NULL, 0x0000, 0x0A);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 1);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0xFF);
    rl_gb_destroy(g);
    free(rom);
}

TEST(rom_only_ram_is_always_on)
{
    uint8_t *rom = gbt_rom(2, 0x08, 0x02, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 2);
    CHECK(g != NULL);
    CHECK(rl_gb_ram_enabled(g));
    rl_gb_write(g, NULL, 0xA000, 0x42);
    rl_gb_write(g, NULL, 0xBFFF, 0x43);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0x42);
    CHECK_EQ(rl_gb_read(g, NULL, 0xBFFF), 0x43);
    size_t n = 0;
    CHECK(rl_gb_ram(g, &n) != NULL);
    CHECK_EQ(n, 0x2000);
    rl_gb_destroy(g);
    free(rom);
}

TEST(small_ram_chip_mirrors)
{
    uint8_t *rom = gbt_rom(2, 0x08, 0x01, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 2);
    CHECK(g != NULL);
    rl_gb_write(g, NULL, 0xA000, 0x11);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA800), 0x11);
    CHECK_EQ(rl_gb_read(g, NULL, 0xB800), 0x11);
    rl_gb_destroy(g);
    free(rom);
}

TEST(vram_work_ram_and_echo)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 2);
    rl_gb_write(g, NULL, 0x8000, 0x12);
    rl_gb_write(g, NULL, 0x9FFF, 0x34);
    CHECK_EQ(rl_gb_vram(g, 0)[0], 0x12);
    CHECK_EQ(rl_gb_vram(g, 0)[0x1FFF], 0x34);
    rl_gb_write(g, NULL, 0xC000, 0x56);
    rl_gb_write(g, NULL, 0xDDFF, 0x78);
    CHECK_EQ(rl_gb_read(g, NULL, 0xE000), 0x56);
    CHECK_EQ(rl_gb_read(g, NULL, 0xFDFF), 0x78);
    rl_gb_write(g, NULL, 0xE001, 0x9A);
    CHECK_EQ(rl_gb_read(g, NULL, 0xC001), 0x9A);
    rl_gb_write(g, NULL, 0xF123, 0xBC);
    CHECK_EQ(rl_gb_read(g, NULL, 0xD123), 0xBC);
    CHECK_EQ(rl_gb_read(g, NULL, 0xF123), 0xBC);
    rl_gb_destroy(g);
    free(rom);
}

TEST(oam_and_hram)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 2);
    rl_gb_write(g, NULL, 0xFE00, 0x21);
    rl_gb_write(g, NULL, 0xFE9F, 0x22);
    CHECK_EQ(rl_gb_oam(g)[0], 0x21);
    CHECK_EQ(rl_gb_oam(g)[0x9F], 0x22);
    rl_gb_write(g, NULL, 0xFF80, 0x31);
    rl_gb_write(g, NULL, 0xFFFE, 0x32);
    CHECK_EQ(rl_gb_hram(g)[0], 0x31);
    CHECK_EQ(rl_gb_read(g, NULL, 0xFFFE), 0x32);
    rl_gb_destroy(g);
    free(rom);
}

TEST(unusable_cells_read_zero_on_monochrome_hardware)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 2);
    rl_gb_write(g, NULL, 0xFEA0, 0x55);
    CHECK_EQ(rl_gb_read(g, NULL, 0xFEA0), 0x00);
    CHECK_EQ(rl_gb_read(g, NULL, 0xFEFF), 0x00);
    rl_gb_destroy(g);
    free(rom);
}

TEST(unusable_cells_alias_on_color_hardware)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0x80);
    rl_gb *g = gbt_machine(RL_GB_CGB, rom, 2);
    rl_gb_write(g, NULL, 0xFEA0, 0x55);
    CHECK_EQ(rl_gb_read(g, NULL, 0xFEA0), 0x55);
    CHECK_EQ(rl_gb_read(g, NULL, 0xFEB8), 0x55);
    CHECK_EQ(rl_gb_read(g, NULL, 0xFEE0 - 0x40), 0x55);
    rl_gb_destroy(g);
    free(rom);
}

TEST(color_vram_bank_register)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0x80);
    rl_gb *g = gbt_machine(RL_GB_CGB, rom, 2);
    CHECK(rl_gb_color_mode(g));
    CHECK_EQ(rl_gb_read(g, NULL, 0xFF4F), 0xFE);
    rl_gb_write(g, NULL, 0x8000, 0xA1);
    rl_gb_write(g, NULL, 0xFF4F, 0x01);
    CHECK_EQ(rl_gb_vram_bank(g), 1);
    CHECK_EQ(rl_gb_read(g, NULL, 0xFF4F), 0xFF);
    CHECK_EQ(rl_gb_read(g, NULL, 0x8000), 0x00);
    rl_gb_write(g, NULL, 0x8000, 0xB2);
    rl_gb_write(g, NULL, 0xFF4F, 0xFE);
    CHECK_EQ(rl_gb_read(g, NULL, 0x8000), 0xA1);
    CHECK_EQ(rl_gb_vram(g, 1)[0], 0xB2);
    rl_gb_destroy(g);
    free(rom);
}

TEST(color_work_ram_bank_register)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0x80);
    rl_gb *g = gbt_machine(RL_GB_CGB, rom, 2);
    CHECK_EQ(rl_gb_read(g, NULL, 0xFF70), 0xF8);
    CHECK_EQ(rl_gb_wram_bank(g), 1);
    for (unsigned b = 0; b < 8; b++) {
        rl_gb_write(g, NULL, 0xFF70, (uint8_t)b);
        rl_gb_write(g, NULL, 0xD000, (uint8_t)(0x10 + b));
        rl_gb_write(g, NULL, 0xDFFF, (uint8_t)(0x20 + b));
    }
    rl_gb_write(g, NULL, 0xC000, 0x77);
    for (unsigned b = 1; b < 8; b++) {
        rl_gb_write(g, NULL, 0xFF70, (uint8_t)b);
        CHECK_EQ(rl_gb_wram_bank(g), b);
        CHECK_EQ(rl_gb_read(g, NULL, 0xFF70), 0xF8 | b);
        CHECK_EQ(rl_gb_read(g, NULL, 0xD000), 0x10 + b);
        CHECK_EQ(rl_gb_read(g, NULL, 0xDFFF), 0x20 + b);
        CHECK_EQ(rl_gb_read(g, NULL, 0xF000), 0x10 + b);
        CHECK_EQ(rl_gb_read(g, NULL, 0xC000), 0x77);
        CHECK_EQ(rl_gb_read(g, NULL, 0xE000), 0x77);
    }
    rl_gb_write(g, NULL, 0xFF70, 0x00);
    CHECK_EQ(rl_gb_wram_bank(g), 1);
    rl_gb_destroy(g);
    free(rom);
}

TEST(bank_registers_do_nothing_without_color_mode)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0x00);
    rl_gb *g = gbt_machine(RL_GB_CGB, rom, 2);
    CHECK(!rl_gb_color_mode(g));
    rl_gb_write(g, NULL, 0xFF4F, 1);
    rl_gb_write(g, NULL, 0xFF70, 5);
    CHECK_EQ(rl_gb_vram_bank(g), 0);
    CHECK_EQ(rl_gb_wram_bank(g), 1);
    CHECK_EQ(rl_gb_read(g, NULL, 0xFF4F), 0xFF);
    CHECK_EQ(rl_gb_read(g, NULL, 0xFF70), 0xFF);
    rl_gb_destroy(g);
    free(rom);
    rom = gbt_rom(2, 0x00, 0x00, 0x80);
    g = gbt_machine(RL_GB_DMG, rom, 2);
    CHECK(!rl_gb_color_mode(g));
    rl_gb_write(g, NULL, 0xFF70, 5);
    CHECK_EQ(rl_gb_wram_bank(g), 1);
    rl_gb_destroy(g);
    free(rom);
}
