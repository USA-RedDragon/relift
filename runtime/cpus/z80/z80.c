/**
 * @file z80.c
 * @brief The Z80 CPU layer: the descriptor the core allocates contexts
 * from, the dispatch miss entry, undefined instruction and trap reports,
 * ports and HALT (docs/abi.md section 13), the interrupt entry and return
 * hooks, the thread entry and register summaries for crash reports.
 */
#include "z80.h"

#include <stdio.h>

#define LINES_ALL ((1u << RL_Z80_LINE_INT) | (1u << RL_Z80_LINE_NMI))

static uint8_t pending(const rl_ctx *c)
{
    return c->ints ? c->ints->pending(c->ints->opaque) : 0;
}

static int deliverable(const rl_ctx *c, uint8_t iff1)
{
    uint8_t p = pending(c);
    return (p & RL_Z80_PEND_NMI) || ((p & RL_Z80_PEND_INT) && iff1);
}

static uint64_t lines_for(uint8_t iff1)
{
    return iff1 ? LINES_ALL : 1u << RL_Z80_LINE_NMI;
}

void rl_z80_iff(rl_ctx *c, uint8_t iff1)
{
    rl_set_irq_mask(c, lines_for(iff1));
}

uint8_t rl_z80_in(rl_ctx *c, uint16_t port)
{
    const rl_z80_ints *m = c->ints;
    return m && m->in ? m->in(m->opaque, port) : 0xFFu;
}

void rl_z80_out(rl_ctx *c, uint16_t port, uint8_t value)
{
    const rl_z80_ints *m = c->ints;
    if (m && m->out)
        m->out(m->opaque, port, value);
}

uint8_t rl_z80_halt(rl_ctx *c, uint8_t *mem, uint8_t iff1, uint8_t r)
{
    (void)mem;
    struct rl_thread *t = rl_common(c)->thread;
    rl_sched_cpu *k = t ? t->clock : NULL;
    if (!k || !c->ints) {
        (void)rl_guest_fatal(c, "z80", "HALT on a thread without a scheduler clock and interrupt inputs");
        return r;
    }
    if (deliverable(c, iff1)) {
        rl_sched_break(k);
        return r;
    }
    uint64_t start = rl_sched_cycles(k);
    rl_sched_set_mask(k, lines_for(iff1));
    c->halted = 1;
    int woke = rl_sched_halt(k);
    c->halted = 0;
    rl_sched_set_mask(k, lines_for(iff1));
    if (!woke) {
        (void)rl_guest_fatal(c, "z80", "HALT with no interrupt that can wake the CPU");
        return r;
    }
    uint64_t spent = rl_sched_cycles(k) - start;
    uint64_t nops = (spent + 3u) / 4u;
    c->time.budget -= (int64_t)(nops * 4u - spent);
    rl_sched_break(k);
    return (uint8_t)((r & 0x80u) | ((r + nops) & 0x7Fu));
}

static uint64_t irq_enter(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    (void)mem;
    const rl_z80_ints *m = c->ints;
    if (!m)
        return RL_NO_IRQ;
    uint8_t pend = m->pending(m->opaque);
    uint32_t kind;
    if (pend & RL_Z80_PEND_NMI) {
        if (m->ack)
            m->ack(m->opaque, RL_Z80_PEND_NMI);
        kind = RL_Z80_ENTRY_NMI;
    } else if ((pend & RL_Z80_PEND_INT) && c->iff1) {
        if (c->ei) {
            struct rl_thread *t = rl_common(c)->thread;
            if (t && t->clock)
                rl_sched_break(t->clock);
            return RL_NO_IRQ;
        }
        if (m->ack)
            m->ack(m->opaque, RL_Z80_PEND_INT);
        kind = c->im == 0 ? RL_Z80_ENTRY_IM0 : c->im == 1 ? RL_Z80_ENTRY_IM1 : RL_Z80_ENTRY_IM2;
    } else {
        return RL_NO_IRQ;
    }
    c->ret = (uint16_t)next;
    return RL_Z80_ENTRY + kind;
}

static uint64_t irq_return(rl_ctx *c, uint8_t *mem, uint64_t expected)
{
    (void)expected;
    uint16_t at = (uint16_t)(c->sp - 2u);
    return (uint64_t)mem[at] | ((uint64_t)mem[(uint16_t)(at + 1u)] << 8);
}

int rl_z80_attach(rl_sched_cpu *k, struct rl_thread *t, const rl_z80_ints *ints)
{
    if (rl_sched_attach(k, t) != 0)
        return -1;
    t->ctx->ints = ints;
    rl_sched_set_mask(k, lines_for(t->ctx->iff1));
    rl_sched_set_irq(k, irq_enter);
    rl_sched_set_irq_return(k, irq_return);
    return 0;
}

int rl_z80_run(rl_ctx *c, uint8_t *mem, uint64_t pc)
{
    c->pc = (uint16_t)pc;
    return rl_run_from(c, mem, pc);
}

const char *rl_z80_flags_str(uint8_t f, char *buf)
{
    static const char names[8] = {'S', 'Z', 'Y', 'H', 'X', 'P', 'N', 'C'};
    for (unsigned i = 0; i < 8; i++)
        buf[i] = f & (0x80u >> i) ? names[i] : '-';
    buf[8] = 0;
    return buf;
}

static void describe(const rl_ctx *c, char *buf, size_t n)
{
    char fl[9];
    snprintf(buf, n,
             "af %02x%02x %s, bc %02x%02x, de %02x%02x, hl %02x%02x, ix %04x, iy %04x, sp %04x, pc %04x, i %02x r %02x, "
             "iff %u%u im %u%s",
             c->a, c->f, rl_z80_flags_str(c->f, fl), c->b, c->c, c->d, c->e, c->h, c->l, c->ix, c->iy, c->sp, c->pc, c->i,
             c->r, c->iff1, c->iff2, c->im, c->halted ? ", halted" : "");
}

static void ctx_init(rl_ctx *c)
{
    c->a = 0xFFu;
    c->f = 0xFFu;
    c->sp = 0xFFFFu;
}

static const rl_cpu_desc g_z80 = {
    .name = "z80",
    .ctx_size = sizeof(rl_ctx),
    .ctx_align = _Alignof(rl_ctx),
    .common_offset = RL_Z80_COMMON_OFFSET,
    .code_shift = RL_Z80_CODE_SHIFT,
    .big_endian = false,
    .time_offset = RL_Z80_TIME_OFFSET,
    .ctx_init = ctx_init,
    .describe = describe,
};

const rl_cpu_desc *rl_z80_cpu(void) { return &g_z80; }

static rl_fn g_miss;

void rl_z80_set_miss(rl_fn fn) { g_miss = fn; }

int rl_dispatch_miss(rl_ctx *c, uint8_t *mem)
{
    if (g_miss)
        RL_MUSTTAIL return g_miss(c, mem);
    return rl_core_miss(c, mem, c->pc, (uintptr_t)__builtin_return_address(0));
}

int rl_dispatch_jump_miss(rl_ctx *c, uint8_t *mem)
{
    rl_miss_info m = { c->pc, c->pend.pc, RL_MISS_JUMP, 0 };
    c->pend.pc = 0;
    if (g_miss)
        RL_MUSTTAIL return g_miss(c, mem);
    return rl_core_miss_at(c, mem, &m);
}

int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn)
{
    (void)mem;
    return rl_guest_stop_at(c, "undefined instruction", pc, "undefined instruction 0x%08x at guest pc 0x%04llx", insn,
                            (unsigned long long)pc);
}

int rl_trap(rl_ctx *c, uint8_t *mem, uint32_t code)
{
    (void)mem;
    return rl_guest_fatal(c, "trap", "trap 0x%x", code);
}
