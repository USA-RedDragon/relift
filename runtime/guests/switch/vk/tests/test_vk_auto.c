/**
 * @file test_vk_auto.c
 * @brief The loader turns Vulkan forwarding on by itself: a module that
 * imports or defines vkGetInstanceProcAddr gets thunks without any
 * rl_switch_add_hook call, and a module without Vulkan symbols reserves
 * nothing for it. Each run happens in a forked child.
 */
#include "hos.h"
#include "loader.h"
#include "runner.h"
#include "vk.h"

#include <relift/window.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define BASE 0x8000000ull
#define GOT (BASE + 0x2800)
#define THUNKS (BASE + 0x3000)
#define PLAIN_VALUE 0x1234ull

static uint8_t g_image[0x3000] __attribute__((aligned(16)));
static uint64_t g_mode;

enum { IMPORT_GIPA, DEFINE_GIPA, NO_VULKAN };

static void say(rl_ctx *c, uint8_t *mem, const char *s)
{
    memcpy(mem + BASE + 0x2C00, s, strlen(s));
    c->x[0] = BASE + 0x2C00;
    c->x[1] = strlen(s);
    (void)rl_svc(c, mem, 0x27);
}

static int entry(rl_ctx *restrict c, uint8_t *mem)
{
    uint64_t got = rl_ld64(mem, GOT);
    bool thunk = got >= THUNKS && got < THUNKS + 0x100000;
    bool free_page = rl_hos_reserve_at(THUNKS, 0x1000) == KR_SUCCESS;
    if (g_mode == NO_VULKAN)
        say(c, mem, got == PLAIN_VALUE && free_page ? "RESULT untouched" : "RESULT touched");
    else if (g_mode == IMPORT_GIPA)
        say(c, mem, thunk && !free_page ? "RESULT thunk" : "RESULT no thunk");
    else
        say(c, mem, !free_page ? "RESULT reserved" : "RESULT not reserved");
    c->x[0] = c->x[1] = c->x[2] = 0;
    return rl_svc(c, mem, 0x07);
}

static const rl_fn_entry g_fns[] = { { BASE, 4, 0, entry } };

static const rl_unit g_unit = {
    .abi_version = RL_ABI_VERSION,
    .ctx_size = sizeof(struct rl_ctx),
    .fn_count = 1,
    .fns = g_fns,
    .name = "vk_auto",
};

static const rl_unit *const g_units[] = { &g_unit };

static const rl_switch_module g_module = {
    .name = "main",
    .base = BASE,
    .image = g_image,
    .image_size = sizeof g_image,
    .text_size = 0x1000,
    .ro_off = 0x1000,
    .ro_size = 0x1000,
    .data_off = 0x2000,
    .data_size = 0x1000,
};

static const rl_switch_program g_program = {
    .modules = &g_module,
    .module_count = 1,
    .entry = BASE,
    .address_space_type = 3,
    .main_prio = 44,
    .main_stack_size = 0x100000,
    .program_id = 0x0100000000017E58ull,
    .units = g_units,
    .unit_count = 1,
};

static void put32(uint32_t off, uint32_t v) { memcpy(g_image + off, &v, 4); }
static void put64(uint32_t off, uint64_t v) { memcpy(g_image + off, &v, 8); }

/** @brief MOD0, a dynamic section, one symbol (named, imported or defined) and a GLOB_DAT against it. */
static void build_image(const char *name, bool defined)
{
    char strtab[64] = { 0 };
    size_t n = strlen(name) + 2;
    memset(g_image, 0, sizeof g_image);
    memcpy(strtab + 1, name, n - 1);
    put32(4, 0x1000);
    memcpy(g_image + 0x1000, "MOD0", 4);
    put32(0x1004, 0x1000);
    put32(0x1100 + 24, 1);
    g_image[0x1100 + 24 + 4] = 0x12;
    if (defined) {
        g_image[0x1100 + 24 + 6] = 1;
        put64(0x1100 + 24 + 8, 0x100);
    }
    memcpy(g_image + 0x1200, strtab, n);
    put64(0x1300, GOT - BASE);
    put64(0x1308, (uint64_t)1 << 32 | 1025);
    put64(0x2800, PLAIN_VALUE);
    const uint64_t dyn[][2] = { { 5, 0x1200 }, { 6, 0x1100 }, { 10, n }, { 11, 24 }, { 7, 0x1300 },
                                { 8, 24 }, { 0, 0 } };
    for (unsigned i = 0; i < sizeof dyn / sizeof dyn[0]; i++) {
        put64(0x2000 + 16 * i, dyn[i][0]);
        put64(0x2008 + 16 * i, dyn[i][1]);
    }
}

static void run_child(char *out, size_t n)
{
    char dir[PATH_MAX];
    int fds[2];
    size_t used = 0;
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/relift-vk-auto-%d", tmp && *tmp ? tmp : ".", (int)getpid());
    setenv("RELIFT_SDMC", dir, 1);
    setenv(RL_WINDOW_ENV, "headless", 1);
    CHECK_EQ(pipe(fds), 0);
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        _exit(rl_switch_main(&g_program, 0, NULL));
    }
    close(fds[1]);
    for (;;) {
        ssize_t k = read(fds[0], out + used, n - 1 - used);
        if (k <= 0)
            break;
        used += (size_t)k;
    }
    out[used] = 0;
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
}

TEST(vk_auto_import_of_vkGetInstanceProcAddr_gets_thunks)
{
    char out[8192];
    build_image("vkGetInstanceProcAddr", false);
    g_mode = IMPORT_GIPA;
    run_child(out, sizeof out);
    CHECK(strstr(out, "RESULT thunk") != NULL);
    CHECK(strstr(out, "forwarding is on") != NULL);
}

TEST(vk_auto_definition_of_vkGetDeviceProcAddr_reserves_thunks)
{
    char out[8192];
    build_image("vkGetDeviceProcAddr", true);
    g_mode = DEFINE_GIPA;
    run_child(out, sizeof out);
    CHECK(strstr(out, "RESULT reserved") != NULL);
}

TEST(vk_auto_program_without_vulkan_symbols_reserves_nothing)
{
    char out[8192];
    build_image("rl_not_vulkan", false);
    g_mode = NO_VULKAN;
    run_child(out, sizeof out);
    CHECK(strstr(out, "RESULT untouched") != NULL);
    CHECK(strstr(out, "forwarding") == NULL);
}
