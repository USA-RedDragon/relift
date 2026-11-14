/**
 * @file test_loader.c
 * @brief rl_switch_main with a hand-written lifted unit: module mapping,
 * guest debug strings, exit codes and load errors. Each run happens in a
 * forked child that ends the way a shipped title does.
 */
#include "loader.h"
#include "t.h"

#include <limits.h>
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

static int fake_entry(rl_ctx *restrict c, uint8_t *mem)
{
    uint64_t x1 = c->x[1];
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

static const rl_fn_entry g_fns[] = { { BASE, 4, 0, fake_entry } };

static const rl_unit g_unit = {
    .abi_version = RL_ABI_VERSION,
    .ctx_size = sizeof(struct rl_ctx),
    .fn_count = 1,
    .fns = g_fns,
    .name = "loader-test",
};

const rl_unit *const rl_static_units[] = { &g_unit };
const uint32_t rl_static_unit_count = 1;

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
};

static void fill_image(void)
{
    memset(g_image, 0, sizeof g_image);
    memcpy(g_image + 0x1000, g_msg, sizeof g_msg - 1);
    memcpy(g_image + 0x1100, "PASS data", 9);
    memcpy(g_image + 0x1200, "PASS regs", 9);
    g_image[0x2000] = 0xAB;
}

static char g_sdmc[PATH_MAX];

/** @brief Runs rl_switch_main in a child; output (stdout and stderr) goes to out, the wait status to *status. */
static void run_program(const rl_switch_program *p, char *out, size_t n, int *status)
{
    int fds[2];
    pid_t pid;
    size_t used = 0;
    const char *home = getenv("HOME");
    snprintf(g_sdmc, sizeof g_sdmc, "%s/spare/relift-phase4/tmp/loader/sdmc/inner", home ? home : ".");
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
}
