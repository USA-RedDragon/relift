/**
 * @file thread.c
 * @brief Host threads backing guest threads, FPCR, process exit and the
 * system-instruction entries of docs/abi.md section 10.
 */
#include "core.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static _Thread_local struct rl_thread *t_self;
static rl_exit_hook *g_exit_hook;
static uint8_t *g_mem;

/** @brief Host stack size of a guest thread: lifted frames live there. */
#define HOST_STACK (16u << 20)

struct rl_thread *rl_thread_current(void) { return t_self; }

uint8_t *rl_mem_base(void) { return g_mem; }
void rl_set_mem_base(uint8_t *base) { g_mem = base; }

void rl_host_set_fpcr(uint32_t fpcr)
{
#if defined(__aarch64__)
    uint64_t v = fpcr;
    __asm__ volatile("msr fpcr, %0" ::"r"(v));
#else
    (void)fpcr;
#endif
}

uint32_t rl_host_get_fpcr(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return (uint32_t)v;
#else
    return 0;
#endif
}

void rl_thread_bind(struct rl_thread *t)
{
    t_self = t;
    rl_fault_thread_init();
    if (t && t->ctx)
        rl_host_set_fpcr(t->ctx->fpcr);
}

struct rl_thread *rl_thread_alloc(void)
{
    struct rl_thread *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    void *p = NULL;
    if (posix_memalign(&p, 64, sizeof(rl_ctx)) != 0) {
        free(t);
        return NULL;
    }
    t->ctx = p;
    memset(t->ctx, 0, sizeof(rl_ctx));
    t->ctx->thread = t;
    t->ctx->fpcr = RL_FPCR_DEFAULT;
    t->ctx->pend.sp = RL_SP_NONE;
    rl_dispatch_view(t->ctx);
    return t;
}

void rl_thread_free(struct rl_thread *t)
{
    if (!t)
        return;
    free(t->ctx);
    free(t->host);
    free(t);
}

typedef struct spawn_arg {
    struct rl_thread *t;
    rl_thread_body *body;
} spawn_arg;

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

void rl_set_exit_hook(rl_exit_hook *hook) { g_exit_hook = hook; }

void rl_process_exit(uint32_t code)
{
    if (g_exit_hook) {
        g_exit_hook(code);
        return;
    }
    fflush(stdout);
    fflush(stderr);
    _exit((int)code);
}

int rl_pend_exit_process(rl_ctx *c, uint32_t code)
{
    c->pend.kind = RL_PEND_EXIT_PROCESS;
    c->pend.code = code;
    c->pend.sp = RL_SP_NONE;
    c->pend.pc = 0;
    return RL_PENDING;
}

int rl_pend_exit_thread(rl_ctx *c)
{
    c->pend.kind = RL_PEND_EXIT_THREAD;
    c->pend.code = 0;
    c->pend.sp = RL_SP_NONE;
    c->pend.pc = 0;
    return RL_PENDING;
}

void rl_msr_fpcr(rl_ctx *c, uint64_t v)
{
    uint32_t f = (uint32_t)v;
    c->fpcr = f;
    if (c->thread == t_self)
        rl_host_set_fpcr(f);
    if ((f & RL_FPCR_MODE) != (RL_FPCR_DEFAULT & RL_FPCR_MODE) && rl_once(0x4650435200000000ull | (f & RL_FPCR_MODE)))
        rl_log(RL_LOG_WARN, "fpcr", "guest set FPCR 0x%08x: a mode the lifted code was not generated for", f);
}

void rl_wfe(rl_ctx *c)
{
    (void)c;
    rl_host_yield();
}

int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn)
{
    (void)mem;
    rl_log(RL_LOG_ERROR, "guest", "undefined instruction 0x%08x at guest pc 0x%llx (lr 0x%llx, sp 0x%llx)", insn,
           (unsigned long long)pc, (unsigned long long)c->x[30], (unsigned long long)c->sp);
    return rl_pend_exit_process(c, RL_EXIT_SOFTWARE);
}
