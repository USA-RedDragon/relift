/**
 * @file crash_test.c
 * @brief The crash handler end to end: hand-written functions with the
 * lifted signature, in the lifted-code section and in a function map, make
 * a wild guest access (or run an illegal instruction, or crash in host code
 * they call). The test runs itself as a child for each case, reads the
 * child's report from a pipe and checks the guest address, the guest
 * function, the guest backtrace and the host backtrace.
 *
 * Links only the crash handler (crash.c and its host-OS part), so it builds
 * and runs on Windows, where the rest of the core is not ported yet.
 */
#include "crash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

RL_TEXT_BOUNDS_DECLARE();
RL_TEXT_BOUNDS_DEFINE()

enum {
    MODULE_BASE = 0x7100000000ull,
    G_OUTER = MODULE_BASE + 0x1000,
    G_INNER = MODULE_BASE + 0x1100,
    G_WILD = MODULE_BASE + 0x1200,
    G_ILL = MODULE_BASE + 0x1300,
    G_CALLS_HOST = MODULE_BASE + 0x1400,
    WILD_ADDR = 0x40001230ull,
    SPACE = 1ull << 31,
};

static int g_mode;
static volatile int g_sink;

__attribute__((noinline)) static void host_null_store(void)
{
    volatile int *p = (volatile int *)(uintptr_t)g_sink;
    *p = 1;
}

__attribute__((noinline)) RL_TEXT RL_LIFTED(_ZN5guest4test4wildEv);
__attribute__((noinline)) RL_TEXT RL_LIFTED(_ZN5guest4test3illEv);
__attribute__((noinline)) RL_TEXT RL_LIFTED(_ZN5guest4test9callshostEv);
__attribute__((noinline)) RL_TEXT RL_LIFTED(_ZN5guest4test5innerEv);
__attribute__((noinline)) RL_TEXT RL_LIFTED(_ZN5guest4test5outerEv);

RL_LIFTED(_ZN5guest4test4wildEv)
{
    (void)c;
    uint32_t v = 0x55AA;
    memcpy(mem + WILD_ADDR, &v, 4);
    g_sink += (int)v;
    return RL_OK;
}

RL_LIFTED(_ZN5guest4test3illEv)
{
    (void)c;
    (void)mem;
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_trap();
#else
    __asm__ volatile(".long 0");
#endif
    g_sink++;
    return RL_OK;
}

RL_LIFTED(_ZN5guest4test9callshostEv)
{
    (void)c;
    (void)mem;
    host_null_store();
    g_sink++;
    return RL_OK;
}

RL_LIFTED(_ZN5guest4test5innerEv)
{
    int rc;
    if (g_mode == 1)
        rc = _ZN5guest4test3illEv(c, mem);
    else if (g_mode == 2)
        rc = _ZN5guest4test9callshostEv(c, mem);
    else
        rc = _ZN5guest4test4wildEv(c, mem);
    if (RL_UNLIKELY(rc))
        return RL_PENDING;
    g_sink++;
    return RL_OK;
}

RL_LIFTED(_ZN5guest4test5outerEv)
{
    int rc = _ZN5guest4test5innerEv(c, mem);
    if (RL_UNLIKELY(rc))
        return RL_PENDING;
    g_sink++;
    return RL_OK;
}

static const rl_fn_info g_fns[] = {
    { G_OUTER, 0x100, 0, _ZN5guest4test5outerEv, "_ZN5guest4test5outerEv", "guest::test::outer()" },
    { G_INNER, 0x100, 0, _ZN5guest4test5innerEv, "_ZN5guest4test5innerEv", "guest::test::inner()" },
    { G_WILD, 0x100, 0, _ZN5guest4test4wildEv, "_ZN5guest4test4wildEv", "guest::test::wild()" },
    { G_ILL, 0x100, 0, _ZN5guest4test3illEv, "_ZN5guest4test3illEv", "guest::test::ill()" },
    { G_CALLS_HOST, 0x100, 0, _ZN5guest4test9callshostEv, "_ZN5guest4test9callshostEv", "guest::test::callshost()" },
};

static rl_fn_map g_map = {
    .version = RL_FN_MAP_VERSION,
    .count = sizeof g_fns / sizeof g_fns[0],
    .fns = g_fns,
    .module = "test",
    .shard = "test-0",
    .module_base = MODULE_BASE,
};

static uint8_t *reserve(void)
{
#if defined(_WIN32)
    return VirtualAlloc(NULL, SPACE, MEM_RESERVE, PAGE_NOACCESS);
#else
    void *p = mmap(NULL, SPACE, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static int child(const char *mode)
{
    uint8_t *mem = reserve();
    if (!mem)
        return 2;
    rl_crash_set_space(mem, (uintptr_t)mem, SPACE);
    g_map.text_lo = RL_TEXT_LO;
    g_map.text_hi = RL_TEXT_HI;
    const rl_fn_map *maps[] = { &g_map };
    if (strcmp(mode, "nomap") != 0 && rl_fn_maps_register(maps, 1) != 0)
        return 3;
    rl_crash_install();
    g_mode = !strcmp(mode, "ill") ? 1 : !strcmp(mode, "host") ? 2 : 0;
    _Alignas(64) uint8_t ctx[1024];
    memset(ctx, 0, sizeof ctx);
    int rc = _ZN5guest4test5outerEv((rl_ctx *)(void *)ctx, mem);
    fprintf(stderr, "no crash (rc %d)\n", rc);
    return 4;
}

typedef struct run {
    char out[32768];
    int crashed;
    unsigned long code;
} run;

#if defined(_WIN32)
static int spawn(const char *self, const char *mode, run *r)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi;
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" child %s", self, mode);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return -1;
    CloseHandle(wr);
    size_t len = 0;
    DWORD got;
    while (len + 1 < sizeof r->out && ReadFile(rd, r->out + len, (DWORD)(sizeof r->out - 1 - len), &got, NULL) && got)
        len += got;
    r->out[len] = 0;
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    r->code = code;
    r->crashed = code == (DWORD)EXCEPTION_ACCESS_VIOLATION || code == (DWORD)EXCEPTION_ILLEGAL_INSTRUCTION;
    return 0;
}
#else
static int spawn(const char *self, const char *mode, run *r)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    char *argv[] = { (char *)self, (char *)"child", (char *)mode, NULL };
    pid_t pid;
    int e = posix_spawn(&pid, self, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (e != 0) {
        close(fds[0]);
        return -1;
    }
    size_t len = 0;
    ssize_t got;
    while (len + 1 < sizeof r->out && (got = read(fds[0], r->out + len, sizeof r->out - 1 - len)) > 0)
        len += (size_t)got;
    r->out[len] = 0;
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    r->crashed = WIFSIGNALED(st) &&
                 (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS || WTERMSIG(st) == SIGILL);
    r->code = WIFSIGNALED(st) ? (unsigned long)WTERMSIG(st) : (unsigned long)WEXITSTATUS(st);
    return 0;
}
#endif

static int g_failed;

static void expect(const run *r, const char *what, int ok)
{
    if (!ok) {
        g_failed++;
        fprintf(stderr, "FAIL %s\n", what);
    }
    (void)r;
}

static const char *after(const char *hay, const char *needle)
{
    return hay ? strstr(hay, needle) : NULL;
}

static void check_backtrace(const run *r, const char *innermost)
{
    const char *g = after(r->out, "guest backtrace");
    const char *a = after(g, innermost);
    const char *b = after(a, "guest::test::inner()");
    const char *c = after(b, "guest::test::outer()");
    const char *h = after(r->out, "host backtrace:");
    expect(r, "guest backtrace lists the innermost lifted function, then inner, then outer", a && b && c && (!h || c < h));
    expect(r, "host backtrace shows the lifted frames by host symbol", after(h, "_ZN5guest4test5innerEv+0x") &&
                                                                           after(h, "_ZN5guest4test5outerEv+0x"));
}

static int parent(const char *self)
{
    run *r = calloc(1, sizeof *r);
    if (!r)
        return 1;
    char line[256];

    if (spawn(self, "wild", r) != 0)
        return 1;
    printf("--- wild guest store ---\n%s", r->out);
    expect(r, "wild: the child crashed", r->crashed);
    snprintf(line, sizeof line, "= guest address 0x%llx", (unsigned long long)WILD_ADDR);
    expect(r, "wild: report names the guest address", strstr(r->out, line) != NULL);
    expect(r, "wild: report names the faulting guest function",
           strstr(r->out, "in guest function guest 0x7100001200 test+0x1200 guest::test::wild(), host _ZN5guest4test4wildEv+0x") != NULL);
    check_backtrace(r, "#0 guest 0x7100001200 test+0x1200 guest::test::wild()");
    expect(r, "wild: no fallback match", strstr(r->out, "(nearest)") == NULL);

    if (spawn(self, "ill", r) != 0)
        return 1;
    printf("--- illegal instruction ---\n%s", r->out);
    expect(r, "ill: the child crashed", r->crashed);
    expect(r, "ill: report names the guest function", strstr(r->out, "in guest function guest 0x7100001300 test+0x1300 guest::test::ill()") != NULL);
    check_backtrace(r, "guest::test::ill()");

    if (spawn(self, "host", r) != 0)
        return 1;
    printf("--- host crash under lifted code ---\n%s", r->out);
    expect(r, "host: the child crashed", r->crashed);
    expect(r, "host: fault is outside guest memory", strstr(r->out, "(outside guest memory)") != NULL);
    expect(r, "host: pc is in host code", strstr(r->out, "in host code, not in a lifted function") != NULL);
    check_backtrace(r, "guest::test::callshost()");

    if (spawn(self, "nomap", r) != 0)
        return 1;
    printf("--- control: no function map registered ---\n%s", r->out);
    expect(r, "control: the child crashed", r->crashed);
    expect(r, "control: guest address still reported", strstr(r->out, line) != NULL);
    expect(r, "control: no guest names without a map", strstr(r->out, "guest::test::") == NULL);
    expect(r, "control: no lifted frame without a map", strstr(r->out, "(no lifted frame)") != NULL);

    printf("%s\n", g_failed ? "crash_test: FAILED" : "crash_test: ok");
    free(r);
    return g_failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "child"))
        return child(argv[2]);
    return parent(argv[0]);
}
