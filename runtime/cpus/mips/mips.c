/**
 * @file mips.c
 * @brief The VR4300 CPU layer: the descriptor the core allocates contexts
 * from, CP0 exception entry and return, interrupts over the IP bits, Count
 * and Compare from the cycle clock, CACHE, the return address stack of
 * lifted calls, the run loop, dispatch misses and register summaries for
 * crash reports.
 */
#include "mips.h"

#include "fpu.h"

#include <stdio.h>

static rl_mips_stats g_stats;
static rl_mips_cache_hook *g_cache_hook;

void (*rl_mips_call_trace)(rl_ctx *c, uint64_t ret);

rl_mips_stats *rl_mips_get_stats(void) { return &g_stats; }

void rl_mips_set_cache_hook(rl_mips_cache_hook *hook) { g_cache_hook = hook; }

static uint64_t cycle(const rl_ctx *c) { return c->time.deadline - (uint64_t)c->time.budget; }

const char *rl_mips_exc_name(uint32_t code)
{
    static const char *const names[32] = {
        [0] = "Int",   [1] = "Mod", [2] = "TLBL", [3] = "TLBS", [4] = "AdEL", [5] = "AdES",  [6] = "IBE",
        [7] = "DBE",   [8] = "Sys", [9] = "Bp",   [10] = "RI",  [11] = "CpU", [12] = "Ov",   [13] = "Tr",
        [15] = "FPE", [23] = "WATCH",
    };
    const char *n = code < 32 ? names[code] : NULL;
    return n ? n : "???";
}

uint64_t rl_mips_exception(rl_ctx *c, uint32_t code, uint64_t pc, bool in_delay, uint32_t ce)
{
    g_stats.exceptions++;
    uint32_t cause = c->cp0_cause & ~(uint32_t)(RL_MIPS_CAUSE_EXC | RL_MIPS_CAUSE_CE);
    cause |= (code & 31u) << 2 | (ce & 3u) << 28;
    if (!(c->cp0_status & RL_MIPS_SR_EXL)) {
        c->cp0_epc = in_delay ? pc - 4u : pc;
        cause = in_delay ? cause | RL_MIPS_CAUSE_BD : cause & ~(uint32_t)RL_MIPS_CAUSE_BD;
        c->cp0_status |= RL_MIPS_SR_EXL;
    }
    c->cp0_cause = cause;
    uint64_t base = (c->cp0_status & RL_MIPS_SR_BEV) ? UINT64_C(0xFFFFFFFFBFC00200) : UINT64_C(0xFFFFFFFF80000000);
    return base + 0x180u;
}

uint64_t rl_mips_eret(rl_ctx *c)
{
    uint64_t to;
    if (c->cp0_status & RL_MIPS_SR_ERL) {
        to = c->cp0_error_epc;
        c->cp0_status &= ~(uint32_t)RL_MIPS_SR_ERL;
    } else {
        to = c->cp0_epc;
        c->cp0_status &= ~(uint32_t)RL_MIPS_SR_EXL;
    }
    c->cp0_ll_bit = 0;
    return to;
}

uint64_t rl_mips_reset(rl_ctx *c)
{
    memset(c->tlb, 0, sizeof c->tlb);
    c->cp0_status = RL_MIPS_SR_BEV | RL_MIPS_SR_ERL;
    c->cp0_wired = 0;
    c->cp0_config = UINT64_C(0x7006E463);
    c->wired_seen = 0;
    c->wired_cycle = cycle(c);
    rl_mips_set_count(c, 0);
    return RL_MIPS_RESET_VECTOR;
}

uint64_t rl_mips_nmi(rl_ctx *c, uint64_t pc)
{
    c->cp0_error_epc = pc;
    c->cp0_status = (c->cp0_status & ~(uint32_t)RL_MIPS_SR_TS) | RL_MIPS_SR_BEV | RL_MIPS_SR_SR | RL_MIPS_SR_ERL;
    return RL_MIPS_RESET_VECTOR;
}

/** @brief Reads a guest write of Count into the clock's base, then gives Count now, not wrapped. */
static uint64_t count_now(rl_ctx *c)
{
    uint64_t now = cycle(c);
    if (c->cp0_count != c->count_seen) {
        c->count_base = c->cp0_count;
        c->count_cycle = now;
        c->count_last = c->count_base;
        c->count_seen = c->cp0_count;
    }
    return c->count_base + (now - c->count_cycle) / 2u;
}

static void store_count(rl_ctx *c, uint64_t now)
{
    c->cp0_count = (uint32_t)now;
    c->count_seen = c->cp0_count;
}

void rl_mips_set_count(rl_ctx *c, uint32_t value)
{
    c->count_base = value;
    c->count_cycle = cycle(c);
    c->count_last = value;
    store_count(c, value);
}

uint32_t rl_mips_count(rl_ctx *c)
{
    uint64_t now = count_now(c);
    store_count(c, now);
    return (uint32_t)now;
}

/** @brief Sets Cause.IP7 when Count reached Compare since the last check. */
static void timer_check(rl_ctx *c)
{
    uint64_t now = count_now(c);
    uint64_t remaining = (uint32_t)(c->cp0_compare - (uint32_t)c->count_last);
    if (!remaining)
        remaining = UINT64_C(1) << 32;
    if (now - c->count_last >= remaining)
        c->cp0_cause |= RL_MIPS_CAUSE_IP7;
    c->count_last = now;
    store_count(c, now);
}

uint64_t rl_mips_timer_cycles(rl_ctx *c)
{
    uint64_t now = count_now(c);
    uint64_t remaining = (uint32_t)(c->cp0_compare - (uint32_t)now);
    if (!remaining)
        remaining = UINT64_C(1) << 32;
    return remaining * 2u - ((cycle(c) - c->count_cycle) & 1u);
}

bool rl_mips_irq_pending(const rl_ctx *c)
{
    uint32_t st = c->cp0_status;
    if ((st & (RL_MIPS_SR_IE | RL_MIPS_SR_EXL | RL_MIPS_SR_ERL)) != RL_MIPS_SR_IE)
        return false;
    return (c->cp0_cause & st & RL_MIPS_CAUSE_IP) != 0;
}

/** @brief Ends the budget at the current cycle so the next sync point runs. */
static void end_budget(rl_ctx *c)
{
    c->time.deadline = cycle(c);
    c->time.budget = 0;
}

void rl_mips_cp0_sync(rl_ctx *c)
{
    timer_check(c);
    rl_mips_random(c);
    uint32_t st = c->cp0_status;
    bool on = (st & (RL_MIPS_SR_IE | RL_MIPS_SR_EXL | RL_MIPS_SR_ERL)) == RL_MIPS_SR_IE;
    rl_set_irq_mask(c, on ? (st >> 8) & 0x7Cu : 0u);
    if (rl_mips_irq_pending(c))
        end_budget(c);
}

void rl_mips_set_ip(rl_ctx *c, uint32_t ip)
{
    c->cp0_cause = (c->cp0_cause & ~(uint32_t)0x7C00u) | (ip & 0x7Cu) << 8;
    rl_mips_cp0_sync(c);
}

/** @brief The frame stack entry of an interrupt entry: its return address with bit 0 set, which no JR matches. */
static uint64_t irq_frame(uint64_t next) { return next | 1u; }

uint64_t rl_mips_irq_enter(rl_ctx *c, uint64_t next, uint32_t ip)
{
    c->cp0_cause = (c->cp0_cause & ~(uint32_t)0x7C00u) | (ip & 0x7Cu) << 8;
    timer_check(c);
    if (!rl_mips_irq_pending(c))
        return RL_NO_IRQ;
    uint64_t v = rl_mips_exception(c, RL_MIPS_EXC_INT, next, false, 0);
    rl_mips_call(c, irq_frame(next));
    c->irq_open++;
    return v;
}

uint64_t rl_mips_irq_return(rl_ctx *c, uint8_t *mem, uint64_t expected)
{
    (void)mem;
    (void)expected;
    return c->pc;
}

int rl_mips_return(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->depth--;
    g_stats.returns++;
    return RL_OK;
}

int rl_mips_pending(rl_ctx *c, uint8_t *mem)
{
    (void)c;
    (void)mem;
    return RL_PENDING;
}

int rl_mips_redirect(rl_ctx *c, uint64_t pc)
{
    rl_pending *p = &rl_common(c)->pend;
    p->kind = RL_PEND_REDIRECT;
    p->pc = pc;
    p->sp = RL_SP_NONE;
    p->code = 0;
    return RL_PENDING;
}

rl_fn rl_mips_eret_to(rl_ctx *c, uint64_t target)
{
    c->pc = target;
    if (c->depth - 1u < RL_MIPS_SHADOW && c->shadow[c->depth - 1u] == irq_frame(target)) {
        c->irq_open--;
        return rl_mips_return;
    }
    if (c->irq_open) {
        rl_mips_redirect(c, target);
        return rl_mips_pending;
    }
    return rl_lookup(c, target);
}

void rl_mips_cache(rl_ctx *c, uint32_t op, uint64_t vaddr, uint32_t run)
{
    if ((run & 1u) && g_cache_hook)
        g_cache_hook(c, op & 31u, vaddr);
}

bool rl_mips_fpu(rl_ctx *c, uint32_t insn) { return rl_mips_fpu_eval(insn, c->cp0_status, &c->fcr31, c->f); }

static int enter(rl_ctx *c, uint8_t *mem, uint64_t pc)
{
    c->pc = pc;
    rl_fn fn = rl_dispatch_find(pc);
    return fn ? fn(c, mem) : rl_dispatch_miss(c, mem);
}

int rl_mips_run(rl_ctx *c, uint8_t *mem, uint64_t pc)
{
    for (;;) {
        c->depth = 0;
        c->irq_open = 0;
        int r = enter(c, mem, pc);
        rl_pending *p = &rl_common(c)->pend;
        if (r == RL_OK || p->kind != RL_PEND_REDIRECT)
            return r;
        g_stats.redirects++;
        pc = p->pc;
        p->kind = RL_PEND_NONE;
        p->sp = RL_SP_NONE;
    }
}

static void describe(const rl_ctx *c, char *buf, size_t n)
{
    int k = snprintf(buf, n, "status %08x cause %08x (%s) epc %016llx badvaddr %016llx", c->cp0_status,
                     c->cp0_cause, rl_mips_exc_name(c->cp0_cause >> 2 & 31u), (unsigned long long)c->cp0_epc,
                     (unsigned long long)c->cp0_bad_vaddr);
    for (unsigned i = 1; i < 32 && k > 0 && (size_t)k < n; i++)
        k += snprintf(buf + k, n - (size_t)k, ", r%u %016llx", i, (unsigned long long)c->r[i]);
    if (k > 0 && (size_t)k < n)
        snprintf(buf + k, n - (size_t)k, ", hi %016llx, lo %016llx, fcr31 %08x, pc %016llx, frames %u",
                 (unsigned long long)c->hi, (unsigned long long)c->lo, c->fcr31, (unsigned long long)c->pc,
                 c->depth);
}

static void ctx_init(rl_ctx *c) { rl_mips_reset(c); }

static const rl_cpu_desc g_mips = {
    .name = "mips",
    .ctx_size = sizeof(rl_ctx),
    .ctx_align = _Alignof(rl_ctx),
    .common_offset = RL_MIPS_COMMON_OFFSET,
    .code_shift = RL_MIPS_CODE_SHIFT,
    .big_endian = true,
    .time_offset = RL_MIPS_TIME_OFFSET,
    .ctx_init = ctx_init,
    .describe = describe,
};

const rl_cpu_desc *rl_mips_cpu(void) { return &g_mips; }

int rl_dispatch_miss(rl_ctx *c, uint8_t *mem)
{
    g_stats.misses++;
    return rl_core_miss(c, mem, c->pc, (uintptr_t)__builtin_return_address(0));
}

int rl_dispatch_jump_miss(rl_ctx *c, uint8_t *mem)
{
    g_stats.misses++;
    rl_miss_info m = { c->pc, c->pend.pc, RL_MISS_JUMP, 0 };
    c->pend.pc = 0;
    return rl_core_miss_at(c, mem, &m);
}

int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn)
{
    (void)mem;
    return rl_guest_stop_at(c, "undefined instruction", pc, "undefined instruction 0x%08x at guest pc 0x%016llx", insn,
                            (unsigned long long)pc);
}
