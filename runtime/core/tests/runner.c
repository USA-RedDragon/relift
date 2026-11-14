/**
 * @file runner.c
 * @brief Test runner and the fault probes. Usage: <layer>_tests [substring...].
 * Set RELIFT_TEST_NOFORK=1 to run tests in one process (for a debugger).
 */
#include "runner.h"

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

uint8_t *t_mem(void) { return rl_mem_base(); }

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
