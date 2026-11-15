/**
 * @file sync.c
 * @brief The lifted-code entries of the device scheduler, rl_sync,
 * rl_io_read and rl_io_write, and rl_sched_attach, which binds a guest
 * thread's clock to a scheduler CPU (relift/sched.h); and rl_preempt, the
 * preemption point's entry to the guest layer's scheduler.
 */
#include "core.h"
#include "sched_int.h"

int rl_sched_attach(rl_sched_cpu *c, struct rl_thread *t)
{
    rl_time *tm = t && t->ctx ? rl_ctx_time(t->ctx) : NULL;
    if (!tm)
        return -1;
    *tm = *c->time;
    c->time = tm;
    c->thread = t;
    c->sched->bind = rl_thread_bind;
    t->clock = c;
    return 0;
}

static rl_preempt_hook *g_preempt;

void rl_set_preempt_hook(rl_preempt_hook *hook) { g_preempt = hook; }

void rl_preempt(rl_ctx *c)
{
    if (g_preempt)
        g_preempt(c);
    else
        rl_preempt_clear(c);
}

void rl_preempt_request(rl_ctx *c)
{
    rl_time *t = rl_ctx_time(c);
    if (t)
        __atomic_store_n(&t->budget, 0, __ATOMIC_RELEASE);
}

void rl_preempt_clear(rl_ctx *c)
{
    rl_time *t = rl_ctx_time(c);
    if (t)
        __atomic_store_n(&t->budget, INT64_MAX, __ATOMIC_RELAXED);
}

static rl_sched_cpu *ctx_clock(rl_ctx *c)
{
    struct rl_thread *t = rl_common(c)->thread;
    return t ? t->clock : NULL;
}

int rl_sync(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    rl_sched_cpu *k = ctx_clock(c);
    if (!k)
        return rl_guest_fatal(c, "sched", "sync point reached on a thread without a scheduler clock");
    for (;;) {
        if (k->time->budget <= 0)
            rl_sched_run(k);
        uint64_t pc = k->irq ? k->irq(c, mem, next) : RL_NO_IRQ;
        if (pc == RL_NO_IRQ)
            return RL_OK;
        k->sched->stats.irqs++;
        if (k->irq_depth >= k->sched->cfg.irq_depth)
            return rl_guest_fatal(c, "sched", "%u interrupt entries are open at once; a handler neither returns nor leaves the interrupt state", k->irq_depth);
        rl_fn fn = rl_dispatch_find(pc);
        k->irq_depth++;
        int r = fn ? fn(c, mem) : rl_core_miss(c, mem, pc);
        k->irq_depth--;
        if (r != RL_OK)
            return RL_PENDING;
        if (k->irq_ret) {
            uint64_t to = k->irq_ret(c, mem, next);
            if (to != next) {
                k->sched->stats.redirects++;
                rl_pending *p = &rl_common(c)->pend;
                p->kind = RL_PEND_REDIRECT;
                p->pc = to;
                p->sp = RL_SP_NONE;
                p->code = 0;
                return RL_PENDING;
            }
        }
    }
}

int rl_run_from(rl_ctx *c, uint8_t *mem, uint64_t pc)
{
    for (;;) {
        rl_fn fn = rl_dispatch_find(pc);
        int r = fn ? fn(c, mem) : rl_core_miss(c, mem, pc);
        rl_pending *p = &rl_common(c)->pend;
        if (r == RL_OK || p->kind != RL_PEND_REDIRECT)
            return r;
        pc = p->pc;
        p->kind = RL_PEND_NONE;
        p->sp = RL_SP_NONE;
    }
}

void rl_set_irq_mask(rl_ctx *c, uint64_t mask)
{
    rl_sched_cpu *k = ctx_clock(c);
    if (k)
        rl_sched_set_mask(k, mask);
}

uint64_t rl_io_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size)
{
    (void)mem;
    rl_sched_cpu *k = ctx_clock(c);
    if (!k) {
        rl_guest_fatal(c, "sched", "device read at 0x%llx on a thread without a scheduler clock", (unsigned long long)addr);
        return 0;
    }
    return rl_sched_read(k, addr, size);
}

void rl_io_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value)
{
    (void)mem;
    rl_sched_cpu *k = ctx_clock(c);
    if (!k) {
        rl_guest_fatal(c, "sched", "device write at 0x%llx on a thread without a scheduler clock", (unsigned long long)addr);
        return;
    }
    rl_sched_write(k, addr, size, value);
}
