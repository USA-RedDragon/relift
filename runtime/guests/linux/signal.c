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
 * program with a report. The pc in the context a handler sees is the one
 * after the svc that delivered it, and a handler that changes it continues
 * there when it returns.
 */
#include "arch.h"
#include "compat.h"
#include "hostio.h"
#include "linux_priv.h"

#include <string.h>

static uint64_t bit(int sig) { return 1ull << (sig - 1); }

static uint64_t unblockable(void) { return bit(L_SIGKILL) | bit(L_SIGSTOP); }

void lx_signal_init(void)
{
    memset(lx.sig, 0, sizeof lx.sig);
}

static void raise_on(lx_thread *t, int sig, int32_t code)
{
    t->pending[sig] = (lx_pending_sig){ .code = code, .pid = lx.pid, .uid = hio_uid(), .queued = false };
    __atomic_store_n(&t->pending[sig].queued, true, __ATOMIC_RELEASE);
    if (t != lx_cur()) {
        rl_preempt_request(t->rt->ctx);
        lx_thread_kick(t);
    }
}

void lx_signal_post(int sig, int32_t code, int32_t pid, uint32_t uid)
{
    if (sig < 1 || sig > L_NSIG)
        return;
    lx_mutex_lock(&lx_big);
    lx_thread *pick = NULL;
    for (lx_thread *t = lx.threads; t; t = t->next) {
        if (!pick && t->main)
            pick = t;
        if (!(t->blocked & bit(sig))) {
            pick = t;
            break;
        }
    }
    if (!pick)
        pick = lx.threads;
    if (pick) {
        pick->pending[sig] = (lx_pending_sig){ .code = code, .pid = pid, .uid = uid, .queued = false };
        __atomic_store_n(&pick->pending[sig].queued, true, __ATOMIC_RELEASE);
    }
    lx_mutex_unlock(&lx_big);
    if (pick) {
        rl_preempt_request(pick->rt->ctx);
        lx_thread_kick(pick);
    }
}

void lx_signal_raise(int sig, int32_t code)
{
    if (sig < 1 || sig > L_NSIG)
        return;
    raise_on(lx_cur(), sig, code);
}

/** @brief The thread a signal sent to the process goes to: this one unless it blocks the signal, then the first that does not. */
static lx_thread *receiver(int sig)
{
    lx_thread *me = lx_cur();
    if (!(me->blocked & (1ull << (sig - 1))))
        return me;
    lx_mutex_lock(&lx_big);
    lx_thread *pick = me;
    for (lx_thread *t = lx.threads; t; t = t->next)
        if (!(t->blocked & (1ull << (sig - 1)))) {
            pick = t;
            break;
        }
    lx_mutex_unlock(&lx_big);
    return pick;
}

static bool default_ignores(int sig)
{
    return sig == L_SIGCHLD || sig == L_SIGURG || sig == L_SIGWINCH || sig == L_SIGCONT || sig == L_SIGSTOP || sig == L_SIGTSTP || sig == L_SIGTTIN ||
           sig == L_SIGTTOU;
}

static bool default_ignores(int sig);

static int next_pending(void)
{
    uint64_t mask = lx_cur()->blocked & ~unblockable();
    for (int s = 1; s <= L_NSIG; s++)
        if (__atomic_load_n(&lx_cur()->pending[s].queued, __ATOMIC_ACQUIRE) && !(mask & bit(s)))
            return s;
    return 0;
}

static bool does_something(int sig)
{
    lx_sigaction act = lx.sig[sig];
    return !(act.handler == L_SIG_IGN || (act.handler == L_SIG_DFL && default_ignores(sig)));
}

bool lx_signal_ready(void)
{
    uint64_t mask = lx_cur()->blocked & ~unblockable();
    for (int s = 1; s <= L_NSIG; s++)
        if (__atomic_load_n(&lx_cur()->pending[s].queued, __ATOMIC_ACQUIRE) && !(mask & bit(s)) && does_something(s))
            return true;
    return false;
}

int lx_signal_peek(uint64_t *flags)
{
    uint64_t mask = lx_cur()->blocked & ~unblockable();
    for (int s = 1; s <= L_NSIG; s++)
        if (__atomic_load_n(&lx_cur()->pending[s].queued, __ATOMIC_ACQUIRE) && !(mask & bit(s))) {
            if (!does_something(s))
                continue;
            if (lx.sig[s].handler == L_SIG_DFL)
                return 0;
            *flags = lx.sig[s].flags;
            return 1;
        }
    return 0;
}

static int take(int sig, lx_pending_sig *p, bool *ignored, rl_ctx *c);
static int lx_signal_deliver_from(rl_ctx *c, uint8_t *mem);

static int run_handler(rl_ctx *c, uint8_t *mem, int sig, const lx_pending_sig *p)
{
    lx_sigaction act = lx.sig[sig];
    uint64_t sp;
    bool on_alt = false;
    if ((act.flags & L_SA_ONSTACK) && lx_cur()->altstack_size && !lx_cur()->on_altstack) {
        sp = lx_cur()->altstack_sp + lx_cur()->altstack_size;
        on_alt = true;
    } else {
        sp = lx_arch_sp(c);
    }
    uint64_t frame = lx_arch_frame_place(sp);
    if (!lx_ok(frame, lx_arch_frame_bytes(), RL_VM_W))
        return rl_guest_fatal(c, "signal", "no room on the stack to deliver signal %d (sp 0x%llx)", sig, (unsigned long long)sp);
    const uint64_t bias = lx_cur()->resume_bias;
    const uint64_t resume = lx_arch_pc(c) + bias;
    if (lx_arch_frame_build(c, mem, frame, sig, p, &act, resume) != 0)
        return rl_guest_fatal(c, "signal", "signal %d cannot be delivered: the handler has no return trampoline", sig);

    uint64_t old_blocked = lx_cur()->blocked;
    bool old_on_alt = lx_cur()->on_altstack;
    lx_cur()->blocked |= act.mask;
    if (!(act.flags & L_SA_NODEFER))
        lx_cur()->blocked |= bit(sig);
    lx_cur()->blocked &= ~unblockable();
    if (act.flags & L_SA_RESETHAND)
        lx.sig[sig] = (lx_sigaction){ 0 };
    if (on_alt)
        lx_cur()->on_altstack = true;

    int rc = RL_OK;
    for (int more = next_pending(); more; more = next_pending()) {
        lx_pending_sig q;
        bool ignored;
        if (take(more, &q, &ignored, c) == RL_PENDING)
            return RL_PENDING;
        if (ignored)
            continue;
        rc = run_handler(c, mem, more, &q);
        break;
    }
    if (rc != RL_OK)
        return rc;
    lx_cur()->handlers++;
    rc = lx_arch_call_guest(c, mem, act.handler);
    lx_cur()->handlers--;
    if (rc != RL_OK)
        return rc;

    uint64_t mask;
    uint64_t new_pc = lx_arch_frame_restore(c, mem, frame, &mask);
    lx_cur()->blocked = mask & ~unblockable();
    lx_cur()->on_altstack = old_on_alt;
    (void)old_blocked;
    if (new_pc != resume && !lx_arch_trampoline()) {
        rl_guest_fatal(c, "signal",
                       "a signal handler changed the pc to 0x%llx, which a build with direct returns does not take: "
                       "rebuild with emit.trampoline = true",
                       (unsigned long long)new_pc);
        return RL_PENDING;
    }
    if (new_pc != resume) {
        lx_arch_redirect(c, resume - bias);
        rl_ctx_common *k = rl_common(c);
        k->pend.pc = new_pc;
        k->pend.sp = RL_SP_NONE;
        k->pend.kind = RL_PEND_REDIRECT;
        k->pend.code = 0;
        return RL_PENDING;
    }
    return RL_OK;
}

int64_t lx_sigreturn(rl_ctx *c)
{
    if (lx_cur()->handlers == 0)
        return -L_ENOSYS;
    rl_common(c)->pend.kind = RL_PEND_RETURN;
    return LX_PENDING;
}

/**
 * @brief Takes signal sig off the queue. Sets *ignored when its disposition
 * discards it; returns RL_PENDING when the default action ends the process.
 */
static int take(int sig, lx_pending_sig *p, bool *ignored, rl_ctx *c)
{
    *p = lx_cur()->pending[sig];
    lx_cur()->pending[sig].queued = false;
    *ignored = false;
    lx_sigaction act = lx.sig[sig];
    if (act.handler == L_SIG_IGN || (act.handler == L_SIG_DFL && default_ignores(sig))) {
        if (act.handler == L_SIG_DFL && (sig == L_SIGSTOP || sig == L_SIGTSTP || sig == L_SIGTTIN || sig == L_SIGTTOU))
            rl_report_unknown(0x4c58535400ull | (uint64_t)sig, "linux", "signal %d would stop the process; it keeps running", sig);
        *ignored = true;
        return RL_OK;
    }
    if (act.handler == L_SIG_DFL) {
        lx.death_signal = sig;
        rl_pend_exit_process(c, 128u + (uint32_t)sig);
        return RL_PENDING;
    }
    return RL_OK;
}

/**
 * @brief Delivers the queued signals. As the kernel does, the frame of the
 * next deliverable signal is built before the handler of the one before it
 * has run its first instruction, so with two pending the handler of the
 * second runs first (run_handler nests).
 */
int lx_signal_deliver(rl_ctx *c, uint8_t *mem)
{
    lx_cur()->resume_bias = lx_arch_trap_bias();
    return lx_signal_deliver_from(c, mem);
}

static int lx_signal_deliver_from(rl_ctx *c, uint8_t *mem)
{
    for (int sig = next_pending(); sig; sig = next_pending()) {
        lx_pending_sig p;
        bool ignored;
        if (take(sig, &p, &ignored, c) == RL_PENDING)
            return RL_PENDING;
        if (ignored)
            continue;
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
        if (!lx_get_sigaction(a[1], k))
            return -L_EFAULT;
        lx_sigaction n = { .handler = k[0], .flags = k[1], .restorer = k[2], .mask = k[3] & ~unblockable() };
        if (a[2]) {
            uint64_t o[4] = { old.handler, old.flags, old.restorer, old.mask };
            if (!lx_put_sigaction(a[2], o))
                return -L_EFAULT;
        }
        lx.sig[sig] = n;
        if (n.handler == L_SIG_IGN || (n.handler == L_SIG_DFL && default_ignores(sig)))
            lx_cur()->pending[sig].queued = false;
        return 0;
    }
    if (a[2]) {
        uint64_t o[4] = { old.handler, old.flags, old.restorer, old.mask };
        if (!lx_put_sigaction(a[2], o))
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
    uint64_t old = lx_cur()->blocked;
    if (a[1]) {
        uint64_t set;
        if (!lx_get(&set, a[1], 8))
            return -L_EFAULT;
        switch ((uint32_t)a[0]) {
        case L_SIG_BLOCK:
            lx_cur()->blocked |= set;
            break;
        case L_SIG_UNBLOCK:
            lx_cur()->blocked &= ~set;
            break;
        case L_SIG_SETMASK:
            lx_cur()->blocked = set;
            break;
        default:
            return -L_EINVAL;
        }
        lx_cur()->blocked &= ~unblockable();
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
        if (lx_cur()->pending[s].queued && (lx_cur()->blocked & bit(s)))
            set |= bit(s);
    return lx_put(a[0], &set, 8) ? 0 : -L_EFAULT;
}

static int64_t sys_sigaltstack(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if (a[1]) {
        int32_t flags = lx_cur()->on_altstack ? L_SS_ONSTACK : (lx_cur()->altstack_size ? 0 : L_SS_DISABLE);
        if (!lx_put_stack_t(a[1], lx_cur()->altstack_sp, flags, lx_cur()->altstack_size))
            return -L_EFAULT;
    }
    if (a[0]) {
        uint64_t sp, size;
        int32_t flags;
        if (!lx_get_stack_t(a[0], &sp, &flags, &size))
            return -L_EFAULT;
        if (lx_cur()->on_altstack)
            return -L_EPERM;
        if (flags & ~L_SS_DISABLE)
            return -L_EINVAL;
        if (flags & L_SS_DISABLE) {
            lx_cur()->altstack_sp = 0;
            lx_cur()->altstack_size = 0;
        } else {
            if (size < L_MINSIGSTKSZ)
                return -L_ENOMEM;
            lx_cur()->altstack_sp = sp;
            lx_cur()->altstack_size = size;
        }
    }
    return 0;
}

static int64_t send_to(lx_thread *t, int sig, int32_t code)
{
    if (sig < 0 || sig > L_NSIG)
        return -L_EINVAL;
    if (sig)
        raise_on(t, sig, code);
    return 0;
}

static lx_thread *find_thread(int32_t tid)
{
    lx_thread *found = NULL;
    lx_mutex_lock(&lx_big);
    for (lx_thread *t = lx.threads; t; t = t->next)
        if (t->tid == tid)
            found = t;
    lx_mutex_unlock(&lx_big);
    return found;
}

static int64_t sys_kill(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int32_t pid = (int32_t)a[0];
    int sig = (int)a[1];
    if (sig < 0 || sig > L_NSIG)
        return -L_EINVAL;
    if (pid != lx.pid && pid != -1)
        return hio_kill(pid, sig);
    return send_to(sig ? receiver(sig) : lx_cur(), sig, L_SI_USER);
}

static int64_t sys_tkill(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if ((int32_t)a[0] <= 0)
        return -L_EINVAL;
    lx_thread *t = find_thread((int32_t)a[0]);
    return t ? send_to(t, (int)a[1], L_SI_TKILL) : -L_ESRCH;
}

static int64_t sys_tgkill(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if ((int32_t)a[0] <= 0 || (int32_t)a[1] <= 0)
        return -L_EINVAL;
    if ((int32_t)a[0] != lx.pid)
        return -L_ESRCH;
    lx_thread *t = find_thread((int32_t)a[1]);
    return t ? send_to(t, (int)a[2], L_SI_TKILL) : -L_ESRCH;
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
            if (lx_cur()->pending[s].queued && (set & bit(s))) {
                lx_pending_sig p = lx_cur()->pending[s];
                lx_cur()->pending[s].queued = false;
                if (a[1]) {
                    uint8_t info[L_SIGINFO_BYTES] = { 0 };
                    int32_t si[4] = { s, 0, p.code, 0 };
                    memcpy(info, si, 16);
                    memcpy(info + lx_siginfo_union_offset(), &p.pid, 4);
                    memcpy(info + lx_siginfo_union_offset() + 4, &p.uid, 4);
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
        if (!lx_get_timespec(a[2], &ts))
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
    (void)c;
    if (a[1] != L_SIGSET_BYTES)
        return -L_EINVAL;
    uint64_t set;
    if (!lx_get(&set, a[0], 8))
        return -L_EFAULT;
    uint64_t old = lx_cur()->blocked;
    lx_cur()->blocked = set & ~unblockable();
    while (lx_wait(-1, 0, -1) != -L_EINTR) {
    }
    int rc = lx_signal_deliver(c, mem);
    lx_cur()->blocked = old;
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

int lx_safepoint(rl_ctx *c, uint8_t *mem)
{
    if (!lx_signal_ready())
        return RL_OK;
    lx_cur()->resume_bias = 0;
    return lx_signal_deliver_from(c, mem);
}
