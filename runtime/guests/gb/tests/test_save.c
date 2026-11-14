/**
 * @file test_save.c
 * @brief Battery saves: a round trip through a host file for RAM and for
 * RAM with a clock, the dirty flag, and the refusals.
 */
#include "gbt.h"

#include <stdio.h>
#include <unistd.h>

static char g_dir[512];

static const char *save_path(const char *name)
{
    static char path[600];
    if (!g_dir[0]) {
        const char *base = getenv("TMPDIR");
        snprintf(g_dir, sizeof g_dir, "%s/relift-gb-XXXXXX", base && *base ? base : "/tmp");
        if (!mkdtemp(g_dir))
            g_dir[0] = 0;
    }
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    return path;
}

static void put(rl_gb *g, uint16_t addr, uint8_t v) { rl_gb_write(g, NULL, addr, v); }

TEST(save_round_trips_ram_through_a_file)
{
    uint8_t *rom = gbt_rom(4, 0x1B, 0x03, 0);
    rl_gb *a = gbt_machine(RL_GB_DMG, rom, 4);
    CHECK(!rl_gb_save_dirty(a));
    put(a, 0x0000, 0x0A);
    for (unsigned b = 0; b < 4; b++) {
        put(a, 0x4000, (uint8_t)b);
        for (unsigned i = 0; i < 0x2000; i += 0x101)
            put(a, (uint16_t)(0xA000 + i), (uint8_t)(b * 16 + i / 0x101));
    }
    CHECK(rl_gb_save_dirty(a));
    char path[600];
    snprintf(path, sizeof path, "%s", save_path("a.sav"));
    CHECK_EQ(rl_gb_save(a, path), 0);
    CHECK(!rl_gb_save_dirty(a));
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    fseek(f, 0, SEEK_END);
    CHECK_EQ(ftell(f), 0x8000);
    fclose(f);
    CHECK(access(save_path("a.sav.tmp"), F_OK) != 0);

    rl_gb *b = gbt_machine(RL_GB_DMG, rom, 4);
    CHECK_EQ(rl_gb_load(b, path), 0);
    CHECK(!rl_gb_save_dirty(b));
    put(b, 0x0000, 0x0A);
    for (unsigned bank = 0; bank < 4; bank++) {
        put(b, 0x4000, (uint8_t)bank);
        for (unsigned i = 0; i < 0x2000; i += 0x101)
            CHECK_EQ(rl_gb_read(b, NULL, (uint16_t)(0xA000 + i)), bank * 16 + i / 0x101);
    }
    remove(path);
    rl_gb_destroy(a);
    rl_gb_destroy(b);
    free(rom);
}

TEST(save_rewrites_a_file_that_exists)
{
    uint8_t *rom = gbt_rom(2, 0x09, 0x02, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 2);
    char path[600];
    snprintf(path, sizeof path, "%s", save_path("b.sav"));
    put(g, 0xA000, 0x11);
    CHECK_EQ(rl_gb_save(g, path), 0);
    put(g, 0xA000, 0x22);
    CHECK(rl_gb_save_dirty(g));
    CHECK_EQ(rl_gb_save(g, path), 0);
    rl_gb *h = gbt_machine(RL_GB_DMG, rom, 2);
    CHECK_EQ(rl_gb_load(h, path), 0);
    CHECK_EQ(rl_gb_read(h, NULL, 0xA000), 0x22);
    remove(path);
    rl_gb_destroy(g);
    rl_gb_destroy(h);
    free(rom);
}

TEST(save_keeps_mbc2_nibbles)
{
    uint8_t *rom = gbt_rom(2, 0x06, 0, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 2);
    CHECK_EQ(rl_gb_save_size(g), 512);
    put(g, 0x0000, 0x0A);
    put(g, 0xA010, 0x0C);
    uint8_t buf[512];
    CHECK_EQ(rl_gb_save_bytes(g, buf), 512);
    CHECK_EQ(buf[0x10], 0x0C);
    buf[0x20] = 0xF7;
    CHECK_EQ(rl_gb_load_bytes(g, buf, 512), 0);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA020), 0xF7);
    CHECK_EQ(rl_gb_read(g, NULL, 0xA010), 0xFC);
    rl_gb_destroy(g);
    free(rom);
}

typedef struct fake_time {
    uint64_t now;
} fake_time;

static uint64_t fake_now(void *ctx) { return ((fake_time *)ctx)->now; }

TEST(save_carries_the_clock_and_time_passes_while_it_is_off)
{
    uint8_t *rom = gbt_rom(4, 0x10, 0x02, 0);
    fake_time t = {10000};
    rl_gb *a = gbt_machine(RL_GB_DMG, rom, 4);
    rl_gb_set_time_source(a, fake_now, &t);
    put(a, 0x0000, 0x0A);
    put(a, 0x4000, 0x08);
    put(a, 0xA000, 5);
    put(a, 0x4000, 0x00);
    put(a, 0xA123, 0x77);
    t.now += 90;
    CHECK_EQ(rl_gb_save_size(a), 0x2000 + RL_GB_SAVE_CLOCK_BYTES);
    uint8_t *buf = malloc(rl_gb_save_size(a));
    CHECK_EQ(rl_gb_save_bytes(a, buf), 0x2000 + RL_GB_SAVE_CLOCK_BYTES);
    CHECK_EQ(buf[0x2000], 5 + 30);
    CHECK_EQ(buf[0x2004], 1);

    rl_gb *b = gbt_machine(RL_GB_DMG, rom, 4);
    t.now += 3600;
    rl_gb_set_time_source(b, fake_now, &t);
    CHECK_EQ(rl_gb_load_bytes(b, buf, 0x2000 + RL_GB_SAVE_CLOCK_BYTES), 0);
    put(b, 0x0000, 0x0A);
    CHECK_EQ(rl_gb_read(b, NULL, 0xA123), 0x77);
    put(b, 0x6000, 0x00);
    put(b, 0x6000, 0x01);
    put(b, 0x4000, 0x08);
    CHECK_EQ(rl_gb_read(b, NULL, 0xA000), 35);
    put(b, 0x4000, 0x09);
    CHECK_EQ(rl_gb_read(b, NULL, 0xA000), 1);
    put(b, 0x4000, 0x0A);
    CHECK_EQ(rl_gb_read(b, NULL, 0xA000), 1);
    CHECK_EQ(rl_gb_load_bytes(b, buf, 0x2000 + 7), -1);
    CHECK_EQ(rl_gb_load_bytes(b, buf, 0x2000), 0);
    free(buf);
    rl_gb_destroy(a);
    rl_gb_destroy(b);
    free(rom);
}

TEST(save_refuses_what_has_no_battery_or_does_not_fit)
{
    uint8_t *rom = gbt_rom(4, 0x1A, 0x02, 0);
    rl_gb *g = gbt_machine(RL_GB_DMG, rom, 4);
    CHECK_EQ(rl_gb_save_size(g), 0);
    CHECK_EQ(rl_gb_save(g, save_path("none.sav")), 1);
    CHECK(access(save_path("none.sav"), F_OK) != 0);
    CHECK_EQ(rl_gb_load(g, save_path("none.sav")), 1);
    uint8_t buf[16] = {0};
    CHECK_EQ(rl_gb_load_bytes(g, buf, sizeof buf), -1);
    rl_gb_destroy(g);
    free(rom);

    rom = gbt_rom(4, 0x1B, 0x02, 0);
    g = gbt_machine(RL_GB_DMG, rom, 4);
    CHECK_EQ(rl_gb_load(g, save_path("missing.sav")), 1);
    FILE *f = fopen(save_path("short.sav"), "wb");
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
    CHECK_EQ(rl_gb_load(g, save_path("short.sav")), -1);
    remove(save_path("short.sav"));
    rl_gb_destroy(g);
    free(rom);
}
