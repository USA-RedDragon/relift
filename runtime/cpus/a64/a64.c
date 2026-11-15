/**
 * @file a64.c
 * @brief The A64 CPU layer: the descriptor the core allocates contexts from,
 * FPCR (docs/abi.md section 10.1), the system-instruction entries of section
 * 10, the dispatch miss entry, rl_resume (section 7) and register summaries
 * for crash reports.
 */
#include "a64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_exact = -1;
int rl_a64_fp_flags_on;

int rl_a64_exact_flags(void)
{
    if (g_exact < 0) {
        const char *e = getenv("RELIFT_FP_EXACT");
        g_exact = e && e[0] == '1';
    }
    return g_exact;
}

int rl_a64_fp_flags(void) { return rl_a64_fp_flags_on; }

void rl_a64_set_fp_flags(int on) { rl_a64_fp_flags_on = on != 0; }

void rl_host_set_fpcr(uint32_t fpcr)
{
#if defined(__aarch64__)
    uint64_t v = fpcr & RL_FPCR_GUEST;
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

static void ctx_init(rl_ctx *c)
{
    c->fpcr = RL_FPCR_DEFAULT | (rl_a64_exact_flags() ? RL_FPCR_EXACT : 0u);
    c->time.budget = INT64_MAX;
}

static void bind(rl_ctx *c)
{
    rl_host_set_fpcr(c->fpcr);
    if (rl_a64_fp_flags_on)
        rl_fp_host_clear();
}

static void describe(const rl_ctx *c, char *buf, size_t n)
{
    snprintf(buf, n, "lr 0x%llx, sp 0x%llx", (unsigned long long)c->x[30], (unsigned long long)c->sp);
}

static const rl_cpu_desc g_a64 = {
    .name = "a64",
    .ctx_size = sizeof(rl_ctx),
    .ctx_align = _Alignof(rl_ctx),
    .common_offset = RL_A64_COMMON_OFFSET,
    .code_shift = RL_A64_CODE_SHIFT,
    .big_endian = false,
    .time_offset = offsetof(rl_ctx, time),
    .ctx_init = ctx_init,
    .bind = bind,
    .describe = describe,
};

const rl_cpu_desc *rl_a64_cpu(void) { return &g_a64; }

int rl_call_guest(rl_ctx *c, uint8_t *mem, uint64_t entry)
{
    c->pc = entry;
    return rl_lookup(c, entry)(c, mem);
}

int rl_dispatch_miss(rl_ctx *c, uint8_t *mem)
{
    rl_fn f = rl_core_miss_load(c, c->pc, c->fpcr);
    if (f)
        RL_MUSTTAIL return f(c, mem);
    return rl_core_miss(c, mem, c->pc);
}

int rl_resume(rl_ctx *c, uint8_t *mem, const rl_resume_regs *r)
{
    (void)mem;
    memcpy(c->x, r->x, sizeof c->x);
    c->sp = r->sp;
    for (int i = 0; i < 8; i++)
        c->v[8 + i].d[0] = r->d8_15[i];
    c->pend.pc = r->pc;
    c->pend.sp = r->sp;
    c->pend.kind = RL_PEND_UNWIND;
    c->pend.code = 0;
    return RL_PENDING;
}

int rl_pad_resume(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->pend.pc = c->pc;
    c->pend.sp = c->sp;
    c->pend.kind = RL_PEND_UNWIND;
    c->pend.code = 0;
    return RL_PENDING;
}

uint64_t rl_mrs_cntpct(rl_ctx *c)
{
    (void)c;
    return rl_ns_to_ticks(rl_clock_ns());
}

void rl_msr_fpcr(rl_ctx *c, uint64_t v)
{
    uint32_t f = ((uint32_t)v & RL_FPCR_GUEST) | (c->fpcr & RL_FPCR_EXACT);
    c->fpcr = f;
    if (c->thread == rl_thread_current())
        rl_host_set_fpcr(f);
    if ((f & RL_FPCR_MODE) != (RL_FPCR_DEFAULT & RL_FPCR_MODE) && rl_once(0x4650435200000000ull | (f & RL_FPCR_MODE)))
        rl_log(RL_LOG_WARN, "fpcr", "guest set FPCR 0x%08x: a mode the lifted code was not generated for", f);
}

void rl_wfe(rl_ctx *c)
{
    (void)c;
    rl_host_yield();
}

const char *rl_a64_insn_kind(uint32_t insn, char *buf, size_t n)
{
    if ((insn & 0xFFFF0000u) == 0)
        snprintf(buf, n, "UDF #%u", insn & 0xFFFFu);
    else if ((insn & 0xFFE0001Fu) == 0xD4200000u)
        snprintf(buf, n, "BRK #%u", (insn >> 5) & 0xFFFFu);
    else if ((insn & 0xFFE0001Fu) == 0xD4400000u)
        snprintf(buf, n, "HLT #%u", (insn >> 5) & 0xFFFFu);
    else
        snprintf(buf, n, "unliftable");
    return buf;
}

int rl_brk(rl_ctx *c, uint8_t *mem, uint32_t imm)
{
    (void)mem;
    return rl_guest_fatal(c, "guest", "breakpoint BRK #%u", imm);
}

int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn)
{
    char kind[32];
    (void)mem;
    return rl_guest_fatal(c, "guest", "undefined instruction 0x%08x (%s) at guest pc 0x%llx", insn,
                          rl_a64_insn_kind(insn, kind, sizeof kind), (unsigned long long)pc);
}
