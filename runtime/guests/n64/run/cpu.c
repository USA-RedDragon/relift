/**
 * @file cpu.c
 * @brief The VR4300 CPU on the Nintendo 64 machine.
 *
 * Lifted code reads and writes RDRAM directly and passes every other
 * physical address to the machine (rl_n64_cpu_read, rl_n64_cpu_write); the
 * guard slow paths use the same bus for TLB-mapped accesses. MI's interrupt
 * output is IP2; the timer (Count reaching Compare) is a scheduler event at
 * the cycle the CPU layer computes, so the sync point after it takes IP7.
 *
 * Code is entered through the binding of the machine (code.c): the dispatch
 * slot of a run that was not checked since its bytes last changed holds the
 * entry check, which binds the run whose hash matches RDRAM. Missed code,
 * an entry that no lifted run matches, stops the guest with a report (and
 * an RDRAM dump with --dump); nothing is interpreted.
 *
 * The RSP is the layer of cpus/rsp over the machine's DMEM and IMEM: the
 * machine runs it in slices through rl_n64_rsp_ops, and its COP0 accesses
 * and BREAK go to the machine's SP and DP registers.
 *
 * The boot is high level: no boot code runs. The registers and CP0 are what
 * the console's boot code leaves when it jumps to the game.
 */
#include "cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static rl_n64_run *g_run;

rl_n64_run *rl_n64_run_current(void) { return g_run; }

static rl_n64 *machine(rl_ctx *c) { return c->machine; }

uint64_t rl_n64_cpu_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size)
{
    (void)mem;
    return rl_mips_swap(rl_n64_read(machine(c), (uint32_t)addr, size), size);
}

void rl_n64_cpu_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value)
{
    (void)mem;
    rl_n64_write(machine(c), (uint32_t)addr, size, rl_mips_swap(value, size));
}

static uint64_t bus_read(rl_ctx *c, uint8_t *mem, uint64_t paddr, uint32_t size)
{
    (void)mem;
    return rl_n64_read(machine(c), (uint32_t)paddr, size);
}

static void bus_write(rl_ctx *c, uint8_t *mem, uint64_t paddr, uint32_t size, uint64_t value)
{
    (void)mem;
    rl_n64_write(machine(c), (uint32_t)paddr, size, value);
}

static const rl_mips_bus g_bus = {bus_read, bus_write};

/** @brief Posts the timer event at the cycle Count next equals Compare. */
static void post_timer(rl_n64_run *r)
{
    rl_sched_post(r->s, &r->timer, rl_sched_now(r->k) + rl_mips_timer_cycles(r->c));
}

static void timer_fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    (void)when;
    rl_n64_run *r = e->opaque;
    rl_sched_break(r->k);
    post_timer(r);
}

void rl_n64_cp0_break(rl_ctx *c)
{
    (void)c;
    rl_sched_break(g_run->k);
}

int rl_n64_cp0_sync(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    rl_mips_cp0_sync(c);
    post_timer(g_run);
    return rl_sync(c, mem, next);
}

int rl_n64_tlb(rl_ctx *c, uint8_t *mem, uint32_t op)
{
    (void)mem;
    rl_mips_tlb(c, op, 1);
    return RL_OK;
}

static uint64_t irq_enter(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    (void)mem;
    rl_n64_run *r = g_run;
    if (r->check)
        r->check(r->check_user, c);
    uint32_t ip = rl_n64_irq_pending(machine(c)) ? 1u << RL_N64_LINE_MI : 0u;
    uint64_t pc = rl_mips_irq_enter(c, next, ip);
    if (pc != RL_NO_IRQ)
        c->pc = pc;
    return pc;
}

static void cache_hook(rl_ctx *c, uint32_t op, uint64_t vaddr)
{
    if ((op & 3u) != 0 || (op != 0x00u && op != 0x10u))
        return;
    rl_n64_code_written(machine(c), (uint32_t)vaddr & 0x1FFFFFE0u, 32);
}

static void dump_rdram(rl_n64_run *r)
{
    if (!r->dump)
        return;
    char path[4096];
    snprintf(path, sizeof path, "%s.rdram", r->dump);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(rl_n64_rdram(r->m), 1, RL_N64_RDRAM_SIZE, f) != RL_N64_RDRAM_SIZE)
        rl_log(RL_LOG_ERROR, "n64", "could not write %s", path);
    else
        rl_log(RL_LOG_ERROR, "n64", "wrote RDRAM to %s", path);
    if (f)
        fclose(f);
}

static int entry_check(rl_ctx *c, uint8_t *mem)
{
    char why[256];
    rl_fn f = rl_n64_code_check(machine(c), c->pc, why, sizeof why);
    if (f && f != entry_check)
        RL_MUSTTAIL return f(c, mem);
    if (f)
        snprintf(why, sizeof why, "the lifted run at 0x%08x has no function there", (uint32_t)c->pc);
    dump_rdram(g_run);
    return rl_guest_fatal(c, "n64", "missed code: %s", why);
}

/** @brief A call site the trace saw: its return address, the VI frame and cycle of its first call, its calls and the frame of its last. */
typedef struct trace_site {
    uint64_t ret;
    uint64_t frame;
    uint64_t cycle;
    uint64_t count;
    uint64_t last;
} trace_site;

static struct {
    trace_site *sites;
    size_t cap;
    size_t count;
    const char *path;
} g_trace;

static void trace_call(rl_ctx *c, uint64_t ret)
{
    size_t mask = g_trace.cap - 1u;
    size_t i = (size_t)((ret >> 2) * UINT64_C(0x9E3779B97F4A7C15) >> 32) & mask;
    while (g_trace.sites[i].ret) {
        if (g_trace.sites[i].ret == ret) {
            g_trace.sites[i].count++;
            g_trace.sites[i].last = rl_n64_get_stats(machine(c))->frames;
            return;
        }
        i = (i + 1u) & mask;
    }
    if (g_trace.count * 2u >= g_trace.cap)
        return;
    uint64_t frame = rl_n64_get_stats(machine(c))->frames;
    g_trace.sites[i] = (trace_site){ret, frame, rl_sched_now(g_run->k), 1, frame};
    g_trace.count++;
}

static int by_cycle(const void *a, const void *b)
{
    const trace_site *x = a, *y = b;
    return x->cycle < y->cycle ? -1 : x->cycle > y->cycle;
}

void rl_n64_trace_calls(const char *path)
{
    g_trace.cap = (size_t)1 << 20;
    g_trace.sites = calloc(g_trace.cap, sizeof *g_trace.sites);
    if (!g_trace.sites)
        return;
    g_trace.path = path;
    rl_mips_call_trace = trace_call;
}

void rl_n64_trace_write(void)
{
    if (!g_trace.sites)
        return;
    rl_mips_call_trace = NULL;
    size_t n = 0;
    for (size_t i = 0; i < g_trace.cap; i++)
        if (g_trace.sites[i].ret)
            g_trace.sites[n++] = g_trace.sites[i];
    qsort(g_trace.sites, n, sizeof *g_trace.sites, by_cycle);
    FILE *f = fopen(g_trace.path, "w");
    if (!f) {
        rl_log(RL_LOG_ERROR, "n64", "could not write %s", g_trace.path);
        return;
    }
    fprintf(f, "call\tframe\tcycle\tcount\tlast_frame\n");
    for (size_t i = 0; i < n; i++) {
        const trace_site *t = &g_trace.sites[i];
        fprintf(f, "0x%08x\t%llu\t%llu\t%llu\t%llu\n", (uint32_t)(t->ret - 8u), (unsigned long long)t->frame,
                (unsigned long long)t->cycle, (unsigned long long)t->count, (unsigned long long)t->last);
    }
    fclose(f);
    free(g_trace.sites);
    g_trace.sites = NULL;
}

static uint64_t rsp_run(void *user, uint32_t *pc, uint64_t from, uint64_t to)
{
    return rl_rsp_run(user, pc, from, to);
}

static void rsp_imem(void *user, uint32_t offset, uint32_t len) { rl_rsp_imem_written(user, offset, len); }

static const rl_n64_rsp_ops g_rsp_ops = {rsp_run, rsp_imem};

static uint32_t rsp_mfc0(void *user, uint32_t rd, uint64_t cycle) { return rl_n64_rsp_mfc0(user, rd, cycle); }

static void rsp_mtc0(void *user, uint32_t rd, uint32_t value, uint64_t cycle)
{
    rl_n64_rsp_mtc0(user, rd, value, cycle);
}

static void rsp_brk(void *user, uint64_t cycle) { rl_n64_rsp_break(user, cycle); }

static bool rsp_halted(void *user) { return rl_n64_rsp_halted(user); }

static const rl_rsp_bus g_rsp_bus = {rsp_mfc0, rsp_mtc0, rsp_brk, rsp_halted};

int rl_n64_run_rsp(rl_n64_run *r, const rl_rsp_image *const *images, uint32_t count, char *why, size_t n)
{
    if (!rl_rsp_images_ok(images, count, why, n))
        return -1;
    uint8_t *sp = rl_n64_spmem(r->m);
    r->rsp = rl_rsp_create(sp, sp + 0x1000, images, count, &g_rsp_bus, r->m);
    if (!r->rsp) {
        snprintf(why, n, "out of memory for the RSP");
        return -1;
    }
    rl_rsp_set_dump(r->rsp, r->dump);
    rl_n64_set_rsp(r->m, &g_rsp_ops, r->rsp);
    return 0;
}

/** @brief The seed the boot variant leaves in s6. */
static uint64_t seed(uint32_t cic)
{
    switch (cic) {
    case 6103:
        return 0x78;
    case 6105:
        return 0x91;
    case 6106:
        return 0x85;
    case 8303:
        return 0xDD;
    default:
        return 0x3F;
    }
}

static uint64_t sext(uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; }

/** @brief The CPU state the boot code leaves at the jump to the game. */
static void boot_cpu(rl_ctx *c, uint32_t cic, bool pal)
{
    memset(c->r, 0, sizeof c->r);
    c->r[11] = sext(0xA4000040u);
    c->r[20] = pal ? 0 : 1;
    c->r[22] = seed(cic);
    c->r[29] = sext(0xA4001FF0u);
    c->r[31] = sext(0xA4001550u);
    c->cp0_status = 0x34000000u;
    c->cp0_cause = 0;
    c->cp0_config = 0x7006E463u;
    c->cp0_compare = 0;
    c->cp0_wired = 0;
    c->cp0_error_epc = 0;
    c->cp0_epc = 0;
    c->fcr31 = 0;
}

int rl_n64_run_init(rl_n64_run *r, rl_n64 *m, uint32_t cic, uint32_t entry)
{
    memset(r, 0, sizeof *r);
    r->m = m;
    r->s = rl_sched_create(NULL);
    r->k = r->s ? rl_sched_add_cpu(r->s, 1) : NULL;
    r->t = rl_thread_alloc();
    if (!r->k || !r->t || rl_n64_attach(m, r->s, r->k) != 0 || rl_sched_attach(r->k, r->t) != 0) {
        rl_n64_run_free(r);
        return -1;
    }
    rl_thread_bind(r->t);
    r->c = r->t->ctx;
    r->c->machine = m;
    g_run = r;
    rl_sched_set_irq(r->k, irq_enter);
    rl_sched_set_irq_return(r->k, rl_mips_irq_return);
    rl_mips_set_bus(&g_bus);
    rl_mips_set_cache_hook(cache_hook);
    rl_n64_code_set_stub(m, entry_check);
    r->timer.fire = timer_fire;
    r->timer.opaque = r;
    rl_n64_boot_info b = {.cic = cic, .entry = entry};
    rl_n64_boot(m, &b);
    uint8_t region = rl_n64_rdram(m)[0x303];
    boot_cpu(r->c, cic, region == 0);
    rl_mips_set_count(r->c, 0);
    rl_mips_cp0_sync(r->c);
    post_timer(r);
    r->entry = sext(entry);
    return 0;
}

void rl_n64_run_free(rl_n64_run *r)
{
    if (r->s) {
        rl_sched_cancel(r->s, &r->timer);
        rl_sched_destroy(r->s);
    }
    rl_mips_set_bus(NULL);
    rl_mips_set_cache_hook(NULL);
    if (r->rsp) {
        rl_n64_set_rsp(r->m, NULL, NULL);
        rl_rsp_destroy(r->rsp);
        r->rsp = NULL;
    }
    if (g_run == r)
        g_run = NULL;
    r->s = NULL;
    r->k = NULL;
}

int rl_n64_run_start(rl_n64_run *r)
{
    rl_sched_run(r->k);
    return rl_mips_run(r->c, rl_n64_rdram(r->m), r->entry);
}
