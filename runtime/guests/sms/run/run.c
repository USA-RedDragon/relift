/**
 * @file run.c
 * @brief Lifted Master System and Game Gear code on the machine: the
 * entries of relift/sms/run.h, dispatch by the mapping, the instruction
 * boundary with its interrupts, and the run loop.
 *
 * Every instruction ends in rl_sms_sync, which advances the machine's clock
 * by the instruction's T-states and runs the boundary of the next one: the
 * NMI edge, then INT while IFF1 is set and the previous instruction was not
 * EI. An interrupt runs its lifted entry block (RL_Z80_ENTRY plus the kind)
 * in a host frame of its own.
 *
 * Host frames follow guest calls. A shadow stack holds the return address
 * of each open frame and the code the caller was lifted from (a ROM bank in
 * a slot, or a RAM group), so a return that goes elsewhere, or comes back
 * to a slot that now shows another bank, or to RAM that now holds other
 * bytes, unwinds to the run loop (RL_PEND_REDIRECT), which goes on at the
 * guest's pc through dispatch. Dispatch picks the function of the bank the
 * slot shows now, or of the RAM group whose opcode bytes the work RAM or
 * the cartridge RAM mapped there holds now. There
 * is no interpreter: the first instruction with no lifted body writes the
 * stop report and ends the run.
 */
#include "smsmain.h"

#include "core.h"
#include "z80.h"

#include <relift/sms/run.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHADOW_MAX 256u
#define BANKS 256u
#define SLOT 0x4000u
/** @brief The lowest address code in RAM can have: cartridge RAM at 0x8000, work RAM at 0xC000. */
#define RAM_LO 0x8000u

typedef struct ram_index {
    const rl_sms_ram_group *g;
    rl_fn *fns;
} ram_index;

typedef struct smsrun {
    sms m;
    rl_ctx *c;
    rl_fn *rom[BANKS][3];
    ram_index *ram;
    uint32_t ram_count;
    rl_fn entries[4];
    uint16_t shadow[SHADOW_MAX];
    uint32_t shadow_tag[SHADOW_MAX];
    uint32_t depth;
    uint16_t ret_to;
    bool done;
    uint16_t done_pc;
    bool stop;
    rl_sms_frame_hook *hook;
    void *user;
    rl_sms_stats st;
    FILE *trace;
    uint64_t trace_left;
} smsrun;

static smsrun G;

const rl_sms_stats *rl_sms_stats_get(void) { return &G.st; }

static sms_input_fn *g_input;
static void *g_input_user;

void rl_sms_input(sms_input_fn *fn, void *user)
{
    g_input = fn;
    g_input_user = user;
}

static FILE *g_trace;
static uint64_t g_trace_count;

void rl_sms_trace(FILE *f, uint64_t count)
{
    g_trace = f;
    g_trace_count = count;
}

static bool tracing(void)
{
    if (!G.trace_left)
        return false;
    G.trace_left--;
    return true;
}

static uint64_t now_in(const rl_ctx *c) { return G.m.cycles + (uint64_t)-c->time.budget; }

/* Code lookup. */

/**
 * @brief Whether a fetch at pc reads the open bus: the memory control port
 * has unmapped the cartridge (or the Game Gear BIOS window, with no BIOS) or
 * work RAM there.
 */
static bool open_bus(uint16_t pc)
{
    const sms *m = &G.m;
    if (m->system == SMS_SYSTEM_SG)
        return false;
    if (pc >= 0xC000 && !m->ram_enable)
        return true;
    if (m->system == SMS_SYSTEM_GG || m->system == SMS_SYSTEM_GG_MS)
        return pc < 0x0400 && m->bios_enable;
    return pc < 0xC000 && !m->cart_enable;
}

/** @brief The ROM bank the bytes at pc come from, or -1 (RAM, cartridge RAM, the open bus or nothing). */
static int32_t code_bank(uint16_t pc)
{
    if (pc >= 0xC000 || open_bus(pc))
        return -1;
    if (G.m.mapper == SMS_MAPPER_CODEMASTERS && G.m.cm_ram && pc >= 0xA000)
        return -1;
    if (pc < 0x0400 && G.m.mapper == SMS_MAPPER_SEGA && G.m.system != SMS_SYSTEM_SG)
        return 0;
    return sms_slot_bank(&G.m, pc >> 14);
}

static bool ram_matches(const rl_sms_ram_group *g, uint16_t pc, uint32_t *at)
{
    uint32_t lo = 0, hi = g->count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g->insns[mid].addr < pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == g->count || g->insns[lo].addr != pc)
        return false;
    const rl_sms_ram_insn *i = &g->insns[lo];
    for (uint32_t k = 0; k < i->len; k++)
        if ((i->check >> k & 1u) && sms_peek(&G.m, (uint16_t)(pc + k)) != i->bytes[k])
            return false;
    *at = lo;
    return true;
}

static rl_fn find(uint16_t pc)
{
    if (open_bus(pc))
        return NULL;
    int32_t b = code_bank(pc);
    if (b >= 0) {
        if ((uint32_t)b >= BANKS)
            return NULL;
        unsigned slot = pc < 0x0400 && G.m.mapper == SMS_MAPPER_SEGA ? 0 : pc >> 14;
        rl_fn *t = G.rom[b][slot];
        return t ? t[pc & (SLOT - 1u)] : NULL;
    }
    if (pc < RAM_LO)
        return NULL;
    for (uint32_t k = 0; k < G.ram_count; k++) {
        uint32_t at;
        if (G.ram[k].fns[pc - RAM_LO] && ram_matches(G.ram[k].g, pc, &at))
            return G.ram[k].fns[pc - RAM_LO];
    }
    return NULL;
}

/** @brief Whether the code a block of unit tag (slot and bank or group plus one) assumes at pc is still there. */
static bool tag_ok(uint32_t tag, uint16_t pc)
{
    uint32_t unit = tag >> 2, slot = tag & 3u;
    if (!unit)
        return true;
    if (open_bus(pc))
        return false;
    if (slot == RL_SMS_SLOT_RAM) {
        uint32_t at;
        return pc < RAM_LO || unit - 1u >= G.ram_count || ram_matches(G.ram[unit - 1u].g, pc, &at);
    }
    if (pc >= 0xC000)
        return true;
    unsigned s = pc < 0x0400 && G.m.mapper == SMS_MAPPER_SEGA ? 0 : pc >> 14;
    if (s != slot)
        return true;
    return code_bank(pc) == (int32_t)(unit - 1u);
}

static uint32_t tag_of(uint32_t info) { return RL_SMS_SYNC_UNIT(info) << 2 | RL_SMS_SYNC_SLOT(info); }

/* Reports. */

static bool report_peek(uint64_t a, void *out, size_t n)
{
    if (a > 0xFFFF || n > 0x10000 - a)
        return false;
    uint8_t *o = out;
    for (size_t i = 0; i < n; i++)
        o[i] = sms_peek(&G.m, (uint16_t)(a + i));
    return true;
}

static void report_where(uint64_t a, char *buf, size_t n)
{
    if (a <= 0xFFFF && open_bus((uint16_t)a)) {
        snprintf(buf, n, "the open bus: the memory control port unmapped it");
        return;
    }
    if (a >= 0xC000) {
        snprintf(buf, n, "work RAM");
        return;
    }
    int32_t b = code_bank((uint16_t)a);
    if (b < 0)
        snprintf(buf, n, "cartridge RAM");
    else
        snprintf(buf, n, "ROM bank %d (mapped now), file offset 0x%llx", b,
                 (unsigned long long)((uint64_t)b * SLOT + (a & (SLOT - 1u))));
}

static const rl_report_guest g_report_guest = {report_peek, report_where, 0, NULL};

static int missed(rl_ctx *c, uint16_t pc)
{
    rl_miss_info mi = {pc, RL_SITE_NONE, RL_MISS_JUMP, 0};
    char where[96], what[256];
    report_where(pc, where, sizeof where);
    uint8_t b[4];
    report_peek(pc, b, 4);
    snprintf(what, sizeof what, "no lifted code at guest 0x%04x (%s), bytes %02x %02x %02x %02x", pc, where, b[0], b[1],
             b[2], b[3]);
    rl_report_stop(c, "missed code", what, &mi, NULL, 0);
    return rl_pend_exit_process(c, RL_EXIT_SOFTWARE);
}

static uint64_t miss_fn(rl_ctx *c, uint8_t *mem)
{
    G.st.dispatches++;
    rl_fn f = find(c->pc);
    if (f)
        RL_MUSTTAIL return f(c, mem);
    return (uint64_t)missed(c, c->pc);
}

/* The boundary. */

static int redirect(rl_ctx *c, uint16_t pc)
{
    G.st.redirects++;
    G.done = false;
    rl_pending *p = &rl_common(c)->pend;
    p->kind = RL_PEND_REDIRECT;
    p->pc = pc;
    p->sp = RL_SP_NONE;
    p->code = 0;
    return RL_PENDING;
}

static int enter_frame(uint16_t ret, uint32_t tag)
{
    if (G.depth >= SHADOW_MAX)
        return -1;
    G.shadow_tag[G.depth] = tag;
    G.shadow[G.depth++] = ret;
    return 0;
}

/**
 * @brief Runs the boundary before the instruction at pc: takes the
 * interrupts that are due, each in a host frame that returns to pc in code
 * of tag. RL_OK when the instruction at pc may run.
 */
static int boundary(rl_ctx *c, uint8_t *mem, uint16_t pc, uint32_t tag)
{
    for (;;) {
        if (G.stop)
            return rl_pend_exit_process(c, 0);
        sms *m = &G.m;
        if (m->cycles >= m->event_cycle)
            sms_catch_up(m, m->cycles);
        if (G.stop)
            return rl_pend_exit_process(c, 0);
        uint32_t kind;
        if (m->nmi) {
            m->nmi = false;
            kind = RL_Z80_ENTRY_NMI;
            G.st.nmis++;
        } else if (c->iff1 && !c->ei && sms_int_line(m, m->cycles)) {
            kind = c->im == 0 ? RL_Z80_ENTRY_IM0 : c->im == 1 ? RL_Z80_ENTRY_IM1 : RL_Z80_ENTRY_IM2;
            G.st.irqs++;
        } else {
            G.done = true;
            G.done_pc = pc;
            if (G.trace && tracing())
                fprintf(G.trace, "I\t%llu\t%04x\t%02x%02x\t%02x%02x\t%02x%02x\t%02x%02x\t%04x\t%04x\t%04x\t%02x\n",
                        (unsigned long long)m->cycles, pc, c->a, c->f, c->b, c->c, c->d, c->e, c->h, c->l, c->ix, c->iy,
                        c->sp, c->r);
            return RL_OK;
        }
        if (G.trace && G.trace_left)
            fprintf(G.trace, "Q\t%llu\t%s\t%04x\n", (unsigned long long)m->cycles, kind == RL_Z80_ENTRY_NMI ? "nmi" : "irq",
                    pc);
        G.done = false;
        if (enter_frame(pc, tag) != 0)
            return redirect(c, pc);
        c->ret = pc;
        c->pc = (uint16_t)(RL_Z80_ENTRY + kind);
        int r = (int)G.entries[kind](c, mem);
        if (r != RL_OK)
            return r;
        if (G.done && G.done_pc == pc)
            return RL_OK;
    }
}

static int returned(rl_ctx *c, uint8_t *mem, uint16_t pc)
{
    if (G.depth == 0) {
        G.ret_to = pc;
        return boundary(c, mem, pc, 0);
    }
    if (G.shadow[G.depth - 1] != pc)
        return redirect(c, pc);
    uint32_t tag = G.shadow_tag[--G.depth];
    if (!tag_ok(tag, pc))
        return redirect(c, pc);
    return boundary(c, mem, pc, tag);
}

int rl_sms_sync(rl_ctx *c, uint8_t *mem, uint64_t next, uint32_t info)
{
    if (next > 0xFFFFu)
        return RL_OK;
    G.m.cycles = now_in(c);
    c->time.budget = 0;
    G.st.insns++;
    uint16_t pc = (uint16_t)next;
    uint32_t tag = tag_of(info);
    switch (RL_SMS_SYNC_KIND(info)) {
    case RL_SMS_SYNC_RETURN:
        return returned(c, mem, pc);
    case RL_SMS_SYNC_CALL:
        G.st.calls++;
        if ((info & RL_SMS_SYNC_CHECK) && !tag_ok(tag, pc))
            return redirect(c, pc);
        if (enter_frame(RL_SMS_SYNC_RET(info), tag) != 0)
            return redirect(c, pc);
        break;
    default:
        if ((info & RL_SMS_SYNC_CHECK) && !tag_ok(tag, pc))
            return redirect(c, pc);
        break;
    }
    return boundary(c, mem, pc, (info & RL_SMS_SYNC_CHECK) ? tag : 0);
}

/* Bus. */

uint64_t rl_sms_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size)
{
    (void)mem;
    uint64_t now = now_in(c) + 3u, v = 0;
    for (uint32_t i = 0; i < size; i++)
        v |= (uint64_t)sms_read(&G.m, (uint16_t)(addr + i), now + 3u * i) << (8 * i);
    return v;
}

void rl_sms_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value)
{
    (void)mem;
    uint64_t now = now_in(c) + 3u;
    for (uint32_t i = 0; i < size; i++)
        sms_write(&G.m, (uint16_t)(addr + i), (uint8_t)(value >> (8 * i)), now + 3u * i);
}

uint8_t rl_sms_in(rl_ctx *c, uint16_t port)
{
    uint64_t t = now_in(c) + 4u;
    uint8_t v = sms_in(&G.m, port, t);
    if (G.trace && G.trace_left)
        fprintf(G.trace, "N\t%llu\t%02x\t%02x\n", (unsigned long long)t, port & 0xFFu, v);
    return v;
}

void rl_sms_out(rl_ctx *c, uint16_t port, uint8_t value)
{
    uint64_t t = now_in(c) + 4u;
    if (G.trace && G.trace_left)
        fprintf(G.trace, "O\t%llu\t%02x\t%02x\n", (unsigned long long)t, port & 0xFFu, value);
    sms_out(&G.m, port, value, t);
}

uint8_t rl_sms_halt(rl_ctx *c, uint8_t *mem, uint8_t iff1, uint8_t r)
{
    (void)mem;
    G.st.halts++;
    sms *m = &G.m;
    uint64_t now = now_in(c), k = 0;
    for (;;) {
        uint64_t t = now + 4u * k;
        if (t >= m->event_cycle)
            sms_catch_up(m, t);
        if (G.stop || m->nmi || (iff1 && (m->system == SMS_SYSTEM_SG ? m->tms.irq_level : m->vdp.irq_level)))
            break;
        uint64_t e = m->event_cycle;
        k = e <= t ? k + 1 : (e - now + 3u) / 4u;
    }
    c->time.budget -= (int64_t)(4u * k);
    return (uint8_t)((r & 0x80u) | ((r + k) & 0x7Fu));
}

/* The run. */

static void on_frame(void *user, uint64_t frame)
{
    (void)user;
    if (G.hook && G.hook(G.user, &G.m, frame))
        G.stop = true;
}

static int add_rom(const rl_sms_rom_unit *u)
{
    if (u->bank >= BANKS || u->slot > 2)
        return -1;
    rl_fn **t = &G.rom[u->bank][u->slot];
    if (!*t && !(*t = calloc(SLOT, sizeof(rl_fn))))
        return -1;
    uint32_t base = u->slot * SLOT;
    for (uint32_t i = 0; i < u->unit->fn_count; i++) {
        const rl_fn_entry *e = &u->unit->fns[i];
        uint64_t n = (e->flags & RL_FN_RANGE) ? e->guest_size : 1;
        for (uint64_t j = 0; j < n; j++) {
            uint64_t a = e->guest_addr + j;
            if (a >= base && a < base + SLOT && !(*t)[a - base])
                (*t)[a - base] = e->fn;
        }
    }
    return 0;
}

static int add_ram(ram_index *x, const rl_sms_ram_group *g)
{
    x->g = g;
    x->fns = calloc(0x10000u - RAM_LO, sizeof(rl_fn));
    if (!x->fns)
        return -1;
    for (uint32_t i = 0; i < g->unit->fn_count; i++) {
        const rl_fn_entry *e = &g->unit->fns[i];
        uint64_t n = (e->flags & RL_FN_RANGE) ? e->guest_size : 1;
        for (uint64_t j = 0; j < n; j++) {
            uint64_t a = e->guest_addr + j;
            if (a >= RAM_LO && a <= 0xFFFF && !x->fns[a - RAM_LO])
                x->fns[a - RAM_LO] = e->fn;
        }
    }
    return 0;
}

static void release(void)
{
    for (uint32_t b = 0; b < BANKS; b++)
        for (uint32_t s = 0; s < 3; s++)
            free(G.rom[b][s]);
    for (uint32_t k = 0; k < G.ram_count; k++)
        free(G.ram[k].fns);
    free(G.ram);
    sms_free(&G.m);
}

int rl_sms_run(const rl_sms_program *p, const uint8_t *inputs, size_t input_count, rl_sms_frame_hook *hook, void *user)
{
    memset(&G, 0, sizeof G);
    G.hook = hook;
    G.user = user;
    G.trace = g_trace;
    G.trace_left = g_trace_count;
    sms_config cfg = {.system = p->system, .region = p->region, .mapper = p->mapper};
    if (sms_init(&G.m, &cfg, p->rom, p->rom_len) != 0)
        return 1;
    G.m.inputs = inputs;
    G.m.input_count = input_count;
    G.m.input = g_input;
    G.m.input_user = g_input_user;
    G.m.buttons = input_count ? inputs[0] : g_input ? g_input(g_input_user) : 0;
    G.m.on_frame = on_frame;
    static const rl_guest_desc guest = {.name = "sms", .page_size = 0x400, .address_bits = 18, .tick_hz = 3579545};
    if (rl_core_init(&guest, rl_z80_cpu()) != 0)
        return 1;
    rl_units_reset();
    const rl_unit *list[] = {p->entries};
    if (rl_units_register(list, 1) != 0) {
        fprintf(stderr, "relift-sms: the lifted units do not match this runtime\n");
        return 1;
    }
    for (uint32_t k = 0; k < 4; k++) {
        G.entries[k] = rl_dispatch_find(RL_Z80_ENTRY + k);
        if (!G.entries[k]) {
            fprintf(stderr, "relift-sms: interrupt entry %u is not lifted\n", k);
            return 1;
        }
    }
    for (uint32_t k = 0; k < p->rom_count; k++)
        if (add_rom(&p->roms[k]) != 0)
            return 1;
    G.ram = calloc(p->ram_count ? p->ram_count : 1, sizeof *G.ram);
    if (!G.ram)
        return 1;
    G.ram_count = p->ram_count;
    for (uint32_t k = 0; k < p->ram_count; k++)
        if (add_ram(&G.ram[k], &p->rams[k]) != 0)
            return 1;
    rl_z80_set_miss(miss_fn);
    rl_report_set_guest(&g_report_guest);
    rl_report_info("guest", "sms");
    rl_report_module("rom", 0, 0x10000, NULL);
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return 1;
    rl_thread_bind(t);
    rl_ctx *c = t->ctx;
    G.c = c;
    c->sp = p->system == SMS_SYSTEM_SG ? 0xFFFFu : 0xFFFDu;
    c->time.budget = 0;
    uint16_t pc = 0;
    int r;
    for (;;) {
        G.depth = 0;
        r = G.done && G.done_pc == pc ? RL_OK : boundary(c, NULL, pc, 0);
        if (r == RL_OK) {
            rl_fn f = find(pc);
            if (!f) {
                r = missed(c, pc);
            } else {
                c->pc = pc;
                G.ret_to = pc;
                r = (int)f(c, NULL);
                if (r == RL_OK) {
                    pc = G.ret_to;
                    continue;
                }
            }
        }
        rl_pending *pend = &rl_common(c)->pend;
        if (r == RL_PENDING && pend->kind == RL_PEND_REDIRECT) {
            pc = (uint16_t)pend->pc;
            pend->kind = RL_PEND_NONE;
            pend->sp = RL_SP_NONE;
            continue;
        }
        break;
    }
    rl_pending *pend = &rl_common(c)->pend;
    int code = pend->kind == RL_PEND_EXIT_PROCESS ? (int)pend->code : 1;
    release();
    return G.stop ? 0 : code;
}
