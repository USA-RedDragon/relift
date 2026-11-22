/**
 * @file runner.c
 * @brief Test runner and the fault probes. Usage: <layer>_tests [substring...].
 * Set RELIFT_TEST_NOFORK=1 to run tests in one process (for a debugger).
 *
 * Each test runs in a child. On Apple hosts the child is a new process
 * running only that test (RELIFT_TEST_ONE names it), not a fork: a forked
 * child cannot reach XPC services such as Metal's shader compiler.
 */
#include "runner.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <spawn.h>
extern char **environ;
#endif

#define ONE_ENV "RELIFT_TEST_ONE"

static struct test {
    const char *name;
    t_fn *fn;
} *g_tests;
static int g_count, g_cap;
static int g_failed;

void t_register(const char *name, t_fn *fn)
{
    if (g_count == g_cap) {
        int cap = g_cap ? 2 * g_cap : 256;
        struct test *t = realloc(g_tests, (size_t)cap * sizeof *t);
        if (!t) {
            fprintf(stderr, "cannot register test %s\n", name);
            exit(1);
        }
        g_tests = t;
        g_cap = cap;
    }
    g_tests[g_count].name = name;
    g_tests[g_count].fn = fn;
    g_count++;
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
    if (RL_FAULT_SETJMP(fc.jb) == 0) {
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
    if (RL_FAULT_SETJMP(fc.jb) == 0) {
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

void (*t_timed_out)(void);

static void on_alarm(int sig)
{
    static const char msg[] = "  the test ran past 60 s\n";
    (void)!write(2, msg, sizeof msg - 1);
    if (t_timed_out)
        t_timed_out();
    signal(sig, SIG_DFL);
    raise(sig);
}

static int run_one(int i)
{
    signal(SIGALRM, on_alarm);
    alarm(60);
    g_failed = 0;
    g_tests[i].fn();
    fflush(stdout);
    fflush(stderr);
    return g_failed ? 1 : 0;
}

#if defined(__APPLE__)
static pid_t spawn_one(int i)
{
    char self[4096];
    uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size) != 0)
        return -1;
    size_t n = 0;
    while (environ[n])
        n++;
    char **env = calloc(n + 2, sizeof *env);
    char one[512];
    if (!env)
        return -1;
    snprintf(one, sizeof one, "%s=%s", ONE_ENV, g_tests[i].name);
    size_t m = 0;
    for (size_t j = 0; j < n; j++)
        if (strncmp(environ[j], ONE_ENV "=", sizeof ONE_ENV))
            env[m++] = environ[j];
    env[m++] = one;
    env[m] = NULL;
    char *argv[] = { self, NULL };
    pid_t pid;
    int e = posix_spawn(&pid, self, NULL, NULL, argv, env);
    free(env);
    return e ? -1 : pid;
}
#else
static pid_t spawn_one(int i)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0)
        _exit(run_one(i));
    return pid;
}
#endif

int main(int argc, char **argv)
{
    int run = 0, failed = 0;
    bool nofork = getenv("RELIFT_TEST_NOFORK") != NULL;
    const char *only = getenv(ONE_ENV);
    rl_config_load();
    if (only && *only) {
        for (int i = 0; i < g_count; i++)
            if (!strcmp(g_tests[i].name, only))
                return run_one(i);
        fprintf(stderr, "relift runtime tests: no test %s\n", only);
        return 1;
    }
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
            pid_t pid = spawn_one(i);
            int st = 0;
            if (pid < 0)
                fprintf(stderr, "  cannot start the test's child\n");
            else
                waitpid(pid, &st, 0);
            ok = pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
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
