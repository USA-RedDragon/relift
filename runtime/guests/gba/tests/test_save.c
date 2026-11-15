/**
 * @file test_save.c
 * @brief The cartridge backups: SRAM, the Flash command sets with their
 * busy status, the EEPROM bit stream with its size detection and busy time,
 * the raw .sav image and file, and the library string scan.
 */
#include "oracle.h"

#include <relift/settings.h>

#include <stdio.h>
#include <unistd.h>

#define SAVE 0x0E000000u
#define EEP 0x0D000000u
#define DMA3 0x040000D4u

static mach cart_with(rl_gba_save_kind kind, uint16_t id, uint32_t eeprom_len)
{
    return mach_new_cart(mach_rom(0x100000), 0x100000, (rl_gba_cart){.save = kind, .flash_id = id, .eeprom_len = eeprom_len, .flash_timed = true});
}

static mach instant_cart(rl_gba_save_kind kind, uint16_t id)
{
    return mach_new_cart(mach_rom(0x100000), 0x100000, (rl_gba_cart){.save = kind, .flash_id = id});
}

static void bw(mach *m, uint64_t t, uint32_t addr, uint32_t v)
{
    mach_to(m, t);
    rl_gba_write(m->g, addr, 1, v);
}

static uint32_t br(mach *m, uint64_t t, uint32_t addr)
{
    mach_to(m, t);
    return rl_gba_read(m->g, addr, 1);
}

static uint64_t unlock(mach *m, uint64_t t, uint8_t cmd)
{
    bw(m, t, SAVE + 0x5555, 0xAA);
    bw(m, t + 1, SAVE + 0x2AAA, 0x55);
    bw(m, t + 2, SAVE + 0x5555, cmd);
    return t + 3;
}

static uint64_t program(mach *m, uint64_t t, uint32_t off, uint8_t v)
{
    t = unlock(m, t, 0xA0);
    bw(m, t, SAVE + off, v);
    return t + 1;
}

static uint64_t sector_erase(mach *m, uint64_t t, uint32_t off)
{
    t = unlock(m, t, 0x80);
    bw(m, t, SAVE + 0x5555, 0xAA);
    bw(m, t + 1, SAVE + 0x2AAA, 0x55);
    bw(m, t + 2, SAVE + off, 0x30);
    return t + 3;
}

static uint64_t chip_erase(mach *m, uint64_t t)
{
    t = unlock(m, t, 0x80);
    return unlock(m, t, 0x10);
}

enum { AFTER_PROGRAM = 1000, AFTER_SECTOR = 200000, AFTER_CHIP = 700000 };

TEST(library_strings_name_the_backup)
{
    static const struct {
        const char *s;
        rl_gba_save_kind kind;
        uint16_t id;
    } cases[] = {
        {"EEPROM_V124", RL_GBA_SAVE_EEPROM, 0},
        {"SRAM_V113", RL_GBA_SAVE_SRAM, 0},
        {"SRAM_F_V102", RL_GBA_SAVE_SRAM, 0},
        {"FLASH_V126", RL_GBA_SAVE_FLASH64, RL_GBA_FLASH_PANASONIC},
        {"FLASH512_V131", RL_GBA_SAVE_FLASH64, RL_GBA_FLASH_PANASONIC},
        {"FLASH1M_V103", RL_GBA_SAVE_FLASH128, RL_GBA_FLASH_MACRONIX128},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t rom[256] = {0};
        memcpy(rom + 100 + i, cases[i].s, strlen(cases[i].s));
        rl_gba_save_info in = rl_gba_save_detect(rom, sizeof rom);
        CHECK_EQ(in.kind, cases[i].kind);
        CHECK_EQ(in.flash_id, cases[i].id);
    }
}

TEST(a_rom_without_a_library_string_has_no_backup)
{
    uint8_t rom[80] = "SRAM_V and FLASH_V without digits, EEPROM_Vx";
    CHECK_EQ(rl_gba_save_detect(rom, sizeof rom).kind, RL_GBA_SAVE_NONE);
    CHECK_EQ(rl_gba_save_detect(NULL, 0).kind, RL_GBA_SAVE_NONE);
    memcpy(rom + 60, "SRAM_V10", 8);
    CHECK_EQ(rl_gba_save_detect(rom, 64).kind, RL_GBA_SAVE_NONE);
    CHECK_EQ(rl_gba_save_detect(rom, 66).kind, RL_GBA_SAVE_NONE);
    CHECK_EQ(rl_gba_save_detect(rom, 67).kind, RL_GBA_SAVE_SRAM);
}

TEST(the_first_string_in_the_image_wins)
{
    uint8_t rom[128] = {0};
    memcpy(rom + 80, "FLASH1M_V103", 12);
    memcpy(rom + 20, "EEPROM_V124", 11);
    CHECK_EQ(rl_gba_save_detect(rom, sizeof rom).kind, RL_GBA_SAVE_EEPROM);
}

TEST(sram_keeps_bytes_mirrored_every_32_kib)
{
    mach m = cart_with(RL_GBA_SAVE_SRAM, 0, 0);
    CHECK(m.g != NULL);
    CHECK_EQ(rl_gba_save_len(m.g), 0x8000u);
    CHECK_EQ(br(&m, 1, SAVE + 5), 0xFFu);
    bw(&m, 2, SAVE + 5, 0x42);
    CHECK_EQ(br(&m, 3, SAVE + 0x8005u), 0x42u);
    CHECK_EQ(br(&m, 4, 0x0FFF8005u), 0x42u);
    CHECK_EQ(rl_gba_save_data(m.g)[5], 0x42u);
    CHECK(rl_gba_save_dirty(m.g));
    mach_free(&m);
}

TEST(a_write_of_the_same_byte_does_not_dirty_the_save)
{
    mach m = cart_with(RL_GBA_SAVE_SRAM, 0, 0);
    bw(&m, 1, SAVE + 9, 0xFF);
    CHECK(!rl_gba_save_dirty(m.g));
    bw(&m, 2, SAVE + 9, 0x11);
    CHECK(rl_gba_save_dirty(m.g));
    rl_gba_save_clean(m.g);
    CHECK(!rl_gba_save_dirty(m.g));
    mach_free(&m);
}

TEST(a_caller_s_sram_buffer_is_still_the_backup)
{
    uint8_t buf[0x8000];
    memset(buf, 0xFF, sizeof buf);
    mach m = mach_new_sram(buf, sizeof buf);
    CHECK_EQ(rl_gba_save_kind_of(m.g), RL_GBA_SAVE_SRAM);
    bw(&m, 1, SAVE + 3, 0x77);
    CHECK_EQ(buf[3], 0x77u);
    CHECK(rl_gba_save_data(m.g) == buf);
    mach_free(&m);
}

TEST(a_machine_without_a_backup_reads_ff_and_refuses_images)
{
    mach m = mach_new();
    CHECK_EQ(rl_gba_save_kind_of(m.g), RL_GBA_SAVE_NONE);
    CHECK_EQ(br(&m, 1, SAVE), 0xFFu);
    uint8_t img[0x8000] = {0};
    CHECK_EQ(rl_gba_save_load(m.g, img, sizeof img), -1);
    CHECK_EQ(rl_gba_save_read(m.g, "/nonexistent"), 1);
    mach_free(&m);
}

TEST(bad_cart_backup_fields_fail_the_creation)
{
    rl_gba_cart c = {.rom = NULL, .save = RL_GBA_SAVE_FLASH128, .flash_id = RL_GBA_FLASH_ATMEL};
    CHECK(rl_gba_create(&c) == NULL);
    c = (rl_gba_cart){.save = RL_GBA_SAVE_FLASH64, .flash_id = RL_GBA_FLASH_SANYO};
    CHECK(rl_gba_create(&c) == NULL);
    c = (rl_gba_cart){.save = RL_GBA_SAVE_EEPROM, .eeprom_len = 1024};
    CHECK(rl_gba_create(&c) == NULL);
    c = (rl_gba_cart){.save = (rl_gba_save_kind)9};
    CHECK(rl_gba_create(&c) == NULL);
}

static const uint16_t ids64[] = {RL_GBA_FLASH_PANASONIC, RL_GBA_FLASH_MACRONIX64, RL_GBA_FLASH_SST, RL_GBA_FLASH_ATMEL};

TEST(flash_chip_id_mode_reports_manufacturer_then_device)
{
    for (unsigned i = 0; i < 4; i++) {
        mach m = cart_with(RL_GBA_SAVE_FLASH64, ids64[i], 0);
        CHECK_EQ(br(&m, 1, SAVE), 0xFFu);
        uint64_t t = unlock(&m, 2, 0x90);
        CHECK_EQ(br(&m, t, SAVE), ids64[i] & 0xFFu);
        CHECK_EQ(br(&m, t + 1, SAVE + 1), ids64[i] >> 8);
        CHECK_EQ(br(&m, t + 2, SAVE + 2), 0xFFu);
        unlock(&m, t + 3, 0xF0);
        CHECK_EQ(br(&m, t + 10, SAVE), 0xFFu);
        mach_free(&m);
    }
}

TEST(flash_default_ids_and_the_128_kib_chips)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH64, 0, 0);
    unlock(&m, 1, 0x90);
    CHECK_EQ(br(&m, 10, SAVE), 0x32u);
    CHECK_EQ(br(&m, 11, SAVE + 1), 0x1Bu);
    mach_free(&m);
    m = cart_with(RL_GBA_SAVE_FLASH128, 0, 0);
    unlock(&m, 1, 0x90);
    CHECK_EQ(br(&m, 10, SAVE), 0xC2u);
    CHECK_EQ(br(&m, 11, SAVE + 1), 0x09u);
    mach_free(&m);
    m = cart_with(RL_GBA_SAVE_FLASH128, RL_GBA_FLASH_SANYO, 0);
    unlock(&m, 1, 0x90);
    CHECK_EQ(br(&m, 10, SAVE), 0x62u);
    CHECK_EQ(br(&m, 11, SAVE + 1), 0x13u);
    mach_free(&m);
}

TEST(a_single_f0_leaves_id_mode)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH64, 0, 0);
    uint64_t t = unlock(&m, 1, 0x90);
    bw(&m, t, SAVE, 0xF0);
    CHECK_EQ(br(&m, t + 1, SAVE), 0xFFu);
    mach_free(&m);
}

TEST(flash_programs_one_byte_and_reads_status_while_busy)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH64, 0, 0);
    uint64_t t = program(&m, 10, 0x1234, 0x5A);
    uint32_t a = br(&m, t, SAVE + 0x1234), b = br(&m, t + 1, SAVE + 0x1234);
    CHECK_EQ(a & 0x80, ~0x5Au & 0x80u);
    CHECK_EQ(b & 0x80, ~0x5Au & 0x80u);
    CHECK((a ^ b) & 0x40);
    CHECK_EQ(br(&m, t + AFTER_PROGRAM, SAVE + 0x1234), 0x5Au);
    CHECK_EQ(br(&m, t + AFTER_PROGRAM, SAVE + 0x1235), 0xFFu);
    mach_free(&m);
}

TEST(flash_program_can_only_clear_bits)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH64, 0, 0);
    uint64_t t = program(&m, 10, 0x40, 0xF0);
    t = program(&m, t + AFTER_PROGRAM, 0x40, 0x3C);
    CHECK_EQ(br(&m, t + AFTER_PROGRAM, SAVE + 0x40), 0x30u);
    mach_free(&m);
}

TEST(flash_ignores_commands_while_busy)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH64, 0, 0);
    uint64_t t = program(&m, 10, 0x10, 0x00);
    t = program(&m, t + 5, 0x11, 0x00);
    CHECK_EQ(br(&m, t + AFTER_PROGRAM, SAVE + 0x10), 0x00u);
    CHECK_EQ(br(&m, t + AFTER_PROGRAM, SAVE + 0x11), 0xFFu);
    mach_free(&m);
}

TEST(flash_sector_erase_clears_4_kib_and_reports_erase_status)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH64, 0, 0);
    uint64_t t = 10;
    uint32_t offs[] = {0x0FFF, 0x1000, 0x1FFF, 0x2000};
    for (unsigned i = 0; i < 4; i++)
        t = program(&m, t + AFTER_PROGRAM, offs[i], 0x00);
    t = sector_erase(&m, t + AFTER_PROGRAM, 0x1000);
    uint32_t a = br(&m, t, SAVE + 0x1000), b = br(&m, t + 1, SAVE + 0x1000);
    CHECK_EQ(a & 0x80, 0u);
    CHECK((a ^ b) & 0x40);
    t += AFTER_SECTOR;
    CHECK_EQ(br(&m, t, SAVE + 0x0FFF), 0x00u);
    CHECK_EQ(br(&m, t, SAVE + 0x1000), 0xFFu);
    CHECK_EQ(br(&m, t, SAVE + 0x1FFF), 0xFFu);
    CHECK_EQ(br(&m, t, SAVE + 0x2000), 0x00u);
    mach_free(&m);
}

TEST(flash_chip_erase_clears_everything)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH64, 0, 0);
    uint64_t t = program(&m, 10, 0x0, 0x00);
    t = program(&m, t + AFTER_PROGRAM, 0xFFFF, 0x00);
    t = chip_erase(&m, t + AFTER_PROGRAM);
    CHECK_EQ(br(&m, t + 10, SAVE) & 0x80, 0u);
    CHECK_EQ(br(&m, t + AFTER_CHIP, SAVE), 0xFFu);
    CHECK_EQ(br(&m, t + AFTER_CHIP, SAVE + 0xFFFF), 0xFFu);
    mach_free(&m);
}

TEST(flash_erase_needs_the_second_unlock_and_a_valid_address)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH64, 0, 0);
    uint64_t t = program(&m, 10, 0x1000, 0x00);
    t = unlock(&m, t + AFTER_PROGRAM, 0x80);
    bw(&m, t, SAVE + 0x5555, 0xAA);
    bw(&m, t + 1, SAVE + 0x2AAA, 0x55);
    bw(&m, t + 2, SAVE + 0x1008, 0x30);
    CHECK_EQ(br(&m, t + AFTER_SECTOR, SAVE + 0x1000), 0x00u);
    t = unlock(&m, t + AFTER_SECTOR, 0x10);
    CHECK_EQ(br(&m, t + AFTER_SECTOR, SAVE + 0x1000), 0x00u);
    mach_free(&m);
}

TEST(flash_sequences_at_the_wrong_addresses_do_nothing)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH64, 0, 0);
    bw(&m, 1, SAVE + 0x5554, 0xAA);
    bw(&m, 2, SAVE + 0x2AAA, 0x55);
    bw(&m, 3, SAVE + 0x5555, 0xA0);
    bw(&m, 4, SAVE + 0x10, 0x00);
    CHECK_EQ(br(&m, AFTER_PROGRAM, SAVE + 0x10), 0xFFu);
    bw(&m, 2000, SAVE + 0x5555, 0xAA);
    bw(&m, 2001, SAVE + 0x2AAB, 0x55);
    bw(&m, 2002, SAVE + 0x5555, 0x90);
    CHECK_EQ(br(&m, 2003, SAVE), 0xFFu);
    mach_free(&m);
}

TEST(flash_128_banks_the_upper_64_kib)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH128, 0, 0);
    CHECK_EQ(rl_gba_save_len(m.g), 0x20000u);
    uint64_t t = program(&m, 10, 0x100, 0x11);
    t = unlock(&m, t + AFTER_PROGRAM, 0xB0);
    bw(&m, t, SAVE, 1);
    t = program(&m, t + 1, 0x100, 0x22);
    CHECK_EQ(br(&m, t + AFTER_PROGRAM, SAVE + 0x100), 0x22u);
    CHECK_EQ(rl_gba_save_data(m.g)[0x100], 0x11u);
    CHECK_EQ(rl_gba_save_data(m.g)[0x10100], 0x22u);
    t = unlock(&m, t + AFTER_PROGRAM, 0xB0);
    bw(&m, t, SAVE, 0);
    CHECK_EQ(br(&m, t + 1, SAVE + 0x100), 0x11u);
    mach_free(&m);
}

TEST(flash_bank_select_only_takes_address_zero_and_only_on_128_kib)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH128, 0, 0);
    uint64_t t = program(&m, 10, 0x100, 0x11);
    t = unlock(&m, t + AFTER_PROGRAM, 0xB0);
    bw(&m, t, SAVE + 4, 1);
    CHECK_EQ(br(&m, t + 1, SAVE + 0x100), 0x11u);
    mach_free(&m);
    m = cart_with(RL_GBA_SAVE_FLASH64, 0, 0);
    t = program(&m, 10, 0x100, 0x11);
    t = unlock(&m, t + AFTER_PROGRAM, 0xB0);
    bw(&m, t, SAVE, 1);
    CHECK_EQ(br(&m, t + 1, SAVE + 0x100), 0x11u);
    mach_free(&m);
}

TEST(flash_128_chip_erase_clears_both_banks)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH128, 0, 0);
    uint64_t t = unlock(&m, 1, 0xB0);
    bw(&m, t, SAVE, 1);
    t = program(&m, t + 1, 0x7, 0x00);
    t = chip_erase(&m, t + AFTER_PROGRAM);
    CHECK_EQ(br(&m, t + AFTER_CHIP, SAVE + 7), 0xFFu);
    CHECK_EQ(rl_gba_save_data(m.g)[0x10007], 0xFFu);
    mach_free(&m);
}

TEST(atmel_writes_a_128_byte_page_that_erases_its_other_bytes)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH64, RL_GBA_FLASH_ATMEL, 0);
    uint64_t t = unlock(&m, 10, 0xA0);
    for (unsigned i = 0; i < 128; i++)
        bw(&m, t + i, SAVE + 0x2000 + i, i);
    t += 128;
    uint32_t a = br(&m, t, SAVE + 0x2000), b = br(&m, t + 1, SAVE + 0x2000);
    CHECK_EQ(a & 0x80, 0x80u);
    CHECK((a ^ b) & 0x40);
    t += 200000;
    for (unsigned i = 0; i < 128; i++)
        CHECK_EQ(br(&m, t, SAVE + 0x2000 + i), i);
    t = unlock(&m, t, 0xA0);
    bw(&m, t, SAVE + 0x2000, 0xAB);
    bw(&m, t + 1, SAVE + 0x2001, 0xCD);
    t += 200000;
    CHECK_EQ(br(&m, t, SAVE + 0x2000), 0xABu);
    CHECK_EQ(br(&m, t, SAVE + 0x2001), 0xCDu);
    CHECK_EQ(br(&m, t, SAVE + 0x2002), 0xFFu);
    CHECK_EQ(br(&m, t, SAVE + 0x207F), 0xFFu);
    CHECK_EQ(br(&m, t, SAVE + 0x2080), 0xFFu);
    mach_free(&m);
}

TEST(atmel_has_no_sector_erase_but_erases_the_whole_chip)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH64, RL_GBA_FLASH_ATMEL, 0);
    uint64_t t = unlock(&m, 10, 0xA0);
    bw(&m, t, SAVE + 0x3000, 0x00);
    t += 200000;
    t = sector_erase(&m, t, 0x3000);
    CHECK_EQ(br(&m, t + AFTER_SECTOR, SAVE + 0x3000), 0x00u);
    t = chip_erase(&m, t + AFTER_SECTOR);
    CHECK_EQ(br(&m, t + AFTER_CHIP, SAVE + 0x3000), 0xFFu);
    mach_free(&m);
}

TEST(atmel_page_to_another_page_closes_the_first)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH64, RL_GBA_FLASH_ATMEL, 0);
    uint64_t t = unlock(&m, 10, 0xA0);
    bw(&m, t, SAVE + 0x100, 0x12);
    bw(&m, t + 1, SAVE + 0x200, 0x34);
    CHECK_EQ(br(&m, t + 200000, SAVE + 0x100), 0x12u);
    CHECK_EQ(br(&m, t + 200000, SAVE + 0x200), 0xFFu);
    mach_free(&m);
}

static uint64_t bit(mach *m, uint64_t t, unsigned v)
{
    mach_to(m, t);
    rl_gba_write(m->g, EEP, 2, v);
    return t + 1;
}

static uint64_t bits(mach *m, uint64_t t, uint64_t v, unsigned n)
{
    for (unsigned i = n; i-- > 0;)
        t = bit(m, t, (v >> i) & 1u);
    return t;
}

static uint64_t eep_write(mach *m, uint64_t t, unsigned addr_bits, unsigned block, uint64_t data)
{
    t = bits(m, t, 2, 2);
    t = bits(m, t, block, addr_bits);
    t = bits(m, t, data, 64);
    return bit(m, t, 0);
}

static uint64_t eep_request(mach *m, uint64_t t, unsigned addr_bits, unsigned block)
{
    t = bits(m, t, 3, 2);
    t = bits(m, t, block, addr_bits);
    return bit(m, t, 0);
}

static uint32_t eep_bit(mach *m, uint64_t t)
{
    mach_to(m, t);
    return rl_gba_read(m->g, EEP, 2);
}

static void eep_read(mach *m, uint64_t t, uint64_t *out)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 4; i++)
        CHECK_EQ(eep_bit(m, t++), 0u);
    for (unsigned i = 0; i < 64; i++)
        v = v << 1 | eep_bit(m, t++);
    *out = v;
}

TEST(eeprom_8k_writes_and_reads_a_block)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 8192);
    uint64_t t = eep_write(&m, 10, 14, 0x155, 0x0123456789ABCDEFull);
    t = eep_request(&m, t + 200000, 14, 0x155);
    uint64_t v = 0;
    eep_read(&m, t, &v);
    CHECK(v == 0x0123456789ABCDEFull);
    const uint8_t *d = rl_gba_save_data(m.g);
    CHECK_EQ(d[0x155 * 8], 0x01u);
    CHECK_EQ(d[0x155 * 8 + 7], 0xEFu);
    CHECK_EQ(rl_gba_save_len(m.g), 8192u);
    mach_free(&m);
}

TEST(eeprom_512_writes_and_reads_a_block)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 512);
    uint64_t t = eep_write(&m, 10, 6, 0x3F, 0xFEDCBA9876543210ull);
    t = eep_request(&m, t + 200000, 6, 0x3F);
    uint64_t v = 0;
    eep_read(&m, t, &v);
    CHECK(v == 0xFEDCBA9876543210ull);
    CHECK_EQ(rl_gba_save_data(m.g)[0x3F * 8], 0xFEu);
    CHECK_EQ(rl_gba_save_len(m.g), 512u);
    mach_free(&m);
}

TEST(eeprom_reads_ready_until_a_write_and_busy_for_about_6_ms_after)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 8192);
    CHECK_EQ(eep_bit(&m, 5), 1u);
    uint64_t t = eep_write(&m, 10, 14, 1, 0);
    CHECK_EQ(eep_bit(&m, t + 10), 0u);
    CHECK_EQ(eep_bit(&m, t + 100000), 0u);
    CHECK_EQ(eep_bit(&m, t + 101500), 1u);
    mach_free(&m);
}

TEST(eeprom_ignores_a_request_while_busy)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 8192);
    uint64_t t = eep_write(&m, 10, 14, 1, 0x1111111111111111ull);
    t = eep_write(&m, t + 10, 14, 2, 0x2222222222222222ull);
    CHECK_EQ(rl_gba_save_data(m.g)[16], 0xFFu);
    t = eep_write(&m, t + 200000, 14, 2, 0x2222222222222222ull);
    CHECK_EQ(rl_gba_save_data(m.g)[16], 0x22u);
    CHECK_EQ(rl_gba_save_data(m.g)[8], 0x11u);
    mach_free(&m);
}

TEST(eeprom_does_not_change_until_the_stop_bit)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 8192);
    uint64_t t = bits(&m, 10, 2, 2);
    t = bits(&m, t, 3, 14);
    t = bits(&m, t, ~0ull, 64);
    CHECK_EQ(rl_gba_save_data(m.g)[24], 0xFFu);
    CHECK(!rl_gba_save_dirty(m.g));
    bit(&m, t, 0);
    CHECK(rl_gba_save_dirty(m.g));
    mach_free(&m);
}

TEST(an_invalid_command_is_dropped_and_the_next_request_works)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 8192);
    uint64_t t = bits(&m, 10, 0, 2);
    t = eep_write(&m, t, 14, 4, 0x5555555555555555ull);
    CHECK_EQ(rl_gba_save_data(m.g)[32], 0x55u);
    mach_free(&m);
}

TEST(eeprom_8k_ignores_the_top_address_bits)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 8192);
    uint64_t t = eep_write(&m, 10, 14, 0x3C05, 0x7777777777777777ull);
    CHECK_EQ(rl_gba_save_data(m.g)[5 * 8], 0x77u);
    (void)t;
    mach_free(&m);
}

static void dma3(mach *m, uint64_t t, uint32_t src, uint32_t dst, uint32_t count, uint16_t ctl)
{
    mach_to(m, t);
    rl_gba_write(m->g, DMA3, 4, src);
    rl_gba_write(m->g, DMA3 + 4, 4, dst);
    rl_gba_write(m->g, DMA3 + 8, 4, count | (uint32_t)ctl << 16);
    mach_to(m, t + 2);
    rl_gba_service(m->g);
    mach_to(m, mach_now(m) + 1);
}

static void put_stream(mach *m, uint32_t at, const unsigned *v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        rl_gba_write(m->g, at + 2 * i, 2, v[i]);
}

static unsigned stream_write(unsigned *out, unsigned addr_bits, unsigned block, uint64_t data)
{
    unsigned n = 0;
    out[n++] = 1;
    out[n++] = 0;
    for (unsigned i = addr_bits; i-- > 0;)
        out[n++] = (block >> i) & 1u;
    for (unsigned i = 64; i-- > 0;)
        out[n++] = (unsigned)(data >> i) & 1u;
    out[n++] = 0;
    return n;
}

TEST(a_first_dma_of_73_units_sizes_the_eeprom_at_512_bytes)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 0);
    CHECK_EQ(rl_gba_save_len(m.g), 0u);
    unsigned s[100];
    unsigned n = stream_write(s, 6, 9, 0x1122334455667788ull);
    CHECK_EQ(n, 73u);
    put_stream(&m, 0x02000000u, s, n);
    dma3(&m, 100, 0x02000000u, EEP, n, 0x8000);
    CHECK_EQ(rl_gba_save_len(m.g), 512u);
    CHECK_EQ(rl_gba_save_data(m.g)[72], 0x11u);
    CHECK_EQ(rl_gba_save_data(m.g)[79], 0x88u);
    mach_free(&m);
}

TEST(a_first_dma_of_81_units_sizes_the_eeprom_at_8_kib)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 0);
    unsigned s[100];
    unsigned n = stream_write(s, 14, 0x20, 0x1122334455667788ull);
    CHECK_EQ(n, 81u);
    put_stream(&m, 0x02000000u, s, n);
    dma3(&m, 100, 0x02000000u, EEP, n, 0x8000);
    CHECK_EQ(rl_gba_save_len(m.g), 8192u);
    CHECK_EQ(rl_gba_save_data(m.g)[0x20 * 8], 0x11u);
    mach_free(&m);
}

TEST(read_requests_of_9_and_17_units_size_the_eeprom_too)
{
    for (unsigned k = 0; k < 2; k++) {
        mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 0);
        unsigned s[32] = {1, 1};
        unsigned ab = k ? 14 : 6;
        for (unsigned i = 0; i < ab; i++)
            s[2 + i] = 0;
        s[2 + ab] = 0;
        put_stream(&m, 0x02000000u, s, 3 + ab);
        dma3(&m, 100, 0x02000000u, EEP, 3 + ab, 0x8000);
        CHECK_EQ(rl_gba_save_len(m.g), k ? 8192u : 512u);
        mach_free(&m);
    }
}

TEST(an_unknown_first_length_falls_back_to_8_kib)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 0);
    dma3(&m, 100, 0x02000000u, EEP, 5, 0x8000);
    CHECK_EQ(rl_gba_save_len(m.g), 8192u);
    dma3(&m, 300, 0x02000000u, EEP, 9, 0x8000);
    CHECK_EQ(rl_gba_save_len(m.g), 8192u);
    mach_free(&m);
}

TEST(only_the_first_transfer_sizes_the_eeprom)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 0);
    dma3(&m, 100, 0x02000000u, EEP, 9, 0x8000);
    CHECK_EQ(rl_gba_save_len(m.g), 512u);
    dma3(&m, 300, 0x02000000u, EEP, 17, 0x8000);
    CHECK_EQ(rl_gba_save_len(m.g), 512u);
    dma3(&m, 500, 0x02000000u, EEP, 81, 0x8000);
    CHECK_EQ(rl_gba_save_len(m.g), 512u);
    mach_free(&m);
}

TEST(a_fixed_size_ignores_the_transfer_length)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 8192);
    dma3(&m, 100, 0x02000000u, EEP, 9, 0x8000);
    CHECK_EQ(rl_gba_save_len(m.g), 8192u);
    mach_free(&m);
}

TEST(a_dma_read_returns_the_four_dummy_bits_then_the_block)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 8192);
    unsigned s[100];
    unsigned n = stream_write(s, 14, 7, 0xA5A5A5A5C3C3C3C3ull);
    put_stream(&m, 0x02000000u, s, n);
    dma3(&m, 100, 0x02000000u, EEP, n, 0x8000);
    unsigned r[17] = {1, 1};
    for (unsigned i = 0; i < 14; i++)
        r[2 + i] = (7u >> (13 - i)) & 1u;
    r[16] = 0;
    put_stream(&m, 0x02001000u, r, 17);
    dma3(&m, 300000, 0x02001000u, EEP, 17, 0x8000);
    dma3(&m, 301000, EEP, 0x02002000u, 68, 0x8100);
    uint64_t v = 0;
    for (unsigned i = 0; i < 4; i++)
        CHECK_EQ(rl_gba_read(m.g, 0x02002000u + 2 * i, 2) & 1u, 0u);
    for (unsigned i = 0; i < 64; i++)
        v = v << 1 | (rl_gba_read(m.g, 0x02002008u + 2 * i, 2) & 1u);
    CHECK(v == 0xA5A5A5A5C3C3C3C3ull);
    mach_free(&m);
}

TEST(an_eeprom_with_a_big_rom_answers_only_at_the_top_of_the_window)
{
    mach m = mach_new_cart(mach_rom(0x1100000), 0x1100000, (rl_gba_cart){.save = RL_GBA_SAVE_EEPROM, .eeprom_len = 8192});
    CHECK(m.g != NULL);
    mach_to(&m, 5);
    CHECK_EQ(rl_gba_read(m.g, 0x0D000000u, 2), rl_gba_peek(m.g, 0x0D000000u, 2));
    uint64_t t = 10;
    t = bits(&m, t, 2, 2);
    CHECK_EQ(rl_gba_save_dirty(m.g), false);
    mach_free(&m);
    mach m2 = mach_new_cart(mach_rom(0x1100000), 0x1100000, (rl_gba_cart){.save = RL_GBA_SAVE_EEPROM, .eeprom_len = 8192});
    mach_to(&m2, 5);
    rl_gba_write(m2.g, 0x0DFFFF00u, 2, 1);
    rl_gba_write(m2.g, 0x0DFFFF00u, 2, 0);
    uint64_t block = 0x99;
    for (unsigned i = 0; i < 14; i++)
        rl_gba_write(m2.g, 0x0DFFFF00u, 2, 0);
    for (unsigned i = 64; i-- > 0;)
        rl_gba_write(m2.g, 0x0DFFFF00u, 2, (block >> i) & 1u);
    rl_gba_write(m2.g, 0x0DFFFF00u, 2, 0);
    CHECK(rl_gba_save_dirty(m2.g));
    mach_free(&m2);
}

TEST(a_save_image_loads_by_size_and_the_eeprom_takes_its_size_from_it)
{
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 0);
    uint8_t img[8192];
    memset(img, 0x5A, sizeof img);
    CHECK_EQ(rl_gba_save_load(&*m.g, img, 1000), -1);
    CHECK_EQ(rl_gba_save_load(m.g, img, 512), 0);
    CHECK_EQ(rl_gba_save_len(m.g), 512u);
    CHECK_EQ(rl_gba_save_data(m.g)[511], 0x5Au);
    CHECK(!rl_gba_save_dirty(m.g));
    mach_free(&m);
    m = cart_with(RL_GBA_SAVE_EEPROM, 0, 8192);
    CHECK_EQ(rl_gba_save_load(m.g, img, 512), -1);
    CHECK_EQ(rl_gba_save_load(m.g, img, 8192), 0);
    mach_free(&m);
}

TEST(a_wrong_size_image_changes_nothing)
{
    mach m = cart_with(RL_GBA_SAVE_FLASH128, 0, 0);
    uint8_t img[0x10000];
    memset(img, 0, sizeof img);
    CHECK_EQ(rl_gba_save_load(m.g, img, sizeof img), -1);
    CHECK_EQ(rl_gba_save_data(m.g)[0], 0xFFu);
    mach_free(&m);
    m = cart_with(RL_GBA_SAVE_SRAM, 0, 0);
    CHECK_EQ(rl_gba_save_load(m.g, img, sizeof img), -1);
    CHECK_EQ(rl_gba_save_load(m.g, img, 0x8000), 0);
    CHECK_EQ(br(&m, 1, SAVE + 100), 0u);
    mach_free(&m);
}

static void scratch(char *out, size_t n, const char *name)
{
    const char *d = getenv("TMPDIR");
    snprintf(out, n, "%s/relift-gba-save-%ld-%s", d && *d ? d : "/tmp", (long)getpid(), name);
}

TEST(a_save_file_round_trips_through_an_atomic_write)
{
    char path[512], tmp[520];
    scratch(path, sizeof path, "a.sav");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    unlink(path);
    mach m = cart_with(RL_GBA_SAVE_FLASH64, 0, 0);
    uint64_t t = program(&m, 10, 0x77, 0x12);
    CHECK(rl_gba_save_dirty(m.g));
    CHECK_EQ(rl_gba_save_write(m.g, path), 0);
    CHECK(!rl_gba_save_dirty(m.g));
    CHECK(access(tmp, F_OK) != 0);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    uint8_t buf[0x10001];
    CHECK_EQ(fread(buf, 1, sizeof buf, f), 0x10000u);
    fclose(f);
    CHECK_EQ(buf[0x77], 0x12u);
    CHECK_EQ(buf[0x78], 0xFFu);
    mach b = cart_with(RL_GBA_SAVE_FLASH64, 0, 0);
    CHECK_EQ(rl_gba_save_read(b.g, path), 0);
    CHECK_EQ(br(&b, t + AFTER_PROGRAM, SAVE + 0x77), 0x12u);
    mach_free(&b);
    mach_free(&m);
    unlink(path);
}

TEST(a_wrong_size_file_is_refused_and_left_alone)
{
    char path[512];
    scratch(path, sizeof path, "b.sav");
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    uint8_t junk[100] = {1};
    fwrite(junk, 1, sizeof junk, f);
    fclose(f);
    mach m = cart_with(RL_GBA_SAVE_SRAM, 0, 0);
    CHECK_EQ(rl_gba_save_read(m.g, path), -1);
    CHECK_EQ(rl_gba_save_data(m.g)[0], 0xFFu);
    f = fopen(path, "rb");
    CHECK(f != NULL);
    CHECK_EQ(fread(junk, 1, sizeof junk, f), 100u);
    fclose(f);
    mach_free(&m);
    unlink(path);
}

TEST(a_failed_write_leaves_the_save_dirty)
{
    char path[512], bad[600];
    scratch(path, sizeof path, "c.sav");
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    fclose(f);
    snprintf(bad, sizeof bad, "%s/x.sav", path);
    mach m = cart_with(RL_GBA_SAVE_SRAM, 0, 0);
    bw(&m, 1, SAVE, 0);
    CHECK_EQ(rl_gba_save_write(m.g, bad), -1);
    CHECK(rl_gba_save_dirty(m.g));
    mach_free(&m);
    unlink(path);
}

TEST(a_write_creates_the_save_folder)
{
    char dir[512], path[600];
    scratch(dir, sizeof dir, "dir");
    snprintf(path, sizeof path, "%s/sub/g.sav", dir);
    mach m = cart_with(RL_GBA_SAVE_SRAM, 0, 0);
    bw(&m, 1, SAVE, 0);
    CHECK_EQ(rl_gba_save_write(m.g, path), 0);
    CHECK_EQ(access(path, F_OK), 0);
    unlink(path);
    char sub[600];
    snprintf(sub, sizeof sub, "%s/sub", dir);
    rmdir(sub);
    rmdir(dir);
    mach_free(&m);
}

TEST(an_eeprom_never_used_writes_nothing_and_a_used_one_writes_its_size)
{
    char path[512];
    scratch(path, sizeof path, "d.sav");
    unlink(path);
    mach m = cart_with(RL_GBA_SAVE_EEPROM, 0, 0);
    CHECK_EQ(rl_gba_save_write(m.g, path), 1);
    CHECK(access(path, F_OK) != 0);
    eep_write(&m, 10, 6, 1, 1);
    CHECK_EQ(rl_gba_save_len(m.g), 8192u);
    mach_free(&m);
    m = cart_with(RL_GBA_SAVE_EEPROM, 0, 512);
    eep_write(&m, 10, 6, 1, 1);
    CHECK_EQ(rl_gba_save_write(m.g, path), 0);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    fseek(f, 0, SEEK_END);
    CHECK_EQ(ftell(f), 512);
    fclose(f);
    unlink(path);
    mach_free(&m);
}

TEST(poll_writes_after_a_quiet_period_only)
{
    char path[512];
    scratch(path, sizeof path, "e.sav");
    unlink(path);
    mach m = cart_with(RL_GBA_SAVE_SRAM, 0, 0);
    CHECK_EQ(rl_gba_save_poll(m.g, path, 1000), 0);
    bw(&m, 100, SAVE, 1);
    CHECK_EQ(rl_gba_save_poll(m.g, path, 1000), 0);
    mach_to(&m, 900);
    bw(&m, 900, SAVE + 1, 2);
    mach_to(&m, 1500);
    CHECK_EQ(rl_gba_save_poll(m.g, path, 1000), 0);
    mach_to(&m, 2000);
    CHECK_EQ(rl_gba_save_poll(m.g, path, 1000), 1);
    CHECK_EQ(rl_gba_save_poll(m.g, path, 1000), 0);
    CHECK_EQ(access(path, F_OK), 0);
    unlink(path);
    mach_free(&m);
}

TEST(the_save_path_follows_save_dir_the_app_folder_or_the_rom)
{
    char out[600];
    rl_gba_declare_settings();
    CHECK(rl_gba_save_path(out, sizeof out, "/roms/Game.v1.gba"));
    CHECK(!strcmp(out, "/roms/Game.v1.sav"));
    CHECK(rl_gba_save_path(out, sizeof out, "Game.gba"));
    CHECK(!strcmp(out, "./Game.sav"));
    CHECK(rl_gba_save_path(out, sizeof out, "/Game"));
    CHECK(!strcmp(out, "/Game.sav"));
    CHECK_EQ(rl_settings_set("save.dir", "/saves/gba/"), RL_SETTING_OK);
    CHECK(rl_gba_save_path(out, sizeof out, "/roms/Game.v1.gba"));
    CHECK(!strcmp(out, "/saves/gba/Game.v1.sav"));
    CHECK(!rl_gba_save_path(out, 10, "/roms/Game.gba"));
}

TEST(the_save_path_falls_back_to_the_user_folder_of_the_app)
{
    char out[600], want[600];
    rl_gba_declare_settings();
    rl_settings_set_app("relift-test");
    CHECK(rl_settings_user_dir(want, sizeof want - 20, "relift-test"));
    strcat(want, "/save/Game.sav");
    CHECK(rl_gba_save_path(out, sizeof out, "/roms/Game.gba"));
    CHECK(!strcmp(out, want));
}

TEST(untimed_flash_finishes_programs_and_erases_at_once)
{
    mach m = instant_cart(RL_GBA_SAVE_FLASH64, 0);
    uint64_t t = program(&m, 10, 0x40, 0x12);
    CHECK_EQ(br(&m, t, SAVE + 0x40), 0x12u);
    t = sector_erase(&m, t + 1, 0x0);
    CHECK_EQ(br(&m, t, SAVE + 0x40), 0xFFu);
    t = program(&m, t + 1, 0x8000, 0x34);
    t = chip_erase(&m, t);
    CHECK_EQ(br(&m, t, SAVE + 0x8000), 0xFFu);
    mach_free(&m);
}

TEST(untimed_atmel_writes_a_page_at_once_and_closes_it_after_the_window)
{
    mach m = instant_cart(RL_GBA_SAVE_FLASH64, RL_GBA_FLASH_ATMEL);
    uint64_t t = unlock(&m, 10, 0xA0);
    bw(&m, t, SAVE + 0x400, 0x12);
    bw(&m, t + 1, SAVE + 0x401, 0x34);
    CHECK_EQ(br(&m, t + 2, SAVE + 0x400), 0x12u);
    CHECK_EQ(br(&m, t + 3, SAVE + 0x401), 0x34u);
    CHECK_EQ(br(&m, t + 4, SAVE + 0x402), 0xFFu);
    t = unlock(&m, t + 5000, 0x90);
    CHECK_EQ(br(&m, t, SAVE), 0x1Fu);
    CHECK_EQ(br(&m, t + 1, SAVE + 1), 0x3Du);
    mach_free(&m);
}

TEST(backup_writes_keep_the_exact_address_and_take_its_byte_lane)
{
    mach m = cart_with(RL_GBA_SAVE_SRAM, 0, 0);
    mach_to(&m, 1);
    rl_gba_write(m.g, SAVE + 0xA0, 2, 0xAABB);
    rl_gba_write(m.g, SAVE + 0xA1, 2, 0xAABB);
    rl_gba_write(m.g, SAVE + 0xE0, 4, 0xAABBCCDD);
    rl_gba_write(m.g, SAVE + 0xE1, 4, 0xAABBCCDD);
    rl_gba_write(m.g, SAVE + 0xE2, 4, 0xAABBCCDD);
    rl_gba_write(m.g, SAVE + 0xE3, 4, 0xAABBCCDD);
    const uint8_t *d = rl_gba_save_data(m.g);
    CHECK_EQ(d[0xA0], 0xBBu);
    CHECK_EQ(d[0xA1], 0xAAu);
    CHECK_EQ(d[0xE0], 0xDDu);
    CHECK_EQ(d[0xE1], 0xCCu);
    CHECK_EQ(d[0xE2], 0xBBu);
    CHECK_EQ(d[0xE3], 0xAAu);
    CHECK_EQ(rl_gba_read(m.g, SAVE + 0xA1, 1), 0xAAu);
    CHECK_EQ(rl_gba_read(m.g, SAVE + 0xA1, 2), 0xAAAAu);
    mach_free(&m);
}
