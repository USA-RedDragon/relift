/**
 * @file test_mods.c
 * @brief Bundled mods: their settings' defaults and values, the variant for a
 * combination, exclusive pairs and the data writes in mod order.
 */
#include "runner.h"

#include <relift/mods.h>
#include <relift/settings.h>

static uint64_t v0(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return 0; }
static uint64_t v1(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return 1; }
static uint64_t v2(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return 2; }
static uint64_t v3(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return 3; }

static rl_fn g_slot = v0;
static const uint16_t g_fn_mods[] = {0, 1};
static const rl_fn g_variants[] = {v0, v1, v2, v3};
static const rl_mod_fn g_fns[] = {{0x1000, &g_slot, 2, g_fn_mods, g_variants}};
static const uint8_t g_a[] = {1, 2};
static const uint8_t g_b[] = {3};
static const rl_mod_data g_da[] = {{0x2000, 2, g_a}};
static const rl_mod_data g_db[] = {{0x2001, 1, g_b}};
static const rl_mod g_list[] = {
    {"mod.a", 1, g_da},
    {"mod.b", 1, g_db},
    {"mod.c", 0, NULL},
};
static const rl_setting_def g_defs[] = {
    {.key = "mod.a", .type = RL_SETTING_BOOL},
    {.key = "mod.b", .type = RL_SETTING_BOOL, .def_int = 1},
    {.key = "mod.c", .type = RL_SETTING_BOOL},
};

static void declare(void)
{
    rl_settings_reset();
    rl_settings_declare(g_defs, 3);
}
static const uint16_t g_excl[] = {1, 2};
static const rl_mods g_mods = {3, g_list, 1, g_fns, 1, g_excl};

static uint8_t g_mem[4];

static void write_mem(void *user, uint64_t addr, const uint8_t *bytes, uint32_t size)
{
    (void)user;
    for (uint32_t i = 0; i < size; i++)
        g_mem[addr - 0x2000 + i] = bytes[i];
}

TEST(mods_defaults_pick_their_variant)
{
    bool on[3];
    declare();
    CHECK_EQ(rl_mods_choose(&g_mods, on), 0);
    CHECK(!on[0] && on[1] && !on[2]);
    CHECK(g_slot == v2);
}

TEST(mods_settings_choose_and_later_wins)
{
    bool on[3];
    char *argv[] = {"game", "--relift-set", "mod.a=true", "--relift-set", "mod.b=false", "--relift-set", "mod.b=true"};
    declare();
    rl_settings_args(7, argv);
    CHECK_EQ(rl_mods_choose(&g_mods, on), 0);
    CHECK(on[0] && on[1]);
    CHECK(g_slot == v3);
    g_mem[0] = g_mem[1] = g_mem[2] = 0;
    rl_mods_each_data(&g_mods, on, write_mem, NULL);
    CHECK_EQ(g_mem[0], 1);
    CHECK_EQ(g_mem[1], 3);
    char *off[] = {"game", "--relift-set", "mod.a=false", "--relift-set", "mod.b=false"};
    rl_settings_args(5, off);
    CHECK_EQ(rl_mods_choose(&g_mods, on), 0);
    CHECK(g_slot == v0);
}

TEST(mods_exclusive_pair_on_is_refused)
{
    char *argv[] = {"game", "--relift-set", "mod.c=true"};
    declare();
    rl_settings_args(3, argv);
    CHECK_EQ(rl_mods_choose(&g_mods, NULL), -1);
}
