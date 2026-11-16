/**
 * @file cpu.c
 * @brief The ARM9 and the ARM7 on the Nintendo DS machine.
 *
 * Each CPU is a lifted-code thread with a scheduler CPU of its own (the ARM9
 * one tick per cycle, the ARM7 two) on a fiber of the ordered scheduler, and
 * a dispatch table over the functions of its own units: the CPUs see
 * different code at the same addresses. Lifted code makes every data access
 * through the machine's bus (rl_nds_load, rl_nds_store), which waits for the
 * other CPU before a shared access and charges the cost to the CPU's clock;
 * code fetches cost what the machine's table says for the region, read once
 * into the context's fetch table (rl_ctx.waits) and again when the TCMs
 * move.
 *
 * The first instruction of every lifted function checks that memory holds
 * the bytes it was lifted from. A check is a compare only after a write to
 * code memory (the machine's code epoch moves) or a change of the memory
 * map; otherwise it is two loads. A failed check goes on in the function of
 * another unit lifted from the bytes there, or stops the program with a
 * report: lifted code never falls back to an interpreter.
 *
 * Interrupts are taken at sync points: the budget ends at the tick the IRQ
 * input rises (whenever an access, an event or a write of the I bit could
 * have moved it), the scheduler hook polls the machine, enters IRQ mode and
 * runs the high-level BIOS handler: it stacks r0 to r3, r12 and LR, calls
 * the handler whose address the game stored in its slot (DTCM + 0x3FFC on
 * the ARM9, 0x0380FFFC on the ARM7) and, when the handler returns to the
 * BIOS, unstacks and returns from the exception. SWI runs the high-level
 * BIOS calls the tests need; the cycles of both are not measured yet.
 */
#include "cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Where an IRQ handler returns into the high-level BIOS (a BIOS address of each CPU no code is lifted at). */
#define BIOS_IRQ_RETURN9 0xFFFF0290u
#define BIOS_IRQ_RETURN7 0x00000290u
/** @brief Cycles charged for an IRQ entry and return, and for a BIOS call, until measured. */
#define BIOS_CYCLES 16u

static rl_nds_arm *cpu_of(rl_ctx *c) { return c->machine; }

static const char *cpu_name(const rl_nds_arm *k) { return k->id == RL_NDS_ARM9 ? "ARM9" : "ARM7"; }

/** @brief Ends the budget by the tick the IRQ input rises at, while the I bit lets it in. */
static void clip(rl_nds_arm *k)
{
    if (k->i)
        return;
    uint64_t at = rl_nds_irq_at(k->run->m, k->id);
    if (at == UINT64_MAX)
        return;
    rl_time *t = &k->c->time;
    uint64_t now = rl_sched_now(k->k);
    uint64_t ratio = k->id == RL_NDS_ARM9 ? 1u : 2u;
    int64_t left = at <= now ? 0 : (int64_t)((at - now + ratio - 1u) / ratio);
    if (left < t->budget) {
        t->deadline -= (uint64_t)(t->budget - left);
        t->budget = left;
    }
}

/** @brief The CPU halted with nothing that can wake it: it leaves the run for good (does not return). */
static void stop_halted(rl_nds_arm *k)
{
    k->stopped = 1;
    rl_sched_leave(k->k);
}

/** @brief The memory map changed under the code without a write: every entry checks its bytes again. */
static void code_moved(rl_nds_run *r) { memset(r->seen, 0, (size_t)r->fn_total * sizeof r->seen[0]); }

/** @brief Fills the code fetch table of k from the machine's costs. */
static void build_waits(rl_nds_arm *k)
{
    const rl_nds_costs *t = rl_nds_cost_table(k->run->m);
    bool arm9 = k->id == RL_NDS_ARM9;
    rl_arm32_tcm tcm = {0};
    if (arm9)
        rl_arm32_cp15_tcm(rl_nds_cp15(k->run->m), &tcm);
    for (unsigned r = 0; r < 16; r++)
        for (unsigned l = 0; l < 3; l++)
            for (unsigned s = 0; s < 2; s++) {
                unsigned ticks = t->t[k->id][RL_NDS_CODE][r][l][arm9 ? 0 : s];
                unsigned cyc = arm9 ? ticks : (ticks + 1u) / 2u;
                if (arm9 && tcm.itcm_on && ((uint64_t)r << 24) < tcm.itcm_size)
                    cyc = 1;
                k->waits[r * 6u + l * 2u + s] = (uint8_t)(cyc ? cyc : 1u);
            }
    k->c->waits = k->waits;
    k->c->fast_fetch = 0;
}

uint64_t rl_nds_cpu_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size)
{
    (void)mem;
    rl_nds_arm *k = cpu_of(c);
    unsigned seq = rl_arm32_data_seq(c, (uint32_t)addr & ~(size - 1u), size);
    uint32_t v = rl_nds_load(k->run->m, k->id, (uint32_t)addr, size, seq);
    clip(k);
    return v;
}

void rl_nds_cpu_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value)
{
    (void)mem;
    rl_nds_arm *k = cpu_of(c);
    uint32_t a = (uint32_t)addr & ~(size - 1u);
    unsigned seq = rl_arm32_data_seq(c, a, size);
    rl_nds_store(k->run->m, k->id, a, size, (uint32_t)value, seq);
    if (rl_nds_halt_lost(k->run->m, k->id))
        stop_halted(k);
    if (k->id == RL_NDS_ARM9 && a <= 0x04000249u && a + size > 0x04000240u)
        code_moved(k->run);
    clip(k);
}

/** @brief The bytes cpu fetches at pc, as hex for a report. */
static void bytes_at(rl_nds_arm *k, uint32_t pc, char *out, size_t n)
{
    uint8_t b[8];
    rl_nds_code_read(k->run->m, k->id, pc, b, sizeof b);
    size_t o = 0;
    for (unsigned i = 0; i < sizeof b && o + 4 < n; i++)
        o += (size_t)snprintf(out + o, n - o, i ? " %02x" : "%02x", b[i]);
}

/** @brief Code no unit was lifted from: stops the program with the CPU, the address and the bytes there. */
static int unfound(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    rl_nds_arm *k = cpu_of(c);
    uint32_t pc = c->pc & ~1u;
    char hex[32];
    bytes_at(k, pc, hex, sizeof hex);
    k->stats.unfound++;
    k->stopped = 2;
    return rl_guest_fatal(c, "nds", "unfound code: the %s at 0x%08x (%s state) fetches %s, which no lifted unit holds there", cpu_name(k),
                          pc, c->t ? "Thumb" : "ARM", hex);
}

int rl_nds_cpu_enter(rl_ctx *c, uint32_t unit, uint32_t fn)
{
    rl_nds_arm *k = cpu_of(c);
    rl_nds_run *r = k->run;
    uint64_t *seen = &r->seen[r->first[unit] + fn];
    if (*seen == rl_nds_code_epoch(r->m) + 1u)
        return 0;
    const rl_nds_unit *u = &r->p->units[unit];
    const rl_nds_fn *f = &u->fns[fn];
    uint32_t at = f->at & ~1u;
    k->stats.compares++;
    if (!rl_nds_code_equal(r->m, k->id, at, u->blob + f->offset, f->len))
        return 1;
    rl_nds_code_watch(r->m, k->id, at, f->len);
    *seen = rl_nds_code_epoch(r->m) + 1u;
    return 0;
}

rl_fn rl_nds_cpu_variant(rl_ctx *c, uint32_t pc, unsigned thumb)
{
    rl_nds_arm *k = cpu_of(c);
    rl_nds_run *r = k->run;
    uint32_t at = pc | (thumb & 1u);
    c->pc = pc;
    size_t lo = 0, hi = k->entry_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (k->entries[mid].at < at)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (; lo < k->entry_count && k->entries[lo].at == at; lo++) {
        const rl_nds_unit *u = &r->p->units[k->entries[lo].unit];
        const rl_nds_fn *f = &u->fns[k->entries[lo].fn];
        if (rl_nds_code_equal(r->m, k->id, pc, u->blob + f->offset, f->len)) {
            k->stats.variants++;
            return f->fn;
        }
    }
    return unfound;
}

/** @brief HALT: the CPU waits for IE & IF; when nothing can end the wait it leaves for good. */
static void halt(rl_nds_arm *k)
{
    if (!rl_nds_halt(k->run->m, k->id))
        stop_halted(k);
    clip(k);
}

void rl_nds_cpu_cp_write(rl_ctx *c, uint32_t key, uint32_t value, unsigned cond)
{
    rl_nds_arm *k = cpu_of(c);
    if (!cond || k->id != RL_NDS_ARM9)
        return;
    rl_nds *m = k->run->m;
    unsigned what = rl_nds_cp15_write(m, key, value);
    if (what & RL_CP15_CHANGED_TCM) {
        build_waits(k);
        code_moved(k->run);
    }
    if (what & RL_CP15_CHANGED_VECTORS)
        k->vectors = rl_arm32_cp15_vectors(rl_nds_cp15(m));
    if (what & (RL_CP15_UNSUPPORTED | RL_CP15_UNDEFINED))
        rl_report_unknown(0x4D435200000000ull | key, "nds", "MCR to coprocessor register key 0x%x (value 0x%08x) is not supported; kept", key,
                          value);
    if (what & RL_CP15_WAIT)
        halt(k);
}

uint32_t rl_nds_cpu_cp_read(rl_ctx *c, uint32_t key)
{
    rl_nds_arm *k = cpu_of(c);
    return rl_arm32_cp15_read(rl_nds_cp15(k->run->m), key);
}

void rl_nds_cpu_set_i(rl_ctx *c, uint32_t i)
{
    rl_nds_arm *k = cpu_of(c);
    k->i = (uint8_t)(i & 1u);
    clip(k);
}

int rl_nds_cpu_cond_fail(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    if (c->time.budget > 0)
        return RL_OK;
    return rl_sync(c, mem, next);
}

/** @brief A timed word access of the high-level BIOS. */
static uint32_t ld32(rl_nds_arm *k, uint32_t a) { return rl_nds_load(k->run->m, k->id, a, 4, false); }
static void st32(rl_nds_arm *k, uint32_t a, uint32_t v) { rl_nds_store(k->run->m, k->id, a, 4, v, false); }

/** @brief The IRQ handler slot the BIOS reads. */
static uint32_t irq_slot(rl_nds_arm *k)
{
    if (k->id == RL_NDS_ARM7)
        return 0x0380FFFCu;
    rl_arm32_tcm tcm;
    rl_arm32_cp15_tcm(rl_nds_cp15(k->run->m), &tcm);
    return tcm.dtcm_base + 0x3FFCu;
}

static void check_ticks(rl_nds_arm *k)
{
    uint64_t max = k->run->max_ticks;
    if (!max || rl_sched_now(k->k) <= max)
        return;
    rl_guest_fatal(k->c, "nds", "the %s passed tick %llu (--max-ticks)", cpu_name(k), (unsigned long long)max);
    rl_process_exit(RL_EXIT_SOFTWARE);
}

static uint64_t irq_enter(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    (void)mem;
    rl_nds_arm *k = cpu_of(c);
    check_ticks(k);
    k->i = c->i;
    if (c->i || !rl_nds_irq_poll(k->run->m, k->id)) {
        clip(k);
        return RL_NO_IRQ;
    }
    if (k->id == RL_NDS_ARM9 && k->vectors != 0xFFFF0000u) {
        rl_guest_fatal(c, "nds", "an IRQ with the ARM9's vectors at 0x%08x: only the BIOS's high vectors are implemented", k->vectors);
        rl_process_exit(RL_EXIT_SOFTWARE);
        return RL_NO_IRQ;
    }
    k->stats.irqs++;
    rl_arm32_exception(c, RL_ARM32_IRQ, (uint32_t)next + 4u);
    k->i = 1;
    uint32_t sp = c->r[13] - 24u;
    const uint32_t saved[6] = {c->r[0], c->r[1], c->r[2], c->r[3], c->r[12], c->r[14]};
    for (unsigned n = 0; n < 6; n++)
        st32(k, sp + 4u * n, saved[n]);
    c->r[13] = sp;
    uint32_t h = ld32(k, irq_slot(k));
    uint32_t ret = k->id == RL_NDS_ARM9 ? BIOS_IRQ_RETURN9 : BIOS_IRQ_RETURN7;
    c->r[0] = 0x04000000u;
    c->r[14] = ret;
    rl_sched_stall(k->k, BIOS_CYCLES);
    rl_arm32_call(c, ret);
    c->t = (uint8_t)(h & 1u);
    return h & (c->t ? ~1u : ~3u);
}

static uint64_t irq_return(rl_ctx *c, uint8_t *mem, uint64_t expected)
{
    (void)mem;
    (void)expected;
    rl_nds_arm *k = cpu_of(c);
    uint32_t sp = c->r[13], v[6];
    for (unsigned n = 0; n < 6; n++)
        v[n] = ld32(k, sp + 4u * n);
    c->r[0] = v[0];
    c->r[1] = v[1];
    c->r[2] = v[2];
    c->r[3] = v[3];
    c->r[12] = v[4];
    c->r[13] = sp + 24u;
    uint32_t to = v[5] - 4u;
    rl_arm32_restore(c);
    k->i = c->i;
    rl_sched_stall(k->k, BIOS_CYCLES);
    clip(k);
    return to;
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0;
    for (uint32_t bit = 1u << 30; bit; bit >>= 2) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
    }
    return r;
}

int rl_nds_swi_trap(rl_ctx *c, uint8_t *mem, uint32_t code)
{
    (void)mem;
    rl_nds_arm *k = cpu_of(c);
    unsigned number = code & 0xFFu;
    k->stats.swis++;
    rl_sched_stall(k->k, BIOS_CYCLES);
    switch (number) {
    case 0x06:
        halt(k);
        break;
    case 0x09: {
        int32_t n = (int32_t)c->r[0], d = (int32_t)c->r[1];
        if (d == 0)
            return rl_guest_fatal(c, "nds", "SWI Div by zero never returns on the console");
        int32_t q = (n == INT32_MIN && d == -1) ? INT32_MIN : n / d;
        int32_t rem = (n == INT32_MIN && d == -1) ? 0 : n % d;
        c->r[0] = (uint32_t)q;
        c->r[1] = (uint32_t)rem;
        c->r[3] = q < 0 ? 0u - (uint32_t)q : (uint32_t)q;
        break;
    }
    case 0x0D:
        c->r[0] = isqrt(c->r[0]);
        break;
    default:
        k->stats.unknown_swis++;
        rl_report_unknown(0x5357490000ull | (uint64_t)k->id << 8 | number, "nds", "%s SWI 0x%02x is not implemented; it does nothing",
                          cpu_name(k), number);
        break;
    }
    rl_arm32_restore(c);
    k->i = c->i;
    clip(k);
    return RL_OK;
}

/** @brief A word of the cartridge header. */
static uint32_t hdr32(const rl_nds_program *p, size_t at)
{
    const uint8_t *b = p->rom + at;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/** @brief Copies len bytes of the cartridge from off to addr as cpu sees it. */
static void load(rl_nds *m, unsigned cpu, uint32_t addr, const uint8_t *src, size_t len)
{
    for (size_t i = 0; i < len; i++)
        rl_nds_write(m, cpu, addr + (uint32_t)i, 1, src[i]);
}

/** @brief The registers GBATEK gives for cartridge boot: system mode, r12 and LR at the entry, the three stacks. */
static void boot_regs(rl_nds_arm *k, uint32_t sp, uint32_t sp_irq, uint32_t sp_svc)
{
    rl_ctx *c = k->c;
    rl_arm32_set_cpsr(c, RL_ARM32_IRQ | 0xC0u);
    c->r[13] = sp_irq;
    c->r[14] = 0;
    rl_arm32_set_mode(c, RL_ARM32_SVC);
    c->r[13] = sp_svc;
    c->r[14] = 0;
    rl_arm32_set_cpsr(c, RL_ARM32_SYS);
    for (unsigned n = 0; n < 12; n++)
        c->r[n] = 0;
    c->r[12] = k->entry;
    c->r[13] = sp;
    c->r[14] = k->entry;
    c->t = (uint8_t)(k->entry & 1u);
    k->entry &= ~1u;
    k->i = c->i;
}

/**
 * @brief The memory the BIOS and firmware leave at cartridge boot (GBATEK
 * "BIOS RAM Usage"): the header at 0x027FFE00, the boot indicator, both
 * binaries at their RAM addresses as stored, the shared work RAM given to
 * the ARM7, POWCNT1 at 0x820F (the oracle's boot) and the ARM9's vectors
 * high. Runs before the machine is attached
 * to the scheduler, whose clocks have not started.
 */
static int boot_memory(rl_nds_run *r)
{
    const rl_nds_program *p = r->p;
    rl_nds *m = r->m;
    if (p->rom_len < 0x170)
        return -1;
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        size_t h = cpu == RL_NDS_ARM9 ? 0x20 : 0x30;
        uint32_t off = hdr32(p, h), entry = hdr32(p, h + 4), ram = hdr32(p, h + 8), size = hdr32(p, h + 12);
        if ((uint64_t)off + size > p->rom_len)
            return -1;
        load(m, cpu, ram, p->rom + off, size);
        r->cpu[cpu].entry = entry;
    }
    load(m, RL_NDS_ARM9, 0x027FFE00u, p->rom, 0x170);
    if (rl_nds_set_cartridge(m, p->rom, p->rom_len) != 0)
        return -1;
    rl_nds_boot_state(m);
    rl_nds_write(m, RL_NDS_ARM9, 0x027FFC40u, 2, 1);
    rl_nds_write(m, RL_NDS_ARM9, 0x04000247u, 1, 3);
    rl_nds_write(m, RL_NDS_ARM9, 0x04000304u, 2, 0x820F);
    rl_nds_cp15_write(m, RL_CP15_KEY(0, 1, 0, 0), rl_nds_cp15(m)->control | RL_CP15_HIGH_VECTORS);
    r->cpu[RL_NDS_ARM9].vectors = rl_arm32_cp15_vectors(rl_nds_cp15(m));
    return 0;
}

static int by_entry(const void *a, const void *b)
{
    const nds_entry *x = a, *y = b;
    if (x->at != y->at)
        return x->at < y->at ? -1 : 1;
    if (x->unit != y->unit)
        return x->unit < y->unit ? -1 : 1;
    return x->fn < y->fn ? -1 : x->fn > y->fn;
}

/** @brief The dispatch table and the entry list of k's units; the first unit to name an address holds its slot. */
static int build_dispatch(rl_nds_run *r, rl_nds_arm *k)
{
    const rl_nds_program *p = r->p;
    uint64_t lo = UINT64_MAX, hi = 0;
    uint32_t n = 0;
    for (uint32_t u = 0; u < p->unit_count; u++) {
        const rl_nds_unit *x = &p->units[u];
        if (x->cpu != k->id)
            continue;
        if (x->unit->abi_version != RL_ABI_VERSION || x->unit->ctx_size != sizeof(rl_ctx)) {
            rl_log(RL_LOG_ERROR, "nds", "unit %s was built for ABI %u and a %u-byte context, the runtime has %u and %u", x->name,
                   x->unit->abi_version, x->unit->ctx_size, RL_ABI_VERSION, (unsigned)sizeof(rl_ctx));
            return -1;
        }
        for (uint32_t f = 0; f < x->fn_count; f++) {
            uint64_t a = x->fns[f].at & ~1u;
            lo = a < lo ? a : lo;
            hi = a + 2 > hi ? a + 2 : hi;
            n++;
        }
    }
    if (!n)
        return 0;
    k->lo = lo;
    k->count = (hi - lo) >> RL_ARM32_CODE_SHIFT;
    k->table = calloc(k->count, sizeof *k->table);
    k->entries = calloc(n, sizeof *k->entries);
    if (!k->table || !k->entries)
        return -1;
    for (uint32_t u = 0; u < p->unit_count; u++) {
        const rl_nds_unit *x = &p->units[u];
        if (x->cpu != k->id)
            continue;
        for (uint32_t f = 0; f < x->fn_count; f++) {
            uint64_t i = ((x->fns[f].at & ~1u) - lo) >> RL_ARM32_CODE_SHIFT;
            if (!k->table[i])
                k->table[i] = x->fns[f].fn;
            k->entries[k->entry_count++] = (nds_entry){x->fns[f].at, u, f};
        }
    }
    qsort(k->entries, k->entry_count, sizeof *k->entries, by_entry);
    rl_dispatch_own(k->c, k->table, k->lo, k->count);
    return 0;
}

int rl_nds_run_init(rl_nds_run *r, const rl_nds_program *p)
{
    memset(r, 0, sizeof *r);
    r->p = p;
    rl_sched_config cfg = {.ordered = true};
    r->s = rl_sched_create(&cfg);
    r->m = rl_nds_create();
    if (!r->s || !r->m)
        goto fail;
    for (unsigned i = 0; i < 2; i++) {
        rl_nds_arm *k = &r->cpu[i];
        k->run = r;
        k->id = i;
        k->k = rl_sched_add_cpu(r->s, i == RL_NDS_ARM9 ? 1u : 2u);
        k->t = rl_thread_alloc();
        if (!k->k || !k->t)
            goto fail;
    }
    if (boot_memory(r) != 0) {
        rl_log(RL_LOG_ERROR, "nds", "the cartridge's binaries lie outside the image");
        goto fail;
    }
    if (rl_nds_attach(r->m, r->s, r->cpu[0].k, r->cpu[1].k) != 0)
        goto fail;
    r->first = calloc(p->unit_count + 1u, sizeof *r->first);
    if (!r->first)
        goto fail;
    for (uint32_t u = 0; u < p->unit_count; u++) {
        r->first[u] = r->fn_total;
        r->fn_total += p->units[u].fn_count;
    }
    r->seen = calloc(r->fn_total + 1u, sizeof *r->seen);
    if (!r->seen)
        goto fail;
    for (unsigned i = 0; i < 2; i++) {
        rl_nds_arm *k = &r->cpu[i];
        if (rl_sched_attach(k->k, k->t) != 0)
            goto fail;
        k->c = k->t->ctx;
        k->c->machine = k;
        if (build_dispatch(r, k) != 0)
            goto fail;
        build_waits(k);
        rl_sched_set_irq(k->k, irq_enter);
        rl_sched_set_irq_return(k->k, irq_return);
        rl_arm32_set_lines(k->c, 0, 1ull << RL_NDS_LINE_WAKE9 | 1ull << RL_NDS_LINE_WAKE7);
    }
    rl_arm32_set_miss(unfound);
    boot_regs(&r->cpu[RL_NDS_ARM9], 0x03002F7Cu, 0x03003F80u, 0x03003FC0u);
    boot_regs(&r->cpu[RL_NDS_ARM7], 0x0380FD80u, 0x0380FF80u, 0x0380FFC0u);
    return 0;
fail:
    rl_nds_run_free(r);
    return -1;
}

static void body(rl_sched_cpu *s, void *arg)
{
    rl_nds_arm *k = arg;
    rl_thread_bind(k->t);
    rl_sched_run(s);
    int rc = rl_arm32_run(k->c, NULL, k->entry);
    const rl_pending *pd = &rl_common(k->c)->pend;
    if (rc != RL_OK && pd->kind == RL_PEND_EXIT_PROCESS)
        rl_process_exit(pd->code);
    if (!k->stopped)
        k->stopped = 2;
}

int rl_nds_run_start(rl_nds_run *r)
{
    for (unsigned i = 0; i < 2; i++)
        if (rl_sched_spawn(r->cpu[i].k, body, &r->cpu[i]) != 0)
            return -1;
    if (rl_sched_main(r->s) != 0)
        return -1;
    return r->cpu[0].stopped == 1 && r->cpu[1].stopped == 1 ? 0 : 1;
}

void rl_nds_run_free(rl_nds_run *r)
{
    rl_arm32_set_miss(NULL);
    if (r->s)
        rl_sched_destroy(r->s);
    if (r->m)
        rl_nds_destroy(r->m);
    for (unsigned i = 0; i < 2; i++) {
        free(r->cpu[i].table);
        free(r->cpu[i].entries);
        if (r->cpu[i].t)
            rl_thread_free(r->cpu[i].t);
    }
    free(r->first);
    free(r->seen);
    memset(r, 0, sizeof *r);
}
