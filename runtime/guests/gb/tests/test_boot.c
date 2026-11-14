/**
 * @file test_boot.c
 * @brief The post-boot table: registers per revision against the values the
 * Pan Docs list, the I/O defaults, the header-dependent parts, and what
 * rl_gb_reset and rl_gb_attach do with it.
 */
#include "gbt.h"

TEST(every_revision_and_mode_has_an_entry)
{
    for (unsigned m = 0; m < RL_GB_MODELS; m++) {
        for (unsigned mode = 0; mode < 2; mode++) {
            const rl_gb_boot *b = rl_gb_boot_state((rl_gb_model)m, mode != 0);
            CHECK(b != NULL);
            CHECK_EQ(b->model, m);
            CHECK_EQ(b->color_mode, mode && rl_gb_model_is_cgb((rl_gb_model)m));
            CHECK_EQ(b->pc, 0x0100);
            CHECK_EQ(b->sp, 0xFFFE);
            CHECK_EQ(b->f & 0x0F, 0);
            CHECK_EQ(b->io[0x40], 0x91);
            CHECK_EQ(b->io[0x47], 0xFC);
            CHECK_EQ(b->io[0x0F], 0xE1);
            CHECK_EQ(b->io[0x50], 0xFF);
            CHECK_EQ(b->io[0x04], b->div_counter >> 8);
            CHECK_EQ(b->ie, 0);
        }
    }
    CHECK(rl_gb_boot_state(RL_GB_MODELS, false) == NULL);
}

#define REGS(m, mode, ea, ef, ebc, ede, ehl)                                                                         \
    do {                                                                                                        \
        const rl_gb_boot *b_ = rl_gb_boot_state(m, mode);                                                       \
        CHECK_EQ(b_->a, ea);                                                                                     \
        CHECK_EQ(b_->f, ef);                                                                                     \
        CHECK_EQ(((unsigned)b_->b << 8) | b_->c, ebc);                                                           \
        CHECK_EQ(((unsigned)b_->d << 8) | b_->e, ede);                                                           \
        CHECK_EQ(((unsigned)b_->h << 8) | b_->l, ehl);                                                           \
    } while (0)

TEST(registers_match_the_documented_hand_off)
{
    REGS(RL_GB_DMG0, false, 0x01, 0x00, 0xFF13, 0x00C1, 0x8403);
    REGS(RL_GB_DMG, false, 0x01, 0x80, 0x0013, 0x00D8, 0x014D);
    REGS(RL_GB_MGB, false, 0xFF, 0x80, 0x0013, 0x00D8, 0x014D);
    REGS(RL_GB_SGB, false, 0x01, 0x00, 0x0014, 0x0000, 0xC060);
    REGS(RL_GB_SGB2, false, 0xFF, 0x00, 0x0014, 0x0000, 0xC060);
    REGS(RL_GB_CGB0, true, 0x11, 0x80, 0x0000, 0xFF56, 0x000D);
    REGS(RL_GB_CGBB, true, 0x11, 0x80, 0x0000, 0xFF56, 0x000D);
    REGS(RL_GB_CGB, true, 0x11, 0x80, 0x0000, 0xFF56, 0x000D);
    REGS(RL_GB_CGBE, true, 0x11, 0x80, 0x0000, 0xFF56, 0x000D);
    REGS(RL_GB_AGB, true, 0x11, 0x00, 0x0100, 0xFF56, 0x000D);
}

TEST(color_hardware_with_a_monochrome_cartridge_hands_off_differently)
{
    REGS(RL_GB_CGB, false, 0x11, 0x80, 0x0000, 0x0008, 0x007C);
    REGS(RL_GB_AGB, false, 0x11, 0x00, 0x0100, 0x0008, 0x007C);
    CHECK_EQ(rl_gb_boot_state(RL_GB_CGB, false)->io[0x00], 0xFF);
    CHECK_EQ(rl_gb_boot_state(RL_GB_CGB, true)->io[0x00], 0xCF);
    CHECK_EQ(rl_gb_boot_state(RL_GB_CGB, false)->io[0x04], 0x26);
    CHECK_EQ(rl_gb_boot_state(RL_GB_CGB, true)->io[0x04], 0x1E);
    CHECK_EQ(rl_gb_boot_state(RL_GB_DMG, true)->color_mode, false);
}

TEST(io_defaults_of_the_original_model)
{
    const rl_gb_boot *b = rl_gb_boot_state(RL_GB_DMG, false);
    CHECK_EQ(b->io[0x00], 0xCF);
    CHECK_EQ(b->io[0x01], 0x00);
    CHECK_EQ(b->io[0x02], 0x7E);
    CHECK_EQ(b->io[0x04], 0xAB);
    CHECK_EQ(b->io[0x05], 0x00);
    CHECK_EQ(b->io[0x06], 0x00);
    CHECK_EQ(b->io[0x07], 0xF8);
    CHECK_EQ(b->io[0x10], 0x80);
    CHECK_EQ(b->io[0x11], 0xBF);
    CHECK_EQ(b->io[0x12], 0xF3);
    CHECK_EQ(b->io[0x14], 0xBF);
    CHECK_EQ(b->io[0x24], 0x77);
    CHECK_EQ(b->io[0x25], 0xF3);
    CHECK_EQ(b->io[0x26], 0xF1);
    CHECK_EQ(b->io[0x41], 0x85);
    CHECK_EQ(b->io[0x42], 0x00);
    CHECK_EQ(b->io[0x43], 0x00);
    CHECK_EQ(b->io[0x45], 0x00);
    CHECK_EQ(b->io[0x48], 0xFF);
    CHECK_EQ(b->io[0x49], 0xFF);
    CHECK_EQ(b->io[0x4A], 0x00);
    CHECK_EQ(b->io[0x4B], 0x00);
    CHECK_EQ(b->io[0x4D], 0xFF);
    CHECK_EQ(b->io[0x4F], 0xFF);
    CHECK_EQ(b->io[0x70], 0xFF);
    CHECK_EQ(b->div_counter, 0xABCC);
    CHECK_EQ(b->frame_dot, 153 * 456 + 396);
    CHECK_EQ(rl_gb_boot_state(RL_GB_DMG0, false)->div_counter, 0x1830);
    CHECK_EQ(rl_gb_boot_state(RL_GB_DMG0, false)->io[0x41], 0x81);
    CHECK_EQ(rl_gb_boot_state(RL_GB_DMG0, false)->frame_dot, 145 * 456 + 198);
    CHECK_EQ(rl_gb_boot_state(RL_GB_SGB, false)->io[0x26], 0xF0);
}

TEST(io_defaults_of_color_hardware)
{
    const rl_gb_boot *b = rl_gb_boot_state(RL_GB_CGB, true);
    CHECK_EQ(b->io[0x4D], 0x7E);
    CHECK_EQ(b->io[0x4F], 0xFE);
    CHECK_EQ(b->io[0x51], 0xFF);
    CHECK_EQ(b->io[0x55], 0xFF);
    CHECK_EQ(b->io[0x56], 0x3E);
    CHECK_EQ(b->io[0x68], 0xC0);
    CHECK_EQ(b->io[0x6A], 0xC1);
    CHECK_EQ(b->io[0x6C], 0xFE);
    CHECK_EQ(b->io[0x70], 0xF8);
    CHECK_EQ(b->io[0x75], 0x8F);
    CHECK_EQ(b->io[0x48], 0x00);
    CHECK_EQ(b->io[0x49], 0x00);
    CHECK_EQ(b->div_counter, 0x1EA0);
    CHECK_EQ(b->frame_dot, 144 * 456 + 164);
    CHECK_EQ(rl_gb_boot_state(RL_GB_AGB, true)->frame_dot, 144 * 456 + 168);
    CHECK_EQ(rl_gb_boot_state(RL_GB_AGB, true)->div_counter, 0x1EA4);
    CHECK_EQ(rl_gb_boot_state(RL_GB_CGB0, true)->div_counter, 0x2884);
    CHECK_EQ(rl_gb_boot_state(RL_GB_CGBE, true)->div_counter, 0x1EA0);
}

TEST(wave_ram_defaults_differ_between_families)
{
    const rl_gb_boot *d = rl_gb_boot_state(RL_GB_DMG, false);
    const rl_gb_boot *c = rl_gb_boot_state(RL_GB_CGB, true);
    CHECK_EQ(d->io[0x30], 0x71);
    CHECK_EQ(d->io[0x3F], 0xD9);
    CHECK_EQ(c->io[0x30], 0x00);
    CHECK_EQ(c->io[0x31], 0xFF);
    CHECK_EQ(c->io[0x3E], 0x00);
    CHECK_EQ(c->io[0x3F], 0xFF);
}

TEST(flags_of_the_original_model_follow_the_header_checksum)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0);
    rl_gb_boot b;
    rl_gb_boot_state_for(RL_GB_DMG, false, rom, 2 * 0x4000, &b);
    CHECK_EQ(b.f, 0x80);
    rom[0x14D] = 0x10;
    rl_gb_boot_state_for(RL_GB_DMG, false, rom, 2 * 0x4000, &b);
    CHECK_EQ(b.f, 0x90);
    rom[0x14D] = 0x99;
    rl_gb_boot_state_for(RL_GB_DMG, false, rom, 2 * 0x4000, &b);
    CHECK_EQ(b.f, 0xB0);
    rl_gb_boot_state_for(RL_GB_MGB, false, rom, 2 * 0x4000, &b);
    CHECK_EQ(b.f, 0xB0);
    rl_gb_boot_state_for(RL_GB_CGB, true, rom, 2 * 0x4000, &b);
    CHECK_EQ(b.f, 0x80);
    rl_gb_boot_state_for(RL_GB_DMG0, false, rom, 2 * 0x4000, &b);
    CHECK_EQ(b.f, 0x00);
    free(rom);
}

TEST(super_game_boy_divider_follows_the_header_bits)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0);
    rl_gb_boot b;
    rl_gb_boot_state_for(RL_GB_SGB, false, rom, 2 * 0x4000, &b);
    CHECK_EQ(b.div_counter, 0xDC88 - 4 * 0);
    unsigned bits = 0;
    for (unsigned a = 0x104; a <= 0x14F; a++)
        bits += (unsigned)__builtin_popcount(rom[a]);
    rom[0x110] = 0xFF;
    rom[0x14E] = 0x0F;
    rl_gb_boot_state_for(RL_GB_SGB2, false, rom, 2 * 0x4000, &b);
    CHECK_EQ(b.div_counter, 0xDC88 - 4 * (bits + 12));
    CHECK_EQ(b.io[0x04], b.div_counter >> 8);
    free(rom);
}

TEST(create_applies_the_state_for_the_model_and_cartridge)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0x00);
    rl_gb *g = gbt_machine(RL_GB_CGB, rom, 2);
    const rl_gb_boot *b = rl_gb_boot_of(g);
    CHECK_EQ(b->model, RL_GB_CGB);
    CHECK(!b->color_mode);
    CHECK_EQ(b->e, 0x08);
    rl_gb_destroy(g);
    rom[0x143] = 0x80;
    g = gbt_machine(RL_GB_CGB, rom, 2);
    CHECK(rl_gb_boot_of(g)->color_mode);
    CHECK_EQ(rl_gb_boot_of(g)->e, 0x56);
    CHECK_EQ(rl_gb_vram_bank(g), 0);
    CHECK_EQ(rl_gb_wram_bank(g), 1);
    rl_gb_write(g, NULL, 0xFF4F, 1);
    rl_gb_write(g, NULL, 0xFF70, 6);
    rl_gb_reset(g);
    CHECK_EQ(rl_gb_vram_bank(g), 0);
    CHECK_EQ(rl_gb_wram_bank(g), 1);
    rl_gb_destroy(g);
    free(rom);
}

TEST(reset_restores_the_controller_registers_and_keeps_the_ram)
{
    uint8_t *rom = gbt_rom(8, 0x03, 0x02, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 8);
    rl_gb_write(g, NULL, 0x0000, 0x0A);
    rl_gb_write(g, NULL, 0x2000, 0x05);
    rl_gb_write(g, NULL, 0xA000, 0x42);
    rl_gb_write(g, NULL, 0xC000, 0x43);
    rl_gb_reset(g);
    CHECK_EQ(rl_gb_rom_bank1(g), 1);
    CHECK(!rl_gb_ram_enabled(g));
    rl_gb_write(g, NULL, 0x0000, 0x0A);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA000), 0x42);
    CHECK_EQ(rl_gb_read(g, NULL, 0xC000), 0x43);
    rl_gb_destroy(g);
    free(rom);
}

TEST(attached_stubs_read_the_post_boot_values)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0x80);
    rl_gb *g = gbt_machine(RL_GB_CGB, rom, 2);
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    CHECK_EQ(rl_gb_attach(g, s, NULL), 0);
    const rl_gb_boot *b = rl_gb_boot_of(g);
    for (unsigned a = 0xFF00; a < 0xFF80; a++) {
        bool owned = false;
        for (const rl_gb_io_range *r = rl_gb_io_ranges; r->hi; r++)
            owned = owned || (a >= r->lo && a <= r->hi);
        if (owned)
            CHECK_EQ(rl_gb_read(g, k, (uint16_t)a), b->io[a - 0xFF00]);
    }
    CHECK_EQ(rl_gb_read(g, k, 0xFF40), 0x91);
    CHECK_EQ(rl_gb_read(g, k, 0xFF4D), 0x7E);
    CHECK_EQ(rl_gb_read(g, k, 0xFFFF), 0x00);
    CHECK_EQ(rl_gb_read(g, k, 0xFF4F), 0xFE);
    CHECK_EQ(rl_gb_read(g, k, 0xFF70), 0xF8);
    rl_gb_write(g, k, 0xFF40, 0x00);
    rl_gb_reset(g);
    CHECK_EQ(rl_gb_read(g, k, 0xFF40), 0x91);
    rl_gb_destroy(g);
    rl_sched_destroy(s);
    free(rom);
}
