/**
 * @file test_backup.c
 * @brief The backup chip on the AUXSPI bus: the command sets of the EEPROM,
 * FRAM and FLASH families (GBATEK "DS Cartridge Backup"), the rule that picks
 * the chip, the status register, page writes, erases and the save image. The
 * oracle's test cartridge has no backup chip, so only its AUXSPI byte time
 * and the 0xFF of an absent device come from it.
 */
#include "ndst.h"

#include "nds_int.h"

#include <stdlib.h>
#include <string.h>

#define AUXCNT 0x040001A0u
#define AUXDATA 0x040001A2u

/** @brief Sends bytes as one command (chip select held between them, released after the last); returns the last byte received. */
static uint8_t cmd(nds_backup *b, const uint8_t *p, size_t n, uint8_t *rx)
{
    uint8_t last = 0;
    for (size_t i = 0; i < n; i++) {
        last = nds_bk_byte(b, p[i], i + 1 < n, 0);
        if (rx)
            rx[i] = last;
    }
    return last;
}

static uint8_t rdsr(nds_backup *b)
{
    static const uint8_t c[2] = {0x05, 0};
    return cmd(b, c, 2, NULL);
}

static void wren(nds_backup *b)
{
    static const uint8_t c[1] = {0x06};
    cmd(b, c, 1, NULL);
}

TEST(eeprom_64k_reads_writes_and_wraps_pages_with_two_address_bytes)
{
    nds_backup b = {0};
    CHECK(nds_bk_set(&b, NDS_BK_EEPROM, 65536));
    CHECK_EQ(rdsr(&b), 0);
    uint8_t wr[8] = {0x02, 0x12, 0x7E, 1, 2, 3, 4, 5};
    cmd(&b, wr, sizeof wr, NULL);
    CHECK_EQ(b.data[0x127E], 0xFF);
    wren(&b);
    CHECK_EQ(rdsr(&b), 2);
    cmd(&b, wr, sizeof wr, NULL);
    CHECK_EQ(rdsr(&b), 0);
    CHECK_EQ(b.data[0x127E], 1);
    CHECK_EQ(b.data[0x127F], 2);
    CHECK_EQ(b.data[0x1200], 3);
    CHECK_EQ(b.data[0x1201], 4);
    CHECK_EQ(b.data[0x1202], 5);
    uint8_t rd[6] = {0x03, 0x12, 0x7E, 0, 0, 0}, rx[6];
    cmd(&b, rd, sizeof rd, rx);
    CHECK_EQ(rx[3], 1);
    CHECK_EQ(rx[4], 2);
    CHECK_EQ(rx[5], 0xFF);
    CHECK(b.dirty);
    nds_bk_free(&b);
}

TEST(the_half_kilobyte_eeprom_carries_address_bit_8_in_the_opcode_and_reads_f0)
{
    nds_backup b = {0};
    CHECK(nds_bk_set(&b, NDS_BK_EEPROM, 512));
    CHECK_EQ(rdsr(&b), 0xF0);
    wren(&b);
    CHECK_EQ(rdsr(&b), 0xF2);
    uint8_t wr[4] = {0x0A, 0x20, 0xAB, 0xCD};
    cmd(&b, wr, sizeof wr, NULL);
    CHECK_EQ(b.data[0x120], 0xAB);
    CHECK_EQ(b.data[0x121], 0xCD);
    wren(&b);
    uint8_t lo[3] = {0x02, 0x20, 0x77};
    cmd(&b, lo, sizeof lo, NULL);
    CHECK_EQ(b.data[0x20], 0x77);
    uint8_t rd[4] = {0x0B, 0x20, 0, 0}, rx[4];
    cmd(&b, rd, sizeof rd, rx);
    CHECK_EQ(rx[2], 0xAB);
    CHECK_EQ(rx[3], 0xCD);
    uint8_t rl[3] = {0x03, 0x20, 0};
    cmd(&b, rl, sizeof rl, rx);
    CHECK_EQ(rx[2], 0x77);
    nds_bk_free(&b);
}

TEST(the_128k_eeprom_uses_three_address_bytes)
{
    nds_backup b = {0};
    CHECK(nds_bk_set(&b, NDS_BK_EEPROM, 131072));
    wren(&b);
    uint8_t wr[7] = {0x02, 0x01, 0xFF, 0xFE, 9, 8, 7};
    cmd(&b, wr, sizeof wr, NULL);
    CHECK_EQ(b.data[0x1FFFE], 9);
    CHECK_EQ(b.data[0x1FFFF], 8);
    CHECK_EQ(b.data[0x1FF00], 7);
    nds_bk_free(&b);
}

TEST(flash_programs_pages_with_and_erases_pages_and_sectors_and_answers_the_jedec_id)
{
    nds_backup b = {0};
    CHECK(nds_bk_set(&b, NDS_BK_FLASH, 524288));
    uint8_t id[4] = {0x9F, 0, 0, 0}, rx[4];
    cmd(&b, id, sizeof id, rx);
    CHECK_EQ(rx[1], 0x20);
    CHECK_EQ(rx[2], 0x40);
    CHECK_EQ(rx[3], 0x13);
    wren(&b);
    uint8_t pw[7] = {0x0A, 0x01, 0x23, 0x00, 0xF0, 0x0F, 0x55};
    cmd(&b, pw, sizeof pw, NULL);
    CHECK_EQ(b.data[0x12300], 0xF0);
    wren(&b);
    uint8_t pp[6] = {0x02, 0x01, 0x23, 0x00, 0x3C, 0xFF};
    cmd(&b, pp, sizeof pp, NULL);
    CHECK_EQ(b.data[0x12300], 0x30);
    CHECK_EQ(b.data[0x12301], 0x0F);
    wren(&b);
    uint8_t pe[4] = {0xDB, 0x01, 0x23, 0x80};
    cmd(&b, pe, sizeof pe, NULL);
    CHECK_EQ(b.data[0x12300], 0xFF);
    CHECK_EQ(b.data[0x12301], 0xFF);
    wren(&b);
    uint8_t pw2[5] = {0x0A, 0x05, 0x00, 0x10, 0x11};
    cmd(&b, pw2, sizeof pw2, NULL);
    wren(&b);
    uint8_t se[4] = {0xD8, 0x05, 0xAA, 0xBB};
    cmd(&b, se, sizeof se, NULL);
    CHECK_EQ(b.data[0x50010], 0xFF);
    uint8_t rd[6] = {0x03, 0x00, 0x00, 0x00, 0, 0};
    b.data[0] = 0x5A;
    uint8_t rr[6];
    cmd(&b, rd, sizeof rd, rr);
    CHECK_EQ(rr[4], 0x5A);
    nds_bk_free(&b);
}

TEST(an_unknown_first_byte_picks_no_chip_and_reads_ff)
{
    nds_backup b = {0};
    uint8_t z[2] = {0x00, 0x00};
    cmd(&b, z, 2, NULL);
    CHECK_EQ(nds_bk_byte(&b, 0x00, false, 0), 0xFF);
    CHECK(b.data == NULL);
    nds_bk_free(&b);
}

TEST(the_chip_is_chosen_by_the_first_command_only_one_family_sends)
{
    nds_backup b = {0};
    uint8_t id[2] = {0x9F, 0};
    cmd(&b, id, 2, NULL);
    CHECK(b.kind == NDS_BK_FLASH);
    nds_bk_free(&b);
    uint8_t hi[2] = {0x0A, 0};
    cmd(&b, hi, 2, NULL);
    CHECK(b.kind == NDS_BK_EEPROM && b.len == 512);
    nds_bk_free(&b);
    uint8_t rd[3] = {0x03, 0, 0};
    cmd(&b, rd, 3, NULL);
    CHECK(b.kind == NDS_BK_EEPROM && b.len == 131072);
    nds_bk_free(&b);
}

TEST(a_loaded_save_names_its_chip_by_length_and_other_lengths_are_refused)
{
    ndst *t = ndst_new();
    static const struct {
        size_t len;
        nds_bk_kind kind;
    } ok[] = {{512, NDS_BK_EEPROM},     {8192, NDS_BK_EEPROM},    {32768, NDS_BK_FRAM},     {65536, NDS_BK_EEPROM},
              {131072, NDS_BK_EEPROM}, {262144, NDS_BK_FLASH},   {524288, NDS_BK_FLASH},   {1048576, NDS_BK_FLASH}};
    static uint8_t img[1048576];
    memset(img, 0x5A, sizeof img);
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        nds_bk_free(&t->m->cart.bk);
        CHECK_EQ(rl_nds_save_load(t->m, img, ok[i].len), 0);
        CHECK(t->m->cart.bk.kind == ok[i].kind);
        CHECK_EQ(rl_nds_save_len(t->m), ok[i].len);
        CHECK_EQ(rl_nds_save_data(t->m)[ok[i].len - 1], 0x5A);
        CHECK(!rl_nds_save_dirty(t->m));
    }
    nds_bk_free(&t->m->cart.bk);
    CHECK(rl_nds_save_load(t->m, img, 1000) != 0);
    CHECK(rl_nds_save_load(t->m, img, 2 * 1048576) != 0);
    CHECK_EQ(rl_nds_save_set(t->m, "eeprom8k"), 0);
    CHECK(rl_nds_save_load(t->m, img, 512) != 0);
    CHECK_EQ(rl_nds_save_load(t->m, img, 8192), 0);
    CHECK(rl_nds_save_set(t->m, "nonsense") != 0);
}

TEST(the_auxspi_bus_takes_the_oracles_byte_times_and_an_absent_chip_reads_ff)
{
    ndst *t = ndst_new();
    ndst_to(t, RL_NDS_ARM7, 1ull << 40);
    static const uint32_t bus[4] = {67, 134, 268, 536};
    for (unsigned baud = 0; baud < 4; baud++) {
        w9(t, AUXCNT, 2, 0xA040 | baud);
        uint64_t t0 = rl_sched_now(t->k[RL_NDS_ARM9]);
        w9(t, AUXDATA, 2, 0x00);
        CHECK(r9(t, AUXCNT, 2) & 0x80);
        ndst_to(t, RL_NDS_ARM9, t0 + 2 * bus[baud] - 2);
        CHECK(r9(t, AUXCNT, 2) & 0x80);
        ndst_to(t, RL_NDS_ARM9, t0 + 2 * bus[baud]);
        CHECK(!(r9(t, AUXCNT, 2) & 0x80));
        CHECK_EQ(r9(t, AUXDATA, 2), 0xFF);
        ndst_to(t, RL_NDS_ARM9, rl_sched_now(t->k[RL_NDS_ARM9]) + 10);
    }
}

TEST(a_status_read_and_a_write_through_the_registers_reach_the_chip)
{
    ndst *t = ndst_new();
    ndst_to(t, RL_NDS_ARM7, 1ull << 40);
    CHECK_EQ(rl_nds_save_set(t->m, "eeprom64k"), 0);
    static const uint8_t seq[] = {0x06};
    uint64_t now = 0;
    for (unsigned i = 0; i < sizeof seq; i++) {
        w9(t, AUXCNT, 2, 0xA000);
        w9(t, AUXDATA, 2, seq[i]);
        now = rl_sched_now(t->k[RL_NDS_ARM9]) + 1000;
        ndst_to(t, RL_NDS_ARM9, now);
    }
    static const uint8_t wr[] = {0x02, 0x00, 0x10, 0xAA, 0xBB};
    for (unsigned i = 0; i < sizeof wr; i++) {
        w9(t, AUXCNT, 2, (uint16_t)(0xA000 | (i + 1 < sizeof wr ? 0x40 : 0)));
        w9(t, AUXDATA, 2, wr[i]);
        ndst_to(t, RL_NDS_ARM9, rl_sched_now(t->k[RL_NDS_ARM9]) + 1000);
    }
    CHECK_EQ(rl_nds_save_data(t->m)[0x10], 0xAA);
    CHECK_EQ(rl_nds_save_data(t->m)[0x11], 0xBB);
    CHECK(rl_nds_save_dirty(t->m));
}
