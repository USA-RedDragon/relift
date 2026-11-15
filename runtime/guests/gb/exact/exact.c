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
#include "sm83.h"

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
    bool open;
    uint8_t op;
    uint8_t operands;
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
} gbx;

static gbx G;

static int interp(rl_ctx *c, uint8_t *mem);

static int window_fn(rl_ctx *c, uint8_t *mem);

const rl_gbx_stats *rl_gbx_stats_get(void) { return &G.st; }

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
    return NULL;
}

static rl_fn find(uint16_t pc)
{
    rl_fn f = lifted_at(pc);
    return f ? f : interp;
}

static int window_fn(rl_ctx *c, uint8_t *mem) { RL_MUSTTAIL return find(c->pc)(c, mem); }

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
    return 0;
}

int rl_gbx_init(rlgbd *h, const uint8_t *rom, size_t rom_len, bool dmg, bool mbc1)
{
    for (uint32_t i = 0; i < BANKS; i++)
        free(G.banks[i]);
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
    rl_sm83_set_miss(interp);
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

/** @brief Runs the open instruction on the library's CPU with the registers of c. */
static void run_native(rl_ctx *c)
{
    rlgbd_regs r;
    rl_gbx_regs(c, rlgbd_pc(G.h), &r);
    rlgbd_set_regs(G.h, &r);
    rlgbd_exec_native(G.h, G.op);
    G.open = false;
    pull(c);
}

static int redirect(rl_ctx *c, uint16_t pc, int why)
{
    G.st.redirects[why]++;
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
            G.off = 4;
            c->time.budget = 0;
            if (lifted) {
                int why = lpc != (uint16_t)(pc + 1) ? RL_GBX_HALT_BUG : native_reason(c, pc, (uint8_t)op);
                if (why >= 0) {
                    G.st.native[why]++;
                    run_native(c);
                    return MOVED;
                }
                if (G.dmg) {
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
    return find(pc)(c, mem);
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

static void fill_to(uint32_t at)
{
    while (G.off < at) {
        if (G.operands) {
            rlgbd_read(G.h, G.fpc++);
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
    for (;;) {
        uint16_t at = G.ipc, sp0 = c->sp;
        uint8_t op = G.op;
        G.st.interp[region(at)]++;
        run_native(c);
        uint16_t pc = c->pc;
        uint16_t after = (uint16_t)(at + g_len[op]);
        if (is_return(op) && c->sp > base)
            return returned(c, mem, pc);
        rl_fn f = lifted_at(pc);
        if (f && pc != after && is_call(op) && c->sp == (uint16_t)(sp0 - 2)) {
            if (enter_frame(after, 0) != 0)
                return redirect(c, pc, RL_GBX_DEPTH);
            int b = boundary(c, mem, pc, true);
            if (b == MOVED)
                return redirect(c, c->pc, RL_GBX_NATIVE);
            if (b != RL_OK)
                return b;
            int r = f(c, mem);
            if (r != RL_OK)
                return r;
            continue;
        }
        if (f) {
            int b = boundary(c, mem, pc, true);
            if (b == MOVED)
                return redirect(c, c->pc, RL_GBX_NATIVE);
            if (b != RL_OK)
                return b;
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
        if (r == RL_OK)
            r = find(pc)(c, mem);
        if (r == RL_OK) {
            pc = G.ret_to;
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
