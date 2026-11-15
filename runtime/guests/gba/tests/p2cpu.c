/**
 * @file p2cpu.c
 * @brief The probe replays' ARM7TDMI interpreter (see p2cpu.h).
 */
#include "p2cpu.h"

#include "bios/bios.h"

#include <stdio.h>
#include <stdlib.h>

#define T_BIT 0x20u
#define I_BIT 0x80u

static int bank_of(uint32_t mode) { return mode == P2_IRQ ? 1 : mode == P2_SVC ? 2 : 0; }

void p2cpu_init(p2cpu *c, rl_gba *g, rl_sched_cpu *sc)
{
    *c = (p2cpu){.g = g, .sc = sc, .cpsr = P2_SYS | I_BIT};
}

void p2cpu_set_cpsr(p2cpu *c, uint32_t v)
{
    int from = bank_of(c->cpsr & 0x1Fu), to = bank_of(v & 0x1Fu);
    if (from != to) {
        c->bank_sp[from] = c->r[13];
        c->bank_lr[from] = c->r[14];
        c->r[13] = c->bank_sp[to];
        c->r[14] = c->bank_lr[to];
    }
    c->cpsr = v;
}

uint32_t *p2cpu_spsr(p2cpu *c)
{
    int b = bank_of(c->cpsr & 0x1Fu);
    return b ? &c->spsr[b] : NULL;
}

static unsigned isz(const p2cpu *c) { return (c->cpsr & T_BIT) ? 2u : 4u; }

static uint64_t now(const p2cpu *c) { return rl_sched_now(c->sc); }

static void fetch(p2cpu *c)
{
    unsigned s = isz(c);
    c->last = now(c);
    rl_gba_fetch(c->g, c->r[15] + 2 * s, s, !c->ns);
    c->ns = false;
}

static void idle(p2cpu *c, unsigned n)
{
    for (; n; n--) {
        c->last = now(c);
        rl_gba_idle(c->g, 1);
    }
    c->ns = true;
}

static uint32_t load(p2cpu *c, uint32_t a, unsigned size, bool seq)
{
    c->last = now(c);
    uint32_t v = rl_gba_load(c->g, a, size, seq);
    c->ns = true;
    return v;
}

static void store(p2cpu *c, uint32_t a, unsigned size, uint32_t v, bool seq)
{
    c->last = now(c);
    rl_gba_store(c->g, a, size, v, seq);
    c->ns = true;
}

void p2cpu_jump(p2cpu *c, uint32_t target)
{
    if (target & 1u)
        c->cpsr |= T_BIT;
    else
        c->cpsr &= ~T_BIT;
    unsigned s = isz(c);
    c->r[15] = target & ~(s - 1u);
    rl_gba_fetch(c->g, c->r[15], s, false);
    c->last = now(c);
    rl_gba_fetch(c->g, c->r[15] + s, s, true);
    c->ns = false;
}

void p2cpu_resume(p2cpu *c, uint32_t pc)
{
    c->last = now(c) - 1;
    c->r[15] = pc;
    c->ns = false;
}

static void stub_entry(p2cpu *c)
{
    rl_gba_bios *b = rl_gba_bios_of(c->g);
    rl_gba_irq_cpu ic = {.r = {c->r[0], c->r[1], c->r[2], c->r[3]}, .r12 = c->r[12], .lr = c->r[14], .sp = c->r[13]};
    uint32_t h = rl_gba_bios_irq_entry(b, rl_gba_bios_bus(c->g), &ic);
    for (unsigned i = 0; i < 4; i++)
        c->r[i] = ic.r[i];
    c->r[12] = ic.r12;
    c->r[14] = ic.lr;
    c->r[13] = ic.sp;
    if (b->cycles)
        rl_gba_idle(c->g, (unsigned)b->cycles);
    p2cpu_resume(c, h);
}

static void stub_return(p2cpu *c)
{
    rl_gba_bios *b = rl_gba_bios_of(c->g);
    rl_gba_irq_cpu ic = {.r = {c->r[0], c->r[1], c->r[2], c->r[3]}, .r12 = c->r[12], .lr = c->r[14], .sp = c->r[13]};
    uint32_t to = rl_gba_bios_irq_return(b, rl_gba_bios_bus(c->g), &ic);
    for (unsigned i = 0; i < 4; i++)
        c->r[i] = ic.r[i];
    c->r[12] = ic.r12;
    c->r[14] = ic.lr;
    c->r[13] = ic.sp;
    if (b->cycles)
        rl_gba_idle(c->g, (unsigned)b->cycles);
    if (c->depth)
        c->depth--;
    uint32_t sp = c->spsr[1];
    p2cpu_set_cpsr(c, sp);
    p2cpu_resume(c, to & ((sp & T_BIT) ? ~1u : ~3u));
}

void p2cpu_irq(p2cpu *c, uint32_t lr, uint32_t spsr)
{
    p2cpu_set_cpsr(c, (spsr & ~(0x1Fu | T_BIT)) | P2_IRQ | I_BIT);
    c->spsr[1] = spsr;
    c->r[14] = lr;
    c->depth++;
    p2cpu_jump(c, 0x18);
    stub_entry(c);
}

bool p2cpu_run_irq(p2cpu *c, uint64_t limit)
{
    unsigned d = c->depth - 1;
    for (uint64_t n = 0; n < limit; n++) {
        if (c->r[15] == RL_GBA_BIOS_IRQ_RETURN && !(c->cpsr & T_BIT)) {
            stub_return(c);
            if (c->depth == d)
                return true;
            continue;
        }
        if (p2cpu_poll(c))
            continue;
        p2cpu_step(c);
    }
    return false;
}

bool p2cpu_poll(p2cpu *c)
{
    rl_sched_run(c->sc);
    rl_gba_service(c->g);
    bool masked = (c->cpsr & I_BIT) && !c->msr_masked;
    c->msr_masked = false;
    if (masked || !rl_gba_irq_sample(c->g))
        return false;
    if (getenv("RL_TRACE_O"))
        fprintf(stderr, "    cpu irq at %08x cpsr %08x t %llu\n", c->r[15], c->cpsr, (unsigned long long)rl_sched_now(c->sc));
    p2cpu_irq(c, c->r[15] + 4, c->cpsr);
    return true;
}

static bool cond(uint32_t cpsr, unsigned cc)
{
    bool n = cpsr >> 31, z = (cpsr >> 30) & 1, cf = (cpsr >> 29) & 1, v = (cpsr >> 28) & 1;
    switch (cc) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return cf;
    case 0x3: return !cf;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return cf && !z;
    case 0x9: return !cf || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && n == v;
    case 0xD: return z || n != v;
    default: return true;
    }
}

static uint32_t reg(const p2cpu *c, unsigned n) { return n == 15 ? c->r[15] + 2 * isz(c) : c->r[n]; }

static uint32_t shift_imm(uint32_t v, unsigned type, unsigned amt, bool *carry)
{
    switch (type) {
    case 0:
        if (amt) {
            *carry = (v >> (32 - amt)) & 1;
            v <<= amt;
        }
        return v;
    case 1:
        if (!amt) {
            *carry = v >> 31;
            return 0;
        }
        *carry = (v >> (amt - 1)) & 1;
        return v >> amt;
    case 2:
        if (!amt) {
            *carry = v >> 31;
            return (uint32_t)((int32_t)v >> 31);
        }
        *carry = (v >> (amt - 1)) & 1;
        return (uint32_t)((int32_t)v >> amt);
    default:
        if (!amt) {
            bool c0 = *carry;
            *carry = v & 1;
            return (v >> 1) | (uint32_t)c0 << 31;
        }
        *carry = (v >> (amt - 1)) & 1;
        return (v >> amt) | (v << (32 - amt));
    }
}

static void set_nz(p2cpu *c, uint32_t r) { c->cpsr = (c->cpsr & 0x3FFFFFFFu) | (r & 0x80000000u) | (r ? 0 : 0x40000000u); }

static void set_c(p2cpu *c, bool v) { c->cpsr = (c->cpsr & ~0x20000000u) | (uint32_t)v << 29; }

static void set_v(p2cpu *c, bool v) { c->cpsr = (c->cpsr & ~0x10000000u) | (uint32_t)v << 28; }

static void write_pc(p2cpu *c, uint32_t v, bool restore)
{
    if (restore) {
        uint32_t *s = p2cpu_spsr(c);
        if (s)
            p2cpu_set_cpsr(c, *s);
        p2cpu_jump(c, v | ((c->cpsr & T_BIT) ? 1u : 0u));
        return;
    }
    p2cpu_jump(c, v | ((c->cpsr & T_BIT) ? 1u : 0u));
}

static void dataproc(p2cpu *c, uint32_t op)
{
    unsigned opc = (op >> 21) & 15, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    bool s = (op >> 20) & 1, carry = (c->cpsr >> 29) & 1;
    uint32_t b;
    bool regshift = false;
    if (op & (1u << 25)) {
        unsigned rot = ((op >> 8) & 15) * 2;
        b = op & 0xFFu;
        if (rot) {
            b = (b >> rot) | (b << (32 - rot));
            carry = b >> 31;
        }
    } else if (op & 0x10u) {
        regshift = true;
        uint32_t v = reg(c, op & 15) + ((op & 15) == 15 ? 4 : 0);
        unsigned amt = c->r[(op >> 8) & 15] & 0xFFu, type = (op >> 5) & 3;
        if (amt == 0)
            b = v;
        else if (amt < 32 || type == 3)
            b = shift_imm(v, type, type == 3 ? (amt & 31) : amt, &carry);
        else if (type == 0) {
            carry = amt == 32 ? v & 1 : 0;
            b = 0;
        } else if (type == 1) {
            carry = amt == 32 ? v >> 31 : 0;
            b = 0;
        } else {
            carry = v >> 31;
            b = (uint32_t)((int32_t)v >> 31);
        }
    } else {
        b = shift_imm(reg(c, op & 15), (op >> 5) & 3, (op >> 7) & 31, &carry);
    }
    uint32_t a = reg(c, rn) + (regshift && rn == 15 ? 4 : 0), r = 0;
    uint64_t wide;
    bool logic = false, write = true;
    switch (opc) {
    case 0x0: r = a & b; logic = true; break;
    case 0x1: r = a ^ b; logic = true; break;
    case 0x2: wide = (uint64_t)a - b; r = (uint32_t)wide; if (s) { set_c(c, a >= b); set_v(c, ((a ^ b) & (a ^ r)) >> 31); } break;
    case 0x3: wide = (uint64_t)b - a; r = (uint32_t)wide; if (s) { set_c(c, b >= a); set_v(c, ((b ^ a) & (b ^ r)) >> 31); } break;
    case 0x4: wide = (uint64_t)a + b; r = (uint32_t)wide; if (s) { set_c(c, wide >> 32); set_v(c, (~(a ^ b) & (a ^ r)) >> 31); } break;
    case 0x5: wide = (uint64_t)a + b + ((c->cpsr >> 29) & 1); r = (uint32_t)wide; if (s) { set_c(c, wide >> 32); set_v(c, (~(a ^ b) & (a ^ r)) >> 31); } break;
    case 0x6: { uint32_t ci = (c->cpsr >> 29) & 1; r = a - b - (1 - ci); if (s) { set_c(c, (uint64_t)a >= (uint64_t)b + (1 - ci)); set_v(c, ((a ^ b) & (a ^ r)) >> 31); } break; }
    case 0x7: { uint32_t ci = (c->cpsr >> 29) & 1; r = b - a - (1 - ci); if (s) { set_c(c, (uint64_t)b >= (uint64_t)a + (1 - ci)); set_v(c, ((b ^ a) & (b ^ r)) >> 31); } break; }
    case 0x8: r = a & b; logic = true; write = false; break;
    case 0x9: r = a ^ b; logic = true; write = false; break;
    case 0xA: r = a - b; write = false; set_c(c, a >= b); set_v(c, ((a ^ b) & (a ^ r)) >> 31); break;
    case 0xB: wide = (uint64_t)a + b; r = (uint32_t)wide; write = false; set_c(c, wide >> 32); set_v(c, (~(a ^ b) & (a ^ r)) >> 31); break;
    case 0xC: r = a | b; logic = true; break;
    case 0xD: r = b; logic = true; break;
    case 0xE: r = a & ~b; logic = true; break;
    default: r = ~b; logic = true; break;
    }
    if (s || !write) {
        set_nz(c, r);
        if (logic)
            set_c(c, carry);
    }
    if (regshift)
        idle(c, 1);
    if (write) {
        if (rd == 15)
            write_pc(c, r, s);
        else
            c->r[rd] = r;
    }
    if (!(write && rd == 15))
        c->r[15] += 4;
}

static unsigned mul_m(uint32_t rs)
{
    unsigned n = 1;
    for (unsigned k = 8; k < 32; k += 8) {
        uint32_t hi = rs >> k;
        if (hi && hi != 0xFFFFFFFFu >> k)
            n++;
    }
    return n;
}

static void arm_step(p2cpu *c, uint32_t op)
{
    if (!cond(c->cpsr, op >> 28)) {
        fetch(c);
        c->r[15] += 4;
        return;
    }
    fetch(c);
    if ((op & 0x0FFFFFF0u) == 0x012FFF10u) {
        p2cpu_jump(c, c->r[op & 15]);
        return;
    }
    if ((op & 0x0FC000F0u) == 0x00000090u) {
        unsigned rd = (op >> 16) & 15;
        uint32_t r = c->r[op & 15] * c->r[(op >> 8) & 15];
        if (op & (1u << 21))
            r += c->r[(op >> 12) & 15];
        idle(c, mul_m(c->r[(op >> 8) & 15]) + ((op >> 21) & 1));
        c->r[rd] = r;
        if (op & (1u << 20))
            set_nz(c, r);
        c->r[15] += 4;
        return;
    }
    if ((op & 0x0FBF0FFFu) == 0x010F0000u) {
        uint32_t *sp = p2cpu_spsr(c);
        c->r[(op >> 12) & 15] = (op & (1u << 22)) ? (sp ? *sp : c->cpsr) : c->cpsr;
        c->r[15] += 4;
        return;
    }
    if ((op & 0x0DB0F000u) == 0x0120F000u) {
        uint32_t v = (op & (1u << 25)) ? (((op & 0xFFu) >> (((op >> 8) & 15) * 2)) | ((op & 0xFFu) << (32 - ((op >> 8) & 15) * 2))) : c->r[op & 15];
        if ((op & (1u << 25)) && !((op >> 8) & 15))
            v = op & 0xFFu;
        uint32_t mask = 0;
        if (op & (1u << 16))
            mask |= 0xFFu;
        if (op & (1u << 19))
            mask |= 0xF0000000u;
        if (op & (1u << 22)) {
            uint32_t *sp = p2cpu_spsr(c);
            if (sp)
                *sp = (*sp & ~mask) | (v & mask);
        } else {
            uint32_t was = c->cpsr;
            p2cpu_set_cpsr(c, (c->cpsr & ~mask) | (v & mask));
            c->msr_masked = !(was & I_BIT) && (c->cpsr & I_BIT);
        }
        c->r[15] += 4;
        return;
    }
    if ((op & 0x0E000090u) == 0x00000090u && (op & 0x60u)) {
        bool pre = (op >> 24) & 1, up = (op >> 23) & 1, wb = (op >> 21) & 1, ld = (op >> 20) & 1;
        unsigned rn = (op >> 16) & 15, rd = (op >> 12) & 15, sh = (op >> 5) & 3;
        uint32_t off = (op & (1u << 22)) ? (((op >> 4) & 0xF0u) | (op & 15)) : c->r[op & 15];
        uint32_t base = reg(c, rn), addr = pre ? (up ? base + off : base - off) : base;
        if (ld) {
            uint32_t v;
            if (sh == 1) {
                v = load(c, addr & ~1u, 2, false);
                if (addr & 1)
                    v = (v >> 8) | (v << 24);
            } else if (sh == 2) {
                v = (uint32_t)(int32_t)(int8_t)load(c, addr, 1, false);
            } else {
                v = load(c, addr & ~1u, 2, false);
                v = (addr & 1) ? (uint32_t)(int32_t)(int8_t)(v >> 8) : (uint32_t)(int32_t)(int16_t)v;
            }
            if (!pre || wb)
                c->r[rn] = up ? base + off : base - off;
            idle(c, 1);
            if (rd == 15) {
                write_pc(c, v, false);
                return;
            }
            c->r[rd] = v;
        } else {
            store(c, addr & ~1u, 2, reg(c, rd) + (rd == 15 ? 4 : 0), false);
            if (!pre || wb)
                c->r[rn] = up ? base + off : base - off;
        }
        c->r[15] += 4;
        return;
    }
    switch ((op >> 25) & 7) {
    case 0:
    case 1:
        dataproc(c, op);
        return;
    case 2:
    case 3: {
        bool pre = (op >> 24) & 1, up = (op >> 23) & 1, byte = (op >> 22) & 1, wb = (op >> 21) & 1, ld = (op >> 20) & 1;
        unsigned rn = (op >> 16) & 15, rd = (op >> 12) & 15;
        uint32_t off;
        if (op & (1u << 25)) {
            bool cy = (c->cpsr >> 29) & 1;
            off = shift_imm(c->r[op & 15], (op >> 5) & 3, (op >> 7) & 31, &cy);
        } else {
            off = op & 0xFFFu;
        }
        uint32_t base = reg(c, rn), addr = pre ? (up ? base + off : base - off) : base;
        if (ld) {
            uint32_t v;
            if (byte) {
                v = load(c, addr, 1, false) & 0xFFu;
            } else {
                v = load(c, addr & ~3u, 4, false);
                unsigned rot = (addr & 3u) * 8;
                if (rot)
                    v = (v >> rot) | (v << (32 - rot));
            }
            if (!pre || wb)
                c->r[rn] = up ? base + off : base - off;
            idle(c, 1);
            if (rd == 15) {
                write_pc(c, v, false);
                return;
            }
            c->r[rd] = v;
        } else {
            uint32_t v = reg(c, rd) + (rd == 15 ? 4 : 0);
            if (byte)
                store(c, addr, 1, v & 0xFFu, false);
            else
                store(c, addr & ~3u, 4, v, false);
            if (!pre || wb)
                c->r[rn] = up ? base + off : base - off;
        }
        c->r[15] += 4;
        return;
    }
    case 4: {
        bool pre = (op >> 24) & 1, up = (op >> 23) & 1, wb = (op >> 21) & 1, ld = (op >> 20) & 1;
        unsigned rn = (op >> 16) & 15, n = 0;
        for (unsigned i = 0; i < 16; i++)
            n += (op >> i) & 1;
        uint32_t base = c->r[rn], lo = up ? base : base - 4 * n;
        uint32_t addr = lo + ((pre == up) ? 4 : 0);
        uint32_t nb = up ? base + 4 * n : base - 4 * n;
        bool seq = false, pc = false;
        uint32_t pcv = 0;
        for (unsigned i = 0; i < 16; i++) {
            if (!((op >> i) & 1))
                continue;
            if (ld) {
                uint32_t v = load(c, addr, 4, seq);
                if (i == 15) {
                    pc = true;
                    pcv = v;
                } else {
                    c->r[i] = v;
                }
            } else {
                store(c, addr, 4, i == 15 ? c->r[15] + 12 : c->r[i], seq);
            }
            seq = true;
            addr += 4;
        }
        if (wb && !(ld && ((op >> rn) & 1)))
            c->r[rn] = nb;
        if (ld) {
            idle(c, 1);
            if (pc) {
                write_pc(c, pcv, (op >> 22) & 1);
                return;
            }
        }
        c->r[15] += 4;
        return;
    }
    case 5: {
        int32_t off = (int32_t)(op << 8) >> 6;
        if (op & (1u << 24))
            c->r[14] = c->r[15] + 4;
        p2cpu_jump(c, c->r[15] + 8 + (uint32_t)off);
        return;
    }
    default:
        if ((op & 0x0F000000u) == 0x0F000000u) {
            uint32_t lr = c->r[15] + 4, sp = c->cpsr;
            p2cpu_set_cpsr(c, (c->cpsr & ~(0x1Fu | T_BIT)) | P2_SVC | I_BIT);
            c->spsr[2] = sp;
            c->r[14] = lr;
            c->hooks.swi(c, (op >> 16) & 0xFFu);
            return;
        }
        c->r[15] += 4;
        return;
    }
}

static void thumb_step(p2cpu *c, uint32_t op)
{
    fetch(c);
    if ((op & 0xFF00u) == 0xDF00u) {
        uint32_t lr = c->r[15] + 2, sp = c->cpsr;
        p2cpu_set_cpsr(c, (c->cpsr & ~(0x1Fu | T_BIT)) | P2_SVC | I_BIT);
        c->spsr[2] = sp;
        c->r[14] = lr;
        c->hooks.swi(c, op & 0xFFu);
        return;
    }
    if ((op & 0xFF00u) == 0x4700u) {
        unsigned rm = (op >> 3) & 15;
        uint32_t v = rm == 15 ? (c->r[15] + 4) & ~2u : c->r[rm];
        p2cpu_jump(c, v);
        return;
    }
    if ((op & 0xFF00u) == 0x4600u) {
        unsigned rd = (op & 7) | ((op >> 4) & 8), rm = (op >> 3) & 15;
        c->r[rd] = rm == 15 ? c->r[15] + 4 : c->r[rm];
        c->r[15] += 2;
        return;
    }
    unsigned rd = op & 7, rb = (op >> 3) & 7, imm = (op >> 6) & 31;
    if ((op & 0xF800u) == 0x6800u) {
        uint32_t a = c->r[rb] + imm * 4, v = load(c, a & ~3u, 4, false), rot = (a & 3u) * 8;
        c->r[rd] = rot ? (v >> rot) | (v << (32 - rot)) : v;
        idle(c, 1);
    } else if ((op & 0xF800u) == 0x8800u) {
        uint32_t a = c->r[rb] + imm * 2, v = load(c, a & ~1u, 2, false);
        c->r[rd] = (a & 1) ? (v >> 8) | (v << 24) : v;
        idle(c, 1);
    } else if ((op & 0xF800u) == 0x7800u) {
        c->r[rd] = load(c, c->r[rb] + imm, 1, false) & 0xFFu;
        idle(c, 1);
    }
    c->r[15] += 2;
}

void p2cpu_step(p2cpu *c)
{
    c->steps++;
    if (c->cpsr & T_BIT)
        thumb_step(c, rl_gba_peek(c->g, c->r[15], 2));
    else
        arm_step(c, rl_gba_peek(c->g, c->r[15], 4));
}

bool p2cpu_run(p2cpu *c, uint32_t stop, uint64_t limit)
{
    for (uint64_t n = 0; n < limit; n++) {
        if (c->r[15] == stop)
            return true;
        if (c->r[15] == RL_GBA_BIOS_IRQ_RETURN && !(c->cpsr & T_BIT)) {
            stub_return(c);
            continue;
        }
        if (p2cpu_poll(c))
            continue;
        p2cpu_step(c);
    }
    return false;
}
