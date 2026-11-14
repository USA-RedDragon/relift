/**
 * @file main.c
 * @brief Test runner and shared test helpers. Usage: rt_tests [substring...].
 * Set RELIFT_TEST_NOFORK=1 to run tests in one process (for a debugger).
 */
#include "t.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/** @brief Most tests. */
#define MAX_TESTS 256

static struct {
    const char *name;
    t_fn *fn;
} g_tests[MAX_TESTS];
static int g_count;
static int g_failed;
int t_svc_rc;

void t_register(const char *name, t_fn *fn)
{
    if (g_count < MAX_TESTS) {
        g_tests[g_count].name = name;
        g_tests[g_count].fn = fn;
        g_count++;
    }
}

void t_fail(const char *file, int line, const char *expr, unsigned long long a, unsigned long long b, int has_values)
{
    g_failed++;
    if (has_values)
        fprintf(stderr, "  %s:%d: CHECK(%s) failed: 0x%llx != 0x%llx\n", file, line, expr, a, b);
    else
        fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", file, line, expr);
}

rl_ctx *t_boot(const rl_hos_config *cfg)
{
    if (rl_hos_init(cfg) != 0) {
        fprintf(stderr, "  rl_hos_init failed\n");
        exit(2);
    }
    rl_ctx *c = rl_hos_attach_thread(44, 0);
    if (!c) {
        fprintf(stderr, "  attach failed\n");
        exit(2);
    }
    return c;
}

uint8_t *t_mem(void) { return hos_mem(); }

uint32_t t_svc(rl_ctx *c, uint32_t imm, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    c->x[4] = a4;
    c->x[5] = a5;
    t_svc_rc = rl_svc(c, t_mem(), imm);
    return (uint32_t)c->x[0];
}

uint64_t t_heap(rl_ctx *c, uint64_t size)
{
    if (t_svc(c, SVC_SET_HEAP_SIZE, 0, size, 0, 0, 0, 0) != 0)
        return 0;
    return c->x[1];
}

void t_guest_fns(const uint64_t *addrs, rl_fn const *fns, uint32_t n)
{
    rl_fn_entry *e = calloc(n, sizeof *e);
    rl_unit *u = calloc(1, sizeof *u);
    const rl_unit **list = calloc(1, sizeof *list);
    for (uint32_t i = 0; i < n; i++) {
        e[i].guest_addr = addrs[i];
        e[i].guest_size = 4;
        e[i].fn = fns[i];
    }
    u->abi_version = RL_ABI_VERSION;
    u->ctx_size = sizeof(rl_ctx);
    u->fn_count = n;
    u->fns = e;
    u->name = "tests";
    list[0] = u;
    rl_units_register((const rl_unit *const *)list, 1);
    if (rl_thread_current())
        rl_dispatch_view(rl_thread_current()->ctx);
}

uint64_t t_free_map_addr(void)
{
    const hos_layout *l = &hos.lay;
    uint64_t ends[4] = { l->heap_end, l->alias_end, l->stack_end, l->tlsio_end };
    uint64_t a = l->code_end;
    for (int i = 0; i < 4; i++)
        if (ends[i] > a)
            a = ends[i];
    return a + 0x100000;
}

t_probe t_read(uint64_t a)
{
    t_probe p = { false, 0 };
    rl_fault_catch fc;
    rl_fault_catch *prev = rl_fault_arm(&fc);
    if (sigsetjmp(fc.jb, 1) == 0) {
        volatile uint8_t *m = t_mem();
        (void)m[a];
    } else {
        p.faulted = true;
        p.addr = fc.fault.guest_addr;
    }
    rl_fault_disarm(prev);
    return p;
}

t_probe t_write(uint64_t a, uint8_t v)
{
    t_probe p = { false, 0 };
    rl_fault_catch fc;
    rl_fault_catch *prev = rl_fault_arm(&fc);
    if (sigsetjmp(fc.jb, 1) == 0) {
        volatile uint8_t *m = t_mem();
        m[a] = v;
    } else {
        p.faulted = true;
        p.addr = fc.fault.guest_addr;
    }
    rl_fault_disarm(prev);
    return p;
}

static bool selected(const char *name, int argc, char **argv)
{
    if (argc < 2)
        return true;
    for (int i = 1; i < argc; i++)
        if (strstr(name, argv[i]))
            return true;
    return false;
}

int main(int argc, char **argv)
{
    int run = 0, failed = 0;
    bool nofork = getenv("RELIFT_TEST_NOFORK") != NULL;
    rl_config_load();
    fprintf(stderr, "relift runtime tests: host page %u\n", rl_os_page_size());
    for (int i = 0; i < g_count; i++) {
        if (!selected(g_tests[i].name, argc, argv))
            continue;
        run++;
        uint64_t t0 = rl_clock_ns();
        int ok;
        if (nofork) {
            g_failed = 0;
            g_tests[i].fn();
            ok = g_failed == 0;
        } else {
            fflush(stdout);
            fflush(stderr);
            pid_t pid = fork();
            if (pid == 0) {
                alarm(60);
                g_failed = 0;
                g_tests[i].fn();
                fflush(stdout);
                fflush(stderr);
                _exit(g_failed ? 1 : 0);
            }
            int st = 0;
            waitpid(pid, &st, 0);
            ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
            if (WIFSIGNALED(st))
                fprintf(stderr, "  killed by signal %d\n", WTERMSIG(st));
        }
        fprintf(stderr, "%s %s (%llu ms)\n", ok ? "ok  " : "FAIL", g_tests[i].name,
                (unsigned long long)((rl_clock_ns() - t0) / 1000000));
        failed += !ok;
    }
    fprintf(stderr, "%d tests, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
