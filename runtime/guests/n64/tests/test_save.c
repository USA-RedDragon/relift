/**
 * @file test_save.c
 * @brief The Nintendo 64 cartridge's save memory: SRAM and Flash behind the
 * PI (CPU accesses and DMA), the Flash chip's command sequences and timing,
 * EEPROM on joybus channel 4, and the save files.
 */
#include "runner.h"

#include "n64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** @brief Whether the n guest bytes of RDRAM at a equal b. */
static bool ram_eq(rl_n64 *m, uint32_t a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t v;
        rl_n64_rdram_get(m, a + i, &v, 1);
        if (v != b[i])
            return false;
    }
    return true;
}

/** @brief Sets the n guest bytes of RDRAM at a to v. */
static void ram_set(rl_n64 *m, uint32_t a, uint8_t v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        rl_n64_rdram_put(m, a + i, &v, 1);
}

enum {
    SI_DRAM = 0x04800000u,
    SI_PIF_RD = 0x04800004u,
    SI_PIF_WR = 0x04800010u,
    SI_STATUS = 0x04800018u,
    PI_DRAM = 0x04600000u,
    PI_CART = 0x04600004u,
    PI_RD_LEN = 0x04600008u,
    PI_WR_LEN = 0x0460000Cu,
    PI_STATUS = 0x04600010u,
    PI_BSD2_PGS = 0x0460002Cu,
    PIF_RAM = 0x1FC007C0u,
    SRAM = 0x08000000u,
    FLASH_CMD = 0x08010000u,
    MS = 93750,
};

typedef struct mach {
    rl_sched *s;
    rl_sched_cpu *k;
    rl_n64 *m;
    uint8_t rom[0x2000];
} mach;

static void mach_new(mach *t)
{
    memset(t->rom, 0x55, sizeof t->rom);
    t->s = rl_sched_create(NULL);
    t->k = rl_sched_add_cpu(t->s, 1);
    t->m = rl_n64_create(t->rom, sizeof t->rom);
    rl_n64_attach(t->m, t->s, t->k);
    rl_n64_write(t->m, PI_BSD2_PGS, 4, 7);
}

static void mach_free(mach *t)
{
    rl_n64_destroy(t->m);
    rl_sched_destroy(t->s);
}

static void advance(mach *t, uint64_t n)
{
    rl_sched_stall(t->k, n);
    rl_sched_run(t->k);
}

/** @brief A PI DMA of len bytes between RDRAM at dram and the cartridge bus at cart, then the time it takes. */
static void pi_dma(mach *t, bool to_cart, uint32_t dram, uint32_t cart, uint32_t len)
{
    rl_n64_write(t->m, PI_DRAM, 4, dram);
    rl_n64_write(t->m, PI_CART, 4, cart);
    rl_n64_write(t->m, to_cart ? PI_RD_LEN : PI_WR_LEN, 4, len - 1u);
    advance(t, 2 * MS);
    rl_n64_write(t->m, PI_STATUS, 4, 2);
}

static void flash_cmd(mach *t, uint32_t word)
{
    rl_n64_write(t->m, FLASH_CMD, 4, word);
    advance(t, 1000);
}

/** @brief Runs one joybus block of channel 4 (channels 0 to 3 skipped) and returns the reply bytes of the block. */
static void eeprom_block(mach *t, const uint8_t *cmd, unsigned n, uint8_t *reply)
{
    uint8_t b[64];
    memset(b, 0xFF, sizeof b);
    b[0] = b[1] = b[2] = b[3] = 0;
    memcpy(b + 4, cmd, n);
    b[4 + n] = 0xFE;
    b[63] = 1;
    rl_n64_rdram_put(t->m, 0x1000, b, 64);
    rl_n64_write(t->m, SI_DRAM, 4, 0x1000);
    rl_n64_write(t->m, SI_PIF_WR, 4, PIF_RAM);
    advance(t, 7000);
    rl_n64_write(t->m, SI_STATUS, 4, 0);
    rl_n64_write(t->m, SI_DRAM, 4, 0x2000);
    rl_n64_write(t->m, SI_PIF_RD, 4, PIF_RAM);
    advance(t, 100000);
    rl_n64_write(t->m, SI_STATUS, 4, 0);
    rl_n64_rdram_get(t->m, 0x2000 + 4, reply, n);
}

TEST(n64_sram_answers_at_domain_2_and_mirrors_every_32_kib)
{
    mach t;
    mach_new(&t);
    CHECK_EQ(rl_n64_read(t.m, SRAM, 4), 0u);
    rl_n64_write(t.m, SRAM + 0x10, 4, 0x01020304u);
    CHECK_EQ(rl_n64_read(t.m, PI_STATUS, 4), 2u);
    rl_n64_write(t.m, SRAM + 0x14, 4, 0xDEADBEEFu);
    advance(&t, 200);
    CHECK_EQ(rl_n64_read(t.m, PI_STATUS, 4), 0u);
    CHECK_EQ(rl_n64_read(t.m, SRAM + 0x14, 4), 0u);
    rl_n64_write(t.m, SRAM + 0x14, 4, 0xDEADBEEFu);
    advance(&t, 200);
    CHECK_EQ(rl_n64_read(t.m, SRAM + 0x10, 4), 0x01020304u);
    CHECK_EQ(rl_n64_read(t.m, SRAM + 0x14, 4), 0xDEADBEEFu);
    CHECK_EQ(rl_n64_read(t.m, SRAM + 0x8010, 4), 0x01020304u);
    CHECK_EQ(rl_n64_read(t.m, SRAM + 0x16, 2), 0xBEEFu);
    CHECK(rl_n64_save_dirty(t.m));
    mach_free(&t);
}

TEST(n64_a_read_of_nothing_in_domain_2_or_past_the_rom_gives_the_address)
{
    mach t;
    mach_new(&t);
    CHECK_EQ(rl_n64_read(t.m, 0x05000100u, 4), 0x01000100u);
    CHECK_EQ(rl_n64_read(t.m, 0x10002000u, 4), 0x20002000u);
    CHECK_EQ(rl_n64_read(t.m, 0x10000000u, 4), 0x55555555u);
    mach_free(&t);
}

TEST(n64_sram_by_dma_in_both_directions)
{
    mach t;
    mach_new(&t);
    for (unsigned i = 0; i < 64; i++)
        rl_n64_rdram(t.m)[(0x3000 + i) ^ 3] = (uint8_t)(i * 3 + 1);
    pi_dma(&t, true, 0x3000, SRAM + 0x100, 64);
    ram_set(t.m, 0x4000, 0, 64);
    pi_dma(&t, false, 0x4000, SRAM + 0x100, 64);
    {
        uint8_t x_[64];
        rl_n64_rdram_get(t.m, 0x3000, x_, 64);
        CHECK(ram_eq(t.m, 0x4000, x_, 64));
    }
    CHECK_EQ(rl_n64_read(t.m, SRAM + 0x100, 4), 0x0104070Au);
    mach_free(&t);
}

TEST(n64_the_first_write_to_the_flash_command_register_makes_the_chip_answer)
{
    mach t;
    mach_new(&t);
    rl_n64_write(t.m, SRAM + 0x20, 4, 0x11111111u);
    advance(&t, 200);
    CHECK_EQ(rl_n64_read(t.m, SRAM + 0x20, 4), 0x11111111u);
    flash_cmd(&t, 0xE1000000u);
    CHECK_EQ(rl_n64_read(t.m, SRAM, 4), 0x11118001u);
    pi_dma(&t, false, 0x5000, SRAM, 8);
    const uint8_t id[8] = {0x11, 0x11, 0x80, 0x01, 0x00, 0xC2, 0x00, 0x1D};
    CHECK(ram_eq(t.m, 0x5000, id, 8));
    flash_cmd(&t, 0xF0000000u);
    CHECK_EQ(rl_n64_read(t.m, SRAM + 0x20, 4), 0xFFFFFFFFu);
    mach_free(&t);
}

TEST(n64_flash_erase_program_and_status_take_their_time)
{
    mach t;
    mach_new(&t);
    flash_cmd(&t, 0xD2000000u);
    flash_cmd(&t, 0xD2000000u);
    CHECK_EQ(rl_n64_read(t.m, SRAM, 4) & 0xFFFFu, 0x008Cu);

    flash_cmd(&t, 0x4B000000u | (3u << 7));
    flash_cmd(&t, 0x78000000u);
    CHECK_EQ(rl_n64_read(t.m, SRAM, 4) & 0xFFFFu, 0x000Eu);
    CHECK_EQ(rl_n64_read(t.m, SRAM, 4) & 0xFFFFu, 0x000Eu);
    advance(&t, 84 * MS);
    CHECK_EQ(rl_n64_read(t.m, SRAM, 4) & 0x80u, 0u);
    advance(&t, 2 * MS);
    CHECK_EQ(rl_n64_read(t.m, SRAM, 4) & 0xFFFFu, 0x008Cu);

    for (unsigned i = 0; i < 128; i++)
        rl_n64_rdram(t.m)[(0x3000 + i) ^ 3] = (uint8_t)(0xF0 | (i & 15));
    flash_cmd(&t, 0xF0000000u);
    flash_cmd(&t, 0xB4000000u);
    pi_dma(&t, true, 0x3000, SRAM, 128);
    flash_cmd(&t, 0xA5000000u | (3u * 128u));
    CHECK_EQ(rl_n64_read(t.m, SRAM, 4) & 0xFFFFu, 0x000Du);
    advance(&t, 4 * MS);
    CHECK_EQ(rl_n64_read(t.m, SRAM, 4) & 0xFFFFu, 0x008Cu);
    flash_cmd(&t, 0xF0000000u);
    ram_set(t.m, 0x4000, 0, 256);
    pi_dma(&t, false, 0x4000, SRAM + 0xC000, 128);
    {
        uint8_t x_[128];
        rl_n64_rdram_get(t.m, 0x3000, x_, 128);
        CHECK(ram_eq(t.m, 0x4000, x_, 128));
    }
    CHECK_EQ(rl_n64_read(t.m, SRAM + 0xC000 + 128, 4), 0xFFFFFFFFu);

    ram_set(t.m, 0x3000, 0x3C, 128);
    flash_cmd(&t, 0xB4000000u);
    pi_dma(&t, true, 0x3000, SRAM, 128);
    flash_cmd(&t, 0xA5000000u | (3u * 128u));
    advance(&t, 4 * MS);
    flash_cmd(&t, 0xF0000000u);
    CHECK_EQ(rl_n64_read(t.m, SRAM + 0xC000, 4), 0x30303030u);
    CHECK_EQ(rl_n64_read(t.m, SRAM + 0xC004, 4), 0x34343434u);
    mach_free(&t);
}

TEST(n64_flash_commands_are_ignored_while_the_chip_is_busy_and_a_chip_erase_clears_all)
{
    mach t;
    mach_new(&t);
    flash_cmd(&t, 0x3C000000u);
    flash_cmd(&t, 0x78000000u);
    flash_cmd(&t, 0xF0000000u);
    CHECK_EQ(rl_n64_read(t.m, SRAM + 0x100, 4) & 0xFFFFu, 0x000Eu);
    advance(&t, 86 * MS);
    flash_cmd(&t, 0xF0000000u);
    CHECK_EQ(rl_n64_read(t.m, SRAM + 0x100, 4), 0xFFFFFFFFu);
    mach_free(&t);
}

TEST(n64_eeprom_status_read_and_write_over_joybus_channel_4)
{
    mach t;
    mach_new(&t);
    uint8_t status[] = {0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF};
    uint8_t r[16];
    eeprom_block(&t, status, sizeof status, r);
    CHECK_EQ(r[3], 0x00);
    CHECK_EQ(r[4], 0xC0);
    CHECK_EQ(r[5], 0x00);

    uint8_t write[] = {0x0A, 0x01, 0x05, 0x02, 1, 2, 3, 4, 5, 6, 7, 8, 0xFF};
    eeprom_block(&t, write, sizeof write, r);
    CHECK_EQ(r[1], 0x01);
    CHECK_EQ(r[12], 0x00);
    CHECK(rl_n64_save_dirty(t.m));
    eeprom_block(&t, status, sizeof status, r);
    CHECK_EQ(r[5], 0x80);
    uint8_t read[] = {0x02, 0x08, 0x04, 0x02, 0, 0, 0, 0, 0, 0, 0, 0};
    eeprom_block(&t, read, sizeof read, r);
    CHECK_EQ(r[4], 0xFF);
    advance(&t, 6 * MS);
    eeprom_block(&t, read, sizeof read, r);
    for (unsigned i = 0; i < 8; i++)
        CHECK_EQ(r[4 + i], i + 1);
    eeprom_block(&t, status, sizeof status, r);
    CHECK_EQ(r[5], 0x00);
    mach_free(&t);
}

static void path_of(char *out, size_t n, const char *name)
{
    const char *d = getenv("TMPDIR");
    snprintf(out, n, "%s/relift-n64-save-%ld-%s", d && *d ? d : ".", (long)getpid(), name);
}

static void remove_saves(const char *base)
{
    static const char *const ext[] = {".eep", ".sra", ".fla", ".eep.tmp", ".sra.tmp", ".fla.tmp"};
    char p[600];
    for (unsigned i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        snprintf(p, sizeof p, "%s%s", base, ext[i]);
        remove(p);
    }
}

static size_t file_size(const char *base, const char *ext)
{
    char p[600];
    snprintf(p, sizeof p, "%s%s", base, ext);
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    size_t n = (size_t)ftell(f);
    fclose(f);
    return n;
}

TEST(n64_the_save_files_hold_only_what_the_game_changed_and_load_back)
{
    char base[512];
    path_of(base, sizeof base, "a");
    remove_saves(base);
    mach t;
    mach_new(&t);
    CHECK_EQ(rl_n64_save_load(t.m, base), 0);
    CHECK_EQ(rl_n64_save_write(t.m, base), 0);
    rl_n64_write(t.m, SRAM + 4, 4, 0x0A0B0C0Du);
    advance(&t, 200);
    CHECK_EQ(rl_n64_save_poll(t.m, base, 1000000), 0);
    advance(&t, 1000000);
    CHECK_EQ(rl_n64_save_poll(t.m, base, 1000000), 1);
    CHECK(!rl_n64_save_dirty(t.m));
    CHECK_EQ(file_size(base, ".sra"), 0x8000u);
    CHECK_EQ(file_size(base, ".eep"), 0u);
    CHECK_EQ(file_size(base, ".fla"), 0u);
    mach_free(&t);

    mach u;
    mach_new(&u);
    CHECK_EQ(rl_n64_save_load(u.m, base), 1);
    CHECK_EQ(rl_n64_read(u.m, SRAM + 4, 4), 0x0A0B0C0Du);
    CHECK(!rl_n64_save_dirty(u.m));
    mach_free(&u);
    remove_saves(base);
}

TEST(n64_a_flash_file_makes_the_chip_answer_from_the_start_and_a_small_eeprom_file_reports_4_kbit)
{
    char base[512];
    path_of(base, sizeof base, "b");
    remove_saves(base);
    static uint8_t fl[0x20000];
    memset(fl, 0xA5, sizeof fl);
    char p[600];
    snprintf(p, sizeof p, "%s.fla", base);
    FILE *f = fopen(p, "wb");
    CHECK(f != NULL);
    fwrite(fl, 1, sizeof fl, f);
    fclose(f);
    snprintf(p, sizeof p, "%s.eep", base);
    f = fopen(p, "wb");
    CHECK(f != NULL);
    memset(fl, 0x5A, 512);
    fwrite(fl, 1, 512, f);
    fclose(f);

    mach t;
    mach_new(&t);
    CHECK_EQ(rl_n64_save_load(t.m, base), 2);
    CHECK_EQ(rl_n64_read(t.m, SRAM, 4), 0xA5A5A5A5u);
    uint8_t status[] = {0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF};
    uint8_t r[16];
    eeprom_block(&t, status, sizeof status, r);
    CHECK_EQ(r[4], 0x80);
    mach_free(&t);
    remove_saves(base);

    snprintf(p, sizeof p, "%s.sra", base);
    f = fopen(p, "wb");
    CHECK(f != NULL);
    fwrite(fl, 1, 100, f);
    fclose(f);
    mach u;
    mach_new(&u);
    CHECK_EQ(rl_n64_save_load(u.m, base), -1);
    mach_free(&u);
    remove_saves(base);
}
