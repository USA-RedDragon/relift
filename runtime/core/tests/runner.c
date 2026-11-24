/**
 * @file runner.c
 * @brief Test runner and the fault probes. Usage: <layer>_tests [substring...].
 * Set RELIFT_TEST_NOFORK=1 to run tests in one process (for a debugger).
 *
 * Each test runs in a child. On Linux, after the run, an NVIDIA Xid in the kernel log (a GPU fault the driver
 * reports and the test does not see) fails the test whose child it named or in whose time it fell (xid_check).
 * On Apple hosts the child is a new process
 * running only that test (RELIFT_TEST_ONE names it), not a fork: a forked
 * child cannot reach XPC services such as Metal's shader compiler.
 */
#include "runner.h"

#include "spirvval.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <spawn.h>
extern char **environ;
#endif

#define ONE_ENV "RELIFT_TEST_ONE"

/** @brief Sets RELIFT_SPIRV_VAL to spirv-val when it is unset and spirv-val is on the path, so tests check every module. */
static void find_spirv_val(void)
{
    const char *path = getenv("PATH");
    if (getenv(RL_SPIRV_VAL_ENV) || !path)
        return;
    while (*path) {
        size_t len = strcspn(path, ":");
        char exe[4096];
        if (len && len < sizeof exe - 16) {
            snprintf(exe, sizeof exe, "%.*s/spirv-val", (int)len, path);
            if (access(exe, X_OK) == 0) {
                setenv(RL_SPIRV_VAL_ENV, "spirv-val --target-env vulkan1.3 -", 0);
                return;
            }
        }
        path += len;
        if (*path)
            path++;
    }
}

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

/** @brief The wall clock in seconds since the epoch, as the kernel log's short-unix times. */
static double wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/**
 * @brief The NVIDIA driver's names for a graphics engine class and one of its methods (NVIDIA's open-gpu-doc class
 * headers, classes/3d/clc797.h for AMPERE_B), for the Xid failure line; "" when not known.
 */
static const char *xid_class_name(unsigned cls)
{
    switch (cls) {
    case 0xc797: return "AMPERE_B (Ampere 3D)";
    case 0xc7c0: return "AMPERE_COMPUTE_B";
    case 0xc997: return "ADA_A (Ada 3D)";
    case 0xc9c0: return "ADA_COMPUTE_A";
    default: return "";
    }
}

static const char *xid_method_name(unsigned cls, unsigned off)
{
    if ((cls & 0xff) != 0x97)
        return "";
    switch (off) {
    case 0x0270: return "DRAW_VERTEX_ARRAY_BEGIN_END_A (a draw launch)";
    case 0x0274: return "DRAW_VERTEX_ARRAY_BEGIN_END_B (a draw launch)";
    case 0x0278: return "DRAW_VERTEX_ELEMENTS (a draw launch)";
    case 0x1614: return "DRAW_BEGIN (a draw launch)";
    case 0x1618: return "DRAW_END";
    default: return "";
    }
}

int t_xid_attribute(const char *log, const char *comm, const t_span *spans, int n, char *why, size_t why_len,
                    const char **next)
{
    *next = NULL;
    for (const char *line = log; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char buf[1024];
        if (len >= sizeof buf)
            len = sizeof buf - 1;
        memcpy(buf, line, len);
        buf[len] = 0;
        line = end ? end + 1 : NULL;
        const char *x = strstr(buf, "NVRM: Xid");
        if (!x)
            continue;
        double at = strtod(buf, NULL);
        const char *p = strstr(x, "pid=");
        long pid = p ? strtol(p + 4, NULL, 10) : -1;
        const char *nm = strstr(x, "name=");
        bool ours = false;
        if (nm && comm) {
            size_t cl = strcspn(nm + 5, ", ");
            ours = cl && !strncmp(nm + 5, comm, cl);
        }
        int hit = -1;
        for (int i = 0; i < n && hit < 0; i++)
            if (spans[i].pid > 0 && spans[i].pid == pid)
                hit = i;
        for (int i = 0; i < n && hit < 0 && ours; i++)
            if (at >= spans[i].start && at <= spans[i].end + 2.0)
                hit = i;
        if (hit < 0)
            continue;
        unsigned xid = 0, cls = 0, off = 0, data = 0, code = 0;
        const char *k = strstr(x, "): ");
        if (k)
            xid = (unsigned)strtoul(k + 3, NULL, 10);
        if ((k = strstr(x, "Class ")) && (k = strstr(k + 6, "Class ")))
            cls = (unsigned)strtoul(k + 6, NULL, 16);
        if ((k = strstr(x, "Offset ")))
            off = (unsigned)strtoul(k + 7, NULL, 16);
        if ((k = strstr(x, "Data ")))
            data = (unsigned)strtoul(k + 5, NULL, 16);
        if ((k = strstr(x, "ErrorCode ")))
            code = (unsigned)strtoul(k + 10, NULL, 16);
        snprintf(why, why_len, "NVIDIA Xid %u (pid %ld): class %04x %s, method 0x%04x %s, data 0x%x, error code 0x%x", xid,
                 pid, cls, xid_class_name(cls), off, xid_method_name(cls, off), data, code);
        *next = line;
        return hit;
    }
    return -1;
}

#if defined(__linux__) && !defined(__ANDROID__)
/**
 * @brief After a run: the kernel log since start (journalctl -k, or RELIFT_TEST_KLOG's command, which gets the start
 * time as its argument) for NVIDIA Xid errors, each laid to the test whose child it named or in whose time it fell.
 * A test that faulted the GPU fails though it passed its checks: the driver reports the fault and the test does not
 * see it. RELIFT_TEST_XID=0 turns the check off. Returns how many passed tests now fail.
 */
static int xid_check(const t_span *spans, int n, bool *ok, double start)
{
    const char *off = getenv("RELIFT_TEST_XID");
    if ((off && *off == '0') || !n)
        return 0;
    char cmd[512];
    const char *custom = getenv("RELIFT_TEST_KLOG");
    if (custom && *custom)
        snprintf(cmd, sizeof cmd, "%s %.6f", custom, start);
    else
        snprintf(cmd, sizeof cmd, "journalctl -k -q --no-pager -o short-unix --since @%.6f 2>/dev/null", start);
    FILE *f = popen(cmd, "r");
    if (!f)
        return 0;
    size_t cap = 1 << 16, len = 0;
    char *log = malloc(cap);
    for (size_t r; log && (r = fread(log + len, 1, cap - len - 1, f)) > 0;) {
        len += r;
        if (len + 1 == cap) {
            char *g = realloc(log, cap *= 2);
            if (!g)
                break;
            log = g;
        }
    }
    pclose(f);
    if (!log)
        return 0;
    log[len] = 0;
    char comm[32] = "";
    FILE *c = fopen("/proc/self/comm", "r");
    if (c) {
        if (fgets(comm, sizeof comm, c))
            comm[strcspn(comm, "\n")] = 0;
        fclose(c);
    }
    int newly = 0;
    for (const char *at = log; at && *at;) {
        char why[512];
        const char *next;
        int i = t_xid_attribute(at, comm, spans, n, why, sizeof why, &next);
        if (i < 0)
            break;
        fprintf(stderr, "FAIL %s faulted the GPU: %s\n", g_tests[spans[i].test].name, why);
        newly += ok[i];
        ok[i] = false;
        at = next;
    }
    free(log);
    return newly;
}
#endif

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
    find_spirv_val();
    rl_config_load();
    if (only && *only) {
        for (int i = 0; i < g_count; i++)
            if (!strcmp(g_tests[i].name, only))
                return run_one(i);
        fprintf(stderr, "relift runtime tests: no test %s\n", only);
        return 1;
    }
    fprintf(stderr, "relift runtime tests: host page %u\n", rl_os_page_size());
    t_span *spans = calloc((size_t)g_count + 1, sizeof *spans);
    bool *oks = calloc((size_t)g_count + 1, sizeof *oks);
    double start = wall_s();
    for (int i = 0; i < g_count; i++) {
        if (!selected(g_tests[i].name, argc, argv))
            continue;
        if (spans) {
            spans[run].test = i;
            spans[run].start = wall_s();
        }
        run++;
        uint64_t t0 = rl_clock_ns();
        int ok;
        if (nofork) {
            g_failed = 0;
            g_tests[i].fn();
            ok = g_failed == 0;
        } else {
            pid_t pid = spawn_one(i);
            if (spans)
                spans[run - 1].pid = pid;
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
        if (spans) {
            spans[run - 1].end = wall_s();
            oks[run - 1] = ok;
        }
    }
#if defined(__linux__) && !defined(__ANDROID__)
    if (spans && oks && !nofork)
        failed += xid_check(spans, run, oks, start);
#endif
    (void)start;
    free(spans);
    free(oks);
    fprintf(stderr, "%d tests, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
