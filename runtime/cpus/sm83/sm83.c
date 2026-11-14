/**
 * @file sm83.c
 * @brief The SM83 CPU layer: the descriptor the core allocates contexts
 * from, the dispatch miss entry, undefined instruction and trap reports,
 * interrupt master enable and HALT (docs/abi.md section 13), the interrupt
 * entry and return hooks, the thread entry and register summaries for crash
 * reports.
 */
#include "sm83.h"

#include <stdio.h>

static void apply_mask(rl_ctx *c) { rl_set_irq_mask(c, c->ime ? (uint64_t)c->irq_lines : 0u); }

void rl_sm83_ei(rl_ctx *c)
{
    c->ime = 1;
    c->ei_delay = 1;
    apply_mask(c);
}

void rl_sm83_di(rl_ctx *c)
{
    c->ime = 0;
    c->ei_delay = 0;
    apply_mask(c);
}

void rl_sm83_reti(rl_ctx *c)
{
    c->ime = 1;
    c->ei_delay = 0;
    apply_mask(c);
}

void rl_sm83_set_ie(rl_ctx *c, uint8_t lines)
{
    c->irq_lines = lines & RL_SM83_IRQ_MASK;
    apply_mask(c);
}

int rl_sm83_halt(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    struct rl_thread *t = rl_common(c)->thread;
    rl_sched_cpu *k = t ? t->clock : NULL;
    if (!k || !c->ints)
        return rl_guest_fatal(c, "sm83", "HALT on a thread without a scheduler clock and interrupt registers");
    if (c->ints->pending(c->ints->opaque) & RL_SM83_IRQ_MASK)
        return RL_OK;
    rl_sched_set_mask(k, c->irq_lines);
    c->halted = 1;
    int woke = rl_sched_halt(k);
    c->halted = 0;
    apply_mask(c);
    if (!woke)
        return rl_guest_fatal(c, "sm83", "HALT with no interrupt that can wake the CPU");
    return RL_OK;
}

int rl_sm83_stop(rl_ctx *c, uint8_t *mem)
{
    struct rl_thread *t = rl_common(c)->thread;
    rl_sched_cpu *k = t ? t->clock : NULL;
    const rl_sm83_ints *ints = c->ints;
    if (!k || !ints)
        return rl_guest_fatal(c, "sm83", "STOP on a thread without a scheduler clock and interrupt registers");
    if (ints->pending(ints->opaque) & RL_SM83_IRQ_MASK)
        return 1;
    rl_sm83_stop_info r = {.kind = RL_SM83_STOP_MODE};
    if (ints->stop)
        ints->stop(ints->opaque, &r);
    switch (r.kind) {
    case RL_SM83_STOP_SWITCH:
        rl_sched_set_ratio(k, r.ratio);
        c->time.budget -= RL_SM83_STOP_STALL;
        return r.run_operand ? 1 : 0;
    case RL_SM83_STOP_HALT:
        (void)rl_sm83_halt(c, mem);
        return 0;
    default:
        rl_sched_set_mask(k, RL_SM83_IRQ_MASK);
        c->halted = 1;
        int woke = rl_sched_halt(k);
        c->halted = 0;
        apply_mask(c);
        if (!woke)
            return rl_guest_fatal(c, "sm83", "STOP with no joypad line that can wake the CPU");
        c->time.budget -= RL_SM83_STOP_WAKE_CYCLES;
        return 0;
    }
}

static uint64_t irq_enter(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    const rl_sm83_ints *ints = c->ints;
    if (!c->ime || !ints)
        return RL_NO_IRQ;
    uint8_t pend = ints->pending(ints->opaque) & RL_SM83_IRQ_MASK;
    if (!pend)
        return RL_NO_IRQ;
    if (c->ei_delay) {
        struct rl_thread *t = rl_common(c)->thread;
        c->ei_delay = 0;
        if (t && t->clock)
            rl_sched_break(t->clock);
        return RL_NO_IRQ;
    }
    uint32_t line = (uint32_t)__builtin_ctz(pend);
    ints->ack(ints->opaque, line);
    c->ime = 0;
    apply_mask(c);
    c->sp = (uint16_t)(c->sp - 2u);
    mem[c->sp] = (uint8_t)next;
    mem[(uint16_t)(c->sp + 1u)] = (uint8_t)(next >> 8);
    c->time.budget -= RL_SM83_IRQ_CYCLES;
    return RL_SM83_IRQ_VECTOR(line);
}

static uint64_t irq_return(rl_ctx *c, uint8_t *mem, uint64_t expected)
{
    (void)expected;
    uint16_t at = (uint16_t)(c->sp - 2u);
    return (uint64_t)mem[at] | ((uint64_t)mem[(uint16_t)(at + 1u)] << 8);
}

int rl_sm83_attach(rl_sched_cpu *k, struct rl_thread *t, const rl_sm83_ints *ints)
{
    if (rl_sched_attach(k, t) != 0)
        return -1;
    t->ctx->ints = ints;
    rl_sched_set_irq(k, irq_enter);
    rl_sched_set_irq_return(k, irq_return);
    return 0;
}

int rl_sm83_run(rl_ctx *c, uint8_t *mem, uint64_t pc)
{
    c->pc = (uint16_t)pc;
    return rl_run_from(c, mem, pc);
}

const char *rl_sm83_flags_str(uint8_t f, char *buf)
{
    buf[0] = f & 0x80u ? 'Z' : '-';
    buf[1] = f & 0x40u ? 'N' : '-';
    buf[2] = f & 0x20u ? 'H' : '-';
    buf[3] = f & 0x10u ? 'C' : '-';
    buf[4] = 0;
    return buf;
}

static void describe(const rl_ctx *c, char *buf, size_t n)
{
    char fl[5];
    snprintf(buf, n, "af %02x%02x %s, bc %02x%02x, de %02x%02x, hl %02x%02x, sp %04x, pc %04x, ime %u%s", c->a, c->f,
             rl_sm83_flags_str(c->f, fl), c->b, c->c, c->d, c->e, c->h, c->l, c->sp, c->pc, c->ime,
             c->halted ? ", halted" : "");
}

static void ctx_init(rl_ctx *c)
{
    c->ime = 0;
    c->ei_delay = 0;
}

static const rl_cpu_desc g_sm83 = {
    .name = "sm83",
    .ctx_size = sizeof(rl_ctx),
    .ctx_align = _Alignof(rl_ctx),
    .common_offset = RL_SM83_COMMON_OFFSET,
    .code_shift = RL_SM83_CODE_SHIFT,
    .big_endian = false,
    .time_offset = RL_SM83_TIME_OFFSET,
    .ctx_init = ctx_init,
    .describe = describe,
};

const rl_cpu_desc *rl_sm83_cpu(void) { return &g_sm83; }

int rl_dispatch_miss(rl_ctx *c, uint8_t *mem)
{
    return rl_core_miss(c, mem, c->pc);
}

int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn)
{
    (void)mem;
    if (insn > 0xFFu)
        return rl_guest_fatal(c, "guest", "undefined instruction 0x%04x at guest pc 0x%04llx", insn, (unsigned long long)pc);
    return rl_guest_fatal(c, "guest", "undefined instruction 0x%02x at guest pc 0x%04llx", insn, (unsigned long long)pc);
}

int rl_trap(rl_ctx *c, uint8_t *mem, uint32_t code)
{
    (void)mem;
    return rl_guest_fatal(c, "guest", "trap 0x%x", code);
}
