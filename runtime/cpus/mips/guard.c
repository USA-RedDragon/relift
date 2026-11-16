/**
 * @file guard.c
 * @brief The slow paths of relift-cpu-mips's guards: the instruction a guard
 * left at runs here. A load or store outside kseg0 and kseg1 goes through
 * the TLB and the bus; a COP1 operation the fast path does not take runs on
 * the FPU rules; anything else enters its exception. A guard in a delay
 * slot also runs the branch (its link write and target), since the lifted
 * block wrote nothing before the guard.
 */
#include "mips.h"

#include "fpu.h"

static const rl_mips_bus *g_bus;

void rl_mips_set_bus(const rl_mips_bus *bus) { g_bus = bus; }

static uint64_t sext32(uint64_t v) { return (uint64_t)(int64_t)(int32_t)(uint32_t)v; }
static uint64_t simm(uint32_t w) { return (uint64_t)(int64_t)(int16_t)(uint16_t)w; }

static uint64_t bus_read(rl_ctx *c, uint8_t *mem, uint64_t pa, uint32_t size)
{
    if (g_bus)
        return g_bus->read(c, mem, pa, size);
    switch (size) {
    case 1:
        return rl_ld8(mem, pa);
    case 2:
        return rl_ld16(mem, pa);
    case 4:
        return rl_ld32(mem, pa);
    default:
        return rl_ld64(mem, pa);
    }
}

static void bus_write(rl_ctx *c, uint8_t *mem, uint64_t pa, uint32_t size, uint64_t v)
{
    if (g_bus) {
        g_bus->write(c, mem, pa, size, v);
        return;
    }
    switch (size) {
    case 1:
        rl_st8(mem, pa, (uint8_t)v);
        break;
    case 2:
        rl_st16(mem, pa, (uint16_t)v);
        break;
    case 4:
        rl_st32(mem, pa, (uint32_t)v);
        break;
    default:
        rl_st64(mem, pa, v);
        break;
    }
}

static void set_gpr(rl_ctx *c, unsigned n, uint64_t v)
{
    if (n)
        c->r[n] = v;
}

static bool fr_mode(const rl_ctx *c) { return (c->cp0_status & RL_MIPS_SR_FR) != 0; }

/** @brief The 32-bit move view of FPU register n (MFC1, SWC1): in FR = 0 an odd register is the high word of the even one. */
static uint32_t fpr_get32(const rl_ctx *c, unsigned n)
{
    if (!fr_mode(c) && (n & 1u))
        return (uint32_t)(c->f[n & ~1u] >> 32);
    return (uint32_t)c->f[n];
}

static void fpr_set32(rl_ctx *c, unsigned n, uint32_t v)
{
    if (!fr_mode(c) && (n & 1u))
        c->f[n & ~1u] = (uint64_t)v << 32 | (uint32_t)c->f[n & ~1u];
    else
        c->f[n] = (c->f[n] & ~UINT64_C(0xFFFFFFFF)) | v;
}

static uint64_t fpr_get64(const rl_ctx *c, unsigned n) { return c->f[fr_mode(c) ? n : n & ~1u]; }
static void fpr_set64(rl_ctx *c, unsigned n, uint64_t v) { c->f[fr_mode(c) ? n : n & ~1u] = v; }

/** @brief Size and direction of a load or store opcode; 0 when op is none. */
static unsigned access_size(uint32_t op, bool *store)
{
    *store = false;
    switch (op) {
    case 0x20:
    case 0x24:
        return 1;
    case 0x21:
    case 0x25:
        return 2;
    case 0x22:
    case 0x23:
    case 0x26:
    case 0x27:
    case 0x30:
    case 0x31:
        return 4;
    case 0x1A:
    case 0x1B:
    case 0x34:
    case 0x35:
    case 0x37:
        return 8;
    case 0x28:
        *store = true;
        return 1;
    case 0x29:
        *store = true;
        return 2;
    case 0x2A:
    case 0x2B:
    case 0x2E:
    case 0x38:
    case 0x39:
        *store = true;
        return 4;
    case 0x2C:
    case 0x2D:
    case 0x3C:
    case 0x3D:
    case 0x3F:
        *store = true;
        return 8;
    default:
        return 0;
    }
}

/** @brief Whether op takes an unaligned address (LWL, LWR, LDL, LDR and the stores). */
static bool unaligned_op(uint32_t op)
{
    return op == 0x22 || op == 0x26 || op == 0x1A || op == 0x1B || op == 0x2A || op == 0x2E || op == 0x2C ||
           op == 0x2D;
}

/** @brief Result of running a load or store: done, or the exception it entered (the vector). */
typedef struct mem_result {
    bool done;
    uint64_t vector;
} mem_result;

/** @brief Runs the load or store insn at pc through the TLB. */
static mem_result run_access(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn, bool in_delay)
{
    mem_result out = {false, 0};
    uint32_t op = insn >> 26;
    unsigned base = insn >> 21 & 31u, rt = insn >> 16 & 31u;
    bool store;
    unsigned size = access_size(op, &store);
    uint64_t va = c->r[base] + simm(insn);
    if ((op == 0x38 || op == 0x3C) && !c->cp0_ll_bit) {
        set_gpr(c, rt, 0);
        out.done = true;
        return out;
    }
    if (!unaligned_op(op) && (va & (size - 1u))) {
        out.vector = rl_mips_xlat_exception(c, RL_MIPS_XLAT_ADDRESS, store, va, pc, in_delay);
        return out;
    }
    uint64_t pa = 0;
    rl_mips_xlat x = rl_mips_translate(c, va, store, &pa);
    if (x != RL_MIPS_XLAT_OK) {
        out.vector = rl_mips_xlat_exception(c, x, store, va, pc, in_delay);
        return out;
    }
    uint64_t old = c->r[rt];
    unsigned k4 = (unsigned)(pa & 3u), k8 = (unsigned)(pa & 7u);
    uint64_t a4 = pa & ~UINT64_C(3), a8 = pa & ~UINT64_C(7);
    switch (op) {
    case 0x20:
        set_gpr(c, rt, (uint64_t)(int64_t)(int8_t)bus_read(c, mem, pa, 1));
        break;
    case 0x24:
        set_gpr(c, rt, bus_read(c, mem, pa, 1));
        break;
    case 0x21:
        set_gpr(c, rt, (uint64_t)(int64_t)(int16_t)bus_read(c, mem, pa, 2));
        break;
    case 0x25:
        set_gpr(c, rt, bus_read(c, mem, pa, 2));
        break;
    case 0x23:
    case 0x30:
        set_gpr(c, rt, sext32(bus_read(c, mem, pa, 4)));
        break;
    case 0x27:
        set_gpr(c, rt, (uint32_t)bus_read(c, mem, pa, 4));
        break;
    case 0x37:
    case 0x34:
        set_gpr(c, rt, bus_read(c, mem, pa, 8));
        break;
    case 0x31:
        fpr_set32(c, rt, (uint32_t)bus_read(c, mem, pa, 4));
        break;
    case 0x35:
        fpr_set64(c, rt, bus_read(c, mem, pa, 8));
        break;
    case 0x22: {
        uint32_t w = (uint32_t)bus_read(c, mem, a4, 4);
        uint32_t keep = k4 ? (uint32_t)old & ((1u << (8u * k4)) - 1u) : 0u;
        set_gpr(c, rt, sext32(w << (8u * k4) | keep));
        break;
    }
    case 0x26: {
        uint32_t w = (uint32_t)bus_read(c, mem, a4, 4);
        unsigned sh = 8u * (3u - k4);
        if (k4 == 3u)
            set_gpr(c, rt, sext32(w));
        else
            set_gpr(c, rt, (old & ~(uint64_t)(0xFFFFFFFFu >> sh)) | (w >> sh));
        break;
    }
    case 0x1A: {
        uint64_t d = bus_read(c, mem, a8, 8);
        uint64_t keep = k8 ? old & ((UINT64_C(1) << (8u * k8)) - 1u) : 0u;
        set_gpr(c, rt, d << (8u * k8) | keep);
        break;
    }
    case 0x1B: {
        uint64_t d = bus_read(c, mem, a8, 8);
        unsigned sh = 8u * (7u - k8);
        uint64_t m = ~UINT64_C(0) >> sh;
        set_gpr(c, rt, (old & ~m) | (d >> sh));
        break;
    }
    case 0x28:
        bus_write(c, mem, pa, 1, old & 0xFFu);
        break;
    case 0x29:
        bus_write(c, mem, pa, 2, old & 0xFFFFu);
        break;
    case 0x2B:
    case 0x38:
        bus_write(c, mem, pa, 4, (uint32_t)old);
        break;
    case 0x3F:
    case 0x3C:
        bus_write(c, mem, pa, 8, old);
        break;
    case 0x39:
        bus_write(c, mem, pa, 4, fpr_get32(c, rt));
        break;
    case 0x3D:
        bus_write(c, mem, pa, 8, fpr_get64(c, rt));
        break;
    case 0x2A: {
        uint32_t w = (uint32_t)bus_read(c, mem, a4, 4);
        uint32_t m = 0xFFFFFFFFu >> (8u * k4);
        bus_write(c, mem, a4, 4, (w & ~m) | ((uint32_t)old >> (8u * k4)));
        break;
    }
    case 0x2E: {
        uint32_t w = (uint32_t)bus_read(c, mem, a4, 4);
        unsigned sh = 8u * (3u - k4);
        uint32_t m = 0xFFFFFFFFu << sh;
        bus_write(c, mem, a4, 4, (w & ~m) | ((uint32_t)old << sh));
        break;
    }
    case 0x2C: {
        uint64_t d = bus_read(c, mem, a8, 8);
        uint64_t m = ~UINT64_C(0) >> (8u * k8);
        bus_write(c, mem, a8, 8, (d & ~m) | (old >> (8u * k8)));
        break;
    }
    case 0x2D: {
        uint64_t d = bus_read(c, mem, a8, 8);
        unsigned sh = 8u * (7u - k8);
        uint64_t m = ~UINT64_C(0) << sh;
        bus_write(c, mem, a8, 8, (d & ~m) | (old << sh));
        break;
    }
    default:
        break;
    }
    if (op == 0x30 || op == 0x34) {
        c->cp0_ll_bit = 1;
        c->cp0_ll_addr = pa >> 4;
    }
    if (op == 0x38 || op == 0x3C)
        set_gpr(c, rt, 1);
    out.done = true;
    return out;
}

/** @brief The coprocessor an instruction names (Cause.CE of a coprocessor-unusable exception). */
static uint32_t cop_of(uint32_t insn)
{
    uint32_t op = insn >> 26;
    if ((op & 0x3Cu) == 0x10u || (op & 0x38u) == 0x30u)
        return op & 3u;
    return 0;
}

/**
 * @brief The instruction insn at pc whose guard fired with ExcCode exc (or
 * FP_SLOW): runs it or enters its exception. Returns true when it entered
 * one; *vector is then the vector.
 */
static bool run_guarded(rl_ctx *c, uint8_t *mem, uint32_t code, uint64_t pc, uint32_t insn, bool in_delay,
                        uint64_t *vector)
{
    rl_mips_get_stats()->slow++;
    if (code & RL_MIPS_GUARD_FP_SLOW) {
        if (!rl_mips_fpu(c, insn))
            return false;
        *vector = rl_mips_exception(c, RL_MIPS_EXC_FPE, pc, in_delay, 0);
        return true;
    }
    uint32_t exc = code & 0xFFu;
    switch (exc) {
    case RL_MIPS_EXC_TLBL:
    case RL_MIPS_EXC_TLBS:
    case RL_MIPS_EXC_ADEL:
    case RL_MIPS_EXC_ADES: {
        bool store;
        if (access_size(insn >> 26, &store)) {
            mem_result r = run_access(c, mem, pc, insn, in_delay);
            if (r.done)
                return false;
            *vector = r.vector;
            return true;
        }
        break;
    }
    case RL_MIPS_EXC_FPE:
        if (insn >> 26 == 0x11 && (insn >> 21 & 31u) == 6u && (insn >> 11 & 31u) == 31u)
            c->fcr31 = (uint32_t)c->r[insn >> 16 & 31u] & RL_MIPS_FCR_MASK;
        else if (rl_mips_fpu_is_arith(insn) && !rl_mips_fpu(c, insn))
            return false;
        break;
    default:
        break;
    }
    *vector = rl_mips_exception(c, exc, pc, in_delay, exc == RL_MIPS_EXC_CPU ? cop_of(insn) : 0);
    return true;
}

int rl_mips_guard(rl_ctx *c, uint8_t *mem, uint32_t code, uint64_t pc, uint32_t w0)
{
    uint64_t v;
    if (!run_guarded(c, mem, code & ~(uint32_t)RL_MIPS_GUARD_IN_DELAY, pc, w0, false, &v))
        return RL_OK;
    return rl_mips_redirect(c, v);
}

/** @brief Where a branch goes and whether its delay slot runs. */
typedef struct branch {
    bool taken;
    bool slot;
    uint64_t target;
} branch;

/** @brief Runs the branch w at pc up to its delay slot: the condition with the registers before it, then the link write. */
static branch run_branch(rl_ctx *c, uint64_t pc, uint32_t w)
{
    branch b = {false, true, pc + 8u};
    uint32_t op = w >> 26;
    unsigned rs = w >> 21 & 31u, rt = w >> 16 & 31u, rd = w >> 11 & 31u;
    int64_t s = (int64_t)c->r[rs], t = (int64_t)c->r[rt];
    uint64_t rel = pc + 4u + (simm(w) << 2);
    bool likely = false;
    int link = -1;
    switch (op) {
    case 0x00:
        b.taken = true;
        b.target = c->r[rs];
        if ((w & 63u) == 9u)
            link = (int)rd;
        break;
    case 0x01:
        likely = (rt & 2u) != 0;
        b.taken = (rt & 1u) ? s >= 0 : s < 0;
        b.target = rel;
        if (rt & 16u)
            link = 31;
        break;
    case 0x02:
    case 0x03:
        b.taken = true;
        b.target = ((pc + 4u) & ~UINT64_C(0x0FFFFFFF)) | (uint64_t)(w & 0x03FFFFFFu) << 2;
        if (op == 0x03)
            link = 31;
        break;
    case 0x11: {
        likely = (rt & 2u) != 0;
        bool cond = (c->fcr31 & RL_MIPS_FCR_C) != 0;
        b.taken = cond == ((rt & 1u) != 0);
        b.target = rel;
        break;
    }
    default: {
        likely = op >= 0x14u;
        switch (op & 3u) {
        case 0:
            b.taken = s == t;
            break;
        case 1:
            b.taken = s != t;
            break;
        case 2:
            b.taken = s <= 0;
            break;
        default:
            b.taken = s > 0;
            break;
        }
        b.target = rel;
        break;
    }
    }
    if (link >= 0)
        set_gpr(c, (unsigned)link, pc + 8u);
    if (!b.taken) {
        b.target = pc + 8u;
        b.slot = !likely;
    }
    return b;
}

rl_fn rl_mips_guard_exit(rl_ctx *c, uint8_t *mem, uint32_t code, uint64_t pc, uint32_t w0, uint32_t w1)
{
    branch b = run_branch(c, pc, w0);
    if (b.slot) {
        uint64_t v;
        if (run_guarded(c, mem, code & ~(uint32_t)RL_MIPS_GUARD_IN_DELAY, pc + 4u, w1, true, &v)) {
            rl_mips_redirect(c, v);
            return rl_mips_pending;
        }
    }
    return rl_indirect(c, b.target);
}
