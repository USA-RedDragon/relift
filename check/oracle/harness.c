/*
 * The native oracle: runs A64 instruction words on a real (or QEMU user
 * mode) AArch64 CPU and reports the state after them, for relift-check.
 *
 * Build: check/oracle/build.sh. Protocol: check/oracle/README.md and
 * check/src/oracle.rs. Requests arrive on stdin and results leave on
 * stdout, in order, until end of input.
 *
 * Entry and exit go through signals: a BRK in the harness raises SIGTRAP,
 * whose handler writes the whole guest state into the signal context, so
 * the kernel's return from the handler starts the guest words with exactly
 * that state. The words end in a trap of their own (the zero word after
 * them is UDF #0, a fetch from an unmapped address faults, a branch to
 * itself ends on a timer), and that
 * handler reads the whole state back from its signal context and
 * siglongjmps out. Handlers run on an alternate stack, since the guest SP
 * points into the data window.
 */
#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#include <unistd.h>

#if !defined(__aarch64__)
#error "the oracle harness runs on AArch64 only"
#endif

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif

#include "proto.h"

static gstate g_in, g_out;
static volatile sig_atomic_t g_phase; /* 0 idle, 1 entering, 2 running */
static volatile int g_sig;
static volatile uint64_t g_addr;
static volatile uint64_t g_loop_pc; /* where a branch to itself would spin */
static sigjmp_buf g_env;
static uint64_t g_host_tpidr;
static uint8_t g_window[DATA_SIZE];

/* The BRK the harness enters through, at a known address. */
#if defined(__APPLE__)
#define ASM_NAME "_rl_oracle_enter"
#else
#define ASM_NAME "rl_oracle_enter"
#endif
__asm__(".text\n.globl " ASM_NAME "\n.p2align 2\n" ASM_NAME ":\n\tbrk #0x5247\n\tret\n");
void rl_oracle_enter(void);
#define enter_guest rl_oracle_enter

static uint64_t read_tpidr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

static void write_tpidr(uint64_t v) { __asm__ volatile("msr tpidr_el0, %0" ::"r"(v)); }

static void set_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" ::"r"(v)); }

/* The signal context's registers, for both kernels. */
#if defined(__APPLE__)
#define CTX_X(uc, i) ((uc)->uc_mcontext->__ss.__x[i])
#define CTX_FP(uc) ((uc)->uc_mcontext->__ss.__fp)
#define CTX_LR(uc) ((uc)->uc_mcontext->__ss.__lr)
#define CTX_SP(uc) ((uc)->uc_mcontext->__ss.__sp)
#define CTX_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
#define CTX_PSTATE(uc) ((uc)->uc_mcontext->__ss.__cpsr)

static uint64_t ctx_x(ucontext_t *uc, int i)
{
    if (i < 29)
        return CTX_X(uc, i);
    return i == 29 ? CTX_FP(uc) : CTX_LR(uc);
}

static void ctx_set_x(ucontext_t *uc, int i, uint64_t v)
{
    if (i < 29)
        CTX_X(uc, i) = v;
    else if (i == 29)
        CTX_FP(uc) = v;
    else
        CTX_LR(uc) = v;
}

static unsigned __int128 *ctx_v(ucontext_t *uc) { return (unsigned __int128 *)uc->uc_mcontext->__ns.__v; }
static uint32_t *ctx_fpsr(ucontext_t *uc) { return &uc->uc_mcontext->__ns.__fpsr; }
static uint32_t *ctx_fpcr(ucontext_t *uc) { return &uc->uc_mcontext->__ns.__fpcr; }
#else
struct fpsimd_ctx {
    uint32_t magic;
    uint32_t size;
    uint32_t fpsr;
    uint32_t fpcr;
    unsigned __int128 vregs[32];
};
#define FPSIMD_CTX_MAGIC 0x46508001u

static struct fpsimd_ctx *fpsimd_of(ucontext_t *uc)
{
    uint8_t *p = (uint8_t *)&uc->uc_mcontext.__reserved;
    for (;;) {
        struct fpsimd_ctx *h = (struct fpsimd_ctx *)p;
        if (h->magic == FPSIMD_CTX_MAGIC)
            return h;
        if (h->magic == 0x53564501u && h->size > 16) {
            /* With live SVE state the kernel restores Z, not V, so the
             * V values written here would be lost. */
            static const char m[] = "harness: SVE state in the signal frame; run QEMU with sve=off,sme=off\n";
            write(2, m, sizeof m - 1);
            _exit(1);
        }
        if (h->magic == 0 || h->size == 0)
            abort();
        p += h->size;
    }
}

#define CTX_SP(uc) ((uc)->uc_mcontext.sp)
#define CTX_PC(uc) ((uc)->uc_mcontext.pc)
#define CTX_PSTATE(uc) ((uc)->uc_mcontext.pstate)
static uint64_t ctx_x(ucontext_t *uc, int i) { return uc->uc_mcontext.regs[i]; }
static void ctx_set_x(ucontext_t *uc, int i, uint64_t v) { uc->uc_mcontext.regs[i] = v; }
static unsigned __int128 *ctx_v(ucontext_t *uc) { return fpsimd_of(uc)->vregs; }
static uint32_t *ctx_fpsr(ucontext_t *uc) { return &fpsimd_of(uc)->fpsr; }
static uint32_t *ctx_fpcr(ucontext_t *uc) { return &fpsimd_of(uc)->fpcr; }
#endif

static void capture(ucontext_t *uc)
{
    for (int i = 0; i < 31; i++)
        g_out.x[i] = ctx_x(uc, i);
    g_out.sp = CTX_SP(uc);
    g_out.pc = CTX_PC(uc);
    g_out.nzcv = (uint32_t)CTX_PSTATE(uc) & 0xf0000000u;
    g_out.fpsr = *ctx_fpsr(uc);
    g_out.fpcr = *ctx_fpcr(uc);
    memcpy(g_out.v, ctx_v(uc), sizeof g_out.v);
}

static int in_code(uint64_t a) { return a >= CODE_MAP && a < CODE_MAP + CODE_SIZE; }
static int in_data(uint64_t a) { return a >= DATA && a < DATA + DATA_SIZE; }

static void on_signal(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;
    if (g_phase == 1 && sig == SIGTRAP && CTX_PC(uc) == (uint64_t)(uintptr_t)&enter_guest) {
        for (int i = 0; i < 31; i++)
            ctx_set_x(uc, i, g_in.x[i]);
        CTX_SP(uc) = g_in.sp;
        CTX_PC(uc) = g_in.pc;
        CTX_PSTATE(uc) = (CTX_PSTATE(uc) & ~(uint64_t)0xf0000000u) | g_in.nzcv;
        *ctx_fpsr(uc) = g_in.fpsr;
        *ctx_fpcr(uc) = g_in.fpcr;
        memcpy(ctx_v(uc), g_in.v, sizeof g_in.v);
        g_phase = 2;
        return;
    }
    if (sig == SIGALRM && (g_phase != 2 || CTX_PC(uc) != g_loop_pc)) {
        /* Not a branch to itself (QEMU may still be translating): wait on. */
        if (g_phase == 2) {
            struct itimerval t = {{0, 0}, {0, 50000}};
            setitimer(ITIMER_REAL, &t, NULL);
        }
        return;
    }
    if (g_phase != 2) {
        /* A fault in the harness itself. */
        signal(sig, SIG_DFL);
        return;
    }
    g_phase = 0;
    write_tpidr(g_host_tpidr);
    capture(uc);
    g_sig = sig;
    g_addr = (uint64_t)(uintptr_t)si->si_addr;
    siglongjmp(g_env, 1);
}

static uint8_t *g_code;
static uint8_t *g_data;

static void place(const uint32_t *words, unsigned from, unsigned n)
{
    if (mprotect(g_code, CODE_SIZE, PROT_READ | PROT_WRITE))
        proto_die("mprotect rw");
    memset(g_code + (CODE - CODE_MAP), 0, 4 * MAX_WORDS);
    memcpy(g_code + (CODE - CODE_MAP) + 4 * from, words + from, 4 * (n - from));
    if (mprotect(g_code, CODE_SIZE, PROT_READ | PROT_EXEC))
        proto_die("mprotect rx");
#if defined(__APPLE__)
    sys_icache_invalidate(g_code + (CODE - CODE_MAP), 4 * MAX_WORDS);
#else
    __builtin___clear_cache((char *)g_code + (CODE - CODE_MAP), (char *)g_code + (CODE - CODE_MAP) + 4 * MAX_WORDS);
#endif
}

/* Runs from g_in.pc until the guest traps; 0 when it ended normally. */
static int run(void)
{
    struct itimerval t = {{0, 0}, {0, 50000}}, off = {{0, 0}, {0, 0}};
    g_sig = 0;
    if (sigsetjmp(g_env, 1) == 0) {
        setitimer(ITIMER_REAL, &t, NULL);
        g_phase = 1;
        enter_guest();
        abort(); /* not reached: the guest always traps */
    }
    setitimer(ITIMER_REAL, &off, NULL);
    set_fpcr(0);
    uint64_t pc = g_out.pc;
    switch (g_sig) {
    case SIGILL:
        /* UDF #0, the zero word after the sequence or any word the
         * sequence did not fill, is the end of the run. */
        return in_code(pc) && *(uint32_t *)(uintptr_t)pc == 0 ? 0 : 1;
    case SIGSEGV:
    case SIGBUS:
        /* A fetch from an unmapped address after a branch ends the run,
         * as in Unicorn; any other fault is a fault. */
        return g_addr == pc && !in_code(pc) && !in_data(pc) ? 0 : 1;
    case SIGALRM:
        /* A branch to itself. */
        return 0;
    default: return 1;
    }
}

/* SVC, HVC and SMC would reach the host kernel. */
static int refused(uint32_t w) { return (w & 0xffe0001cu) == 0xd4000000u && (w & 3u) != 0; }

int main(void)
{
    stack_t ss;
    ss.ss_size = 1 << 20;
    ss.ss_sp = malloc(ss.ss_size);
    ss.ss_flags = 0;
    if (!ss.ss_sp || sigaltstack(&ss, NULL))
        proto_die("sigaltstack");
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGTRAP, SIGILL, SIGSEGV, SIGBUS, SIGFPE, SIGALRM, SIGSYS};
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        if (sigaction(sigs[i], &sa, NULL))
            proto_die("sigaction");

    g_code = mmap((void *)(uintptr_t)CODE_MAP, CODE_SIZE, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    g_data = mmap((void *)(uintptr_t)DATA, DATA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_code != (uint8_t *)(uintptr_t)CODE_MAP || g_data != (uint8_t *)(uintptr_t)DATA) {
        fprintf(stderr, "harness: could not map the code and data regions at their addresses\n");
        return 1;
    }
    g_host_tpidr = read_tpidr();

    for (;;) {
        uint32_t words[MAX_WORDS];
        unsigned n;
        gstate in;
        if (!proto_read(words, &n, &in, g_window))
            break;
        memcpy(g_data, g_window, DATA_SIZE);

        uint32_t status = ST_DONE;
        for (unsigned i = 0; i < n; i++)
            if (refused(words[i]))
                status = ST_REFUSED;
        memset(&g_out, 0, sizeof g_out);
        if (status == ST_DONE) {
            /* When the last word is a branch or system instruction, the
             * words before it run first, alone; then it runs alone at its
             * address, so a branch back into the sequence lands on a zero
             * word and ends the run. Other sequences run in one go, which
             * keeps the exclusive monitor (an exception return clears
             * it). */
            g_in = in;
            g_in.pc = CODE;
            g_loop_pc = ((words[n - 1] >> 26) & 7u) == 5u ? CODE + 4 * (n - 1) : 1;
            if (n > 1 && ((words[n - 1] >> 26) & 7u) != 5u) {
                place(words, 0, n);
                if (run())
                    status = ST_FAULT;
            } else if (n > 1) {
                place(words, 0, n - 1);
                if (run() || g_out.pc != CODE + 4 * (n - 1)) {
                    status = ST_FAULT;
                } else {
                    g_in = g_out;
                }
            }
            if (status == ST_DONE && (n == 1 || ((words[n - 1] >> 26) & 7u) == 5u)) {
                place(words, n - 1, n);
                if (run())
                    status = ST_FAULT;
            }
        }
        proto_write(status, (uint32_t)g_sig, g_addr, &g_out, g_data, g_window);
    }
    return 0;
}
