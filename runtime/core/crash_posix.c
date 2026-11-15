/**
 * @file crash_posix.c
 * @brief The crash handler on Linux and macOS: SIGSEGV, SIGBUS and SIGILL,
 * the host pc and frame pointer from the signal context, a frame-pointer
 * walk, and dladdr for host symbols.
 */
#if !defined(_WIN32)

#include "crash.h"

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

/** @brief How far above the faulting sp a frame pointer may point. */
#define RL_CRASH_STACK_SPAN (64ull << 20)

static volatile sig_atomic_t g_reporting;
static int g_installed;

typedef struct host_regs {
    uintptr_t pc, sp, fp;
} host_regs;

static host_regs regs_of(void *uc)
{
    host_regs r = { 0, 0, 0 };
    ucontext_t *u = uc;
    if (!u)
        return r;
#if defined(__APPLE__) && defined(__aarch64__)
    r.pc = (uintptr_t)u->uc_mcontext->__ss.__pc;
    r.sp = (uintptr_t)u->uc_mcontext->__ss.__sp;
    r.fp = (uintptr_t)u->uc_mcontext->__ss.__fp;
#elif defined(__APPLE__) && defined(__x86_64__)
    r.pc = (uintptr_t)u->uc_mcontext->__ss.__rip;
    r.sp = (uintptr_t)u->uc_mcontext->__ss.__rsp;
    r.fp = (uintptr_t)u->uc_mcontext->__ss.__rbp;
#elif defined(__linux__) && defined(__x86_64__)
    r.pc = (uintptr_t)u->uc_mcontext.gregs[REG_RIP];
    r.sp = (uintptr_t)u->uc_mcontext.gregs[REG_RSP];
    r.fp = (uintptr_t)u->uc_mcontext.gregs[REG_RBP];
#elif defined(__linux__) && defined(__aarch64__)
    r.pc = (uintptr_t)u->uc_mcontext.pc;
    r.sp = (uintptr_t)u->uc_mcontext.sp;
    r.fp = (uintptr_t)u->uc_mcontext.regs[29];
#endif
    return r;
}

static int walk(host_regs r, uintptr_t *frames, int max)
{
    int n = 0;
    frames[n++] = r.pc;
    uintptr_t fp = r.fp;
    while (n < max && fp && (fp & 7) == 0 && fp >= r.sp && fp - r.sp < RL_CRASH_STACK_SPAN) {
        const uintptr_t *f = (const uintptr_t *)fp;
        uintptr_t next = f[0], ret = f[1];
        if (!ret)
            break;
        frames[n++] = ret;
        if (next <= fp)
            break;
        fp = next;
    }
    return n;
}

static const char *sig_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    default: return "signal";
    }
}

void rl_crash_signal(int sig, void *siginfo, void *ucontext)
{
    siginfo_t *si = siginfo;
    if (g_reporting)
        return;
    g_reporting = 1;
    rl_crash_info ci;
    memset(&ci, 0, sizeof ci);
    host_regs r = regs_of(ucontext);
    ci.kind = sig_name(sig);
    ci.has_addr = si && sig != SIGILL;
    ci.addr = si ? (uintptr_t)si->si_addr : 0;
    ci.pc = r.pc ? r.pc : ci.addr;
    ci.sp = r.sp;
    r.pc = ci.pc;
    ci.nframes = walk(r, ci.frames, RL_CRASH_MAX_FRAMES);
    rl_crash_report(&ci);
    g_reporting = 0;
}

static void handler(int sig, siginfo_t *si, void *uc)
{
    if (g_reporting) {
        signal(sig, SIG_DFL);
        return;
    }
    rl_crash_signal(sig, si, uc);
    signal(sig, SIG_DFL);
    if (si && si->si_code <= 0)
        raise(sig);
}

void rl_crash_install(void)
{
    if (g_installed)
        return;
    g_installed = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);
#if defined(__ANDROID__)
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#else
    struct sigaction old;
    if (sigaction(SIGSEGV, NULL, &old) == 0 && !(old.sa_flags & SA_SIGINFO))
        sigaction(SIGSEGV, &sa, NULL);
    if (sigaction(SIGBUS, NULL, &old) == 0 && !(old.sa_flags & SA_SIGINFO))
        sigaction(SIGBUS, &sa, NULL);
#endif
}

bool rl_crash_os_symbolize(uintptr_t pc, char *buf, size_t n)
{
    Dl_info d;
    if (!dladdr((void *)pc, &d) || !d.dli_fname)
        return false;
    const char *base = strrchr(d.dli_fname, '/');
    base = base ? base + 1 : d.dli_fname;
    if (d.dli_sname && d.dli_saddr)
        snprintf(buf, n, "%s+0x%llx [%s]", d.dli_sname, (unsigned long long)(pc - (uintptr_t)d.dli_saddr), base);
    else
        snprintf(buf, n, "%s+0x%llx", base, (unsigned long long)(pc - (uintptr_t)d.dli_fbase));
    return true;
}

void rl_crash_os_write(const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(2, s, n);
        if (w <= 0)
            return;
        s += w;
        n -= (size_t)w;
    }
}

#endif
