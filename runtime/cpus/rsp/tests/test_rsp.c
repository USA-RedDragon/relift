/**
 * @file test_rsp.c
 * @brief The RSP layer over hand-written functions shaped like lifted code:
 * binding an image only where IMEM holds its bytes, the slices and their
 * cycles, BREAK and the bus, IMEM writes that send the next entry back to
 * the check, and one image bound at several places, across the end of IMEM.
 */
#include "rsp.h"

#include "runner.h"

#include <relift/rsp/abi.h>

#include <string.h>

/** @brief The test's bus: SP registers it records, halted after BREAK. */
typedef struct bus_state {
    bool halted;
    uint32_t breaks;
    uint64_t break_cycle;
    uint32_t last_rd, last_value;
    uint32_t writes;
} bus_state;

static uint32_t t_mfc0(void *user, uint32_t rd, uint64_t cycle)
{
    (void)user;
    return rd * 0x100u + (uint32_t)cycle;
}

static void t_mtc0(void *user, uint32_t rd, uint32_t value, uint64_t cycle)
{
    (void)cycle;
    bus_state *b = user;
    b->last_rd = rd;
    b->last_value = value;
    b->writes++;
}

static void t_brk(void *user, uint64_t cycle)
{
    bus_state *b = user;
    b->halted = true;
    b->breaks++;
    b->break_cycle = cycle;
}

static bool t_halted(void *user) { return ((bus_state *)user)->halted; }

static const rl_rsp_bus g_bus = {t_mfc0, t_mtc0, t_brk, t_halted};

/*
 * The loop image: 0x0 addiu r1, r1, 1; 0x4 b 0x0 (slot at 0x8 addiu r2,
 * r2, 1); 0xC break. The loop runs until the budget ends.
 */
static const uint8_t g_loop_bytes[16] = {0x24, 0x21, 0x00, 0x01, 0x10, 0x00, 0xFF, 0xFE,
                                         0x24, 0x42, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0D};
static const uint16_t g_loop_succ[12] = {1, RL_RSP_NO_SUCC, 0, 0, RL_RSP_NO_SUCC, RL_RSP_SUCC_SLOT,
                                         RL_RSP_NO_SUCC, RL_RSP_NO_SUCC, 0, RL_RSP_NO_SUCC, RL_RSP_NO_SUCC, 0};

static uint32_t g_entries;

static int loop_fn(rl_ctx *c, uint8_t *mem)
{
    g_entries++;
    int rc;
    switch (c->pc) {
    case 0x4:
        goto L_4;
    case 0xC:
        goto L_c;
    default:
        break;
    }
    for (;;) {
        c->r[1]++;
        c->time.budget -= 1;
        if (c->time.budget <= 0 && (rc = rl_rsp_sync(c, mem, 0x4)) != RL_OK)
            return rc;
    L_4:
        c->r[2]++;
        c->time.budget -= 2;
        if (c->time.budget <= 0 && (rc = rl_rsp_sync(c, mem, 0x0)) != RL_OK)
            return rc;
    }
L_c:
    rl_rsp_break(c, 1);
    c->time.budget -= 1;
    if (c->time.budget <= 0 && (rc = rl_rsp_sync(c, mem, 0x10)) != RL_OK)
        return rc;
    return rl_indirect(c, 0x10)(c, mem);
}

static const rl_fn_entry g_loop_fns[1] = {{0x0, 16, RL_FN_RANGE, loop_fn}};

static const rl_unit g_loop_unit = {
    .abi_version = RL_ABI_VERSION,
    .ctx_size = sizeof(rl_ctx),
    .fn_count = 1,
    .fns = g_loop_fns,
    .name = "loop",
};

static const rl_rsp_image g_loop = {&g_loop_unit, 16, 0x1234, g_loop_bytes, g_loop_succ};

typedef struct fixture {
    uint8_t dmem[0x1000];
    uint8_t imem[0x1000];
    bus_state bus;
    rl_rsp *r;
} fixture;

static void setup(fixture *f, const rl_rsp_image *const *images, uint32_t n)
{
    memset(f, 0, sizeof *f);
    memcpy(f->imem + 0x100, g_loop_bytes, sizeof g_loop_bytes);
    f->r = rl_rsp_create(f->dmem, f->imem, images, n, &g_bus, &f->bus);
    g_entries = 0;
}

TEST(rsp_images_are_checked_against_the_runtime)
{
    const rl_rsp_image *imgs[1] = {&g_loop};
    char why[128];
    CHECK(rl_rsp_images_ok(imgs, 1, why, sizeof why));
    rl_unit bad = g_loop_unit;
    bad.ctx_size = 8;
    rl_rsp_image im = g_loop;
    im.unit = &bad;
    const rl_rsp_image *bads[1] = {&im};
    CHECK(!rl_rsp_images_ok(bads, 1, why, sizeof why));
    CHECK(strstr(why, "another runtime") != NULL);
    im = g_loop;
    im.len = 0x1004;
    bads[0] = &im;
    CHECK(!rl_rsp_images_ok(bads, 1, why, sizeof why));
}

TEST(rsp_a_slice_runs_its_cycles_and_binds_once)
{
    const rl_rsp_image *imgs[1] = {&g_loop};
    fixture f;
    setup(&f, imgs, 1);
    uint32_t pc = 0x100;
    uint64_t at = rl_rsp_run(f.r, &pc, 10, 40);
    CHECK_EQ(at, 40);
    CHECK_EQ(rl_rsp_gpr(f.r, 1), 10);
    CHECK_EQ(rl_rsp_gpr(f.r, 2), 10);
    CHECK_EQ(pc, 0x100);
    CHECK_EQ(rl_rsp_get_stats(f.r)->binds, 1);
    at = rl_rsp_run(f.r, &pc, at, at + 4);
    CHECK_EQ(at, 44);
    CHECK_EQ(pc, 0x104);
    CHECK_EQ(rl_rsp_get_stats(f.r)->binds, 1);
    CHECK_EQ(rl_rsp_get_stats(f.r)->cycles, 34);
    CHECK_EQ(rl_rsp_get_stats(f.r)->slices, 2);
    rl_rsp_destroy(f.r);
}

TEST(rsp_break_halts_at_the_blocks_sync_point)
{
    const rl_rsp_image *imgs[1] = {&g_loop};
    fixture f;
    setup(&f, imgs, 1);
    uint32_t pc = 0x10C;
    uint64_t at = rl_rsp_run(f.r, &pc, 0, 1000);
    CHECK(f.bus.halted);
    CHECK_EQ(f.bus.breaks, 1);
    CHECK_EQ(f.bus.break_cycle, 0);
    CHECK_EQ(at, 1);
    CHECK_EQ(pc, 0x110);
    rl_rsp_destroy(f.r);
}

TEST(rsp_imem_writes_send_the_next_entry_back_to_the_check)
{
    const rl_rsp_image *imgs[1] = {&g_loop};
    fixture f;
    setup(&f, imgs, 1);
    uint32_t pc = 0x100;
    uint64_t at = rl_rsp_run(f.r, &pc, 0, 6);
    CHECK_EQ(g_entries, 1);
    rl_rsp_imem_written(f.r, 0x800, 8);
    CHECK_EQ(rl_rsp_get_stats(f.r)->invalidations, 1);
    rl_rsp_run(f.r, &pc, at, at + 6);
    CHECK_EQ(rl_rsp_get_stats(f.r)->binds, 2);
    CHECK_EQ(g_entries, 2);
    rl_rsp_destroy(f.r);
}

/* The COP0 image: mtc0 r1, c4; mfc0 r3, c5; break. */
static const uint8_t g_cop0_bytes[12] = {0x40, 0x81, 0x20, 0x00, 0x40, 0x03, 0x28, 0x00, 0x00, 0x00, 0x00, 0x0D};
static const uint16_t g_cop0_succ[9] = {1, RL_RSP_NO_SUCC, 0, 2, RL_RSP_NO_SUCC, 0, RL_RSP_NO_SUCC, RL_RSP_NO_SUCC, 0};

static int cop0_fn(rl_ctx *c, uint8_t *mem)
{
    int rc;
    rl_rsp_mtc0(c, 4, c->r[1], 1);
    c->time.budget -= 1;
    if (c->time.budget <= 0 && (rc = rl_rsp_sync(c, mem, 0x4)) != RL_OK)
        return rc;
    c->r[3] = rl_rsp_mfc0(c, 5);
    c->time.budget -= 1;
    if (c->time.budget <= 0 && (rc = rl_rsp_sync(c, mem, 0x8)) != RL_OK)
        return rc;
    rl_rsp_break(c, 1);
    c->time.budget -= 1;
    if (c->time.budget <= 0 && (rc = rl_rsp_sync(c, mem, 0xC)) != RL_OK)
        return rc;
    return rl_indirect(c, 0xC)(c, mem);
}

static const rl_fn_entry g_cop0_fns[1] = {{0x0, 12, RL_FN_RANGE, cop0_fn}};

static const rl_unit g_cop0_unit = {
    .abi_version = RL_ABI_VERSION,
    .ctx_size = sizeof(rl_ctx),
    .fn_count = 1,
    .fns = g_cop0_fns,
    .name = "cop0",
};

static const rl_rsp_image g_cop0 = {&g_cop0_unit, 12, 0x5678, g_cop0_bytes, g_cop0_succ};

TEST(rsp_cop0_goes_to_the_bus_at_the_rsp_cycle)
{
    const rl_rsp_image *imgs[2] = {&g_loop, &g_cop0};
    fixture f;
    setup(&f, imgs, 2);
    memcpy(f.imem + 0x200, g_cop0_bytes, sizeof g_cop0_bytes);
    uint32_t pc = 0x200;
    uint64_t at = rl_rsp_run(f.r, &pc, 100, 1000);
    CHECK_EQ(f.bus.writes, 1);
    CHECK_EQ(f.bus.last_rd, 4);
    CHECK_EQ(rl_rsp_gpr(f.r, 3), 0x500u + 101u);
    CHECK(f.bus.halted);
    CHECK_EQ(f.bus.break_cycle, 102);
    CHECK_EQ(at, 103);
    rl_rsp_destroy(f.r);
}

TEST(rsp_one_image_runs_at_several_places_and_across_the_end_of_imem)
{
    const rl_rsp_image *imgs[1] = {&g_loop};
    fixture f;
    setup(&f, imgs, 1);
    memcpy(f.imem + 0x300, g_loop_bytes, sizeof g_loop_bytes);
    uint32_t pc = 0x300;
    uint64_t at = rl_rsp_run(f.r, &pc, 0, 3);
    CHECK_EQ(pc, 0x300);
    CHECK_EQ(rl_rsp_gpr(f.r, 1), 1);
    CHECK_EQ(rl_rsp_gpr(f.r, 2), 1);
    pc = 0x10C;
    at = rl_rsp_run(f.r, &pc, at, at + 100);
    CHECK(f.bus.halted);
    CHECK_EQ(pc, 0x110);
    CHECK_EQ(rl_rsp_get_stats(f.r)->binds, 2);
    f.bus.halted = false;
    memcpy(f.imem + 0xFF8, g_loop_bytes, 8);
    memcpy(f.imem, g_loop_bytes + 8, 8);
    pc = 0xFF8;
    at = rl_rsp_run(f.r, &pc, at, at + 1);
    CHECK_EQ(pc, 0xFFC);
    at = rl_rsp_run(f.r, &pc, at, at + 2);
    CHECK_EQ(pc, 0xFF8);
    CHECK_EQ(rl_rsp_gpr(f.r, 2), 2);
    CHECK_EQ(rl_rsp_get_stats(f.r)->binds, 3);
    rl_rsp_destroy(f.r);
}
