/**
 * @file arm64.c
 * @brief The A64 CPU layer: the descriptor the core allocates contexts from,
 * FPCR (docs/abi.md section 10.1), the system-instruction entries of section
 * 10, the dispatch miss entry, rl_resume (section 7) and register summaries
 * for crash reports.
 */
#include "arm64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_exact = -1;
static int g_fpcr_const;
static unsigned g_va_bits = 39;
int rl_arm64_fp_flags_on;
int rl_arm64_trampoline_on;

int rl_arm64_exact_flags(void)
{
    if (g_exact < 0) {
        const char *e = getenv("RELIFT_FP_EXACT");
        g_exact = e && e[0] == '1';
    }
    return g_exact;
}

int rl_arm64_fp_flags(void) { return rl_arm64_fp_flags_on; }

void rl_arm64_set_trampoline(int on) { rl_arm64_trampoline_on = on != 0; }

int rl_arm64_trampoline(void) { return rl_arm64_trampoline_on; }

void rl_arm64_set_fp_flags(int on) { rl_arm64_fp_flags_on = on != 0; }

void rl_arm64_set_fpcr_const(int on)
{
    g_fpcr_const = on != 0;
    if (g_fpcr_const && rl_arm64_exact_flags())
        rl_log(RL_LOG_WARN, "fpcr",
               "RELIFT_FP_EXACT=1 has no effect: this build proved the FP mode constant and has no FPCR guards; "
               "rebuild with emit.fpcr-guards = true");
}

int rl_arm64_fpcr_const(void) { return g_fpcr_const; }

static void ctx_init(rl_ctx *c)
{
    c->fpcr = RL_FPCR_DEFAULT | (rl_arm64_exact_flags() ? RL_FPCR_EXACT : 0u);
    c->time.budget = INT64_MAX;
}

static void bind(rl_ctx *c)
{
    rl_host_set_fpcr(c->fpcr);
    if (rl_arm64_fp_flags_on)
        rl_fp_host_clear();
}

static void describe(const rl_ctx *c, char *buf, size_t n)
{
    snprintf(buf, n, "lr 0x%llx, sp 0x%llx", (unsigned long long)c->x[30], (unsigned long long)c->sp);
}

static void regs(const rl_ctx *c, char *buf, size_t n)
{
    size_t k = 0;
    for (int i = 0; i < 31 && k < n; i++) {
        int w = snprintf(buf + k, n - k, "x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 4 == 3 || i == 30 ? "\n" : "  ");
        if (w < 0)
            break;
        k += (size_t)w;
    }
    if (k < n)
        snprintf(buf + k, n - k,
                 "sp  %016llx  pc  %016llx  nzcv %08x  fpcr %08x  fpsr %08x\ntpidr_el0 %016llx  tpidrro_el0 %016llx\n",
                 (unsigned long long)c->sp, (unsigned long long)c->pc, rl_nzcv(c), c->fpcr, c->fpsr,
                 (unsigned long long)c->tpidr_el0, (unsigned long long)c->tpidrro_el0);
}

static uint64_t call_site(const rl_ctx *c) { return c->x[30] >= 4 ? c->x[30] - 4 : RL_SITE_NONE; }

static const rl_cpu_desc g_arm64 = {
    .name = "arm64",
    .ctx_size = sizeof(rl_ctx),
    .ctx_align = _Alignof(rl_ctx),
    .common_offset = RL_ARM64_COMMON_OFFSET,
    .code_shift = RL_ARM64_CODE_SHIFT,
    .big_endian = false,
    .time_offset = offsetof(rl_ctx, time),
    .ctx_init = ctx_init,
    .bind = bind,
    .describe = describe,
    .regs = regs,
    .call_site = call_site,
};

const rl_cpu_desc *rl_arm64_cpu(void) { return &g_arm64; }

void rl_arm64_set_va_bits(unsigned bits) { g_va_bits = bits < 64 ? bits : 64; }

static int canonical(uint64_t a) { return g_va_bits >= 64 || a >> g_va_bits == 0; }

static int abort_report(rl_ctx *c, uint64_t to, uint64_t from, int how)
{
    static const char *const kind[] = { "from the function entered at", "from the call at", "from the return or jump at" };
    rl_fault_log_word_sites("the return address", to);
    return rl_guest_fatal(c, "guest",
                          "instruction abort: return to 0x%llx %s guest pc 0x%llx, outside the %u-bit address space (a "
                          "failed pointer authentication of a return address gives such an address)",
                          (unsigned long long)to, kind[how], (unsigned long long)from, g_va_bits);
}

int rl_call_guest(rl_ctx *c, uint8_t *mem, uint64_t entry)
{
    uint64_t ra = c->x[30];
    uint64_t pc = entry;
    uint64_t outer_ra = c->entry_ra;
    uint32_t outer_set = c->entry_set;
    c->entry_ra = ra;
    c->entry_set = 1;
    int result = RL_OK;
    for (;;) {
        c->pc = pc;
        uint64_t to = rl_lookup(c, pc)(c, mem);
        int call = 0;
        if (to == RL_PENDING && c->pend.kind != RL_PEND_NONE) {
            if (c->pend.kind == RL_PEND_RETURN) {
                c->pend.kind = RL_PEND_NONE;
                break;
            }
            if (c->pend.kind != RL_PEND_REDIRECT) {
                result = RL_PENDING;
                break;
            }
            to = c->pend.pc;
            c->pend.kind = RL_PEND_NONE;
            call = 1;
        }
        if (to == ra)
            break;
        if (!canonical(to)) {
            result = abort_report(c, to, call ? c->pc : pc, call ? 1 : 0);
            break;
        }
        pc = to;
    }
    c->entry_ra = outer_ra;
    c->entry_set = outer_set;
    return result;
}

uint64_t rl_arm64_ret_elsewhere(rl_ctx *c, uint64_t to, uint64_t pc)
{
    if (to != RL_PENDING || c->pend.kind == RL_PEND_NONE) {
        c->pc = pc;
        c->pend.pc = to;
        c->pend.code = 0;
        if (to != RL_PENDING && rl_dispatch_find_ctx(c, to) == rl_pad_resume) {
            c->pend.sp = c->sp;
            c->pend.kind = RL_PEND_UNWIND;
        } else {
            c->pend.sp = RL_SP_NONE;
            c->pend.kind = RL_PEND_REDIRECT;
        }
    }
    return RL_PENDING;
}

uint64_t rl_dispatch_miss(rl_ctx *c, uint8_t *mem)
{
    if (!canonical(c->pc)) {
        rl_fault_log_word_sites("the guest pc", c->pc);
        return rl_guest_fatal(c, "guest",
                              "instruction abort: fetch from guest pc 0x%llx, outside the %u-bit address space (a failed "
                              "pointer authentication of a return address gives such an address)",
                              (unsigned long long)c->pc, g_va_bits);
    }
    rl_miss_info m = rl_core_miss_classify(c, c->pc, (uintptr_t)__builtin_return_address(0));
    rl_fn f = rl_core_miss_load(c, &m, c->fpcr);
    if (f)
        RL_MUSTTAIL return f(c, mem);
    return rl_core_miss_at(c, mem, &m);
}

uint64_t rl_dispatch_jump_miss(rl_ctx *c, uint8_t *mem)
{
    const uint32_t call = c->pend.code == RL_CALL_MARK;
    c->pend.code = 0;
    if (c->entry_set && c->pc == c->entry_ra)
        return c->pc;
    if (!canonical(c->pc)) {
        abort_report(c, c->pc, c->pend.pc, 2);
        return RL_PENDING;
    }
    rl_miss_info m = { c->pc, c->pend.pc, call ? RL_MISS_CALL : RL_MISS_JUMP, 0 };
    c->pend.pc = 0;
    rl_fn f = rl_core_miss_load(c, &m, c->fpcr);
    if (f)
        RL_MUSTTAIL return f(c, mem);
    return rl_core_miss_at(c, mem, &m);
}

static rl_arm64_safepoint_hook *g_safepoint;

void rl_arm64_set_safepoint_hook(rl_arm64_safepoint_hook *hook) { g_safepoint = hook; }

uint64_t rl_arm64_preempt_resume(rl_ctx *c, uint8_t *mem)
{
    rl_preempt_clear(c);
    uint64_t pc = c->pc;
    if (g_safepoint && g_safepoint(c, mem) != RL_OK)
        return RL_PENDING;
    RL_JUMP(pc, pc);
}

uint64_t rl_dispatch_cold(rl_ctx *c, uint8_t *mem)
{
    rl_fn f = rl_core_cold(c->pc);
    if (f)
        RL_MUSTTAIL return f(c, mem);
    return rl_core_miss(c, mem, c->pc, (uintptr_t)__builtin_return_address(0));
}

int rl_resume(rl_ctx *c, uint8_t *mem, const rl_resume_regs *r)
{
    (void)mem;
    memcpy(c->x, r->x, sizeof c->x);
    c->sp = r->sp;
    for (int i = 0; i < 8; i++)
        c->v[8 + i].d[0] = r->d8_15[i];
    c->pend.pc = r->pc;
    c->pend.sp = rl_arm64_trampoline_on ? RL_SP_NONE : r->sp;
    c->pend.kind = rl_arm64_trampoline_on ? RL_PEND_REDIRECT : RL_PEND_UNWIND;
    c->pend.code = 0;
    return RL_PENDING;
}

uint64_t rl_pad_resume(rl_ctx *c, uint8_t *mem)
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
    return rl_ns_to_ticks(rl_guest_clock_ns());
}

uint64_t rl_mrs_cntfrq(void)
{
    const rl_guest_desc *g = rl_core_guest();
    return g ? g->tick_hz : RL_CNTFRQ;
}

void rl_msr_fpcr(rl_ctx *c, uint64_t v)
{
    uint32_t f = ((uint32_t)v & RL_FPCR_GUEST) | (c->fpcr & RL_FPCR_EXACT);
    c->fpcr = f;
    if (c->thread == rl_thread_current())
        rl_host_set_fpcr(f);
    if ((f & RL_FPCR_MODE) != (RL_FPCR_DEFAULT & RL_FPCR_MODE) && rl_once(0x4650435200000000ull | (f & RL_FPCR_MODE))) {
        if (g_fpcr_const)
            rl_log(RL_LOG_ERROR, "fpcr",
                   "guest set FPCR 0x%08x, which the build proved it never does: lifted code ignores the mode; "
                   "rebuild with emit.fpcr-guards = true and report it",
                   f);
        else
            rl_log(RL_LOG_WARN, "fpcr", "guest set FPCR 0x%08x: a mode the lifted code was not generated for", f);
    }
}

void rl_wfe(rl_ctx *c)
{
    (void)c;
    rl_host_yield();
}

const char *rl_arm64_insn_kind(uint32_t insn, char *buf, size_t n)
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
    return rl_guest_fatal(c, "breakpoint", "breakpoint BRK #%u", imm);
}

int rl_align_fault(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn)
{
    (void)mem;
    uint32_t rn = insn >> 5 & 31u;
    uint64_t base = rn == 31 ? c->sp : c->x[rn];
    char reg[8];
    if (rn == 31)
        snprintf(reg, sizeof reg, "sp");
    else
        snprintf(reg, sizeof reg, "x%u", rn);
    return rl_guest_stop_at(c, "alignment fault", pc,
                            "alignment fault: the atomic, ordered or exclusive access 0x%08x through %s = 0x%llx is not "
                            "naturally aligned, at guest pc 0x%llx",
                            insn, reg, (unsigned long long)base, (unsigned long long)pc);
}

int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn)
{
    char kind[32];
    (void)mem;
    return rl_guest_stop_at(c, "undefined instruction", pc, "undefined instruction 0x%08x (%s) at guest pc 0x%llx",
                            insn, rl_arm64_insn_kind(insn, kind, sizeof kind), (unsigned long long)pc);
}
