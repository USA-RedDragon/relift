/**
 * @file test_run.c
 * @brief Both CPUs on the Nintendo DS machine with hand-written lifted
 * functions: the boot state, a word each way over the IPC FIFO, halts that
 * end the run, an entry check that goes on in another unit's function, and
 * one that finds no lifted code and stops the program.
 */
#include "cpu.h"
#include "runner.h"

#include <string.h>

static const rl_guest_desc test_guest = {
    .name = "nds-run-test",
    .page_size = 0x1000,
    .address_bits = 32,
    .tick_hz = RL_NDS_TICK_HZ,
};

#define FCNT 0x04000184u
#define FSEND 0x04000188u
#define FRECV 0x04100000u
#define HALTCNT 0x04000301u
#define RESULT 0x02100000u
#define ENTRY9 0x02000000u
#define ENTRY7 0x02380000u
#define CODE_LEN 8u

static struct {
    uint32_t boot_sp9, boot_lr7, boot_mode9;
    int exits;
    bool other_bytes;
} T;

/** @brief The cartridge: a header with both binaries of CODE_LEN bytes; the ARM9's first byte is `first9`. */
static uint8_t g_rom[0x400];

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void make_rom(uint8_t first9)
{
    memset(g_rom, 0, sizeof g_rom);
    put32(g_rom + 0x20, 0x200);
    put32(g_rom + 0x24, ENTRY9);
    put32(g_rom + 0x28, ENTRY9);
    put32(g_rom + 0x2C, CODE_LEN);
    put32(g_rom + 0x30, 0x300);
    put32(g_rom + 0x34, ENTRY7);
    put32(g_rom + 0x38, ENTRY7);
    put32(g_rom + 0x3C, CODE_LEN);
    for (unsigned i = 0; i < CODE_LEN; i++) {
        g_rom[0x200 + i] = (uint8_t)(0x90 + i);
        g_rom[0x300 + i] = (uint8_t)(0x70 + i);
    }
    g_rom[0x200] = first9;
}

/** @brief Polls until the FIFO holds a word, at most a while. */
static void wait_recv(rl_ctx *c, uint8_t *mem)
{
    for (int n = 0; n < 100000 && (rl_nds_cpu_read(c, mem, FCNT, 2) & 0x100); n++)
        c->time.budget -= 4;
}

static int arm9_body(rl_ctx *c, uint8_t *mem)
{
    T.boot_sp9 = c->r[13];
    T.boot_mode9 = c->m;
    rl_nds_cpu_write(c, mem, FCNT, 2, 0x8000);
    rl_nds_cpu_write(c, mem, FSEND, 4, 0x1234);
    wait_recv(c, mem);
    uint32_t v = (uint32_t)rl_nds_cpu_read(c, mem, FRECV, 4);
    rl_nds_cpu_write(c, mem, RESULT, 4, v);
    rl_nds_cpu_cp_write(c, RL_CP15_KEY(0, 7, 0, 4), 0, 1);
    return RL_OK;
}

static int arm9_main(rl_ctx *c, uint8_t *mem)
{
    if (rl_nds_cpu_enter(c, 0, 0))
        return rl_nds_cpu_variant(c, ENTRY9, 0)(c, mem);
    return arm9_body(c, mem);
}

/** @brief The ARM9's function of the second unit at the same entry, lifted from other bytes. */
static int arm9_other(rl_ctx *c, uint8_t *mem)
{
    if (rl_nds_cpu_enter(c, 2, 0))
        return rl_nds_cpu_variant(c, ENTRY9, 0)(c, mem);
    T.other_bytes = true;
    return arm9_body(c, mem);
}

static int arm7_main(rl_ctx *c, uint8_t *mem)
{
    if (rl_nds_cpu_enter(c, 1, 0))
        return rl_nds_cpu_variant(c, ENTRY7, 0)(c, mem);
    T.boot_lr7 = c->r[14];
    rl_nds_cpu_write(c, mem, FCNT, 2, 0x8000);
    wait_recv(c, mem);
    uint32_t v = (uint32_t)rl_nds_cpu_read(c, mem, FRECV, 4);
    rl_nds_cpu_write(c, mem, FSEND, 4, v + 1);
    rl_nds_cpu_write(c, mem, HALTCNT, 1, 0x80);
    return RL_OK;
}

static uint8_t g_blob9[CODE_LEN], g_blob7[CODE_LEN], g_blob9b[CODE_LEN];
static rl_unit g_units[3];
static rl_nds_fn g_fns[3];
static rl_nds_unit g_nds_units[3];
static rl_nds_program g_prog;

static void on_exit_code(uint32_t code)
{
    (void)code;
    T.exits++;
}

/** @brief The program: unit 0 the ARM9 static, 1 the ARM7 static, 2 another ARM9 unit at the same entry lifted from bytes with first byte 0x55. */
static void make_program(void)
{
    for (unsigned i = 0; i < CODE_LEN; i++) {
        g_blob9[i] = (uint8_t)(0x90 + i);
        g_blob7[i] = (uint8_t)(0x70 + i);
        g_blob9b[i] = (uint8_t)(0x90 + i);
    }
    g_blob9b[0] = 0x55;
    rl_fn fns[3] = {arm9_main, arm7_main, arm9_other};
    const uint8_t *blobs[3] = {g_blob9, g_blob7, g_blob9b};
    uint32_t cpus[3] = {RL_NDS_ARM9, RL_NDS_ARM7, RL_NDS_ARM9};
    uint32_t at[3] = {ENTRY9, ENTRY7, ENTRY9};
    for (unsigned u = 0; u < 3; u++) {
        memset(&g_units[u], 0, sizeof g_units[u]);
        g_units[u].abi_version = RL_ABI_VERSION;
        g_units[u].ctx_size = sizeof(rl_ctx);
        g_fns[u] = (rl_nds_fn){at[u], 0, CODE_LEN, fns[u]};
        g_nds_units[u] = (rl_nds_unit){cpus[u], &g_units[u], blobs[u], &g_fns[u], 1, "test", NULL, NULL, 0};
    }
    g_prog = (rl_nds_program){g_rom, sizeof g_rom, g_nds_units, 3, "test", NULL, 0};
}

static int run_once(rl_nds_run *r, uint8_t first9)
{
    memset(&T, 0, sizeof T);
    if (rl_core_init(&test_guest, rl_arm32_cpu()) != 0)
        return -1;
    make_rom(first9);
    make_program();
    if (rl_nds_run_init(r, &g_prog) != 0)
        return -1;
    rl_set_exit_hook(on_exit_code);
    int rc = rl_nds_run_start(r);
    rl_set_exit_hook(NULL);
    return rc;
}

TEST(nds_run_boots_both_cpus_and_trades_words_over_the_fifo)
{
    static rl_nds_run r;
    CHECK_EQ(run_once(&r, 0x90), 0);
    CHECK_EQ(T.boot_sp9, 0x03002F7Cu);
    CHECK_EQ(T.boot_mode9, RL_ARM32_SYS);
    CHECK_EQ(T.boot_lr7, ENTRY7);
    CHECK_EQ(rl_nds_read(r.m, RL_NDS_ARM9, RESULT, 4), 0x1235u);
    CHECK_EQ(rl_nds_read(r.m, RL_NDS_ARM9, 0x027FFE24u, 4), ENTRY9);
    CHECK_EQ(r.cpu[RL_NDS_ARM9].stopped, 1);
    CHECK_EQ(r.cpu[RL_NDS_ARM7].stopped, 1);
    CHECK_EQ(r.cpu[RL_NDS_ARM9].stats.compares, 1);
    CHECK(!T.other_bytes);
    rl_nds_run_free(&r);
}

TEST(nds_run_entry_with_other_bytes_goes_on_in_the_unit_lifted_from_them)
{
    static rl_nds_run r;
    CHECK_EQ(run_once(&r, 0x55), 0);
    CHECK(T.other_bytes);
    CHECK_EQ(r.cpu[RL_NDS_ARM9].stats.variants, 1);
    CHECK_EQ(rl_nds_read(r.m, RL_NDS_ARM9, RESULT, 4), 0x1235u);
    rl_nds_run_free(&r);
}

TEST(nds_run_code_no_unit_holds_stops_the_program)
{
    static rl_nds_run r;
    CHECK_EQ(run_once(&r, 0x33), 1);
    CHECK_EQ(T.exits, 1);
    CHECK_EQ(r.cpu[RL_NDS_ARM9].stats.unfound, 1);
    CHECK_EQ(r.cpu[RL_NDS_ARM9].stopped, 2);
    rl_nds_run_free(&r);
}
