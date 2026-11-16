/**
 * @file fault.c
 * @brief SIGSEGV and SIGBUS handling: host faults inside the reservation are
 * guest faults at si_addr - base (macOS arm64 raises SIGBUS for PROT_NONE).
 */
#include "core.h"
#include "crash.h"
#include "fiber.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#endif

static rl_vm *g_vm;
static rl_fault_hook *g_hook;
static _Thread_local rl_fault_catch *t_catch;
#if !defined(_WIN32)
static _Thread_local void *t_altstack;
#endif
static int g_installed;

#if defined(_WIN32)
static LONG CALLBACK handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if ((er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && er->ExceptionCode != EXCEPTION_IN_PAGE_ERROR) ||
        er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    rl_fault f;
    uintptr_t a = (uintptr_t)er->ExceptionInformation[1];
    memset(&f, 0, sizeof f);
    f.sig = (int)er->ExceptionCode;
    f.host_addr = a;
    f.host_pc = (uintptr_t)er->ExceptionAddress;
    if (g_vm && a >= (uintptr_t)g_vm->map_start && a < (uintptr_t)g_vm->map_start + g_vm->map_len) {
        f.in_guest = true;
        f.guest_addr = (uint64_t)(a - (uintptr_t)g_vm->base);
    }
    if (f.in_guest && g_hook && g_hook(&f))
        return EXCEPTION_CONTINUE_EXECUTION;
    if (t_catch) {
        rl_fault_catch *c = t_catch;
        t_catch = NULL;
        c->fault = f;
        RL_FAULT_LONGJMP(c->jb, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void rl_fault_thread_init(void)
{
    rl_fiber_thread_init();
}

#else
static uintptr_t host_pc(void *uc)
{
    ucontext_t *u = uc;
    if (!u)
        return 0;
#if defined(__APPLE__) && defined(__aarch64__)
    return (uintptr_t)u->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__) && defined(__x86_64__)
    return (uintptr_t)u->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__x86_64__)
    return (uintptr_t)u->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(__aarch64__)
    return (uintptr_t)u->uc_mcontext.pc;
#else
    return 0;
#endif
}

static void handler(int sig, siginfo_t *si, void *uc)
{
    rl_fault f;
    uintptr_t a = (uintptr_t)si->si_addr;
    memset(&f, 0, sizeof f);
    f.sig = sig;
    f.host_addr = a;
    f.host_pc = host_pc(uc);
    if (g_vm && a >= (uintptr_t)g_vm->map_start && a < (uintptr_t)g_vm->map_start + g_vm->map_len) {
        f.in_guest = true;
        f.guest_addr = (uint64_t)(a - (uintptr_t)g_vm->base);
    }
    if (f.in_guest && g_hook && g_hook(&f))
        return;
    if (t_catch) {
        rl_fault_catch *c = t_catch;
        t_catch = NULL;
        c->fault = f;
        RL_FAULT_LONGJMP(c->jb, 1);
    }
    rl_crash_signal(sig, si, uc);
    signal(sig, SIG_DFL);
}

void rl_fault_thread_init(void)
{
    if (t_altstack)
        return;
    size_t sz = 64 * 1024;
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return;
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = p;
    ss.ss_size = sz;
    if (sigaltstack(&ss, NULL) == 0)
        t_altstack = p;
    else
        munmap(p, sz);
}

#endif

static void crash_extra(char *buf, size_t n)
{
    struct rl_thread *t = rl_thread_current();
    const rl_cpu_desc *cpu = rl_core_cpu();
    buf[0] = 0;
    if (!t || !t->ctx || !cpu || !cpu->describe)
        return;
    int w = snprintf(buf, n, "guest thread at its last call boundary: ");
    if (w > 0 && (size_t)w < n)
        cpu->describe(t->ctx, buf + w, n - (size_t)w);
}

void rl_fault_install(rl_vm *vm)
{
    g_vm = vm;
    rl_crash_set_space(vm->base, (uintptr_t)vm->map_start, vm->map_len);
    rl_crash_set_extra(crash_extra);
    rl_report_prepare();
    rl_crash_install();
    rl_fault_thread_init();
    if (g_installed)
        return;
#if defined(_WIN32)
    AddVectoredExceptionHandler(1, handler);
    g_installed = 1;
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    g_installed = 1;
#endif
}

rl_fault_catch *rl_fault_arm(rl_fault_catch *c)
{
    rl_fault_catch *prev = t_catch;
    memset(&c->fault, 0, sizeof c->fault);
    t_catch = c;
    return prev;
}

void rl_fault_disarm(rl_fault_catch *prev)
{
    t_catch = prev;
}

void rl_fault_set_hook(rl_fault_hook *hook)
{
    g_hook = hook;
}

bool rl_fault_installed(void) { return g_installed != 0; }
