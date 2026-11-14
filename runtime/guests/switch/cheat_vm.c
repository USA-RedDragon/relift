/**
 * @file cheat_vm.c
 * @brief The cheat virtual machine, see cheat_vm.h.
 */
#include "cheat_vm.h"

#include <string.h>

enum cond { C_GT = 1, C_GE, C_LT, C_LE, C_EQ, C_NE };

typedef struct vm_state {
    const cheat_env *env;
    cheat_vm *vm;
    const cheat_program *p;
    cheat_result *res;
    uint64_t reg[CHEAT_REGS];
    uint64_t saved[CHEAT_REGS];
    size_t loop_top[CHEAT_REGS];
} vm_state;

static unsigned dig(uint32_t w, unsigned i) { return (w >> (28 - 4 * i)) & 0xF; }

static uint64_t width_mask(unsigned t) { return t >= 8 ? ~0ull : (1ull << (8 * t)) - 1; }

static bool valid_width(unsigned t) { return t == 1 || t == 2 || t == 4 || t == 8; }

static uint64_t base_of(const vm_state *s, unsigned region)
{
    return region <= 3 && s->env->region_base ? s->env->region_base(s->env->user, region) : 0;
}

static uint64_t rd(vm_state *s, uint64_t addr, unsigned t)
{
    uint64_t v = 0;
    if (s->env->read(s->env->user, addr, &v, t) != CHEAT_MEM_OK) {
        s->res->faults++;
        return 0;
    }
    return v;
}

static void wr(vm_state *s, uint64_t addr, uint64_t v, unsigned t)
{
    int rc = s->env->write(s->env->user, addr, &v, t);
    if (rc == CHEAT_MEM_OK)
        s->res->writes++;
    else if (rc == CHEAT_MEM_CODE) {
        if (!s->res->code_writes)
            s->res->code_addr = addr;
        s->res->code_writes++;
    } else
        s->res->faults++;
}

static bool compare(unsigned c, uint64_t a, uint64_t b)
{
    switch (c) {
    case C_GT:
        return a > b;
    case C_GE:
        return a >= b;
    case C_LT:
        return a < b;
    case C_LE:
        return a <= b;
    case C_EQ:
        return a == b;
    case C_NE:
        return a != b;
    default:
        return false;
    }
}

static uint64_t arith(unsigned op, unsigned t, uint64_t a, uint64_t b, bool *ok)
{
    uint64_t m = width_mask(t), r = 0;
    a &= m;
    b &= m;
    *ok = true;
    switch (op) {
    case 0:
        r = a + b;
        break;
    case 1:
        r = a - b;
        break;
    case 2:
        r = a * b;
        break;
    case 3:
        r = b >= 64 ? 0 : a << b;
        break;
    case 4:
        r = b >= 64 ? 0 : a >> b;
        break;
    case 5:
        r = a & b;
        break;
    case 6:
        r = a | b;
        break;
    case 7:
        r = ~a;
        break;
    case 8:
        r = a ^ b;
        break;
    case 9:
        r = a;
        break;
    case 10:
    case 11:
    case 12:
    case 13:
        if (t == 4) {
            float x, y, z;
            uint32_t u;
            memcpy(&x, &a, 4);
            memcpy(&y, &b, 4);
            z = op == 10 ? x + y : op == 11 ? x - y : op == 12 ? x * y : x / y;
            memcpy(&u, &z, 4);
            r = u;
        } else if (t == 8) {
            double x, y, z;
            memcpy(&x, &a, 8);
            memcpy(&y, &b, 8);
            z = op == 10 ? x + y : op == 11 ? x - y : op == 12 ? x * y : x / y;
            memcpy(&r, &z, 8);
        } else
            *ok = false;
        break;
    default:
        *ok = false;
    }
    return r & m;
}

size_t cheat_vm_length(const uint32_t *ops, size_t n, size_t i)
{
    uint32_t w;
    unsigned t;
    size_t len = 0;
    if (i >= n)
        return 0;
    w = ops[i];
    t = dig(w, 1);
    switch (dig(w, 0)) {
    case 0x0:
        len = valid_width(t) ? (t == 8 ? 4 : 3) : 0;
        break;
    case 0x1:
        len = valid_width(t) ? (t == 8 ? 4 : 3) : 0;
        break;
    case 0x2:
        len = 1;
        break;
    case 0x3:
        len = dig(w, 1) == 0 ? 2 : dig(w, 1) == 1 ? 1 : 0;
        break;
    case 0x4:
        len = 3;
        break;
    case 0x5:
        len = valid_width(t) ? 2 : 0;
        break;
    case 0x6:
        len = valid_width(t) ? 3 : 0;
        break;
    case 0x7:
        len = valid_width(t) ? 2 : 0;
        break;
    case 0x8:
        len = 1;
        break;
    case 0x9:
        if (!valid_width(t))
            return 0;
        len = dig(w, 5) == 1 ? (t == 8 ? 3 : 2) : 1;
        break;
    case 0xA: {
        unsigned o = dig(w, 5);
        len = !valid_width(t) ? 0 : o == 2 || o == 4 || o == 5 ? 2 : 1;
        break;
    }
    case 0xC:
        switch (dig(w, 1)) {
        case 0: {
            unsigned x = dig(w, 5);
            if (!valid_width(dig(w, 2)))
                return 0;
            len = x == 0 || x == 2 ? 2 : x == 4 ? (dig(w, 2) == 8 ? 3 : 2) : 1;
            if (x > 5)
                len = 0;
            break;
        }
        case 1:
        case 2:
        case 3:
            len = 1;
            break;
        case 4:
            len = 3;
            break;
        default:
            len = 0;
        }
        break;
    case 0xF:
        if (dig(w, 1) == 0xF && dig(w, 2) == 0xF) {
            unsigned x = dig(w, 5);
            len = x == 0 || x == 2 ? 2 : x <= 4 ? 1 : 0;
        } else if (dig(w, 1) == 0xF && dig(w, 2) <= 1)
            len = 1;
        else
            len = 0;
        break;
    default:
        len = 0;
    }
    if (len == 0 || i + len > n)
        return 0;
    return len;
}

static bool is_begin(uint32_t w)
{
    unsigned o = dig(w, 0);
    return o == 1 || o == 8 || (o == 0xC && (dig(w, 1) == 0 || dig(w, 1) == 4));
}

static bool is_loop_start(uint32_t w) { return dig(w, 0) == 3 && dig(w, 1) == 0; }

static bool is_loop_end(uint32_t w) { return dig(w, 0) == 3 && dig(w, 1) == 1; }

static bool skip_block(const uint32_t *ops, size_t n, size_t *pc, bool stop_at_else)
{
    unsigned depth = 0;
    size_t i = *pc;
    while (i < n) {
        size_t len = cheat_vm_length(ops, n, i);
        uint32_t w = ops[i];
        if (!len)
            return false;
        if (is_begin(w))
            depth++;
        else if (dig(w, 0) == 2) {
            if (depth == 0 && (dig(w, 1) == 0 || stop_at_else)) {
                *pc = i + 1;
                return true;
            }
            if (dig(w, 1) == 0 && depth > 0)
                depth--;
        }
        i += len;
    }
    *pc = n;
    return true;
}

static bool skip_loop(const uint32_t *ops, size_t n, size_t *pc, unsigned reg)
{
    unsigned depth = 0;
    size_t i = *pc;
    while (i < n) {
        size_t len = cheat_vm_length(ops, n, i);
        uint32_t w = ops[i];
        if (!len)
            return false;
        if (is_loop_start(w) && dig(w, 3) == reg)
            depth++;
        else if (is_loop_end(w) && dig(w, 3) == reg) {
            if (depth == 0) {
                *pc = i + 1;
                return true;
            }
            depth--;
        }
        i += len;
    }
    *pc = n;
    return true;
}

static uint64_t addr40(uint32_t hi, uint32_t lo) { return (uint64_t)(hi & 0xFF) << 32 | lo; }

static uint64_t value64(const uint32_t *ops, size_t i, unsigned t)
{
    return t == 8 ? (uint64_t)ops[i] << 32 | ops[i + 1] : ops[i];
}

static uint64_t keys_now(const vm_state *s) { return s->env->keys ? s->env->keys(s->env->user) : 0; }

static void restore_save(vm_state *s, unsigned x, unsigned d, unsigned src)
{
    switch (x) {
    case 0:
        s->reg[d] = s->saved[src];
        break;
    case 1:
        s->saved[d] = s->reg[src];
        break;
    case 2:
        s->saved[d] = 0;
        break;
    case 3:
        s->reg[d] = 0;
        break;
    default:
        break;
    }
}

static bool exec_cond_reg(vm_state *s, const uint32_t *ops, size_t i)
{
    uint32_t w = ops[i];
    unsigned t = dig(w, 2), c = dig(w, 3), src = dig(w, 4), x = dig(w, 5), a = dig(w, 6), b = dig(w, 7);
    uint64_t lhs = s->reg[src] & width_mask(t), rhs = 0;
    switch (x) {
    case 0:
        rhs = rd(s, base_of(s, a) + addr40(w & 0xF, ops[i + 1]) + 0, t);
        break;
    case 1:
        rhs = rd(s, base_of(s, a) + s->reg[b], t);
        break;
    case 2:
        rhs = rd(s, s->reg[a] + addr40(w & 0xF, ops[i + 1]), t);
        break;
    case 3:
        rhs = rd(s, s->reg[a] + s->reg[b], t);
        break;
    case 4:
        rhs = value64(ops, i + 1, t);
        break;
    default:
        rhs = s->reg[a];
        break;
    }
    return compare(c, lhs, rhs & width_mask(t));
}

static bool exec_extended(vm_state *s, const uint32_t *ops, size_t i, size_t *pc, size_t len)
{
    uint32_t w = ops[i];
    switch (dig(w, 1)) {
    case 0:
        if (!exec_cond_reg(s, ops, i) && !skip_block(ops, s->p->n, pc, true))
            return false;
        return true;
    case 1:
        restore_save(s, dig(w, 6), dig(w, 3), dig(w, 5));
        return true;
    case 2:
        for (unsigned r = 0; r < CHEAT_REGS; r++)
            if (w >> r & 1)
                restore_save(s, dig(w, 2), r, r);
        return true;
    case 3: {
        unsigned idx = (w >> 4) & 0xFF, r = w & 0xF;
        if (idx < CHEAT_STATICS)
            s->reg[r] = s->vm->statics[idx];
        else
            s->vm->statics[idx - CHEAT_STATICS] = s->reg[r];
        return true;
    }
    default: {
        uint64_t mask = (uint64_t)ops[i + 1] << 32 | ops[i + 2];
        bool held = (keys_now(s) & mask) == mask, fire = held;
        if (dig(w, 2) == 0 && s->p->edge) {
            fire = held && !s->p->edge[i];
            s->p->edge[i] = held;
        }
        (void)len;
        if (!fire && !skip_block(ops, s->p->n, pc, true))
            return false;
        return true;
    }
    }
}

static bool exec_store_reg(vm_state *s, const uint32_t *ops, size_t i)
{
    uint32_t w = ops[i];
    unsigned t = dig(w, 1), sr = dig(w, 2), r = dig(w, 3), inc = dig(w, 4), o = dig(w, 5), x = dig(w, 6);
    uint64_t imm = (uint64_t)dig(w, 7) << 32 | (o == 2 || o == 4 || o == 5 ? ops[i + 1] : 0), addr;
    switch (o) {
    case 0:
        addr = s->reg[r];
        break;
    case 1:
        addr = s->reg[r] + s->reg[x];
        break;
    case 2:
        addr = s->reg[r] + imm;
        break;
    case 3:
        addr = base_of(s, x) + s->reg[r];
        break;
    case 4:
        addr = base_of(s, x) + imm;
        break;
    case 5:
        addr = base_of(s, x) + s->reg[r] + imm;
        break;
    default:
        return false;
    }
    wr(s, addr, s->reg[sr], t);
    if (inc)
        s->reg[r] += t;
    return true;
}

static bool exec_one(vm_state *s, size_t i, size_t len, size_t *pc)
{
    const uint32_t *ops = s->p->ops;
    uint32_t w = ops[i];
    unsigned t = dig(w, 1);
    switch (dig(w, 0)) {
    case 0x0:
        wr(s, base_of(s, dig(w, 2)) + s->reg[dig(w, 3)] + addr40(w, ops[i + 1]), value64(ops, i + 2, t), t);
        return true;
    case 0x1: {
        unsigned m = dig(w, 2), c = dig(w, 3), x = dig(w, 4), r = dig(w, 5);
        uint64_t addr = base_of(s, m) + (x == 1 ? s->reg[r] : 0) + addr40(w, ops[i + 1]);
        if (!compare(c, rd(s, addr, t), value64(ops, i + 2, t) & width_mask(t)))
            return skip_block(ops, s->p->n, pc, true);
        return true;
    }
    case 0x2:
        if (dig(w, 1) == 1)
            return skip_block(ops, s->p->n, pc, false);
        return true;
    case 0x3: {
        unsigned r = dig(w, 3);
        if (t == 0) {
            s->reg[r] = ops[i + 1];
            s->loop_top[r] = *pc;
            if (s->reg[r] == 0)
                return skip_loop(ops, s->p->n, pc, r);
        } else if (s->reg[r] > 0 && --s->reg[r] > 0)
            *pc = s->loop_top[r];
        return true;
    }
    case 0x4:
        s->reg[dig(w, 3)] = (uint64_t)ops[i + 1] << 32 | ops[i + 2];
        return true;
    case 0x5: {
        unsigned m = dig(w, 2), r = dig(w, 3), mode = dig(w, 4), sr = dig(w, 5);
        uint64_t imm = addr40(w, ops[i + 1]);
        uint64_t addr = mode == 0   ? base_of(s, m) + imm
                        : mode == 1 ? s->reg[r] + imm
                        : mode == 2 ? s->reg[sr] + imm
                                    : base_of(s, m) + s->reg[sr] + imm;
        if (mode > 3)
            return false;
        s->reg[r] = rd(s, addr, t);
        return true;
    }
    case 0x6: {
        unsigned r = dig(w, 3);
        uint64_t addr = s->reg[r] + (dig(w, 5) ? s->reg[dig(w, 6)] : 0);
        wr(s, addr, (uint64_t)ops[i + 1] << 32 | ops[i + 2], t);
        if (dig(w, 4))
            s->reg[r] += t;
        return true;
    }
    case 0x7: {
        bool ok;
        unsigned r = dig(w, 3), c = dig(w, 4);
        if (c > 4)
            return false;
        s->reg[r] = arith(c, t, s->reg[r], ops[i + 1], &ok);
        return ok;
    }
    case 0x8:
        if ((keys_now(s) & (w & 0x0FFFFFFFu)) != (w & 0x0FFFFFFFu))
            return skip_block(ops, s->p->n, pc, true);
        return true;
    case 0x9: {
        bool ok;
        unsigned c = dig(w, 2), r = dig(w, 3), l = dig(w, 4);
        uint64_t rhs = dig(w, 5) == 1 ? value64(ops, i + 1, t) : s->reg[dig(w, 6)];
        s->reg[r] = arith(c, t, s->reg[l], rhs, &ok);
        return ok;
    }
    case 0xA:
        return exec_store_reg(s, ops, i);
    case 0xC:
        return exec_extended(s, ops, i, pc, len);
    default:
        return true;
    }
}

void cheat_vm_run(cheat_vm *vm, const cheat_env *env, const cheat_program *p, cheat_result *out)
{
    vm_state s;
    size_t pc = 0;
    memset(&s, 0, sizeof s);
    memset(out, 0, sizeof *out);
    s.env = env;
    s.vm = vm;
    s.p = p;
    s.res = out;
    while (pc < p->n) {
        size_t at = pc, len = cheat_vm_length(p->ops, p->n, pc);
        if (!len) {
            out->bad_opcode = true;
            out->bad_at = at;
            return;
        }
        pc += len;
        if (!exec_one(&s, at, len, &pc)) {
            out->bad_opcode = true;
            out->bad_at = at;
            return;
        }
    }
}
