/**
 * @file signal.c
 * @brief Signals for one thread: dispositions, the blocked set, queued
 * signals and their delivery after a system call, on the guest stack with
 * the frame the arm64 kernel builds (siginfo, ucontext, and the floating
 * point context in its reserved area), and rt_sigreturn done natively when
 * the handler returns to its restorer.
 *
 * A signal comes from kill, tkill or tgkill on the process, or from a write
 * to a closed pipe. A fault in guest code (a bad address, an undefined
 * instruction) is not delivered to the guest's handlers: it stops the
 * program with a report. The pc in the context a handler sees is 0, since a
 * system call does not tell the runtime where it came from.
 */
#include "arm64.h"
#include "linux_priv.h"

#include <string.h>

enum {
    FRAME_INFO = 0,
    FRAME_UC = L_SIGINFO_BYTES,
    UC_STACK = 16,
    UC_SIGMASK = 40,
    UC_FAULT = L_UCONTEXT_MCONTEXT,
    UC_REGS = UC_FAULT + 8,
    UC_SP = UC_REGS + 31 * 8,
    UC_PC = UC_SP + 8,
    UC_PSTATE = UC_PC + 8,
    UC_RESERVED = UC_PSTATE + 16,
    FRAME_BYTES = L_SIGINFO_BYTES + L_UCONTEXT_MCONTEXT + L_SIGCONTEXT_BYTES
};

#define FPSIMD_MAGIC 0x46508001u
#define FPSIMD_SIZE 0x210u

static uint64_t bit(int sig) { return 1ull << (sig - 1); }

static uint64_t unblockable(void) { return bit(L_SIGKILL) | bit(L_SIGSTOP); }

void lx_signal_init(void)
{
    memset(lx.sig, 0, sizeof lx.sig);
    memset(lx.pending, 0, sizeof lx.pending);
    lx.blocked = 0;
}

void lx_signal_raise(int sig, int32_t code)
{
    if (sig < 1 || sig > L_NSIG)
        return;
    lx.pending[sig] = (lx_pending_sig){ .code = code, .pid = lx.pid, .uid = 0, .queued = true };
}

static bool default_ignores(int sig)
{
    return sig == L_SIGCHLD || sig == L_SIGURG || sig == L_SIGWINCH || sig == L_SIGCONT || sig == L_SIGSTOP || sig == L_SIGTSTP || sig == L_SIGTTIN ||
           sig == L_SIGTTOU;
}

static int next_pending(void)
{
    uint64_t mask = lx.blocked & ~unblockable();
    for (int s = 1; s <= L_NSIG; s++)
        if (lx.pending[s].queued && !(mask & bit(s)))
            return s;
    return 0;
}

static int run_handler(rl_ctx *c, uint8_t *mem, int sig, const lx_pending_sig *p)
{
    lx_sigaction act = lx.sig[sig];
    uint64_t sp;
    bool on_alt = false;
    if ((act.flags & L_SA_ONSTACK) && lx.altstack_size && !lx.on_altstack) {
        sp = lx.altstack_sp + lx.altstack_size;
        on_alt = true;
    } else {
        sp = c->sp;
    }
    uint64_t frame = (sp - FRAME_BYTES) & ~15ull;
    if (!lx_ok(frame, FRAME_BYTES, RL_VM_W))
        return rl_guest_fatal(c, "signal", "no room on the stack to deliver signal %d (sp 0x%llx)", sig, (unsigned long long)sp);
    uint8_t *f = mem + frame;
    memset(f, 0, FRAME_BYTES);
    int32_t si[4] = { sig, 0, p->code, 0 };
    memcpy(f + FRAME_INFO, si, 16);
    memcpy(f + FRAME_INFO + 16, &p->pid, 4);
    memcpy(f + FRAME_INFO + 20, &p->uid, 4);
    uint8_t *uc = f + FRAME_UC;
    int32_t ss_flags = lx.on_altstack ? L_SS_ONSTACK : (lx.altstack_size ? 0 : L_SS_DISABLE);
    memcpy(uc + UC_STACK, &lx.altstack_sp, 8);
    memcpy(uc + UC_STACK + 8, &ss_flags, 4);
    memcpy(uc + UC_STACK + 16, &lx.altstack_size, 8);
    memcpy(uc + UC_SIGMASK, &lx.blocked, 8);
    memcpy(uc + UC_REGS, c->x, 31 * 8);
    memcpy(uc + UC_SP, &c->sp, 8);
    uint32_t hdr[2] = { FPSIMD_MAGIC, FPSIMD_SIZE };
    uint32_t fp[2] = { rl_mrs_fpsr(c), (uint32_t)rl_mrs_fpcr(c) };
    memcpy(uc + UC_RESERVED, hdr, 8);
    memcpy(uc + UC_RESERVED + 8, fp, 8);
    for (int i = 0; i < 32; i++)
        memcpy(uc + UC_RESERVED + 16 + 16 * i, &c->v[i], 16);

    uint64_t old_blocked = lx.blocked;
    bool old_on_alt = lx.on_altstack;
    lx.blocked |= act.mask;
    if (!(act.flags & L_SA_NODEFER))
        lx.blocked |= bit(sig);
    lx.blocked &= ~unblockable();
    if (act.flags & L_SA_RESETHAND)
        lx.sig[sig] = (lx_sigaction){ 0 };
    if (on_alt)
        lx.on_altstack = true;

    c->x[0] = (uint64_t)sig;
    c->x[1] = frame + FRAME_INFO;
    c->x[2] = frame + FRAME_UC;
    c->x[30] = (act.flags & L_SA_RESTORER) ? act.restorer : 0;
    c->sp = frame;
    int rc = rl_call_guest(c, mem, act.handler);
    if (rc != RL_OK)
        return rc;

    memcpy(c->x, uc + UC_REGS, 31 * 8);
    memcpy(&c->sp, uc + UC_SP, 8);
    memcpy(fp, uc + UC_RESERVED + 8, 8);
    rl_msr_fpsr(c, fp[0]);
    rl_msr_fpcr(c, fp[1]);
    for (int i = 0; i < 32; i++)
        memcpy(&c->v[i], uc + UC_RESERVED + 16 + 16 * i, 16);
    uint64_t mask;
    memcpy(&mask, uc + UC_SIGMASK, 8);
    lx.blocked = mask & ~unblockable();
    lx.on_altstack = old_on_alt;
    (void)old_blocked;
    return RL_OK;
}

int lx_signal_deliver(rl_ctx *c, uint8_t *mem)
{
    for (int sig = next_pending(); sig; sig = next_pending()) {
        lx_pending_sig p = lx.pending[sig];
        lx.pending[sig].queued = false;
        lx_sigaction act = lx.sig[sig];
        if (act.handler == L_SIG_IGN || (act.handler == L_SIG_DFL && default_ignores(sig))) {
            if (act.handler == L_SIG_DFL && (sig == L_SIGSTOP || sig == L_SIGTSTP || sig == L_SIGTTIN || sig == L_SIGTTOU))
                rl_report_unknown(0x4c58535400ull | (uint64_t)sig, "linux", "signal %d would stop the process; it keeps running", sig);
            continue;
        }
        if (act.handler == L_SIG_DFL) {
            rl_pend_exit_process(c, 128u + (uint32_t)sig);
            return RL_PENDING;
        }
        int rc = run_handler(c, mem, sig, &p);
        if (rc != RL_OK)
            return rc;
    }
    return RL_OK;
}

static int64_t sys_rt_sigaction(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int sig = (int)a[0];
    if (a[3] != L_SIGSET_BYTES || sig < 1 || sig > L_NSIG)
        return -L_EINVAL;
    if (a[1] && (sig == L_SIGKILL || sig == L_SIGSTOP))
        return -L_EINVAL;
    lx_sigaction old = lx.sig[sig];
    if (a[1]) {
        uint64_t k[4];
        if (!lx_get(k, a[1], sizeof k))
            return -L_EFAULT;
        lx_sigaction n = { .handler = k[0], .flags = k[1], .restorer = k[2], .mask = k[3] & ~unblockable() };
        if (a[2]) {
            uint64_t o[4] = { old.handler, old.flags, old.restorer, old.mask };
            if (!lx_put(a[2], o, sizeof o))
                return -L_EFAULT;
        }
        lx.sig[sig] = n;
        if (n.handler == L_SIG_IGN || (n.handler == L_SIG_DFL && default_ignores(sig)))
            lx.pending[sig].queued = false;
        return 0;
    }
    if (a[2]) {
        uint64_t o[4] = { old.handler, old.flags, old.restorer, old.mask };
        if (!lx_put(a[2], o, sizeof o))
            return -L_EFAULT;
    }
    return 0;
}

static int64_t sys_rt_sigprocmask(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if (a[3] != L_SIGSET_BYTES)
        return -L_EINVAL;
    uint64_t old = lx.blocked;
    if (a[1]) {
        uint64_t set;
        if (!lx_get(&set, a[1], 8))
            return -L_EFAULT;
        switch ((uint32_t)a[0]) {
        case L_SIG_BLOCK:
            lx.blocked |= set;
            break;
        case L_SIG_UNBLOCK:
            lx.blocked &= ~set;
            break;
        case L_SIG_SETMASK:
            lx.blocked = set;
            break;
        default:
            return -L_EINVAL;
        }
        lx.blocked &= ~unblockable();
    }
    if (a[2] && !lx_put(a[2], &old, 8))
        return -L_EFAULT;
    return 0;
}

static int64_t sys_rt_sigpending(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if (a[1] != L_SIGSET_BYTES)
        return -L_EINVAL;
    uint64_t set = 0;
    for (int s = 1; s <= L_NSIG; s++)
        if (lx.pending[s].queued && (lx.blocked & bit(s)))
            set |= bit(s);
    return lx_put(a[0], &set, 8) ? 0 : -L_EFAULT;
}

static int64_t sys_sigaltstack(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if (a[1]) {
        int32_t flags = lx.on_altstack ? L_SS_ONSTACK : (lx.altstack_size ? 0 : L_SS_DISABLE);
        uint8_t o[24] = { 0 };
        memcpy(o, &lx.altstack_sp, 8);
        memcpy(o + 8, &flags, 4);
        memcpy(o + 16, &lx.altstack_size, 8);
        if (!lx_put(a[1], o, sizeof o))
            return -L_EFAULT;
    }
    if (a[0]) {
        uint8_t n[24];
        if (!lx_get(n, a[0], sizeof n))
            return -L_EFAULT;
        uint64_t sp, size;
        int32_t flags;
        memcpy(&sp, n, 8);
        memcpy(&flags, n + 8, 4);
        memcpy(&size, n + 16, 8);
        if (lx.on_altstack)
            return -L_EPERM;
        if (flags & ~L_SS_DISABLE)
            return -L_EINVAL;
        if (flags & L_SS_DISABLE) {
            lx.altstack_sp = 0;
            lx.altstack_size = 0;
        } else {
            if (size < L_MINSIGSTKSZ)
                return -L_ENOMEM;
            lx.altstack_sp = sp;
            lx.altstack_size = size;
        }
    }
    return 0;
}

static int64_t send(int sig, int32_t code)
{
    if (sig < 0 || sig > L_NSIG)
        return -L_EINVAL;
    if (sig)
        lx_signal_raise(sig, code);
    return 0;
}

static int64_t sys_kill(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int32_t pid = (int32_t)a[0];
    if (pid != lx.pid && pid != 0 && pid != -1 && pid != -lx.pid)
        return -L_ESRCH;
    return send((int)a[1], L_SI_USER);
}

static int64_t sys_tkill(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if ((int32_t)a[0] <= 0)
        return -L_EINVAL;
    if ((int32_t)a[0] != lx.pid)
        return -L_ESRCH;
    return send((int)a[1], L_SI_TKILL);
}

static int64_t sys_tgkill(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if ((int32_t)a[0] <= 0 || (int32_t)a[1] <= 0)
        return -L_EINVAL;
    if ((int32_t)a[0] != lx.pid || (int32_t)a[1] != lx.pid)
        return -L_ESRCH;
    return send((int)a[2], L_SI_TKILL);
}

static int64_t sys_rt_sigtimedwait(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    if (a[3] != L_SIGSET_BYTES)
        return -L_EINVAL;
    uint64_t set;
    if (!lx_get(&set, a[0], 8))
        return -L_EFAULT;
    for (;;) {
        for (int s = 1; s <= L_NSIG; s++)
            if (lx.pending[s].queued && (set & bit(s))) {
                lx_pending_sig p = lx.pending[s];
                lx.pending[s].queued = false;
                if (a[1]) {
                    uint8_t info[L_SIGINFO_BYTES] = { 0 };
                    int32_t si[4] = { s, 0, p.code, 0 };
                    memcpy(info, si, 16);
                    memcpy(info + 16, &p.pid, 4);
                    memcpy(info + 20, &p.uid, 4);
                    if (!lx_put(a[1], info, sizeof info))
                        return -L_EFAULT;
                }
                return s;
            }
        if (!a[2]) {
            rl_guest_fatal(c, "signal", "sigwait with no other thread or process to send the signal");
            return LX_PENDING;
        }
        l_timespec ts;
        if (!lx_get(&ts, a[2], sizeof ts))
            return -L_EFAULT;
        if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000)
            return -L_EINVAL;
        uint64_t ns = (uint64_t)ts.sec * 1000000000ull + (uint64_t)ts.nsec;
        if (ns)
            rl_sleep_ns(ns);
        (void)mem;
        return -L_EAGAIN;
    }
}

static int64_t sys_rt_sigsuspend(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)mem;
    if (a[1] != L_SIGSET_BYTES)
        return -L_EINVAL;
    uint64_t set;
    if (!lx_get(&set, a[0], 8))
        return -L_EFAULT;
    uint64_t old = lx.blocked;
    lx.blocked = set & ~unblockable();
    if (!next_pending()) {
        lx.blocked = old;
        rl_guest_fatal(c, "signal", "sigsuspend with no other thread or process to send a signal");
        return LX_PENDING;
    }
    int rc = lx_signal_deliver(c, mem);
    lx.blocked = old;
    if (rc != RL_OK)
        return LX_PENDING;
    return -L_EINTR;
}

lx_sys_fn *lx_signal_call(uint32_t nr)
{
    switch (nr) {
    case NR_rt_sigaction: return sys_rt_sigaction;
    case NR_rt_sigprocmask: return sys_rt_sigprocmask;
    case NR_rt_sigpending: return sys_rt_sigpending;
    case NR_rt_sigtimedwait: return sys_rt_sigtimedwait;
    case NR_rt_sigsuspend: return sys_rt_sigsuspend;
    case NR_sigaltstack: return sys_sigaltstack;
    case NR_kill: return sys_kill;
    case NR_tkill: return sys_tkill;
    case NR_tgkill: return sys_tgkill;
    default: return NULL;
    }
}
