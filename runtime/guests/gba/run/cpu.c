/**
 * @file cpu.c
 * @brief The ARMv4T CPU on the Game Boy Advance machine.
 *
 * Lifted code makes every data access through the machine's bus
 * (rl_gba_load, rl_gba_store), which charges its wait states to the CPU's
 * clock; code fetches cost what the machine's wait table says, with a
 * sequential Game Pak fetch at one cycle while the prefetch buffer is on.
 *
 * SWI runs the high-level BIOS (bios/bios.h) after the lifted exception
 * entry and returns from the exception. An IRQ is taken at a sync point
 * when the machine's IRQ line is up and the I bit clear: the exception
 * entry, the BIOS stub (which stacks registers and calls the handler at
 * 0x03FFFFFC with LR 0x138) and, when the handler jumps to 0x138, the stub's
 * return. Inside a BIOS call that waits (IntrWait, Halt), the machine calls
 * back where the console takes the interrupt; the handler then runs from
 * system mode as under the console's BIOS.
 */
#include "cpu.h"

#include <stdlib.h>

#define FAST_FETCH 0x3F00u
#define BIOS_WAIT_LR (RL_GBA_BIOS_SWI_RETURN + 4u)

static rl_gba_run *g_run;

rl_gba_run *rl_gba_run_current(void) { return g_run; }

static rl_gba *machine(rl_ctx *c) { return c->machine; }

static void update_prefetch(rl_ctx *c)
{
    uint32_t waitcnt = rl_gba_read(machine(c), 0x04000204u, 2);
    c->fast_fetch = (waitcnt & 0x4000u) ? FAST_FETCH : 0u;
}

uint64_t rl_gba_cpu_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size)
{
    (void)mem;
    uint32_t a = (uint32_t)addr & ~(size - 1u);
    unsigned seq = rl_arm32_data_seq(c, a, size);
    return rl_gba_load(machine(c), a, size, seq);
}

void rl_gba_cpu_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value)
{
    (void)mem;
    uint32_t a = (uint32_t)addr & ~(size - 1u);
    unsigned seq = rl_arm32_data_seq(c, a, size);
    rl_gba_store(machine(c), a, size, (uint32_t)value, seq);
    if (a < 0x04000208u && a + size > 0x04000204u)
        update_prefetch(c);
}

static void charge(rl_ctx *c, uint64_t cycles) { c->time.budget -= (int64_t)cycles; }

/** @brief The BIOS stub's entry after the exception entry: stacks registers and gives the handler, whose state its bit 0 sets (BX). */
static uint32_t stub_entry(rl_ctx *c)
{
    rl_gba *g = machine(c);
    rl_gba_irq_cpu ic = {.r = {c->r[0], c->r[1], c->r[2], c->r[3]}, .r12 = c->r[12], .lr = c->r[14], .sp = c->r[13]};
    uint32_t h = rl_gba_bios_irq_entry(rl_gba_bios_of(g), rl_gba_bios_bus(g), &ic);
    for (unsigned i = 0; i < 4; i++)
        c->r[i] = ic.r[i];
    c->r[12] = ic.r12;
    c->r[14] = ic.lr;
    c->r[13] = ic.sp;
    charge(c, rl_gba_bios_of(g)->cycles);
    c->t = (uint8_t)(h & 1u);
    return h & (c->t ? ~1u : ~3u);
}

/** @brief The stub's tail after the handler jumped to 0x138: unstacks and returns from the exception; gives where to. */
static uint32_t stub_return(rl_ctx *c)
{
    rl_gba *g = machine(c);
    rl_gba_irq_cpu ic = {.r = {c->r[0], c->r[1], c->r[2], c->r[3]}, .r12 = c->r[12], .lr = c->r[14], .sp = c->r[13]};
    uint32_t to = rl_gba_bios_irq_return(rl_gba_bios_of(g), rl_gba_bios_bus(g), &ic);
    for (unsigned i = 0; i < 4; i++)
        c->r[i] = ic.r[i];
    c->r[12] = ic.r12;
    c->r[14] = ic.lr;
    c->r[13] = ic.sp;
    charge(c, rl_gba_bios_of(g)->cycles);
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
    rl_gba_service(g);
    if (c->i || !rl_gba_irq_line(g))
        return RL_NO_IRQ;
    r->stats.irqs++;
    rl_arm32_exception(c, RL_ARM32_IRQ, (uint32_t)next + 4u);
    uint32_t h = stub_entry(c);
    rl_arm32_call(c, RL_GBA_BIOS_IRQ_RETURN);
    return h;
}

static uint64_t irq_return(rl_ctx *c, uint8_t *mem, uint64_t expected)
{
    (void)mem;
    (void)expected;
    return stub_return(c);
}

/** @brief The machine's callback inside a BIOS call: takes the interrupt when the caller's I bit lets it in. */
static void bios_irq(void *user)
{
    rl_gba_run *r = user;
    rl_ctx *c = r->c;
    uint32_t *caller = rl_arm32_spsr(c, RL_ARM32_SVC);
    if (c->m != RL_ARM32_SVC || !caller)
        return;
    if (!(*caller & 0x80u)) {
        uint32_t saved = rl_arm32_cpsr(c);
        uint32_t regs[15];
        memcpy(regs, c->r, sizeof regs);
        rl_arm32_set_cpsr(c, (saved & 0xF0000000u) | RL_ARM32_SYS);
        r->stats.bios_irqs++;
        rl_arm32_exception(c, RL_ARM32_IRQ, BIOS_WAIT_LR);
        uint32_t h = stub_entry(c);
        int rc = rl_arm32_call_guest(c, NULL, h, RL_GBA_BIOS_IRQ_RETURN);
        if (rc != RL_OK) {
            rl_log(RL_LOG_ERROR, "gba", "an interrupt handler inside a BIOS call did not return; stopping");
            rl_process_exit(RL_EXIT_SOFTWARE);
            return;
        }
        (void)stub_return(c);
        rl_arm32_set_cpsr(c, saved);
        memcpy(c->r, regs, sizeof regs);
    }
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
    charge(c, rl_gba_bios_of(g)->cycles);
    switch (st) {
    case RL_GBA_SWI_OK:
        rl_arm32_restore(c);
        return RL_OK;
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
    r->c->waits = &rl_gba_wait_table(g)->c[0][0][0];
    g_run = r;
    rl_sched_set_irq(r->k, irq_enter);
    rl_sched_set_irq_return(r->k, irq_return);
    rl_gba_cpu_hooks hooks = {.user = r, .irq = bios_irq};
    rl_gba_set_cpu_hooks(g, &hooks);
    boot(r);
    rl_arm32_set_lines(r->c, 1ull << RL_GBA_LINE_IRQ, 1ull << RL_GBA_LINE_DMA);
    update_prefetch(r->c);
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
