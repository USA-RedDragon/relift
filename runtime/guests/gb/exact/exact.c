/**
 * @file exact.c
 * @brief The exact Game Boy mode: lifted instruction bodies on the device
 * library's bus and boundary.
 *
 * The library (gbdev_abi.h) counts the dots, owns every byte of the bus and
 * runs the instruction boundary: interrupt entry, HALT, STOP, DMA stalls
 * and the opcode fetch. Lifted code runs the body of each instruction in
 * between: its accesses go to the library at the cycle the lifter placed
 * them, the operand fetches and internal cycles before them are made here,
 * and the sync point after the instruction closes it and opens the next.
 *
 * Host frames follow guest calls. A shadow stack holds the return address
 * of every open host frame, so a return that goes elsewhere, or a direct
 * transfer into the window under another bank, unwinds to the run loop
 * (RL_PEND_REDIRECT), which goes on at the guest's real pc through
 * dispatch. Code with no lifted function (code in RAM, missed by discovery,
 * or entered mid-function after an unwind) runs on the library's CPU, one
 * instruction at a time, until its frame returns; calls and jumps from it
 * into lifted functions go back to lifted code.
 */
#include "exact_int.h"
#include "crash.h"
#include "sm83.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHADOW_MAX 256u
#define WINDOW 0x4000u
#define WINDOW_END 0x8000u
#define BANKS 512u
#define MOVED 2

static const uint8_t g_len[256] = {
    1, 3, 1, 1, 1, 1, 2, 1, 3, 1, 1, 1, 1, 1, 2, 1, 2, 3, 1, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 2, 1,
    2, 3, 1, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 2, 1, 2, 3, 1, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 3, 3, 3, 1, 2, 1, 1, 1, 3, 2, 3, 3, 2, 1, 1, 1, 3, 1, 3, 1, 2, 1, 1, 1, 3, 1, 3, 1, 2, 1,
    2, 1, 1, 1, 1, 1, 2, 1, 2, 1, 3, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 2, 1, 2, 1, 3, 1, 1, 1, 2, 1,
};

typedef struct gbx {
    rlgbd *h;
    bool dmg, mbc1;
    const uint8_t *rom;
    size_t rom_len;
    rl_fn *banks[BANKS];
    /** @brief The lifted unit of each ROM bank, for the reports' bank-aware lookup. */
    const rl_unit *bank_units[BANKS];
    rl_gbx_ram_fn *ram[0x10000 - WINDOW_END];
    uint16_t ram_n[0x10000 - WINDOW_END];
    bool open;
    uint8_t op;
    uint8_t operands;
    uint8_t fetched;
    uint8_t imm[2];
    uint64_t dma_until;
    uint16_t ipc;
    uint16_t fpc;
    uint32_t off;
    uint16_t shadow[SHADOW_MAX];
    uint16_t shadow_bank[SHADOW_MAX];
    uint32_t depth;
    uint16_t ret_to;
    rl_gbx_poll_fn *poll;
    void *poll_user;
    rl_gbx_stats st;
    bool logging;
    bool learning;
    uint8_t cause;
    uint64_t run_key;
    rl_gbx_seen *seen;
    size_t seen_cap, seen_n;
} gbx;

static gbx G;

static int interp(rl_ctx *c, uint8_t *mem);
static int ram_fn(rl_ctx *c, uint8_t *mem);

static int window_fn(rl_ctx *c, uint8_t *mem);

const rl_gbx_stats *rl_gbx_stats_get(void) { return &G.st; }

void rl_gbx_log_interpreted(bool on) { G.logging = on; }

void rl_gbx_log_entries(bool on) { G.learning = on; }


/** @brief Report peek: the bus as the library sees it, without side effects. */
static bool report_peek(uint64_t a, void *out, size_t n)
{
    if (!G.h || a > 0xFFFF || n > 0x10000 - a)
        return false;
    uint8_t *o = out;
    for (size_t i = 0; i < n; i++)
        o[i] = rlgbd_peek(G.h, (uint16_t)(a + i));
    return true;
}

/** @brief Report place: the memory region of a, with the ROM bank and file offset the window maps now. */
static void report_where(uint64_t a, char *buf, size_t n)
{
    if (a < WINDOW)
        snprintf(buf, n, "ROM bank %u, file offset 0x%llx", G.h ? rlgbd_rom_bank0(G.h) : 0u,
                 (unsigned long long)((G.h ? rlgbd_rom_bank0(G.h) : 0u) * 0x4000ull + a));
    else if (a < WINDOW_END) {
        uint32_t b = G.h ? rlgbd_rom_bank(G.h) : 1u;
        snprintf(buf, n, "ROM bank %u (mapped now), file offset 0x%llx", b,
                 (unsigned long long)(b * 0x4000ull + a - WINDOW));
    } else if (a < 0xA000)
        snprintf(buf, n, "VRAM");
    else if (a < 0xC000)
        snprintf(buf, n, "cartridge RAM");
    else if (a < 0xE000)
        snprintf(buf, n, "WRAM");
    else if (a < 0xFE00)
        snprintf(buf, n, "echo RAM");
    else if (a < 0xFEA0)
        snprintf(buf, n, "OAM");
    else if (a < 0xFF80)
        snprintf(buf, n, "I/O");
    else
        snprintf(buf, n, "HRAM");
}

/**
 * @brief Report lookup: a function of the bank mapped in the window now, for
 * 0x4000-0x7FFF, where every bank's code shares the addresses.
 */
static int report_find(uint64_t a, rl_fn_hit *out)
{
    if (a < WINDOW || a >= WINDOW_END)
        return -1;
    uint32_t bank = G.h ? rlgbd_rom_bank(G.h) : 1u;
    const rl_unit *u = bank < BANKS ? G.bank_units[bank] : NULL;
    for (uint32_t k = 0; u && k < u->map_count; k++) {
        const rl_fn_map *m = u->maps[k];
        for (uint32_t i = 0; i < m->count; i++)
            if (a >= m->fns[i].guest_addr && a - m->fns[i].guest_addr < m->fns[i].guest_size) {
                out->map = m;
                out->fn = &m->fns[i];
                out->off = a - m->fns[i].guest_addr;
                out->exact = true;
                return 1;
            }
    }
    return 0;
}

static const rl_report_guest g_report_guest = { report_peek, report_where, 1, report_find };

static uint64_t seen_hash(uint64_t k)
{
    k ^= k >> 29;
    k *= 0xBF58476D1CE4E5B9ull;
    return k ^ (k >> 32);
}

static rl_gbx_seen *seen_slot(uint64_t key)
{
    if (G.seen_n * 2 >= G.seen_cap) {
        size_t cap = G.seen_cap ? G.seen_cap * 2 : 4096;
        rl_gbx_seen *t = calloc(cap, sizeof *t);
        if (!t)
            return NULL;
        for (size_t i = 0; i < G.seen_cap; i++) {
            if (!G.seen[i].count)
                continue;
            size_t j = seen_hash(G.seen[i].key) & (cap - 1);
            while (t[j].count)
                j = (j + 1) & (cap - 1);
            t[j] = G.seen[i];
        }
        free(G.seen);
        G.seen = t;
        G.seen_cap = cap;
    }
    size_t j = seen_hash(key) & (G.seen_cap - 1);
    while (G.seen[j].count && G.seen[j].key != key)
        j = (j + 1) & (G.seen_cap - 1);
    if (!G.seen[j].count) {
        G.seen[j].key = key;
        G.seen_n++;
    }
    return &G.seen[j];
}

static uint32_t bank_of(uint16_t pc) { return pc >= WINDOW && pc < WINDOW_END ? rlgbd_rom_bank(G.h) : 0; }

static void log_insn(uint16_t pc, uint8_t op)
{
    rl_gbx_seen *s = seen_slot(RL_GBX_SEEN_KEY(RL_GBX_SEEN_INSN, bank_of(pc), pc));
    if (!s)
        return;
    uint8_t b[3] = {op, 0, 0};
    for (uint32_t i = 1; i < g_len[op]; i++)
        b[i] = rlgbd_peek(G.h, (uint16_t)(pc + i));
    if (!s->count)
        memcpy(s->bytes, b, 3);
    else if (memcmp(s->bytes, b, g_len[op]) != 0)
        s->varied = 1;
    s->count++;
    s = seen_slot(G.run_key);
    if (s)
        s->insns++;
}

static void log_entry(uint16_t pc)
{
    G.run_key = RL_GBX_SEEN_KEY(G.cause, bank_of(pc), pc);
    rl_gbx_seen *s = seen_slot(G.run_key);
    if (s)
        s->count++;
}

const rl_gbx_seen *rl_gbx_seen_get(size_t *n)
{
    *n = G.seen_cap;
    return G.seen;
}

void rl_gbx_set_poll(rl_gbx_poll_fn *fn, void *user)
{
    G.poll = fn;
    G.poll_user = user;
}

static rl_fn bank_fn(uint32_t bank, uint16_t pc)
{
    rl_fn *t = bank < BANKS ? G.banks[bank] : NULL;
    return t ? t[pc - WINDOW] : NULL;
}

static rl_fn lifted_at(uint16_t pc)
{
    if (pc < WINDOW)
        return rl_dispatch_find(pc);
    if (pc < WINDOW_END)
        return bank_fn(rlgbd_rom_bank(G.h), pc);
    return G.ram_n[pc - WINDOW_END] ? ram_fn : NULL;
}

static rl_fn find(uint16_t pc)
{
    rl_fn f = lifted_at(pc);
    return f ? f : interp;
}

static int window_fn(rl_ctx *c, uint8_t *mem) { RL_MUSTTAIL return find(c->pc)(c, mem); }

static int sled_fn(rl_ctx *c, uint8_t *mem);

int rl_gbx_add_ram(uint16_t pc, uint16_t op, rl_fn fn)
{
    if (pc < WINDOW_END || !fn)
        return -1;
    uint32_t i = pc - WINDOW_END;
    for (uint32_t k = 0; k < G.ram_n[i]; k++)
        if (G.ram[i][k].op == op)
            return 0;
    if (op >= 0x200)
        return -1;
    rl_gbx_ram_fn *t = realloc(G.ram[i], (G.ram_n[i] + 1u) * sizeof *t);
    if (!t)
        return -1;
    t[G.ram_n[i]++] = (rl_gbx_ram_fn){.op = op, .fn = fn};
    G.ram[i] = t;
    return 0;
}

/**
 * @brief Whether a read of addr right now gives the byte peek shows: work RAM,
 * echo RAM, cartridge RAM and high RAM outside an OAM DMA. Video RAM and OAM
 * (locked while the picture unit reads them), the I/O registers and every
 * address during a DMA answer the bus differently.
 */
static bool peek_is_bus(uint16_t addr)
{
    if (addr >= 0xFF80 && addr <= 0xFFFE)
        return true;
    return addr >= 0xA000 && addr < 0xFE00 && rlgbd_now(G.h) >= G.dma_until;
}

/** @brief The key of the open instruction at pc for the RAM table: the opcode, or 0x100 plus the byte after a CB prefix. */
static uint16_t ram_key(uint16_t pc)
{
    return G.op == 0xCB ? (uint16_t)(0x100u | rlgbd_peek(G.h, (uint16_t)(pc + 1))) : G.op;
}

static int32_t idu_value(const rl_ctx *c, uint8_t op);

/**
 * @brief The lifted function for the open instruction at pc in RAM, or NULL;
 * makes the DMG increment unit's bus value when it returns one, as boundary
 * does for lifted ROM code.
 */
static rl_fn ram_enter(rl_ctx *c, uint16_t pc)
{
    uint32_t i = pc - WINDOW_END;
    if (pc < WINDOW_END || !G.ram_n[i])
        return NULL;
    if (G.op == 0xCB && !peek_is_bus((uint16_t)(pc + 1)))
        return NULL;
    uint16_t key = ram_key(pc);
    for (uint32_t k = 0; k < G.ram_n[i]; k++)
        if (G.ram[i][k].op == key) {
            if (G.dmg) {
                int32_t pre = idu_value(c, G.op);
                if (pre >= 0) {
                    if ((pre & 0xFF00) == 0xFE00)
                        G.st.native[RL_GBX_OAM_BUG]++;
                    rlgbd_idu(G.h, (uint16_t)pre);
                }
            }
            return G.ram[i][k].fn;
        }
    return NULL;
}

int rl_gbx_add_sled(uint32_t bank, uint16_t start, uint16_t end)
{
    if (start >= end)
        return 0;
    if (bank == UINT32_MAX) {
        if (end > WINDOW)
            return -1;
        uint32_t n = 0;
        rl_fn_entry *e = calloc(end - start, sizeof *e);
        rl_unit *u = calloc(1, sizeof *u);
        if (!e || !u) {
            free(e);
            free(u);
            return -1;
        }
        for (uint32_t a = start; a < end; a++)
            if (!rl_dispatch_find(a))
                e[n++] = (rl_fn_entry){.guest_addr = a, .guest_size = 1, .flags = RL_FN_FRAGMENT | RL_FN_MAY_UNWIND, .fn = sled_fn};
        u->abi_version = RL_ABI_VERSION;
        u->ctx_size = sizeof(rl_ctx);
        u->fn_count = n;
        u->fns = e;
        u->name = "gb-sled";
        const rl_unit *list[] = {u};
        return n ? rl_units_register(list, 1) : 0;
    }
    if (bank >= BANKS || start < WINDOW || end > WINDOW_END)
        return -1;
    if (!G.banks[bank]) {
        G.banks[bank] = calloc(WINDOW, sizeof(rl_fn));
        if (!G.banks[bank])
            return -1;
    }
    for (uint32_t a = start; a < end; a++)
        if (!G.banks[bank][a - WINDOW])
            G.banks[bank][a - WINDOW] = sled_fn;
    return 0;
}

int rl_gbx_add_unit(const rl_unit *u, uint32_t bank)
{
    if (bank == UINT32_MAX)
        return rl_units_register(&u, 1);
    if (bank >= BANKS)
        return -1;
    if (!G.banks[bank]) {
        G.banks[bank] = calloc(WINDOW, sizeof(rl_fn));
        if (!G.banks[bank])
            return -1;
    }
    for (uint32_t i = 0; i < u->fn_count; i++) {
        uint64_t a = u->fns[i].guest_addr;
        if (a >= WINDOW && a < WINDOW_END && !G.banks[bank][a - WINDOW])
            G.banks[bank][a - WINDOW] = u->fns[i].fn;
    }
    G.bank_units[bank] = u;
    return u->map_count ? rl_fn_maps_register(u->maps, u->map_count) : 0;
}

/** @brief The dispatch miss: lifted RAM code for the open instruction, else the interpreter. */
static int miss_fn(rl_ctx *c, uint8_t *mem)
{
    if (c->pc >= WINDOW_END && c->pc <= 0xFFFF && G.ram_n[c->pc - WINDOW_END])
        RL_MUSTTAIL return ram_fn(c, mem);
    RL_MUSTTAIL return interp(c, mem);
}

int rl_gbx_init(rlgbd *h, const uint8_t *rom, size_t rom_len, bool dmg, bool mbc1)
{
    for (uint32_t i = 0; i < BANKS; i++)
        free(G.banks[i]);
    for (uint32_t i = 0; i < 0x10000 - WINDOW_END; i++)
        free(G.ram[i]);
    free(G.seen);
    memset(&G, 0, sizeof G);
    G.h = h;
    G.rom = rom;
    G.rom_len = rom_len;
    G.dmg = dmg;
    G.mbc1 = mbc1;
    if (rl_dispatch_reserve(0, 0x10000) != 0)
        return -1;
    static rl_fn_entry window[WINDOW];
    static rl_unit unit;
    for (uint32_t i = 0; i < WINDOW; i++)
        window[i] = (rl_fn_entry){.guest_addr = WINDOW + i, .guest_size = 1, .flags = RL_FN_FRAGMENT | RL_FN_MAY_UNWIND, .fn = window_fn};
    memset(&unit, 0, sizeof unit);
    unit.abi_version = RL_ABI_VERSION;
    unit.ctx_size = sizeof(rl_ctx);
    unit.fn_count = WINDOW;
    unit.fns = window;
    unit.name = "gb-window";
    const rl_unit *list[] = {&unit};
    rl_sm83_set_miss(miss_fn);
    rl_report_set_guest(&g_report_guest);
    rl_report_info("guest", "gb");
    rl_report_module("rom0", 0, WINDOW, NULL);
    return rl_units_register(list, 1);
}

static uint8_t lib_ime(void)
{
    rlgbd_regs r;
    rlgbd_get_regs(G.h, &r);
    return r.ime;
}

void rl_gbx_regs(const rl_ctx *c, uint16_t pc, rlgbd_regs *r)
{
    *r = (rlgbd_regs){.a = c->a, .f = c->f, .b = c->b, .c = c->c, .d = c->d, .e = c->e, .h = c->h, .l = c->l,
                      .pc = pc, .sp = c->sp, .ime = lib_ime()};
}

static void pull(rl_ctx *c)
{
    rlgbd_regs r;
    rlgbd_get_regs(G.h, &r);
    c->a = r.a;
    c->f = r.f;
    c->b = r.b;
    c->c = r.c;
    c->d = r.d;
    c->e = r.e;
    c->h = r.h;
    c->l = r.l;
    c->sp = r.sp;
    c->pc = r.pc;
}

/** @brief The dots an OAM DMA may hold the bus for after the write that starts it, at either speed. */
#define DMA_DOTS (4u * 164u)

static bool hits_dma(uint16_t a) { return a == 0xFF46; }

static bool pushes_on_dma(uint16_t sp) { return hits_dma((uint16_t)(sp - 1)) || hits_dma((uint16_t)(sp - 2)); }

/** @brief Whether the open instruction op at pc may write the OAM DMA register, from the registers in c. */
static bool may_start_dma(const rl_ctx *c, uint16_t pc, uint8_t op)
{
    uint16_t hl = (uint16_t)(c->h << 8 | c->l);
    uint8_t n1 = rlgbd_peek(G.h, (uint16_t)(pc + 1));
    uint16_t nn = (uint16_t)(rlgbd_peek(G.h, (uint16_t)(pc + 2)) << 8 | n1);
    bool known = pc < WINDOW_END || (peek_is_bus((uint16_t)(pc + 1)) && (g_len[op] < 3 || peek_is_bus((uint16_t)(pc + 2))));
    switch (op) {
    case 0xE0: return !known || hits_dma((uint16_t)(0xFF00 | n1));
    case 0xE2: return hits_dma((uint16_t)(0xFF00 | c->c));
    case 0xEA: return !known || hits_dma(nn);
    case 0x08: return !known || hits_dma(nn) || hits_dma((uint16_t)(nn + 1));
    case 0x02: return hits_dma((uint16_t)(c->b << 8 | c->c));
    case 0x12: return hits_dma((uint16_t)(c->d << 8 | c->e));
    case 0x22: case 0x32: case 0x34: case 0x35: case 0x36: return hits_dma(hl);
    case 0xCB: return (!known || ((n1 & 7) == 6 && (n1 & 0xC0) != 0x40)) && hits_dma(hl);
    case 0xC5: case 0xD5: case 0xE5: case 0xF5: case 0xCD: case 0xC4: case 0xCC: case 0xD4: case 0xDC:
        return pushes_on_dma(c->sp);
    default:
        if (op >= 0x70 && op <= 0x77 && op != 0x76)
            return hits_dma(hl);
        return (op & 0xC7) == 0xC7 && pushes_on_dma(c->sp);
    }
}

static void dma_started(void) { G.dma_until = rlgbd_now(G.h) + DMA_DOTS; }

/** @brief Runs the open instruction on the library's CPU with the registers of c. */
static void run_native(rl_ctx *c)
{
    rlgbd_regs r;
    bool dma = may_start_dma(c, G.ipc, G.op);
    rl_gbx_regs(c, rlgbd_pc(G.h), &r);
    rlgbd_set_regs(G.h, &r);
    rlgbd_exec_native(G.h, G.op);
    G.open = false;
    pull(c);
    if (dma)
        dma_started();
}

static int redirect(rl_ctx *c, uint16_t pc, int why)
{
    G.st.redirects[why]++;
    G.cause = (uint8_t)(RL_GBX_SEEN_REDIRECT + why);
    rl_pending *p = &rl_common(c)->pend;
    p->kind = RL_PEND_REDIRECT;
    p->pc = pc;
    p->sp = RL_SP_NONE;
    p->code = 0;
    return RL_PENDING;
}

static uint8_t rom_byte(uint32_t bank, uint16_t pc)
{
    size_t o = (size_t)bank * WINDOW + (pc & (WINDOW - 1));
    return G.rom_len ? G.rom[o % G.rom_len] : 0xFF;
}

/**
 * @brief The register the 16-bit increment unit of op works on before its
 * internal cycle (INC rr, DEC rr, PUSH), whose value on the address bus is
 * the DMG OAM bug; -1 for other opcodes.
 */
static int32_t idu_value(const rl_ctx *c, uint8_t op)
{
    if (op < 0x40 && ((op & 0xF) == 3 || (op & 0xF) == 0xB)) {
        switch (op >> 4) {
        case 0: return c->b << 8 | c->c;
        case 1: return c->d << 8 | c->e;
        case 2: return c->h << 8 | c->l;
        default: return c->sp;
        }
    }
    if (op >= 0xC0 && (op & 0xF) == 5)
        return c->sp;
    return -1;
}

static int native_reason(const rl_ctx *c, uint16_t pc, uint8_t op)
{
    (void)c;
    if (pc < WINDOW_END && g_len[op] > 1 && rlgbd_now(G.h) < G.dma_until)
        return RL_GBX_DMA;
    if (pc < WINDOW) {
        if (G.mbc1 && rlgbd_rom_bank0(G.h) != 0)
            return RL_GBX_BANK0;
        if (op != rom_byte(0, pc))
            return RL_GBX_FETCH;
    } else if (pc < WINDOW_END && op != rom_byte(rlgbd_rom_bank(G.h), pc)) {
        return RL_GBX_FETCH;
    }
    return -1;
}

static int call_guest(rl_ctx *c, uint8_t *mem, uint16_t pc, uint16_t ret);

/**
 * @brief Runs boundary steps until the instruction at pc is open. RL_OK:
 * open, the lifted body may run (lifted false: open for the interpreter,
 * unchecked). MOVED: the instruction ran on the library's CPU and the guest
 * is at c->pc with nothing open. Else RL_PENDING.
 */
static int boundary(rl_ctx *c, uint8_t *mem, uint16_t pc, bool lifted)
{
    for (;;) {
        if (G.poll) {
            int code = G.poll(G.poll_user, c, pc);
            if (code)
                return rl_pend_exit_process(c, (uint32_t)code);
        }
        G.st.steps++;
        int32_t op = rlgbd_at(G.h, pc, c->sp);
        uint16_t lpc = rlgbd_pc(G.h);
        if (op >= 0) {
            G.open = true;
            G.op = (uint8_t)op;
            G.ipc = pc;
            G.fpc = (uint16_t)(pc + 1);
            G.operands = (uint8_t)(g_len[op] - 1);
            G.fetched = 0;
            G.off = 4;
            c->time.budget = 0;
            if (lifted) {
                int why = lpc != (uint16_t)(pc + 1) ? RL_GBX_HALT_BUG : native_reason(c, pc, (uint8_t)op);
                if (why >= 0) {
                    G.st.native[why]++;
                    run_native(c);
                    return MOVED;
                }
                if (G.dmg && pc < WINDOW_END) {
                    int32_t pre = idu_value(c, (uint8_t)op);
                    if (pre >= 0) {
                        if ((pre & 0xFF00) == 0xFE00)
                            G.st.native[RL_GBX_OAM_BUG]++;
                        rlgbd_idu(G.h, (uint16_t)pre);
                    }
                }
            }
            return RL_OK;
        }
        if (lpc != pc) {
            if (pushes_on_dma(c->sp))
                dma_started();
            c->sp = rlgbd_sp(G.h);
            G.st.irqs++;
            int r = call_guest(c, mem, lpc, pc);
            if (r != RL_OK)
                return r;
            if (G.open && G.ipc == pc)
                return RL_OK;
        }
    }
}

/** @brief Opens a host frame that returns to ret, in code that assumes bank (plus one; 0 for none) at ret. */
static int enter_frame(uint16_t ret, uint32_t bank)
{
    if (G.depth >= SHADOW_MAX)
        return -1;
    G.shadow_bank[G.depth] = (uint16_t)bank;
    G.shadow[G.depth++] = ret;
    if (G.depth > G.st.max_depth)
        G.st.max_depth = G.depth;
    return 0;
}

static uint32_t mapped_at(uint16_t pc) { return pc >= WINDOW && pc < WINDOW_END ? rlgbd_rom_bank(G.h) + 1 : 0; }

static int call_guest(rl_ctx *c, uint8_t *mem, uint16_t pc, uint16_t ret)
{
    if (enter_frame(ret, mapped_at(ret)) != 0)
        return redirect(c, pc, RL_GBX_DEPTH);
    int b = boundary(c, mem, pc, true);
    if (b == MOVED)
        return redirect(c, c->pc, RL_GBX_NATIVE);
    if (b != RL_OK)
        return b;
    uint8_t was = G.cause;
    G.cause = lifted_at(pc) ? RL_GBX_SEEN_DISPATCH : RL_GBX_SEEN_IRQ;
    c->pc = pc;
    int r = find(pc)(c, mem);
    if (r == RL_OK)
        G.cause = was;
    return r;
}

/** @brief After a return to pc: closes the host frame it returns from and opens the instruction there, or redirects. */
static int returned(rl_ctx *c, uint8_t *mem, uint16_t pc)
{
    if (G.depth == 0) {
        G.ret_to = pc;
    } else if (G.shadow[G.depth - 1] == pc) {
        uint32_t bank = G.shadow_bank[--G.depth];
        if (bank && mapped_at(pc) != bank)
            return redirect(c, pc, RL_GBX_BANK);
    } else {
        return redirect(c, pc, RL_GBX_RET);
    }
    int b = boundary(c, mem, pc, true);
    return b == MOVED ? redirect(c, c->pc, RL_GBX_NATIVE) : b;
}

uint8_t rl_gb_operand(rl_ctx *c, uint32_t i)
{
    (void)c;
    while (G.fetched <= i && G.operands) {
        G.imm[G.fetched++ & 1] = rlgbd_read(G.h, G.fpc++);
        G.operands--;
        G.off += 4;
    }
    return G.imm[i & 1];
}

static void fill_to(uint32_t at)
{
    while (G.off < at) {
        if (G.operands) {
            G.imm[G.fetched++ & 1] = rlgbd_read(G.h, G.fpc++);
            G.operands--;
        } else {
            rlgbd_internal(G.h);
        }
        G.off += 4;
    }
}

uint64_t rl_gb_io_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size)
{
    (void)mem;
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; i++) {
        fill_to((uint32_t)-c->time.budget + 4 * i);
        v |= (uint64_t)rlgbd_read(G.h, (uint16_t)(addr + i)) << (8 * i);
        G.off += 4;
    }
    return v;
}

void rl_gb_io_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value)
{
    (void)mem;
    for (uint32_t i = 0; i < size; i++) {
        fill_to((uint32_t)-c->time.budget + 4 * i);
        rlgbd_write(G.h, (uint16_t)(addr + i), (uint8_t)(value >> (8 * i)));
        G.off += 4;
        if (hits_dma((uint16_t)(addr + i)))
            dma_started();
    }
}

void rl_gb_reti(rl_ctx *c)
{
    (void)c;
    rlgbd_set_ime(G.h, 1);
}

int rl_gb_sync(rl_ctx *c, uint8_t *mem, uint64_t next, uint32_t info)
{
    if (next > 0xFFFFu)
        return RL_OK;
    uint16_t pc = (uint16_t)next;
    while (G.operands) {
        rlgbd_read(G.h, G.fpc++);
        G.operands--;
    }
    rlgbd_end(G.h, (uint32_t)-c->time.budget);
    G.open = false;
    G.st.lifted++;
    switch (RL_GB_SYNC_KIND(info)) {
    case RL_GB_SYNC_CALL:
        if (enter_frame(RL_GB_SYNC_RET(info), RL_GB_SYNC_BANK(info)) != 0)
            return redirect(c, pc, RL_GBX_DEPTH);
        break;
    case RL_GB_SYNC_RETURN:
        return returned(c, mem, pc);
    default:
        break;
    }
    uint32_t bank = RL_GB_SYNC_BANK(info);
    if ((info & RL_GB_SYNC_CHECK) && pc >= WINDOW && pc < WINDOW_END && rlgbd_rom_bank(G.h) != bank - 1)
        return redirect(c, pc, RL_GBX_BANK);
    int b = boundary(c, mem, pc, true);
    return b == MOVED ? redirect(c, c->pc, RL_GBX_NATIVE) : b;
}

/** @brief Lifted RAM code: the function for the open instruction at c->pc, else the interpreter. */
static int ram_fn(rl_ctx *c, uint8_t *mem)
{
    rl_fn f = ram_enter(c, (uint16_t)c->pc);
    if (f)
        RL_MUSTTAIL return f(c, mem);
    RL_MUSTTAIL return interp(c, mem);
}

/**
 * @brief A run of nop instructions, entered with the one at c->pc open: ends
 * each (its 4 cycles are the opcode fetch) and opens the next at a boundary
 * of its own, as lifted nop instructions would, until the next is not in a
 * sled.
 */
static int sled_fn(rl_ctx *c, uint8_t *mem)
{
    uint16_t pc = (uint16_t)c->pc;
    for (;;) {
        rlgbd_end(G.h, 4);
        G.open = false;
        G.st.lifted++;
        G.st.sled++;
        pc++;
        int b = boundary(c, mem, pc, true);
        if (b == MOVED)
            return redirect(c, c->pc, RL_GBX_NATIVE);
        if (b != RL_OK)
            return b;
        rl_fn f = lifted_at(pc);
        if (f != sled_fn) {
            c->pc = pc;
            RL_MUSTTAIL return find(pc)(c, mem);
        }
    }
}

int rl_gb_trap(rl_ctx *c, uint8_t *mem, uint32_t code)
{
    uint16_t next = (uint16_t)code;
    run_native(c);
    G.st.traps++;
    if (c->pc != next)
        return redirect(c, c->pc, RL_GBX_TRAP);
    int b = boundary(c, mem, next, true);
    return b == MOVED ? redirect(c, c->pc, RL_GBX_NATIVE) : b;
}

static bool is_return(uint8_t op) { return op == 0xC9 || op == 0xD9 || op == 0xC0 || op == 0xC8 || op == 0xD0 || op == 0xD8; }

static bool is_call(uint8_t op)
{
    return op == 0xCD || op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC || (op & 0xC7) == 0xC7;
}

static int region(uint16_t pc) { return pc < WINDOW_END ? RL_GBX_ROM : RL_GBX_RAM; }

/**
 * @brief The interpreter: the open instruction at G.ipc and the ones after
 * it on the library's CPU until the frame returns (the stack pointer rises
 * above where it was at entry through a return) or reaches the entry of a
 * lifted function, which it calls when a call got there and goes on in
 * otherwise.
 */
static int interp(rl_ctx *c, uint8_t *mem)
{
    uint16_t base = c->sp;
    G.st.interp_entries[region(G.ipc)]++;
    if (G.logging || G.learning)
        log_entry(G.ipc);
    uint64_t run = G.run_key;
    G.cause = RL_GBX_SEEN_DISPATCH;
    for (;;) {
        uint16_t at = G.ipc, sp0 = c->sp;
        uint8_t op = G.op;
        G.st.interp[region(at)]++;
        if (G.logging)
            log_insn(at, op);
        if (G.learning && at >= WINDOW_END && (op != 0xCB || peek_is_bus((uint16_t)(at + 1)))) {
            uint16_t key = op == 0xCB ? (uint16_t)(0x100u | rlgbd_peek(G.h, (uint16_t)(at + 1))) : op;
            rl_gbx_seen *s = seen_slot(RL_GBX_SEEN_KEY(RL_GBX_SEEN_RAMOP, key, at));
            if (s)
                s->count++;
        }
        run_native(c);
        uint16_t pc = c->pc;
        uint16_t after = (uint16_t)(at + g_len[op]);
        if (pc != after && (G.logging || G.learning) && !lifted_at(pc)) {
            rl_gbx_seen *t = seen_slot(RL_GBX_SEEN_KEY(RL_GBX_SEEN_TARGET, bank_of(pc), pc));
            if (t)
                t->count++;
        }
        if (is_return(op) && c->sp > base)
            return returned(c, mem, pc);
        rl_fn f = lifted_at(pc);
        if (f == ram_fn) {
            int b = boundary(c, mem, pc, true);
            if (b == MOVED)
                return redirect(c, c->pc, RL_GBX_NATIVE);
            if (b != RL_OK)
                return b;
            f = ram_enter(c, pc);
            if (!f)
                continue;
            if (pc != after && is_call(op) && c->sp == (uint16_t)(sp0 - 2)) {
                if (enter_frame(after, 0) != 0)
                    return redirect(c, pc, RL_GBX_DEPTH);
                c->pc = pc;
            int r = f(c, mem);
                if (r != RL_OK)
                    return r;
                if (G.logging || G.learning)
                    G.run_key = run;
                continue;
            }
            c->pc = pc;
            return f(c, mem);
        }
        if (f && pc != after && is_call(op) && c->sp == (uint16_t)(sp0 - 2)) {
            if (enter_frame(after, 0) != 0)
                return redirect(c, pc, RL_GBX_DEPTH);
            int b = boundary(c, mem, pc, true);
            if (b == MOVED)
                return redirect(c, c->pc, RL_GBX_NATIVE);
            if (b != RL_OK)
                return b;
            c->pc = pc;
            int r = f(c, mem);
            if (r != RL_OK)
                return r;
            if (G.logging || G.learning)
                G.run_key = run;
            continue;
        }
        if (f) {
            int b = boundary(c, mem, pc, true);
            if (b == MOVED)
                return redirect(c, c->pc, RL_GBX_NATIVE);
            if (b != RL_OK)
                return b;
            c->pc = pc;
            return f(c, mem);
        }
        int b = boundary(c, mem, pc, false);
        if (b != RL_OK)
            return b;
    }
}

int rl_gbx_run(rl_ctx *c, uint8_t *mem, uint16_t pc)
{
    bool need = true;
    G.cause = RL_GBX_SEEN_START;
    for (;;) {
        G.depth = 0;
        rl_pending *p = &rl_common(c)->pend;
        int r = RL_OK;
        if (need) {
            r = boundary(c, mem, pc, true);
            if (r == MOVED) {
                pc = c->pc;
                continue;
            }
        }
        if (r == RL_OK) {
            c->pc = pc;
            r = find(pc)(c, mem);
        }
        G.cause = RL_GBX_SEEN_DISPATCH;
        if (r == RL_OK) {
            pc = G.ret_to;
            G.cause = RL_GBX_SEEN_RETURNED;
            need = false;
            continue;
        }
        if (p->kind == RL_PEND_REDIRECT) {
            pc = (uint16_t)p->pc;
            p->kind = RL_PEND_NONE;
            need = true;
            continue;
        }
        if (p->kind == RL_PEND_EXIT_PROCESS)
            return (int)p->code;
        rl_log(RL_LOG_ERROR, "gb", "lifted code stopped at pc 0x%04x (pending kind %u)", (unsigned)c->pc, (unsigned)p->kind);
        return 1;
    }
}
