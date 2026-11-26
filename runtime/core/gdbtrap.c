/**
 * @file gdbtrap.c
 * @brief The GDB server's host layer (gdbtrap.h): traps over lifted functions and a stepped host instruction, for
 * Linux on x86-64 and 64-bit Arm. The host CPU's trap instruction, signal context and step method live here, not in
 * gdbstub.c.
 */
#include "gdbtrap.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <ucontext.h>

static const rl_gdbtrap_hooks *g_hooks;

#if defined(__aarch64__)
/** @brief Bytes of the trap instruction: BRK #0x7e5, which gdb's own breakpoints never use. */
#define TRAP_LEN 4
static const uint32_t TRAP_INSN = 0xd4200000u | 0x7e5u << 5;
#else
#define TRAP_LEN 1
static const uint32_t TRAP_INSN = 0xccu;
#endif

/** @brief Writes the TRAP_LEN bytes of v over host code at p (the page made writable for the write, then executable). */
static void poke_code(uint8_t *p, uint32_t v)
{
    uintptr_t pg = (uintptr_t)sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)p & ~(pg - 1));
    size_t len = (uintptr_t)p + TRAP_LEN - (uintptr_t)page <= pg ? pg : 2 * pg;
    mprotect(page, len, PROT_READ | PROT_WRITE | PROT_EXEC);
#if defined(__aarch64__)
    __atomic_store_n((uint32_t *)(void *)p, v, __ATOMIC_SEQ_CST);
    __builtin___clear_cache((char *)p, (char *)p + 4);
#else
    __atomic_store_n(p, (uint8_t)v, __ATOMIC_SEQ_CST);
#endif
    mprotect(page, len, PROT_READ | PROT_EXEC);
}

/** @brief The host instruction bytes at p that a trap replaces. */
static uint32_t code_at(const uint8_t *p)
{
    uint32_t v = 0;
    memcpy(&v, p, TRAP_LEN);
    return v;
}

static struct sigaction g_old_trap;

/** @brief A SIGTRAP that is not the stub's: to the handler that was there before, or the default action. */
static void pass_on(int sig, siginfo_t *si, void *uc)
{
    if (g_old_trap.sa_flags & SA_SIGINFO && g_old_trap.sa_sigaction)
        g_old_trap.sa_sigaction(sig, si, uc);
    else if (g_old_trap.sa_handler != SIG_DFL && g_old_trap.sa_handler != SIG_IGN)
        g_old_trap.sa_handler(sig);
    else {
        signal(SIGTRAP, SIG_DFL);
        raise(SIGTRAP);
    }
}

#if defined(__x86_64__)
/** @brief Per host thread: a breakpoint (index + 1) or a watched page to set again after one stepped instruction. */
static _Thread_local int t_reinsert;
static _Thread_local uint64_t t_rewatch;
static _Thread_local bool t_rewatch_on;
/** @brief The guest address of a watched store the stepped instruction made, 0 for none: stops after the store. */
static _Thread_local uint64_t t_watch_hit;

static void set_tf(ucontext_t *u, bool on)
{
    if (on)
        u->uc_mcontext.gregs[REG_EFL] |= 0x100;
    else
        u->uc_mcontext.gregs[REG_EFL] &= ~(greg_t)0x100;
}

static void on_trap(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;
    uint8_t *rip = (uint8_t *)u->uc_mcontext.gregs[REG_RIP];
    if (t_reinsert || t_rewatch_on) {
        /* The one stepped instruction is done: put the breakpoint or the watch back. */
        if (t_reinsert) {
            g_hooks->break_rearm(t_reinsert);
            t_reinsert = 0;
        }
        if (t_rewatch_on) {
            g_hooks->watch_close(t_rewatch);
            t_rewatch_on = false;
        }
        set_tf(u, false);
        uint64_t hit = t_watch_hit;
        t_watch_hit = 0;
        if (hit)
            g_hooks->watch_hit(hit);
        return;
    }
    int slot = g_hooks->break_at(rip - 1);
    if (slot) {
        u->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)(rip - 1);
        /* A lifted function is entered as fn(rl_ctx *c, uint8_t *mem): c is in rdi, and its registers are exact. */
        rl_ctx *c = (rl_ctx *)(uintptr_t)u->uc_mcontext.gregs[REG_RDI];
        if (g_hooks->break_hit(slot, c)) {
            poke_code(rip - 1, g_hooks->break_orig(slot));
            t_reinsert = slot;
            set_tf(u, true);
        }
        return;
    }
    pass_on(sig, si, uc);
}

/** @brief The debugger's fault hook: a write to a watched page stops the guest when it hits a watched range. */
static bool on_watch_fault(const rl_fault *f, void *uc)
{
    uint64_t page;
    bool hit;
    if (!g_hooks->watch_fault(f, &page, &hit))
        return false;
    /* Let the store happen: open the page for one host instruction, watch it again, and stop after a hit, as a
     * data breakpoint does, so the debugger sees the new value. */
    t_watch_hit = hit ? f->guest_addr : 0;
    t_rewatch = page;
    t_rewatch_on = true;
    set_tf(uc, true);
    return true;
}

#else

/*
 * docgen:region arm64-step
 * arm64 has no trap flag a signal handler can set (single-step is ptrace's), so one host instruction is stepped by
 * planting a second trap at every instruction that can run after it: the next one, or for a branch its targets from
 * the saved registers (B and BL, both ways of B.cond, CBZ, CBNZ, TBZ and TBNZ, and the register of BR, BLR and RET).
 * The instruction itself runs in place, so PC-relative ones (ADR, ADRP, LDR literal) are right. When one of the
 * planted traps is hit, all of them go and the breakpoint is set again, or the watched page closed again and a hit
 * reported.
 * docgen:end
 */

/** @brief Planted step traps at once (two per conditional branch, one per thread stepping). */
#define MAX_STEPS 32

/** @brief A step trap: where, the instruction under it, the step it belongs to and what finishing the step does. */
typedef struct step_trap {
    uint32_t *at;
    uint32_t orig;
    /** @brief The step: traps of one step (a branch's two ways) share it; 0 is a free slot. */
    uint32_t step;
    /** @brief The trap is another's (a breakpoint's): finishing the step leaves the instruction alone. */
    bool shared;
    /** @brief Breakpoint to set again (index + 1), or 0. */
    int rearm;
    /** @brief Watched page to close again, and the guest address of a watched store to report (0 for none). */
    bool rewatch;
    uint64_t page, hit;
} step_trap;

static step_trap g_steps[MAX_STEPS];
static uint32_t g_step_seq;
static int g_step_lock;

static void step_lock(void)
{
    while (__atomic_exchange_n(&g_step_lock, 1, __ATOMIC_ACQUIRE))
        ;
}

static void step_unlock(void) { __atomic_store_n(&g_step_lock, 0, __ATOMIC_RELEASE); }

static int64_t sext(uint64_t v, unsigned bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

/** @brief The addresses that can run after the instruction insn at pc, given the saved registers x; returns how many. */
static int successors(uint32_t insn, uint64_t pc, const uint64_t *x, uint64_t out[2])
{
    if ((insn & 0x7c000000u) == 0x14000000u) { /* B, BL */
        out[0] = pc + (uint64_t)(sext(insn & 0x3ffffffu, 26) * 4);
        return 1;
    }
    if ((insn & 0xff000010u) == 0x54000000u || (insn & 0x7e000000u) == 0x34000000u) { /* B.cond, CBZ, CBNZ */
        out[0] = pc + 4;
        out[1] = pc + (uint64_t)(sext(insn >> 5 & 0x7ffffu, 19) * 4);
        return 2;
    }
    if ((insn & 0x7e000000u) == 0x36000000u) { /* TBZ, TBNZ */
        out[0] = pc + 4;
        out[1] = pc + (uint64_t)(sext(insn >> 5 & 0x3fffu, 14) * 4);
        return 2;
    }
    if ((insn & 0xff9ffc1fu) == 0xd61f0000u || (insn & 0xfffffc1fu) == 0xd65f0000u) { /* BR, BLR, RET */
        unsigned rn = insn >> 5 & 31u;
        out[0] = rn == 31 ? 0 : x[rn];
        return out[0] ? 1 : 0;
    }
    out[0] = pc + 4;
    return 1;
}

/** @brief Plants the traps that end a step of insn at pc; the step then does what t says. Takes the lock. */
static void step_over(uint32_t insn, uint64_t pc, const uint64_t *x, step_trap t)
{
    uint64_t next[2];
    int n = successors(insn, pc, x, next);
    step_lock();
    t.step = ++g_step_seq ? g_step_seq : ++g_step_seq;
    for (int k = 0; k < n; k++) {
        uint32_t *at = (uint32_t *)(uintptr_t)next[k];
        int slot = -1;
        bool shared = false;
        for (int i = 0; i < MAX_STEPS; i++) {
            if (g_steps[i].step && g_steps[i].at == at)
                shared = true;
            if (!g_steps[i].step && slot < 0)
                slot = i;
        }
        shared = shared || g_hooks->break_at((const uint8_t *)at) != 0;
        if (slot < 0)
            break;
        step_trap *s = &g_steps[slot];
        *s = t;
        s->at = at;
        s->shared = shared;
        s->rearm = k == 0 ? t.rearm : 0;
        s->rewatch = k == 0 && t.rewatch;
        s->hit = k == 0 ? t.hit : 0;
        if (!shared) {
            s->orig = *at;
            poke_code((uint8_t *)at, TRAP_INSN);
        }
    }
    step_unlock();
}

/** @brief Finishes the steps whose trap is at pc; true when one was. A hit of a watched store parks after it. */
static bool step_done(uint8_t *pc)
{
    bool any = false;
    uint64_t hit = 0;
    step_lock();
    for (int i = 0; i < MAX_STEPS; i++) {
        if (!g_steps[i].step || (uint8_t *)g_steps[i].at != pc)
            continue;
        any = true;
        uint32_t step = g_steps[i].step;
        for (int k = 0; k < MAX_STEPS; k++) {
            step_trap *s = &g_steps[k];
            if (s->step != step)
                continue;
            if (!s->shared)
                poke_code((uint8_t *)s->at, s->orig);
            if (s->rearm)
                g_hooks->break_rearm(s->rearm);
            if (s->rewatch)
                g_hooks->watch_close(s->page);
            hit = hit ? hit : s->hit;
            s->step = 0;
        }
    }
    step_unlock();
    if (hit)
        g_hooks->watch_hit(hit);
    return any;
}

static void on_trap(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;
    uint8_t *pc = (uint8_t *)(uintptr_t)u->uc_mcontext.pc;
    bool stepped = step_done(pc);
    int slot = g_hooks->break_at(pc);
    if (slot) {
        /* A lifted function is entered as fn(rl_ctx *c, uint8_t *mem): c is in x0, and its registers are exact. */
        if (g_hooks->break_hit(slot, (rl_ctx *)(uintptr_t)u->uc_mcontext.regs[0])) {
            uint32_t orig = g_hooks->break_orig(slot);
            poke_code(pc, orig);
            step_over(orig, (uint64_t)(uintptr_t)pc, (const uint64_t *)u->uc_mcontext.regs,
                      (step_trap){ .rearm = slot });
        }
        return;
    }
    if (!stepped)
        pass_on(sig, si, uc);
}

/** @brief The debugger's fault hook: a write to a watched page stops the guest when it hits a watched range. */
static bool on_watch_fault(const rl_fault *f, void *uc)
{
    uint64_t page;
    bool hit;
    if (!g_hooks->watch_fault(f, &page, &hit))
        return false;
    /* Let the store happen: open the page, step the store, then close the page and stop after a hit. */
    ucontext_t *u = uc;
    uint32_t *pc = (uint32_t *)(uintptr_t)u->uc_mcontext.pc;
    step_over(*pc, (uint64_t)(uintptr_t)pc, (const uint64_t *)u->uc_mcontext.regs,
              (step_trap){ .rewatch = true, .page = page, .hit = hit ? f->guest_addr : 0 });
    return true;
}

#endif

bool rl_gdbtrap_install(const rl_gdbtrap_hooks *hooks)
{
    g_hooks = hooks;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_trap;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, &g_old_trap);
    rl_fault_set_debug_hook(on_watch_fault);
    return true;
}

uint32_t rl_gdbtrap_arm(uint8_t *host)
{
    uint32_t orig = code_at(host);
    poke_code(host, TRAP_INSN);
    return orig;
}

void rl_gdbtrap_disarm(uint8_t *host, uint32_t orig) { poke_code(host, orig); }

#else

bool rl_gdbtrap_install(const rl_gdbtrap_hooks *hooks)
{
    (void)hooks;
    return false;
}

uint32_t rl_gdbtrap_arm(uint8_t *host)
{
    (void)host;
    return 0;
}

void rl_gdbtrap_disarm(uint8_t *host, uint32_t orig)
{
    (void)host;
    (void)orig;
}

#endif
