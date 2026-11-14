/**
 * @file fault.c
 * @brief SIGSEGV and SIGBUS handling: host faults inside the reservation are
 * guest faults at si_addr - base (macOS arm64 raises SIGBUS for PROT_NONE).
 */
#include "core.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

static rl_vm *g_vm;
static rl_fault_hook *g_hook;
static _Thread_local rl_fault_catch *t_catch;
static _Thread_local void *t_altstack;
static int g_installed;

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
    if (t_catch) {
        rl_fault_catch *c = t_catch;
        t_catch = NULL;
        c->fault = f;
        siglongjmp(c->jb, 1);
    }
    if (f.in_guest && g_hook && g_hook(&f))
        return;
    char buf[256];
    int n;
    if (f.in_guest)
        n = snprintf(buf, sizeof buf, "[relift error fault] guest fault at 0x%llx (signal %d, host pc %p)\n",
                     (unsigned long long)f.guest_addr, sig, (void *)f.host_pc);
    else
        n = snprintf(buf, sizeof buf, "[relift error fault] host fault at %p (signal %d, host pc %p)\n", (void *)a, sig,
                     (void *)f.host_pc);
    if (n > 0 && write(2, buf, (size_t)n) < 0)
        n = 0;
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

void rl_fault_install(rl_vm *vm)
{
    g_vm = vm;
    rl_fault_thread_init();
    if (g_installed)
        return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    g_installed = 1;
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
