/**
 * @file cpu.c
 * @brief The ARMv4T CPU on the Game Boy Advance machine.
 *
 * Lifted code makes every data access through the machine's bus
 * (rl_gba_load, rl_gba_store, given the address as the CPU puts it out: the
 * machine aligns it except for the 8-bit backup), which charges its wait
 * states to the CPU's clock. Each instruction's bus cycles go to the machine
 * in the ARM7TDMI's order: first the fetch two instructions ahead
 * (rl_gba_fetch, through the prefetch buffer; nonsequential after a data
 * access or an internal cycle), then the data accesses, then the internal
 * cycles (rl_gba_idle), then a taken branch's refill.
 *
 * SWI runs the high-level BIOS (bios/bios.h) after the lifted exception
 * entry and returns from the exception. An IRQ is taken at a sync point
 * when the machine's IRQ line is up, the I bit clear and the line rose by
 * the first cycle of the instruction's last access (rl_gba_irq_sample): the
 * exception entry, the BIOS stub (which stacks registers and calls the
 * handler at 0x03FFFFFC with LR 0x138) and, when the handler jumps to 0x138,
 * the stub's return. The high-level BIOS runs on the machine's clock; an
 * interrupt taken inside a call enters from relift's slide
 * (RL_GBA_BIOS_SLIDE) with the registers the call gives the handler.
 */
#include "cpu.h"

#include <stdlib.h>

/**
 * @brief Where the display is when the console's boot hands over to the
 * cartridge: line 126, dot 841, measured on the NanoBoyAdvance oracle running
 * the BIOS as a black box (the boot takes the same time on every power on).
 */
#define BOOT_LINE 126u
#define BOOT_DOT 841u

static rl_gba_run *g_run;

/** @brief The unit rl_gba_cpu_fetch_code found when it returned 1. */
static uint32_t g_found;

rl_gba_run *rl_gba_run_current(void) { return g_run; }

static rl_gba *machine(rl_ctx *c) { return c->machine; }

uint64_t rl_gba_cpu_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size)
{
    (void)mem;
    unsigned seq = rl_arm32_data_seq(c, (uint32_t)addr & ~(size - 1u), size);
    c->code_ns = 1;
    return rl_gba_load(machine(c), (uint32_t)addr, size, seq);
}

void rl_gba_cpu_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value)
{
    (void)mem;
    unsigned seq = rl_arm32_data_seq(c, (uint32_t)addr & ~(size - 1u), size);
    c->code_ns = 1;
    rl_gba_store(machine(c), (uint32_t)addr, size, (uint32_t)value, seq);
}

static void charge(rl_ctx *c, uint64_t cycles) { c->time.budget -= (int64_t)cycles; }

/** @brief An MSR cleared the I bit and the mask opens at the next fetch (rl_gba_cpu_msr_i). */
static bool g_unmask;

void rl_gba_cpu_msr_i(rl_ctx *c, uint32_t i)
{
    g_unmask = !(i & 1u) && c->i;
    if (g_unmask)
        c->i = 0;
    else
        rl_arm32_set_i(c, i);
}

void rl_gba_cpu_fetch(rl_ctx *c, uint32_t pc, unsigned thumb)
{
    unsigned size = thumb ? 2u : 4u;
    rl_gba_fetch(machine(c), pc + 2u * size, size, !c->code_ns);
    if (g_unmask) {
        g_unmask = false;
        c->i = 1;
        rl_arm32_set_i(c, 0);
        rl_gba_irq_unmasked(machine(c));
    }
    c->code_ns = 0;
    c->data_open = 0;
}

int rl_gba_cpu_fetch_code(rl_ctx *c, uint32_t pc, unsigned thumb, uint32_t unit)
{
    uint32_t got = rl_gba_code_unit(machine(c), pc, thumb ? 2u : 4u);
    if (got != unit) {
        g_found = got;
        return 1;
    }
    rl_gba_cpu_fetch(c, pc, thumb);
    return 0;
}

static uint64_t unknown_code(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return rl_guest_stop_at(c, "changed code", c->pc & ~1u,
                            "the code at 0x%08x (%s) is now 0x%0*x, which no lifted code was made from", c->pc & ~1u,
                            c->t ? "Thumb" : "ARM", c->t ? 4 : 8, g_found);
}

rl_fn rl_gba_cpu_variant(rl_ctx *c, uint32_t pc, unsigned thumb)
{
    rl_gba_run *r = g_run;
    uint32_t at = pc | thumb;
    r->stats.code_changes++;
    c->pc = pc;
    size_t lo = 0, hi = r->variant_count;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        const rl_gba_variant *v = &r->variants[m];
        if (v->at < at || (v->at == at && v->unit < g_found))
            lo = m + 1;
        else
            hi = m;
    }
    if (lo < r->variant_count && r->variants[lo].at == at && r->variants[lo].unit == g_found)
        return r->variants[lo].fn;
    return unknown_code;
}

void rl_gba_cpu_idle(rl_ctx *c, uint32_t n)
{
    if (!n)
        return;
    rl_gba_idle(machine(c), n);
    c->code_ns = 1;
}

void rl_gba_cpu_refill(rl_ctx *c, uint32_t target, unsigned thumb, unsigned taken)
{
    if (!taken)
        return;
    unsigned size = thumb ? 2u : 4u;
    uint32_t t = target & ~(size - 1u);
    rl_gba_fetch(machine(c), t, size, false);
    rl_gba_fetch(machine(c), t + size, size, true);
    c->code_ns = 0;
}

int rl_gba_cpu_cond_fail(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    if (c->time.budget > 0)
        return RL_OK;
    return rl_sync(c, mem, next);
}

/** @brief The BIOS stub's entry after the exception entry: stacks registers and gives the handler, whose state its bit 0 sets (BX). */
static uint32_t stub_entry(rl_ctx *c, bool from_code)
{
    rl_gba *g = machine(c);
    rl_gba_irq_cpu ic = {.r = {c->r[0], c->r[1], c->r[2], c->r[3]}, .r12 = c->r[12], .lr = c->r[14], .sp = c->r[13], .from_code = from_code};
    uint32_t h = rl_gba_bios_irq_entry(rl_gba_bios_of(g), rl_gba_bios_bus(g), &ic);
    for (unsigned i = 0; i < 4; i++)
        c->r[i] = ic.r[i];
    c->r[12] = ic.r12;
    c->r[14] = ic.lr;
    c->r[13] = ic.sp;
    charge(c, rl_gba_bios_of(g)->cycles);
    c->code_ns = 0;
    c->t = (uint8_t)(h & 1u);
    return h & (c->t ? ~1u : ~3u);
}

/** @brief The stub's tail after the handler jumped to 0x138: unstacks and returns from the exception; gives where to. */
static uint32_t stub_return(rl_ctx *c, bool from_code)
{
    rl_gba *g = machine(c);
    rl_gba_irq_cpu ic = {.r = {c->r[0], c->r[1], c->r[2], c->r[3]}, .r12 = c->r[12], .lr = c->r[14], .sp = c->r[13], .from_code = from_code};
    uint32_t to = rl_gba_bios_irq_return(rl_gba_bios_of(g), rl_gba_bios_bus(g), &ic);
    for (unsigned i = 0; i < 4; i++)
        c->r[i] = ic.r[i];
    c->r[12] = ic.r12;
    c->r[14] = ic.lr;
    c->r[13] = ic.sp;
    charge(c, rl_gba_bios_of(g)->cycles);
    c->code_ns = 0;
    rl_arm32_restore(c);
    return to;
}

static uint64_t irq_enter(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    (void)mem;
    rl_gba_run *r = g_run;
    rl_gba *g = machine(c);
    if (r->check)
        r->check(r->check_user, c);
    rl_gba_irq_boundary(g);
    if (c->i || g_unmask || !rl_gba_irq_line(g))
        return RL_NO_IRQ;
    if (!rl_gba_irq_sample(g)) {
        rl_sched_break(r->k);
        return RL_NO_IRQ;
    }
    r->stats.irqs++;
    unsigned size = c->t ? 2u : 4u;
    rl_gba_fetch(g, (uint32_t)next + 2u * size, size, !c->code_ns);
    rl_arm32_exception(c, RL_ARM32_IRQ, (uint32_t)next + 4u);
    uint32_t h = stub_entry(c, true);
    rl_arm32_call(c, RL_GBA_BIOS_IRQ_RETURN);
    return h;
}

static uint64_t irq_return(rl_ctx *c, uint8_t *mem, uint64_t expected)
{
    (void)mem;
    (void)expected;
    uint32_t to = stub_return(c, true);
    unsigned size = c->t ? 2u : 4u;
    rl_gba_fetch(machine(c), to, size, false);
    rl_gba_fetch(machine(c), to + size, size, true);
    c->code_ns = 0;
    return to;
}

/** @brief Where a SWI's call ends when a handler inside it changed its return (RL_GBA_SWI_LEFT). */
static uint32_t g_left;

/**
 * @brief The machine's callback inside a BIOS call: enters IRQ mode with
 * the lr and spsr the call gives, runs the stub and the handler, and goes
 * back into the call unless the handler changed where the exception
 * returns.
 */
static int bios_point(void *user, uint32_t lr, uint32_t spsr, const uint32_t *bios_regs)
{
    rl_gba_run *r = user;
    rl_ctx *c = r->c;
    uint32_t saved = rl_arm32_cpsr(c);
    uint32_t regs[15];
    memcpy(regs, c->r, sizeof regs);
    rl_arm32_set_cpsr(c, spsr);
    uint32_t sp = c->r[13], lr_mode = c->r[14];
    unsigned mode = spsr & 0x1Fu;
    bool banked = mode == RL_ARM32_USR || mode == RL_ARM32_SYS || mode == RL_ARM32_SVC;
    memcpy(c->r, bios_regs, (banked ? 15 : 13) * sizeof c->r[0]);
    uint32_t svc_sp = c->bank_svc[0];
    if (mode != RL_ARM32_SVC)
        c->bank_svc[0] = bios_regs[15];
    r->stats.bios_irqs++;
    unsigned size = (spsr & 0x20u) ? 2u : 4u;
    uint32_t back = (lr - 4u) & ~(size - 1u);
    bool code = back >= 0x4000u;
    if (code)
        rl_gba_fetch(machine(c), back + 2u * size, size, true);
    else
        rl_gba_fetch(machine(c), lr & ~3u, 4, true);
    rl_arm32_exception(c, RL_ARM32_IRQ, lr);
    uint32_t h = stub_entry(c, true);
    if (rl_arm32_call_guest(c, NULL, h, RL_GBA_BIOS_IRQ_RETURN) != RL_OK) {
        rl_log(RL_LOG_ERROR, "gba", "an interrupt handler inside a BIOS call did not return; stopping");
        rl_process_exit(RL_EXIT_SOFTWARE);
        return RL_GBA_POINT_NONE;
    }
    uint32_t to = stub_return(c, code);
    if (to != back || rl_arm32_cpsr(c) != spsr) {
        g_left = to;
        return RL_GBA_POINT_LEFT;
    }
    if (code) {
        rl_gba_fetch(machine(c), to, size, false);
        rl_gba_fetch(machine(c), to + size, size, true);
    }
    c->r[13] = sp;
    c->r[14] = lr_mode;
    if (mode != RL_ARM32_SVC)
        c->bank_svc[0] = svc_sp;
    rl_arm32_set_cpsr(c, saved);
    memcpy(c->r, regs, sizeof regs);
    return RL_GBA_POINT_TAKEN;
}

int rl_gba_swi_trap(rl_ctx *c, uint8_t *mem, uint32_t code)
{
    (void)mem;
    rl_gba_run *r = g_run;
    rl_gba *g = machine(c);
    unsigned number = code & 0xFFu;
    r->stats.swis++;
    rl_gba_swi_cpu s = {
        .ret = c->r[14],
        .cpsr = c->spsr[2],
        .sp_svc = c->r[13],
        .sp_sys = c->bank_usr[5],
        .lr_sys = c->bank_usr[6],
    };
    memcpy(s.r, c->r, sizeof s.r);
    rl_gba_swi_status st = rl_gba_swi(rl_gba_bios_of(g), rl_gba_bios_bus(g), number, &s);
    for (unsigned i = 0; i < 4; i++)
        c->r[i] = s.r[i];
    if (st == RL_GBA_SWI_OK || st == RL_GBA_SWI_UNKNOWN) {
        c->spsr[2] = s.cpsr;
        c->r[13] = s.sp_svc;
        c->r[14] = s.ret;
    }
    charge(c, rl_gba_bios_of(g)->cycles);
    c->code_ns = 0;
    switch (st) {
    case RL_GBA_SWI_OK:
        rl_arm32_restore(c);
        return RL_OK;
    case RL_GBA_SWI_LEFT:
        return rl_arm32_redirect(c, g_left);
    case RL_GBA_SWI_UNKNOWN:
        r->stats.unknown_swis++;
        rl_report_unknown(0x5357490000ull | number, "gba", "SWI 0x%02x is not implemented; it does nothing", number);
        rl_arm32_restore(c);
        return RL_OK;
    case RL_GBA_SWI_RESET: {
        rl_gba_boot b = rl_gba_bios_boot_state();
        rl_arm32_set_cpsr(c, RL_ARM32_IRQ | 0xC0u);
        c->r[13] = b.sp_irq;
        rl_arm32_set_mode(c, RL_ARM32_SVC);
        c->r[13] = b.sp_svc;
        rl_arm32_set_cpsr(c, b.cpsr);
        c->r[13] = b.sp_sys;
        return rl_arm32_redirect(c, s.ret);
    }
    default:
        return rl_guest_fatal(c, "gba", "SWI 0x%02x never returns on the console (division by zero)", number);
    }
}

static void boot(rl_gba_run *r)
{
    rl_ctx *c = r->c;
    rl_gba_boot b = rl_gba_bios_boot_state();
    rl_arm32_set_cpsr(c, RL_ARM32_IRQ | 0xC0u);
    c->r[13] = b.sp_irq;
    rl_arm32_set_mode(c, RL_ARM32_SVC);
    c->r[13] = b.sp_svc;
    rl_arm32_set_cpsr(c, b.cpsr);
    c->r[13] = b.sp_sys;
    r->entry = b.pc;
    rl_gba_bios_boot_io(rl_gba_bios_of(r->g), rl_gba_bios_bus(r->g));
    rl_gba_fetched(r->g, b.pc + 4u, 4);
    rl_gba_lcd_set(r->g, BOOT_LINE, BOOT_DOT);
}

int rl_gba_run_init(rl_gba_run *r, rl_gba *g)
{
    memset(r, 0, sizeof *r);
    r->g = g;
    r->s = rl_sched_create(NULL);
    r->k = r->s ? rl_sched_add_cpu(r->s, 1) : NULL;
    r->t = rl_thread_alloc();
    if (!r->k || !r->t || rl_gba_attach(g, r->s, r->k, NULL) != 0 || rl_sched_attach(r->k, r->t) != 0) {
        rl_gba_run_free(r);
        return -1;
    }
    rl_thread_bind(r->t);
    r->c = r->t->ctx;
    r->c->machine = g;
    g_run = r;
    rl_sched_set_irq(r->k, irq_enter);
    rl_sched_set_irq_return(r->k, irq_return);
    rl_gba_cpu_hooks hooks = {.user = r, .point = bios_point};
    rl_gba_set_cpu_hooks(g, &hooks);
    boot(r);
    rl_arm32_set_lines(r->c, 1ull << RL_GBA_LINE_IRQ, 1ull << RL_GBA_LINE_DMA);
    return 0;
}

void rl_gba_run_free(rl_gba_run *r)
{
    if (r->g)
        rl_gba_set_cpu_hooks(r->g, NULL);
    if (r->s)
        rl_sched_destroy(r->s);
    if (g_run == r)
        g_run = NULL;
    r->s = NULL;
    r->k = NULL;
}

int rl_gba_run_start(rl_gba_run *r)
{
    rl_sched_run(r->k);
    return rl_arm32_run(r->c, NULL, r->entry);
}
