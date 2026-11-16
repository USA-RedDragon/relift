/**
 * @file test_run.c
 * @brief The VR4300 CPU on the Nintendo 64 machine with hand-written lifted
 * functions: the high-level boot's registers, code bound by its hash at the
 * first entry, a run whose bytes changed is missed code, an instruction
 * cache invalidation checks again, and a VI interrupt taken at a sync point
 * enters the general vector and returns. The ROM is synthetic.
 */
#include "cpu.h"
#include "runner.h"

#include <string.h>

static const rl_guest_desc test_guest = {
    .name = "n64-run-test",
    .page_size = 0x1000,
    .address_bits = 32,
    .tick_hz = RL_N64_HZ,
};

#define K0(a) (UINT64_C(0xFFFFFFFF80000000) | (a))

static struct {
    int mains;
    int handlers;
    uint64_t handler_epc;
    int spins;
} hs;

static int handler(rl_ctx *c, uint8_t *mem)
{
    hs.handlers++;
    hs.handler_epc = c->cp0_epc;
    rl_n64_cpu_write(c, mem, 0x04400010u, 4, 0);
    RL_MUSTTAIL return rl_mips_eret_to(c, rl_mips_eret(c))(c, mem);
}

static int main_fn(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    hs.mains++;
    c->r[2] = 42;
    return RL_OK;
}

static int spin_fn(rl_ctx *c, uint8_t *mem)
{
    hs.mains++;
    rl_n64_cpu_write(c, mem, 0x0430000Cu, 4, 0x80u);
    rl_n64_cpu_write(c, mem, 0x04400000u, 4, 0x3002u);
    rl_n64_cpu_write(c, mem, 0x0440000Cu, 4, 0x10u);
    rl_n64_cpu_write(c, mem, 0x04400018u, 4, 0x20Du);
    rl_n64_cpu_write(c, mem, 0x0440001Cu, 4, 0xC15u);
    rl_n64_cpu_write(c, mem, 0x04400020u, 4, 0x0C150C15u);
    c->cp0_status = 0x34000401u;
    int rc = rl_n64_cp0_sync(c, mem, K0(0x1004));
    while (!rc && !hs.handlers && hs.spins < 1000000) {
        hs.spins++;
        c->time.budget -= 100;
        if (c->time.budget <= 0)
            rc = rl_sync(c, mem, K0(0x1004));
    }
    return rc;
}

static const rl_fn_entry fns_main[] = {
    {K0(0x1000), 8, RL_FN_RANGE, main_fn},
};
static const rl_fn_entry fns_spin[] = {
    {K0(0x180), 8, RL_FN_RANGE, handler},
    {K0(0x1000), 8, RL_FN_RANGE, spin_fn},
};

static uint8_t rom[0x102000];
static rl_n64_run run;
static rl_n64 *mach;
static rl_unit unit;
static rl_n64_code code[2];

static uint64_t fnv(const uint8_t *b, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

static int boot(const rl_fn_entry *fns, uint32_t n)
{
    if (rl_core_init(&test_guest, rl_mips_cpu()) != 0)
        return -1;
    rl_units_reset();
    if (mach) {
        rl_n64_run_free(&run);
        rl_n64_destroy(mach);
    }
    memset(&hs, 0, sizeof hs);
    for (size_t i = 0; i < sizeof rom; i++)
        rom[i] = (uint8_t)(i * 13u + 5u);
    rom[0x3E] = 'E';
    memset(&unit, 0, sizeof unit);
    unit.abi_version = RL_ABI_VERSION;
    unit.ctx_size = sizeof(rl_ctx);
    unit.name = "n64-run-test";
    unit.fn_count = n;
    unit.fns = fns;
    code[0] = (rl_n64_code){0, 0, 0x1000u, 8u, K0(0x1000), fnv(rom + 0x1000, 8)};
    static const uint8_t zero[8];
    code[1] = (rl_n64_code){0, 0, 0, 8u, K0(0x180), fnv(zero, 8)};
    mach = rl_n64_create(rom, sizeof rom);
    static const rl_unit *list[] = {&unit};
    if (!mach || rl_n64_code_init(mach, list, 1, code, 2) != 0)
        return -1;
    return rl_n64_run_init(&run, mach, 6102, 0x80001000u);
}

TEST(n64_run_boots_with_the_registers_the_boot_code_leaves)
{
    CHECK(boot(fns_main, 1) == 0);
    rl_ctx *c = run.c;
    CHECK_EQ(run.entry, K0(0x1000));
    CHECK_EQ(c->r[29], UINT64_C(0xFFFFFFFFA4001FF0));
    CHECK_EQ(c->r[22], 0x3Fu);
    CHECK_EQ(c->r[20], 1u);
    CHECK_EQ(c->cp0_status, 0x34000000u);
    CHECK_EQ(rl_n64_read(mach, 0x318u, 4), RL_N64_RDRAM_SIZE);
    CHECK_EQ(memcmp(rl_n64_rdram(mach) + 0x1000, rom + 0x1000, 0x100000), 0);
}

TEST(n64_run_binds_code_by_its_hash_at_the_first_entry)
{
    CHECK(boot(fns_main, 1) == 0);
    CHECK_EQ(rl_n64_run_start(&run), RL_OK);
    CHECK_EQ(hs.mains, 1);
    CHECK_EQ(run.c->r[2], 42u);
    CHECK_EQ(rl_n64_get_stats(mach)->checks, 1u);
    CHECK_EQ(rl_n64_run_start(&run), RL_OK);
    CHECK_EQ(rl_n64_get_stats(mach)->checks, 1u);
    rl_mips_cache(run.c, 0x10u, K0(0x1000), 1);
    CHECK_EQ(rl_n64_get_stats(mach)->invalidations, 1u);
    CHECK_EQ(rl_n64_run_start(&run), RL_OK);
    CHECK_EQ(rl_n64_get_stats(mach)->checks, 2u);
    CHECK_EQ(hs.mains, 3);
}

TEST(n64_run_changed_bytes_are_missed_code)
{
    CHECK(boot(fns_main, 1) == 0);
    rl_n64_rdram(mach)[0x1004] ^= 1u;
    CHECK_EQ(rl_n64_run_start(&run), RL_PENDING);
    CHECK_EQ(hs.mains, 0);
    CHECK_EQ(rl_n64_get_stats(mach)->mismatches, 1u);
}

TEST(n64_run_a_vi_interrupt_enters_the_vector_at_a_sync_point)
{
    CHECK(boot(fns_spin, 2) == 0);
    CHECK_EQ(rl_n64_run_start(&run), RL_OK);
    CHECK_EQ(hs.handlers, 1);
    CHECK_EQ(hs.handler_epc, K0(0x1004));
    CHECK_EQ(rl_n64_irq_pending(mach), 0);
    CHECK_EQ(run.c->cp0_status & 2u, 0u);
}
