/**
 * @file test_run.c
 * @brief The VR4300 CPU on the Nintendo 64 machine with hand-written lifted
 * functions: the high-level boot's registers, code bound by its hash at the
 * first entry, a run whose bytes changed is missed code, an instruction
 * cache invalidation checks again, a VI interrupt taken at a sync point
 * enters the general vector and returns, and boot code in DMEM starts the
 * CPU there and is checked again after a write to DMEM. The ROM is
 * synthetic.
 */
#include "cpu.h"
#include "runner.h"

#include <string.h>

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

static const rl_guest_desc test_guest = {
    .name = "n64-run-test",
    .page_size = 0x1000,
    .address_bits = 32,
    .tick_hz = RL_N64_HZ,
};

#define K0(a) (UINT64_C(0xFFFFFFFF80000000) | (a))
#define K1(a) (UINT64_C(0xFFFFFFFFA0000000) | (a))

static struct {
    int mains;
    int handlers;
    uint64_t handler_epc;
    int spins;
    int boots;
    uint64_t boot_sp, boot_ra, boot_t3;
} hs;

static int boot_fn(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    hs.boots++;
    hs.boot_sp = c->r[29];
    hs.boot_ra = c->r[31];
    hs.boot_t3 = c->r[11];
    rl_n64_write(c->machine, 0x04000044u, 4, 0);
    return RL_OK;
}

static int handler(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    hs.handlers++;
    hs.handler_epc = c->cp0_epc;
    rl_n64_write(c->machine, 0x04400010u, 4, 0);
    RL_MUSTTAIL return rl_mips_eret_to(c, rl_mips_eret(c))(c, mem);
}

/** @brief The budget before the first handler's ERET and after it. */
static int64_t eret_budget[2];

/**
 * @brief Takes the VI interrupt, and the first time raises the SP interrupt
 * while EXL is set. ERET may go anywhere (an OS switching threads), so it
 * must end the budget itself for the line to be taken at the next sync point.
 */
static int handler2(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    hs.handlers++;
    rl_mips_cp0_sync(c);
    rl_n64_write(c->machine, 0x04400010u, 4, 0);
    rl_n64_write(c->machine, 0x04040010u, 4, hs.handlers == 1 ? 0x10u : 0x08u);
    if (hs.handlers == 1)
        eret_budget[0] = c->time.budget;
    uint64_t to = rl_mips_eret(c);
    if (hs.handlers == 1)
        eret_budget[1] = c->time.budget;
    RL_MUSTTAIL return rl_mips_eret_to(c, to)(c, mem);
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
    rl_n64_write(c->machine, 0x0430000Cu, 4, c->r[3] ? 0x82u : 0x80u);
    rl_n64_write(c->machine, 0x04400000u, 4, 0x3002u);
    rl_n64_write(c->machine, 0x0440000Cu, 4, 0x10u);
    rl_n64_write(c->machine, 0x04400018u, 4, 0x20Du);
    rl_n64_write(c->machine, 0x0440001Cu, 4, 0xC15u);
    rl_n64_write(c->machine, 0x04400020u, 4, 0x0C150C15u);
    c->cp0_status = 0x34000401u;
    int rc = rl_n64_cp0_sync(c, mem, K0(0x1004));
    while (!rc && hs.handlers < (c->r[3] ? 2 : 1) && hs.spins < 1000000) {
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

static const rl_fn_entry fns_spin2[] = {
    {K0(0x180), 8, RL_FN_RANGE, handler2},
    {K0(0x1000), 8, RL_FN_RANGE, spin_fn},
};

static const rl_fn_entry fns_boot[] = {
    {K1(0x04000040), 8, RL_FN_RANGE, boot_fn},
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

static int boot_with(const rl_fn_entry *fns, uint32_t n, bool dmem)
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
    if (dmem)
        code[1] = (rl_n64_code){0, 0, 0x40u, 8u, K1(0x04000040), fnv(rom + 0x40, 8)};
    mach = rl_n64_create(rom, sizeof rom);
    static const rl_unit *list[] = {&unit};
    if (!mach || rl_n64_code_init(mach, list, 1, code, 2) != 0)
        return -1;
    return rl_n64_run_init(&run, mach, 6102, 0x80001000u);
}

static int boot(const rl_fn_entry *fns, uint32_t n) { return boot_with(fns, n, false); }

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
    CHECK(ram_eq(mach, 0x1000, rom + 0x1000, 0x100000));
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
    rl_n64_rdram(mach)[(0x1004) ^ 3] ^= 1u;
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

TEST(n64_run_an_interrupt_raised_in_a_handler_is_taken_right_after_eret)
{
    CHECK(boot(fns_spin2, 2) == 0);
    run.c->r[3] = 1;
    CHECK_EQ(rl_n64_run_start(&run), RL_OK);
    CHECK_EQ(hs.handlers, 2);
    CHECK(eret_budget[0] > 0);
    CHECK(eret_budget[1] <= 0);
    CHECK_EQ(rl_n64_irq_pending(mach), 0);
}

TEST(n64_run_starts_the_boot_code_in_dmem)
{
    CHECK(boot_with(fns_boot, 1, true) == 0);
    CHECK_EQ(run.entry, K1(0x04000040));
    CHECK_EQ(memcmp(rl_n64_spmem(mach), rom, 0x1000), 0);
    CHECK_EQ(rl_n64_read(mach, 0x318u, 4), 0);
    CHECK_EQ(rl_n64_read(mach, 0x0470000Cu, 4), 0x14u);
    CHECK_EQ(rl_n64_run_start(&run), RL_OK);
    CHECK_EQ(hs.boots, 1);
    CHECK_EQ(hs.boot_t3, K1(0x04000040));
    CHECK_EQ(hs.boot_sp, UINT64_C(0xFFFFFFFFA4001FF0));
    CHECK_EQ(hs.boot_ra, UINT64_C(0xFFFFFFFFA4001554));
    CHECK_EQ(rl_n64_get_stats(mach)->checks, 1u);
    CHECK_EQ(rl_n64_get_stats(mach)->invalidations, 1u);
    CHECK_EQ(rl_n64_run_start(&run), RL_PENDING);
    CHECK_EQ(hs.boots, 1);
    CHECK_EQ(rl_n64_get_stats(mach)->mismatches, 1u);
}
