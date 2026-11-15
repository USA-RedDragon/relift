/**
 * @file test_save_diff.c
 * @brief The backups against the NanoBoyAdvance oracle: SRAM, Flash 64 and
 * 128 KiB command sequences, and EEPROM 512 B and 8 KiB bit streams read
 * back. NanoBoyAdvance finishes Flash programs and erases at once, so the
 * scenarios wait past the busy time here before every read.
 */
#include "oracle.h"

#include <stdio.h>
#include <unistd.h>

#define SAVE 0x0E000000u
#define EEP 0x0D000000u

typedef struct plan {
    dop ops[4096];
    unsigned n;
} plan;

static void add(plan *p, dop d)
{
    if (p->n < 4095)
        p->ops[p->n++] = d;
    p->ops[p->n] = (dop){0};
}

static void wr8(plan *p, uint32_t off, uint8_t v) { add(p, (dop){'w', 1, SAVE + off, v}); }
static void rd8(plan *p, uint32_t off) { add(p, (dop){'r', 1, SAVE + off, 0}); }
static void idle(plan *p, uint32_t turns) { add(p, (dop){'l', 0, 0, turns}); }

static void unlock(plan *p, uint8_t cmd)
{
    wr8(p, 0x5555, 0xAA);
    wr8(p, 0x2AAA, 0x55);
    wr8(p, 0x5555, cmd);
}

static void program(plan *p, uint32_t off, uint8_t v)
{
    unlock(p, 0xA0);
    wr8(p, off, v);
    idle(p, 200);
}

static void sector_erase(plan *p, uint32_t off)
{
    unlock(p, 0x80);
    wr8(p, 0x5555, 0xAA);
    wr8(p, 0x2AAA, 0x55);
    wr8(p, off, 0x30);
    idle(p, 50000);
}

static void chip_erase(plan *p)
{
    unlock(p, 0x80);
    unlock(p, 0x10);
    idle(p, 200000);
}

static bool run(const char *name, const plan *p, const dsetup *cfg)
{
    unsigned reads = 0, diffs = 0;
    for (unsigned pad = 0; pad < 2; pad++) {
        dres r = oracle_run_cfg(name, p->ops, pad, 0, 0, cfg);
        if (r.skipped) {
            printf("  %s: oracle absent, skipped\n", name);
            return true;
        }
        if (r.broken) {
            printf("  %s: oracle protocol failure\n", name);
            return false;
        }
        reads += r.reads;
        diffs += r.diffs;
    }
    printf("  %s: %u reads, %u differ\n", name, reads, diffs);
    return diffs == 0;
}

static void scratch(char *out, size_t n)
{
    const char *d = getenv("TMPDIR");
    snprintf(out, n, "%s/relift-gba-savediff-%ld.sav", d && *d ? d : "/tmp", (long)getpid());
}

TEST(diff_sram_bytes_and_mirror)
{
    static plan p;
    p.n = 0;
    rd8(&p, 0);
    wr8(&p, 0, 0x12);
    wr8(&p, 0x7FFF, 0x34);
    rd8(&p, 0);
    rd8(&p, 0x7FFF);
    rd8(&p, 0x8000);
    rd8(&p, 0xFFFF);
    add(&p, (dop){'r', 2, SAVE, 0});
    add(&p, (dop){'r', 4, SAVE, 0});
    add(&p, (dop){'w', 2, SAVE + 2, 0xABCD});
    add(&p, (dop){'w', 4, SAVE + 4, 0x11223344});
    rd8(&p, 2);
    rd8(&p, 3);
    rd8(&p, 4);
    rd8(&p, 5);
    char f[512];
    scratch(f, sizeof f);
    dsetup cfg = {.nba_save = "sram", .kind = RL_GBA_SAVE_SRAM};
    CHECK(run("sram", &p, &cfg));
}

TEST(diff_flash64_commands)
{
    static plan p;
    p.n = 0;
    rd8(&p, 0);
    unlock(&p, 0x90);
    rd8(&p, 0);
    rd8(&p, 1);
    rd8(&p, 2);
    unlock(&p, 0xF0);
    rd8(&p, 0);
    program(&p, 0x10, 0x5A);
    program(&p, 0xFFFF, 0x00);
    rd8(&p, 0x10);
    rd8(&p, 0x11);
    rd8(&p, 0xFFFF);
    program(&p, 0x1000, 0x77);
    program(&p, 0x1FFF, 0x66);
    sector_erase(&p, 0x1000);
    rd8(&p, 0x10);
    rd8(&p, 0x1000);
    rd8(&p, 0x1FFF);
    chip_erase(&p);
    rd8(&p, 0x10);
    rd8(&p, 0xFFFF);
    char f[512];
    scratch(f, sizeof f);
    dsetup cfg = {.nba_save = "flash64", .save_file = f, .kind = RL_GBA_SAVE_FLASH64, .flash_id = RL_GBA_FLASH_SST, .flash_timed = true};
    CHECK(run("flash64", &p, &cfg));
}

TEST(diff_flash128_banks)
{
    static plan p;
    p.n = 0;
    unlock(&p, 0x90);
    rd8(&p, 0);
    rd8(&p, 1);
    unlock(&p, 0xF0);
    program(&p, 0x20, 0x11);
    unlock(&p, 0xB0);
    wr8(&p, 0, 1);
    rd8(&p, 0x20);
    program(&p, 0x20, 0x22);
    program(&p, 0x8000, 0x33);
    rd8(&p, 0x20);
    rd8(&p, 0x8000);
    unlock(&p, 0xB0);
    wr8(&p, 0, 0);
    rd8(&p, 0x20);
    rd8(&p, 0x8000);
    chip_erase(&p);
    rd8(&p, 0x20);
    unlock(&p, 0xB0);
    wr8(&p, 0, 1);
    rd8(&p, 0x20);
    rd8(&p, 0x8000);
    char f[512];
    scratch(f, sizeof f);
    dsetup cfg = {.nba_save = "flash128", .save_file = f, .kind = RL_GBA_SAVE_FLASH128, .flash_id = RL_GBA_FLASH_MACRONIX128, .flash_timed = true};
    CHECK(run("flash128", &p, &cfg));
}

static void ebit(plan *p, unsigned v) { add(p, (dop){'w', 2, EEP, v}); }
static void ebits(plan *p, uint64_t v, unsigned n)
{
    for (unsigned i = n; i-- > 0;)
        ebit(p, (v >> i) & 1u);
}
static void eread(plan *p) { add(p, (dop){'r', 2, EEP, 0}); }

static void eeprom_plan(plan *p, unsigned ab)
{
    p->n = 0;
    eread(p);
    ebits(p, 2, 2);
    ebits(p, 5, ab);
    ebits(p, 0xC0FFEE0123456789ull, 64);
    ebit(p, 0);
    eread(p);
    idle(p, 5000);
    eread(p);
    idle(p, 30000);
    eread(p);
    ebits(p, 3, 2);
    ebits(p, 5, ab);
    ebit(p, 0);
    for (unsigned i = 0; i < 68; i++)
        eread(p);
    eread(p);
    ebits(p, 3, 2);
    ebits(p, 6, ab);
    ebit(p, 0);
    for (unsigned i = 0; i < 68; i++)
        eread(p);
}

TEST(diff_eeprom_8k_write_busy_and_read)
{
    static plan p;
    eeprom_plan(&p, 14);
    char f[512];
    scratch(f, sizeof f);
    dsetup cfg = {.nba_save = "eeprom64", .save_file = f, .kind = RL_GBA_SAVE_EEPROM, .eeprom_len = 8192};
    CHECK(run("eeprom 8k", &p, &cfg));
}

TEST(diff_eeprom_512_write_busy_and_read)
{
    static plan p;
    eeprom_plan(&p, 6);
    char f[512];
    scratch(f, sizeof f);
    dsetup cfg = {.nba_save = "eeprom4", .save_file = f, .kind = RL_GBA_SAVE_EEPROM, .eeprom_len = 512};
    CHECK(run("eeprom 512", &p, &cfg));
}
