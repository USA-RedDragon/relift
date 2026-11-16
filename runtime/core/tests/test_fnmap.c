/**
 * @file test_fnmap.c
 * @brief Function maps: registration checks, guest and host lookups, the
 * lifted-code section bound, the nearest-start fallback without a section,
 * and the report text for a synthetic crash.
 */
#include "crash.h"
#include "runner.h"

static uint64_t fa(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return 0; }
static uint64_t fb(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return 1; }

static rl_fn_info g_info[2];
static rl_fn_map g_m;

static void setup(const void *lo, const void *hi)
{
    rl_fn_maps_reset();
    g_info[0] = (rl_fn_info){ 0x1000, 0x1000, 0x20, 0, fa, "_ZN5guest1m1aEv", "guest::m::a()" };
    g_info[1] = (rl_fn_info){ 0x1020, 0x1020, 0x40, 0, fb, "_ZN5guest1m1bEv", NULL };
    g_m = (rl_fn_map){ RL_FN_MAP_VERSION, 2, g_info, "m", "m-0", 0x1000, lo, hi };
}

TEST(fnmap_guest_lookup)
{
    setup(NULL, NULL);
    const rl_fn_map *l[] = { &g_m };
    CHECK_EQ(rl_fn_maps_register(l, 1), 0);
    rl_fn_hit h;
    CHECK(rl_fn_find_guest(0x1024, &h));
    CHECK(h.fn == &g_info[1]);
    CHECK_EQ(h.off, 4);
    CHECK(rl_fn_find_guest(0x1000, &h));
    CHECK(h.fn == &g_info[0]);
    CHECK(!rl_fn_find_guest(0x0FFC, &h));
    CHECK(!rl_fn_find_guest(0x1060, &h));
}

TEST(fnmap_refuses_bad_maps)
{
    setup(NULL, NULL);
    const rl_fn_map *l[] = { &g_m };
    g_m.version = 99;
    CHECK_EQ(rl_fn_maps_register(l, 1), -1);
    g_m.version = RL_FN_MAP_VERSION;
    g_info[1].guest_addr = 0x1010;
    CHECK_EQ(rl_fn_maps_register(l, 1), -1);
    rl_fn_hit h;
    CHECK(!rl_fn_find_guest(0x1000, &h));
}

TEST(fnmap_host_lookup_with_a_section)
{
    uintptr_t a = (uintptr_t)fa, b = (uintptr_t)fb;
    uintptr_t lo = a < b ? a : b, hi = (a < b ? b : a) + 16;
    setup((const void *)lo, (const void *)hi);
    const rl_fn_map *l[] = { &g_m };
    CHECK_EQ(rl_fn_maps_register(l, 1), 0);
    rl_fn_hit h;
    CHECK(rl_fn_find_host(b + 2, &h));
    CHECK(h.fn == &g_info[1]);
    CHECK_EQ(h.off, 2);
    CHECK(h.exact);
    CHECK(!rl_fn_find_host(hi + 1, &h));
    CHECK(!rl_fn_find_host(lo - 1, &h));
}

TEST(fnmap_host_lookup_without_a_section_is_nearest)
{
    setup(NULL, NULL);
    const rl_fn_map *l[] = { &g_m };
    CHECK_EQ(rl_fn_maps_register(l, 1), 0);
    rl_fn_hit h;
    CHECK(rl_fn_find_host((uintptr_t)fa + 1, &h));
    CHECK(h.fn == &g_info[0]);
    CHECK(!h.exact);
    uintptr_t top = (uintptr_t)fa > (uintptr_t)fb ? (uintptr_t)fa : (uintptr_t)fb;
    CHECK(!rl_fn_find_host(top + 0x20000, &h));
}

TEST(fnmap_report_text)
{
    setup(NULL, NULL);
    const rl_fn_map *l[] = { &g_m };
    CHECK_EQ(rl_fn_maps_register(l, 1), 0);
    static uint8_t space[64];
    rl_crash_set_space(space, (uintptr_t)space, sizeof space);
    rl_crash_info ci;
    memset(&ci, 0, sizeof ci);
    ci.kind = "SIGSEGV";
    ci.has_addr = true;
    ci.addr = (uintptr_t)space + 0x10;
    ci.pc = (uintptr_t)fb + 3;
    ci.frames[0] = ci.pc;
    ci.frames[1] = (uintptr_t)fa + 5;
    ci.nframes = 2;
    char buf[4096];
    rl_crash_format(&ci, buf, sizeof buf);
    CHECK(strstr(buf, "SIGSEGV at host") != NULL);
    CHECK(strstr(buf, "= guest address 0x10,") != NULL);
    CHECK(strstr(buf, "in guest function guest 0x1020 m+0x20 _ZN5guest1m1bEv (nearest), host _ZN5guest1m1bEv+0x3") != NULL);
    CHECK(strstr(buf, "#1 guest 0x1000 m+0x0 guest::m::a() (nearest)") != NULL);
    CHECK(strstr(buf, "_ZN5guest1m1aEv+0x5 (guest::m::a())") != NULL);
}

static uint64_t pa(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return 0; }
static uint64_t pb(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return 1; }
static uint64_t pc_(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return 2; }

TEST(fnmap_pieces_resolve_to_their_function_in_any_host_order)
{
    rl_fn_maps_reset();
    rl_fn_info info[4];
    info[0] = (rl_fn_info){ 0x2000, 0x2000, 0, 0, pa, "F_stub", "guest::f()" };
    info[1] = (rl_fn_info){ 0x2000, 0x2000, 0x10, 0, pa, "F_part1", "guest::f()" };
    info[2] = (rl_fn_info){ 0x2010, 0x2000, 0x20, 0, pb, "F_part2", "guest::f()" };
    info[3] = (rl_fn_info){ 0x2030, 0x2000, 0x10, 0, pc_, "F_part3", "guest::f()" };
    rl_fn_map m = { RL_FN_MAP_VERSION, 4, info, "m", "m-0", 0x2000, NULL, NULL };
    const rl_fn_map *l[] = { &m };
    CHECK_EQ(rl_fn_maps_register(l, 1), 0);
    rl_fn_hit h;
    CHECK(rl_fn_find_guest(0x2000, &h));
    CHECK(h.fn == &info[1]);
    CHECK(rl_fn_find_guest(0x2034, &h));
    CHECK(h.fn == &info[3]);
    CHECK_EQ(h.fn->fn_addr, 0x2000);
    CHECK(!rl_fn_find_guest(0x2040, &h));
    uintptr_t b = (uintptr_t)pb;
    CHECK(rl_fn_find_host(b + 1, &h));
    CHECK(h.fn == &info[2]);
    CHECK_EQ(h.fn->fn_addr, 0x2000);
}
