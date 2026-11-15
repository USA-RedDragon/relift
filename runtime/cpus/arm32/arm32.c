/**
 * @file arm32.c
 * @brief The ARMv4T CPU layer: the descriptor the core allocates contexts
 * from, the CPSR with the mode banks as relift-cpu-arm32's lifted code keeps
 * them, exception entry and return, the I bit's interrupt mask, the return
 * address stack of lifted calls, the run loop, dispatch misses and register
 * summaries for crash reports.
 */
#include "arm32.h"

#include <stdio.h>

static rl_arm32_stats g_stats;
static uint64_t g_irq_lines;
static uint64_t g_other_lines;

rl_arm32_stats *rl_arm32_get_stats(void) { return &g_stats; }

static bool valid_mode(uint32_t m)
{
    switch (m & 0x1Fu) {
    case RL_ARM32_USR:
    case RL_ARM32_FIQ:
    case RL_ARM32_IRQ:
    case RL_ARM32_SVC:
    case RL_ARM32_ABT:
    case RL_ARM32_UND:
    case RL_ARM32_SYS:
        return true;
    default:
        return false;
    }
}

/** @brief The bank slot of r[n] (8 to 14) for mode m, or NULL for mode bits that name no mode. */
static uint32_t *bank_slot(rl_ctx *c, uint32_t m, unsigned n)
{
    m &= 0x1Fu;
    if (!valid_mode(m))
        return NULL;
    if (m == RL_ARM32_FIQ)
        return &c->bank_fiq[n - 8];
    if (m == RL_ARM32_USR || m == RL_ARM32_SYS || n < 13)
        return &c->bank_usr[n - 8];
    switch (m) {
    case RL_ARM32_IRQ:
        return &c->bank_irq[n - 13];
    case RL_ARM32_SVC:
        return &c->bank_svc[n - 13];
    case RL_ARM32_ABT:
        return &c->bank_abt[n - 13];
    default:
        return &c->bank_und[n - 13];
    }
}

uint32_t *rl_arm32_banked(rl_ctx *c, uint32_t m, unsigned n)
{
    if (n < 8 || n > 14)
        return n < 8 ? &c->r[n] : NULL;
    uint32_t *want = bank_slot(c, m, n);
    uint32_t *cur = bank_slot(c, c->m, n);
    if (!want || want == cur)
        return &c->r[n];
    return want;
}

void rl_arm32_set_mode(rl_ctx *c, uint32_t m)
{
    m &= 0x1Fu;
    uint32_t old = c->m;
    if (m == old)
        return;
    for (unsigned n = 8; n < 15; n++) {
        uint32_t *was = bank_slot(c, old, n);
        uint32_t *will = bank_slot(c, m, n);
        if (was == will)
            continue;
        if (was)
            *was = c->r[n];
        if (will)
            c->r[n] = *will;
    }
    c->m = (uint8_t)m;
}

uint32_t *rl_arm32_spsr(rl_ctx *c, uint32_t m)
{
    switch (m & 0x1Fu) {
    case RL_ARM32_FIQ:
        return &c->spsr[0];
    case RL_ARM32_IRQ:
        return &c->spsr[1];
    case RL_ARM32_SVC:
        return &c->spsr[2];
    case RL_ARM32_ABT:
        return &c->spsr[3];
    case RL_ARM32_UND:
        return &c->spsr[4];
    default:
        return NULL;
    }
}

uint32_t rl_arm32_cpsr(const rl_ctx *c)
{
    return (uint32_t)c->n << 31 | (uint32_t)c->z << 30 | (uint32_t)c->c << 29 | (uint32_t)c->v << 28 | c->psr_rsvd |
           (uint32_t)c->i << 7 | (uint32_t)c->f << 6 | (uint32_t)c->t << 5 | (c->m & 0x1Fu);
}

static void apply_mask(rl_ctx *c) { rl_set_irq_mask(c, g_other_lines | (c->i ? 0u : g_irq_lines)); }

void rl_arm32_set_lines(rl_ctx *c, uint64_t irq, uint64_t other)
{
    g_irq_lines = irq;
    g_other_lines = other;
    apply_mask(c);
}

void rl_arm32_set_i(rl_ctx *c, uint32_t i)
{
    if ((i & 1u) == c->i)
        return;
    c->i = (uint8_t)(i & 1u);
    apply_mask(c);
}

void rl_arm32_set_cpsr(rl_ctx *c, uint32_t v)
{
    c->n = (uint8_t)(v >> 31 & 1u);
    c->z = (uint8_t)(v >> 30 & 1u);
    c->c = (uint8_t)(v >> 29 & 1u);
    c->v = (uint8_t)(v >> 28 & 1u);
    c->psr_rsvd = v & 0x0FFFFF00u;
    c->f = (uint8_t)(v >> 6 & 1u);
    c->t = (uint8_t)(v >> 5 & 1u);
    rl_arm32_set_mode(c, v);
    rl_arm32_set_i(c, v >> 7 & 1u);
}

void rl_arm32_exception(rl_ctx *c, uint32_t m, uint32_t lr)
{
    uint32_t old = rl_arm32_cpsr(c);
    rl_arm32_set_mode(c, m);
    uint32_t *s = rl_arm32_spsr(c, m);
    if (s)
        *s = old;
    c->r[14] = lr;
    c->t = 0;
    if ((m & 0x1Fu) == RL_ARM32_FIQ)
        c->f = 1;
    rl_arm32_set_i(c, 1);
}

void rl_arm32_restore(rl_ctx *c)
{
    uint32_t *s = rl_arm32_spsr(c, c->m);
    if (s)
        rl_arm32_set_cpsr(c, *s);
}

const char *rl_arm32_mode_name(uint32_t m)
{
    switch (m & 0x1Fu) {
    case RL_ARM32_USR:
        return "usr";
    case RL_ARM32_FIQ:
        return "fiq";
    case RL_ARM32_IRQ:
        return "irq";
    case RL_ARM32_SVC:
        return "svc";
    case RL_ARM32_ABT:
        return "abt";
    case RL_ARM32_UND:
        return "und";
    case RL_ARM32_SYS:
        return "sys";
    default:
        return "???";
    }
}

int rl_arm32_return(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->depth--;
    g_stats.returns++;
    return RL_OK;
}

int rl_arm32_redirect(rl_ctx *c, uint32_t pc)
{
    rl_pending *p = &rl_common(c)->pend;
    p->kind = RL_PEND_REDIRECT;
    p->pc = pc;
    p->sp = RL_SP_NONE;
    p->code = 0;
    return RL_PENDING;
}

static int enter(rl_ctx *c, uint8_t *mem, uint32_t pc)
{
    c->pc = pc;
    rl_fn fn = rl_dispatch_find(pc);
    return fn ? fn(c, mem) : rl_dispatch_miss(c, mem);
}

int rl_arm32_call_guest(rl_ctx *c, uint8_t *mem, uint32_t pc, uint32_t ret)
{
    uint32_t depth = c->depth;
    rl_arm32_call(c, ret);
    if (c->depth > RL_ARM32_SHADOW) {
        c->depth = depth;
        g_stats.overflows++;
        return rl_guest_fatal(c, "arm32", "%u host frames are open at once", depth);
    }
    int r = enter(c, mem, pc);
    if (r == RL_OK && c->depth != depth)
        return rl_guest_fatal(c, "arm32", "the code at 0x%08x left without returning to 0x%08x", pc, ret);
    return r;
}

int rl_arm32_run(rl_ctx *c, uint8_t *mem, uint64_t pc)
{
    for (;;) {
        c->depth = 0;
        int r = enter(c, mem, (uint32_t)pc);
        rl_pending *p = &rl_common(c)->pend;
        if (r == RL_OK || p->kind != RL_PEND_REDIRECT)
            return r;
        g_stats.redirects++;
        pc = p->pc;
        p->kind = RL_PEND_NONE;
        p->sp = RL_SP_NONE;
    }
}

int rl_arm32_cond_fail(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t thumb)
{
    (void)mem;
    rl_arm32_fetch(c, (uint32_t)pc, thumb);
    return RL_OK;
}

static void describe(const rl_ctx *c, char *buf, size_t n)
{
    int k = snprintf(buf, n, "cpsr %08x (%s%s%s%s %s%s%s %s)", rl_arm32_cpsr(c), c->n ? "N" : "-", c->z ? "Z" : "-",
                     c->c ? "C" : "-", c->v ? "V" : "-", c->i ? "I" : "-", c->f ? "F" : "-", c->t ? "T" : "-",
                     rl_arm32_mode_name(c->m));
    for (unsigned i = 0; i < 15 && k > 0 && (size_t)k < n; i++)
        k += snprintf(buf + k, n - (size_t)k, ", r%u %08x", i, c->r[i]);
    if (k > 0 && (size_t)k < n)
        snprintf(buf + k, n - (size_t)k, ", pc %08x, frames %u", c->pc, c->depth);
}

static void ctx_init(rl_ctx *c)
{
    c->m = RL_ARM32_SVC;
    c->i = 1;
    c->f = 1;
}

static const rl_cpu_desc g_arm32 = {
    .name = "arm32",
    .ctx_size = sizeof(rl_ctx),
    .ctx_align = _Alignof(rl_ctx),
    .common_offset = RL_ARM32_COMMON_OFFSET,
    .code_shift = RL_ARM32_CODE_SHIFT,
    .big_endian = false,
    .time_offset = RL_ARM32_TIME_OFFSET,
    .ctx_init = ctx_init,
    .describe = describe,
};

const rl_cpu_desc *rl_arm32_cpu(void) { return &g_arm32; }

int rl_dispatch_miss(rl_ctx *c, uint8_t *mem)
{
    g_stats.misses++;
    return rl_core_miss(c, mem, c->pc);
}

int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn)
{
    (void)mem;
    return rl_guest_fatal(c, "guest", "undefined instruction 0x%08x at guest pc 0x%08llx (%s state)", insn,
                          (unsigned long long)pc, c->t ? "Thumb" : "ARM");
}
