/**
 * @file test_loader.c
 * @brief rl_switch_main with a hand-written lifted unit: module mapping,
 * guest debug strings, exit codes and load errors. Each run happens in a
 * forked child that ends the way a shipped title does.
 */
#include "loader.h"
#include "t.h"

#include <limits.h>
#include <ftw.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BASE 0x8000000ull

/** @brief Image of the fake module: text, rodata and data, 0x1000 each. */
static uint8_t g_image[0x3000] __attribute__((aligned(16)));
static const char g_msg[] = "PASS foo";

static uint64_t g_mode;

static void say(rl_ctx *c, uint8_t *mem, uint64_t addr, uint64_t len)
{
    c->x[0] = addr;
    c->x[1] = len;
    (void)rl_svc(c, mem, 0x27);
}

#define IMPORTED 0x9000000ull

static uint64_t g_code_free;

static uint64_t lifted_export(rl_ctx *restrict c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 1;
    return RL_OK;
}

static uint64_t host_export(rl_ctx *restrict c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 2;
    return RL_OK;
}

static uint64_t linked_entry(rl_ctx *restrict c, uint8_t *mem)
{
    if (rl_ld64(mem, BASE + 0x2800) == IMPORTED && rl_ld64(mem, BASE + 0x2808) == IMPORTED + 0x10 &&
        rl_ld64(mem, BASE + 0x2810) == 0)
        say(c, mem, BASE + 0x1F00, 9);
    if (rl_indirect(c, BASE + 0x100)(c, mem) == RL_OK && c->x[0] == 2 &&
        rl_indirect(c, BASE + 0x200)(c, mem) == RL_OK && c->x[0] == 1)
        say(c, mem, BASE + 0x1F10, 9);
    if (g_code_free == BASE + 0x4000)
        say(c, mem, BASE + 0x1F20, 9);
    c->x[0] = 0;
    return rl_svc(c, mem, 0x07);
}

/** @brief Mode 3: another module defines the import, so the slots keep their link-time zero. */
static uint64_t left_entry(rl_ctx *restrict c, uint8_t *mem)
{
    if (rl_ld64(mem, BASE + 0x2800) == 0 && rl_ld64(mem, BASE + 0x2808) == 0)
        say(c, mem, BASE + 0x1F00, 9);
    c->x[0] = 0;
    return rl_svc(c, mem, 0x07);
}

static uint64_t fake_entry(rl_ctx *restrict c, uint8_t *mem)
{
    uint64_t x1 = c->x[1];
    if (g_mode == 2)
        return linked_entry(c, mem);
    if (g_mode == 3)
        return left_entry(c, mem);
    if (g_mode == 0) {
        say(c, mem, BASE + 0x1000, sizeof g_msg - 1);
        if (mem[BASE + 0x2000] == 0xAB && mem[BASE + 0x2FFF] == 0x00 && mem[BASE + 0x3000] == 0 &&
            mem[BASE + 0x3FFF] == 0)
            say(c, mem, BASE + 0x1100, 9);
        mem[BASE + 0x3000] = 7;
        if (c->tpidrro_el0 != 0 && c->sp != 0 && (c->sp & 15) == 0 && x1 != 0)
            say(c, mem, BASE + 0x1200, 9);
        c->x[0] = 0;
        return rl_svc(c, mem, 0x07);
    }
    c->x[0] = 0;
    c->x[1] = 0;
    c->x[2] = 0;
    return rl_svc(c, mem, 0x26);
}

static const rl_fn_entry g_fns[] = { { BASE, 4, 0, fake_entry },
                                     { BASE + 0x100, 4, 0, lifted_export },
                                     { BASE + 0x200, 4, 0, lifted_export } };

static const rl_unit g_unit = {
    .abi_version = RL_ABI_VERSION,
    .ctx_size = sizeof(struct rl_ctx),
    .fn_count = 3,
    .fns = g_fns,
    .name = "loader-test",
};

static const rl_unit *const g_units[] = { &g_unit };

static rl_switch_module g_module = {
    .name = "main",
    .base = BASE,
    .image = g_image,
    .image_size = sizeof g_image,
    .text_off = 0,
    .text_size = 0x1000,
    .ro_off = 0x1000,
    .ro_size = 0x1000,
    .data_off = 0x2000,
    .data_size = 0x1000,
    .bss_size = 0x1000,
};

static rl_switch_program g_program = {
    .modules = &g_module,
    .module_count = 1,
    .entry = BASE,
    .address_space_type = 3,
    .main_prio = 44,
    .main_core = 0,
    .main_stack_size = 0x100000,
    .program_id = 0x0100000000012345ull,
    .units = g_units,
    .unit_count = 1,
};

static void fill_image(void)
{
    memset(g_image, 0, sizeof g_image);
    memcpy(g_image + 0x1000, g_msg, sizeof g_msg - 1);
    memcpy(g_image + 0x1100, "PASS data", 9);
    memcpy(g_image + 0x1200, "PASS regs", 9);
    g_image[0x2000] = 0xAB;
}

static int hook_start(void *user, uint64_t *code_free)
{
    (void)user;
    g_code_free = *code_free;
    if (rl_hos_reserve_at(*code_free, 0x1000) != KR_SUCCESS)
        return -1;
    *code_free += 0x1000;
    return 0;
}

static uint64_t hook_import(void *user, const char *symbol)
{
    (void)user;
    return strcmp(symbol, "rl_test_import") ? 0 : IMPORTED;
}

static rl_fn hook_replace(void *user, const char *symbol)
{
    (void)user;
    return strcmp(symbol, "rl_test_export") ? NULL : host_export;
}

static const rl_switch_hook g_hook = {
    .name = "loader-test",
    .start = hook_start,
    .import = hook_import,
    .replace = hook_replace,
};

static void put32(uint32_t off, uint32_t v) { memcpy(g_image + off, &v, 4); }
static void put64(uint32_t off, uint64_t v) { memcpy(g_image + off, &v, 8); }

static void put_sym(uint32_t i, uint32_t name, uint16_t shndx, uint64_t value)
{
    uint32_t at = 0x1100 + 24 * i;
    put32(at, name);
    g_image[at + 4] = (uint8_t)(1u << 4 | (shndx ? 2u : 0u));
    memcpy(g_image + at + 6, &shndx, 2);
    put64(at + 8, value);
}

static void put_rela(uint32_t i, uint64_t off, uint32_t sym, uint32_t type, uint64_t addend)
{
    uint32_t at = 0x1300 + 24 * i;
    put64(at, off);
    put64(at + 8, (uint64_t)sym << 32 | type);
    put64(at + 16, addend);
}

/** @brief A module with MOD0, a dynamic section, four symbols and three relocations. */
static void fill_linked_image(void)
{
    static const char strtab[] = "\0rl_test_import\0rl_test_export\0other_export\0";
    memset(g_image, 0, sizeof g_image);
    put32(4, 0x1000);
    memcpy(g_image + 0x1000, "MOD0", 4);
    put32(0x1004, 0x1000);
    put_sym(1, 1, 0, 0);
    put_sym(2, 16, 1, 0x100);
    put_sym(3, 31, 1, 0x200);
    memcpy(g_image + 0x1200, strtab, sizeof strtab);
    put_rela(0, 0x2800, 1, 1025, 0);
    put_rela(1, 0x2808, 1, 257, 0x10);
    put_rela(2, 0x2810, 3, 1026, 0);
    put32(0x1400, 1);
    put32(0x1404, 4);
    const uint64_t dyn[][2] = { { 4, 0x1400 }, { 5, 0x1200 }, { 6, 0x1100 }, { 10, sizeof strtab }, { 7, 0x1300 },
                                { 8, 72 }, { 0, 0 } };
    for (unsigned i = 0; i < sizeof dyn / sizeof dyn[0]; i++) {
        put64(0x2000 + 16 * i, dyn[i][0]);
        put64(0x2008 + 16 * i, dyn[i][1]);
    }
    memcpy(g_image + 0x1F00, "PASS link", 9);
    memcpy(g_image + 0x1F10, "PASS repl", 9);
    memcpy(g_image + 0x1F20, "PASS hook", 9);
}

static char g_base[PATH_MAX];
static char g_sdmc[PATH_MAX];

static int rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static void remove_base(void)
{
    if (g_base[0])
        nftw(g_base, rm_cb, 8, FTW_DEPTH | FTW_PHYS);
    g_base[0] = 0;
    g_sdmc[0] = 0;
}

/** @brief Runs rl_switch_main in a child; output (stdout and stderr) goes to out, the wait status to *status. */
static void run_program(const rl_switch_program *p, char *out, size_t n, int *status)
{
    int fds[2];
    pid_t pid;
    size_t used = 0;
    if (!g_sdmc[0]) {
        const char *base = getenv("TMPDIR");
        if (!base || !*base)
            base = "/tmp";
        snprintf(g_base, sizeof g_base, "%s/relift-loader-XXXXXX", base);
        if (!mkdtemp(g_base))
            _exit(2);
        snprintf(g_sdmc, sizeof g_sdmc, "%s/sdmc/inner", g_base);
    }
    setenv("RELIFT_SDMC", g_sdmc, 1);
    if (pipe(fds) != 0)
        _exit(2);
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        _exit(rl_switch_main(p, 0, NULL));
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
    waitpid(pid, status, 0);
}

static int count_lines(const char *out, const char *text)
{
    int n = 0;
    size_t len = strlen(text);
    for (const char *p = out; (p = strstr(p, text)) != NULL; p += len)
        if (p[len] == '\n' && (p == out || p[-1] == '\n' || p[-1] == ' '))
            n++;
    return n;
}

TEST(loader_runs_a_module_and_prints_guest_strings)
{
    char out[8192];
    int status = 0;
    struct stat st;
    fill_image();
    g_mode = 0;
    run_program(&g_program, out, sizeof out, &status);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
    CHECK_EQ(count_lines(out, "PASS foo"), 1);
    CHECK_EQ(count_lines(out, "PASS data"), 1);
    CHECK_EQ(count_lines(out, "PASS regs"), 1);
    CHECK_EQ(stat(g_sdmc, &st), 0);
    CHECK(S_ISDIR(st.st_mode));
    remove_base();
}

TEST(loader_returns_the_guest_fatal_status)
{
    char out[8192];
    int status = 0;
    fill_image();
    g_mode = 1;
    run_program(&g_program, out, sizeof out, &status);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), RL_EXIT_SOFTWARE);
    remove_base();
}

TEST(loader_refuses_bad_programs)
{
    char out[8192];
    int status = 0;
    rl_switch_module m = g_module;
    rl_switch_program p = g_program;
    fill_image();
    g_mode = 0;

    p.entry = BASE + 0x1000;
    run_program(&p, out, sizeof out, &status);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 1);
    CHECK(strstr(out, "outside the text") != NULL);

    p = g_program;
    m.ro_off = 0x800;
    p.modules = &m;
    run_program(&p, out, sizeof out, &status);
    CHECK_EQ(WEXITSTATUS(status), 1);
    CHECK(strstr(out, "aligned") != NULL);

    m = g_module;
    m.image_size = 0x2800;
    run_program(&p, out, sizeof out, &status);
    CHECK_EQ(WEXITSTATUS(status), 1);
    CHECK(strstr(out, "outside its image") != NULL);

    m = g_module;
    m.base = 0x90000000ull;
    p.entry = 0x90000000ull;
    p.address_space_type = 1;
    run_program(&p, out, sizeof out, &status);
    CHECK_EQ(WEXITSTATUS(status), 1);
    CHECK(strstr(out, "code region") != NULL);

    p = g_program;
    p.module_count = 0;
    run_program(&p, out, sizeof out, &status);
    CHECK_EQ(WEXITSTATUS(status), 1);
    remove_base();
}

/** @brief A second module whose image is the linked one with the import defined. */
static uint8_t g_image2[0x3000] __attribute__((aligned(16)));

TEST(loader_leaves_imports_another_module_defines)
{
    char out[8192];
    int status = 0;
    static bool added;
    if (!added) {
        CHECK_EQ(rl_switch_add_hook(&g_hook), 0);
        added = true;
    }
    fill_linked_image();
    memcpy(g_image2, g_image, sizeof g_image2);
    {
        uint16_t shndx = 1;
        uint64_t value = 0x300;
        memcpy(g_image2 + 0x1100 + 24 + 6, &shndx, 2);
        memcpy(g_image2 + 0x1100 + 24 + 8, &value, 8);
    }
    rl_switch_module mods[2] = { g_module, g_module };
    mods[1].name = "lib";
    mods[1].base = BASE + 0x10000;
    mods[1].image = g_image2;
    rl_switch_program p = g_program;
    p.modules = mods;
    p.module_count = 2;
    g_mode = 3;
    run_program(&p, out, sizeof out, &status);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
    CHECK_EQ(count_lines(out, "PASS link"), 1);
    CHECK(strstr(out, "module main: hooks replaced functions: 1, resolved imports: 0") != NULL);
    g_mode = 0;
    remove_base();
}

TEST(loader_links_hook_symbols)
{
    char out[8192];
    int status = 0;
    static bool added;
    if (!added) {
        CHECK_EQ(rl_switch_add_hook(&g_hook), 0);
        added = true;
    }
    fill_linked_image();
    g_mode = 2;
    run_program(&g_program, out, sizeof out, &status);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
    CHECK_EQ(count_lines(out, "PASS link"), 1);
    CHECK_EQ(count_lines(out, "PASS repl"), 1);
    CHECK_EQ(count_lines(out, "PASS hook"), 1);
    CHECK(strstr(out, "replaced functions: 1, resolved imports: 2") != NULL);
    remove_base();
    fill_image();
    g_mode = 0;
    run_program(&g_program, out, sizeof out, &status);
    CHECK_EQ(WEXITSTATUS(status), 0);
    CHECK_EQ(count_lines(out, "PASS foo"), 1);
    remove_base();
}
