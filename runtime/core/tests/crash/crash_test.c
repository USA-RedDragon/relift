/**
 * @file crash_test.c
 * @brief The crash handler end to end: hand-written functions with the
 * lifted signature, in the lifted-code section and in a function map, make
 * a wild guest access (or run an illegal instruction, or crash in host code
 * they call, or overflow a fiber's stack). The test runs itself as a child
 * for each case, reads the child's report from a pipe and checks the guest
 * address, the guest function, the guest backtrace, the host backtrace and
 * the fiber (guest CPU) named.
 *
 * Links only the crash handler (crash.c and its host-OS part) and the
 * fibers, so it builds and runs on Windows, where the rest of the core is
 * not ported yet.
 */
#include "child.h"

#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#include "crash.h"
#include "fiber.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/mman.h>
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
    G_RECURSE = MODULE_BASE + 0x1500,
    G_PIECED = MODULE_BASE + 0x1600,
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
__attribute__((noinline)) RL_TEXT RL_LIFTED(_ZN5guest4test7recurseEv);
__attribute__((noinline)) RL_TEXT RL_LIFTED(_ZN5guest4test6piecedEv);
__attribute__((noinline)) RL_TEXT RL_LIFTED(_ZN5guest4test11pieced_partE1);
__attribute__((noinline)) RL_TEXT RL_LIFTED(_ZN5guest4test11pieced_partE2);

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

RL_LIFTED(_ZN5guest4test7recurseEv)
{
    if (!mem)
        return RL_OK;
    volatile uint8_t frame[2048];
    frame[0] = (uint8_t)g_sink;
    frame[sizeof frame - 1] = (uint8_t)(uintptr_t)mem;
    int rc = _ZN5guest4test7recurseEv(c, mem);
    g_sink += frame[0] + frame[sizeof frame - 1];
    return rc;
}

RL_LIFTED(_ZN5guest4test11pieced_partE2)
{
    (void)c;
    uint32_t v = 0x77;
    memcpy(mem + WILD_ADDR, &v, 4);
    g_sink += (int)v;
    return RL_OK;
}

RL_LIFTED(_ZN5guest4test11pieced_partE1)
{
    int rc = _ZN5guest4test11pieced_partE2(c, mem);
    g_sink++;
    return rc;
}

RL_LIFTED(_ZN5guest4test6piecedEv)
{
    return _ZN5guest4test11pieced_partE1(c, mem);
}

static const rl_fn_info g_fns[] = {
    { G_OUTER, G_OUTER, 0x100, 0, _ZN5guest4test5outerEv, "_ZN5guest4test5outerEv", "guest::test::outer()" },
    { G_INNER, G_INNER, 0x100, 0, _ZN5guest4test5innerEv, "_ZN5guest4test5innerEv", "guest::test::inner()" },
    { G_WILD, G_WILD, 0x100, 0, _ZN5guest4test4wildEv, "_ZN5guest4test4wildEv", "guest::test::wild()" },
    { G_ILL, G_ILL, 0x100, 0, _ZN5guest4test3illEv, "_ZN5guest4test3illEv", "guest::test::ill()" },
    { G_CALLS_HOST, G_CALLS_HOST, 0x100, 0, _ZN5guest4test9callshostEv, "_ZN5guest4test9callshostEv", "guest::test::callshost()" },
    { G_RECURSE, G_RECURSE, 0x100, 0, _ZN5guest4test7recurseEv, "_ZN5guest4test7recurseEv", "guest::test::recurse()" },
    { G_PIECED, G_PIECED, 0, 0, _ZN5guest4test6piecedEv, "_ZN5guest4test6piecedEv", "guest::test::pieced()" },
    { G_PIECED, G_PIECED, 0x80, 0, _ZN5guest4test11pieced_partE1, "_ZN5guest4test11pieced_partE1", "guest::test::pieced()" },
    { G_PIECED + 0x80, G_PIECED, 0x80, 0, _ZN5guest4test11pieced_partE2, "_ZN5guest4test11pieced_partE2", "guest::test::pieced()" },
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

static rl_fiber g_home, g_fa, g_fb;
static uint8_t *g_mem;
static _Alignas(64) uint8_t g_ctx[1024];

static void parked_main(void *arg)
{
    (void)arg;
    for (;;)
        rl_fiber_switch(&g_fb, &g_home);
}

static void faulting_main(void *arg)
{
    int rc = arg ? _ZN5guest4test7recurseEv((rl_ctx *)(void *)g_ctx, g_mem)
                 : _ZN5guest4test5outerEv((rl_ctx *)(void *)g_ctx, g_mem);
    fprintf(stderr, "no crash on the fiber (rc %d)\n", rc);
    exit(4);
}

static int on_fibers(int overflow)
{
    rl_fiber_thread_init();
    if (rl_fiber_init(&g_fa, overflow ? 64u << 10 : 0, faulting_main, overflow ? &g_fa : NULL) != 0 ||
        rl_fiber_init(&g_fb, 0, parked_main, NULL) != 0)
        return 5;
    snprintf(g_fa.label, sizeof g_fa.label, "guest CPU 0");
    snprintf(g_fb.label, sizeof g_fb.label, "guest CPU 1");
    rl_fiber_switch(&g_home, &g_fb);
    rl_fiber_switch(&g_home, &g_fa);
    return 6;
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
#if !defined(_WIN32)
    if (!strcmp(mode, "core")) {
        struct rlimit all = { RLIM_INFINITY, RLIM_INFINITY };
        setrlimit(RLIMIT_CORE, &all);
    }
#endif
    g_mode = !strcmp(mode, "ill") ? 1 : !strcmp(mode, "host") ? 2 : 0;
    if (!strcmp(mode, "core")) {
        g_mode = 0;
        memset(g_ctx, 0, sizeof g_ctx);
        _ZN5guest4test5outerEv((rl_ctx *)(void *)g_ctx, mem);
        return 4;
    }
    g_mem = mem;
    if (!strcmp(mode, "pieces")) {
        if ((uintptr_t)_ZN5guest4test11pieced_partE2 >= (uintptr_t)_ZN5guest4test11pieced_partE1)
            return 7;
        memset(g_ctx, 0, sizeof g_ctx);
        _ZN5guest4test6piecedEv((rl_ctx *)(void *)g_ctx, mem);
        return 4;
    }
    if (!strcmp(mode, "fiber"))
        return on_fibers(0);
    if (!strcmp(mode, "overflow"))
        return on_fibers(1);
    memset(g_ctx, 0, sizeof g_ctx);
    int rc = _ZN5guest4test5outerEv((rl_ctx *)(void *)g_ctx, mem);
    fprintf(stderr, "no crash (rc %d)\n", rc);
    return 4;
}

typedef child_run run;

static int spawn(const char *self, const char *mode, run *r)
{
    return child_spawn(self, mode, r);
}

#define CRASHED(r) child_crashed(r)

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
    expect(r, "wild: the child crashed", CRASHED(r));
    snprintf(line, sizeof line, "= guest address 0x%llx", (unsigned long long)WILD_ADDR);
    expect(r, "wild: report names the guest address", strstr(r->out, line) != NULL);
    expect(r, "wild: report names the faulting guest function",
           strstr(r->out, "in guest function guest 0x7100001200 test+0x1200 guest::test::wild(), host _ZN5guest4test4wildEv+0x") != NULL);
    check_backtrace(r, "#0 guest 0x7100001200 test+0x1200 guest::test::wild()");
    expect(r, "wild: no fallback match", strstr(r->out, "(nearest)") == NULL);

    if (spawn(self, "pieces", r) != 0)
        return 1;
    printf("--- fault in the second piece of a split function, pieces placed in reverse ---\n%s", r->out);
    expect(r, "pieces: the child crashed", CRASHED(r));
    expect(r, "pieces: report names the guest function of the piece",
           strstr(r->out, "in guest function guest 0x7100001600 test+0x1600 guest::test::pieced(), host _ZN5guest4test11pieced_partE2+0x") != NULL);
    expect(r, "pieces: the backtrace shows both pieces as the function",
           strstr(r->out, "#1 guest 0x7100001600 test+0x1600 guest::test::pieced()") != NULL);
    expect(r, "pieces: no fallback match", strstr(r->out, "(nearest)") == NULL);

    if (spawn(self, "ill", r) != 0)
        return 1;
    printf("--- illegal instruction ---\n%s", r->out);
    expect(r, "ill: the child crashed", CRASHED(r));
    expect(r, "ill: report names the guest function", strstr(r->out, "in guest function guest 0x7100001300 test+0x1300 guest::test::ill()") != NULL);
    check_backtrace(r, "guest::test::ill()");

    if (spawn(self, "host", r) != 0)
        return 1;
    printf("--- host crash under lifted code ---\n%s", r->out);
    expect(r, "host: the child crashed", CRASHED(r));
    expect(r, "host: fault is outside guest memory", strstr(r->out, "(outside guest memory)") != NULL);
    expect(r, "host: pc is in host code", strstr(r->out, "in host code, not in a lifted function") != NULL);
    check_backtrace(r, "guest::test::callshost()");

#if !defined(_WIN32)
    if (spawn(self, "core", r) != 0)
        return 1;
    printf("--- a crash with the core limit raised ---\n%.600s\n", r->out);
    expect(r, "core: the child crashed", CRASHED(r));
    expect(r, "core: the report was written",
           strstr(r->out, "[relift crash] SIGSEGV") != NULL || strstr(r->out, "[relift crash] SIGBUS") != NULL);
    expect(r, "core: no core dump follows the report (it would take minutes for a game's address space)", !r->cored);
#endif

    if (spawn(self, "nomap", r) != 0)
        return 1;
    printf("--- control: no function map registered ---\n%s", r->out);
    expect(r, "control: the child crashed", CRASHED(r));
    expect(r, "control: guest address still reported", strstr(r->out, line) != NULL);
    expect(r, "control: no guest names without a map", strstr(r->out, "guest::test::") == NULL);
    expect(r, "control: no lifted frame without a map", strstr(r->out, "(no lifted frame)") != NULL);

    if (spawn(self, "fiber", r) != 0)
        return 1;
    printf("--- wild guest store on a fiber, another fiber parked ---\n%s", r->out);
    expect(r, "fiber: the child crashed", CRASHED(r));
    expect(r, "fiber: report names the guest CPU of the faulting fiber", strstr(r->out, "] on guest CPU 0 (fiber stack ") != NULL);
    expect(r, "fiber: report does not blame the parked fiber", strstr(r->out, "guest CPU 1") == NULL);
    expect(r, "fiber: report names the guest address", strstr(r->out, line) != NULL);
    check_backtrace(r, "#0 guest 0x7100001200 test+0x1200 guest::test::wild()");
    expect(r, "fiber: the host walk ends at the fiber's first frame", strstr(r->out, "  #12 ") == NULL);

    if (spawn(self, "overflow", r) != 0)
        return 1;
    printf("--- stack overflow on a fiber ---\n%.2000s\n", r->out);
    expect(r, "overflow: the child crashed", CRASHED(r));
    expect(r, "overflow: report names the overflow and the guest CPU", strstr(r->out, "] stack overflow on guest CPU 0 (fiber stack ") != NULL);
    expect(r, "overflow: report names the recursing guest function", strstr(r->out, "guest::test::recurse()") != NULL);
#if defined(_WIN32)
    expect(r, "overflow: Windows reports STATUS_STACK_OVERFLOW", strstr(r->out, "EXCEPTION_STACK_OVERFLOW") != NULL);
#endif

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
