/**
 * @file thread.c
 * @brief Host threads backing guest threads, each with the CPU layer's
 * context, process exit, the pending records and fatal guest reports.
 */
#if defined(_WIN32)
#define _CRT_RAND_S
#endif
#include "core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <malloc.h>
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

static _Thread_local struct rl_thread *t_self;
static rl_exit_hook *g_exit_hook;
static uint8_t *g_mem;

/** @brief Host stack size of a guest thread: lifted frames live there. */
#define HOST_STACK (16u << 20)

struct rl_thread *rl_thread_current(void) { return t_self; }

uint8_t *rl_mem_base(void) { return g_mem; }
void rl_set_mem_base(uint8_t *base) { g_mem = base; }

void rl_thread_bind(struct rl_thread *t)
{
    const rl_cpu_desc *cpu = rl_core_cpu();
    t_self = t;
    rl_fault_thread_init();
    if (t && t->ctx && cpu && cpu->bind)
        cpu->bind(t->ctx);
}

struct rl_thread *rl_thread_alloc(void)
{
    const rl_cpu_desc *cpu = rl_core_cpu();
    if (!cpu)
        return NULL;
    struct rl_thread *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    void *p = NULL;
#if defined(_WIN32)
    p = _aligned_malloc(cpu->ctx_size, cpu->ctx_align);
    if (!p) {
#else
    if (posix_memalign(&p, cpu->ctx_align, cpu->ctx_size) != 0) {
#endif
        free(t);
        return NULL;
    }
    memset(p, 0, cpu->ctx_size);
    t->ctx = p;
    rl_ctx_common *k = rl_common(t->ctx);
    k->thread = t;
    k->pend.sp = RL_SP_NONE;
    rl_dispatch_view(t->ctx);
    if (cpu->ctx_init)
        cpu->ctx_init(t->ctx);
    return t;
}

void rl_thread_free(struct rl_thread *t)
{
    if (!t)
        return;
#if defined(_WIN32)
    _aligned_free(t->ctx);
#else
    free(t->ctx);
#endif
    free(t->host);
    free(t);
}

typedef struct spawn_arg {
    struct rl_thread *t;
    rl_thread_body *body;
} spawn_arg;

#if defined(_WIN32)
static DWORD WINAPI spawn_main(void *p)
{
    spawn_arg a = *(spawn_arg *)p;
    free(p);
    rl_thread_bind(a.t);
    a.body(a.t);
    return 0;
}

int rl_thread_spawn(struct rl_thread *t, rl_thread_body *body)
{
    spawn_arg *a = malloc(sizeof *a);
    if (!a)
        return -1;
    a->t = t;
    a->body = body;
    HANDLE h = CreateThread(NULL, HOST_STACK, spawn_main, a, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) {
        free(a);
        return -1;
    }
    t->host = h;
    return 0;
}

void rl_thread_join(struct rl_thread *t)
{
    if (t && t->host) {
        WaitForSingleObject((HANDLE)t->host, INFINITE);
        CloseHandle((HANDLE)t->host);
        t->host = NULL;
    }
}

void rl_thread_detach(struct rl_thread *t)
{
    if (t && t->host) {
        CloseHandle((HANDLE)t->host);
        t->host = NULL;
    }
}

void rl_sleep_ns(uint64_t ns)
{
    static _Thread_local HANDLE timer;
    if (!timer)
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((ns + 99) / 100);
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep((DWORD)((ns + 999999) / 1000000));
}

void rl_host_yield(void) { SwitchToThread(); }

bool rl_host_random(void *dst, size_t n)
{
    uint8_t *p = dst;
    while (n) {
        unsigned int v;
        if (rand_s(&v) != 0)
            return false;
        size_t k = n < sizeof v ? n : sizeof v;
        memcpy(p, &v, k);
        p += k;
        n -= k;
    }
    return true;
}

#else
static void *spawn_main(void *p)
{
    spawn_arg a = *(spawn_arg *)p;
    free(p);
    rl_thread_bind(a.t);
    a.body(a.t);
    return NULL;
}

int rl_thread_spawn(struct rl_thread *t, rl_thread_body *body)
{
    pthread_attr_t attr;
    spawn_arg *a = malloc(sizeof *a);
    pthread_t *h = malloc(sizeof *h);
    if (!a || !h) {
        free(a);
        free(h);
        return -1;
    }
    a->t = t;
    a->body = body;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, HOST_STACK);
    int rc = pthread_create(h, &attr, spawn_main, a);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(a);
        free(h);
        return -1;
    }
    free(t->host);
    t->host = h;
    return 0;
}

void rl_thread_join(struct rl_thread *t)
{
    if (t && t->host) {
        pthread_join(*(pthread_t *)t->host, NULL);
        free(t->host);
        t->host = NULL;
    }
}

void rl_thread_detach(struct rl_thread *t)
{
    if (t && t->host) {
        pthread_t *h = t->host;
        t->host = NULL;
        pthread_detach(*h);
        free(h);
    }
}

void rl_sleep_ns(uint64_t ns)
{
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    while (nanosleep(&ts, &ts) != 0)
        ;
}

void rl_host_yield(void) { sched_yield(); }

bool rl_host_random(void *dst, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return false;
    bool ok = !n || fread(dst, n, 1, f) == 1;
    fclose(f);
    return ok;
}

#endif

void rl_set_exit_hook(rl_exit_hook *hook) { g_exit_hook = hook; }

void rl_process_exit(uint32_t code)
{
    if (g_exit_hook) {
        g_exit_hook(code);
        return;
    }
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32)
    ExitProcess(code);
#else
    _exit((int)code);
#endif
}

int rl_pend_exit_process(rl_ctx *c, uint32_t code)
{
    rl_pending *p = &rl_common(c)->pend;
    p->kind = RL_PEND_EXIT_PROCESS;
    p->code = code;
    p->sp = RL_SP_NONE;
    p->pc = 0;
    return RL_PENDING;
}

int rl_pend_exit_thread(rl_ctx *c)
{
    rl_pending *p = &rl_common(c)->pend;
    p->kind = RL_PEND_EXIT_THREAD;
    p->code = 0;
    p->sp = RL_SP_NONE;
    p->pc = 0;
    return RL_PENDING;
}

int rl_guest_fatal(rl_ctx *c, const char *tag, const char *fmt, ...)
{
    const rl_cpu_desc *cpu = rl_core_cpu();
    char what[512], regs[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    regs[0] = 0;
    if (cpu && cpu->describe)
        cpu->describe(c, regs, sizeof regs);
    if (regs[0])
        rl_log(RL_LOG_ERROR, tag, "%s (%s)", what, regs);
    else
        rl_log(RL_LOG_ERROR, tag, "%s", what);
    return rl_pend_exit_process(c, RL_EXIT_SOFTWARE);
}
