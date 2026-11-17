/**
 * @file pipe.c
 * @brief The RSP's issue pipeline (pipe.h), the model of relift-cpu-rsp's
 * pipe module word for word.
 */
#include "pipe.h"

#include <string.h>

enum { LOAD = 1, STORE = 2, BRANCH = 4, VECTOR = 8, VNOP_GROUP = 16, BYPASS = 32 };

/** @brief The pipeline classes of a word (pipe::Op). */
typedef struct op {
    uint8_t flags;
    uint8_t vc_use, vc_def;
    uint32_t r_use, r_def, v_use, v_def, vfake;
} op;

static void alu(op *o, uint32_t d, uint32_t uses)
{
    o->r_def |= 1u << d;
    o->flags |= BYPASS;
    o->r_use |= uses;
}

static void cop2(uint32_t word, op *o, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t vd)
{
    uint32_t vs = rd, vt = rt;
    switch (rs) {
    case 0x00:
        o->r_def |= 1u << rt;
        o->v_use |= 1u << vs;
        o->flags |= LOAD | STORE;
        return;
    case 0x02:
        o->r_def |= 1u << rt;
        o->vc_use |= (uint8_t)(1u << (rd & 3));
        o->flags |= LOAD | STORE;
        return;
    case 0x04:
        o->r_use |= 1u << rt;
        o->v_def |= 1u << vs;
        o->flags |= LOAD | STORE | VNOP_GROUP;
        return;
    case 0x06:
        o->r_use |= 1u << rt;
        o->vc_def |= (uint8_t)(1u << (rd & 3));
        o->flags |= LOAD | STORE;
        return;
    default:
        if (rs < 0x10)
            return;
    }
    o->flags |= VECTOR;
    uint32_t f = word & 0x3F;
    uint32_t uses;
    uint8_t vc = 0;
    if (f == 0x02 || f == 0x0A) {
        uses = 1u << vt;
    } else if (f == 0x0B || f == 0x1D) {
        uses = 0;
    } else if (f <= 0x0F || (f >= 0x28 && f <= 0x2D)) {
        uses = 1u << vs | 1u << vt;
    } else if (f == 0x10 || f == 0x11 || (f >= 0x13 && f <= 0x15)) {
        uses = 1u << vs | 1u << vt;
        vc = 1;
    } else if ((f >= 0x20 && f <= 0x23) || f == 0x27) {
        uses = 1u << vs | 1u << vt;
        vc = 3;
    } else if (f >= 0x24 && f <= 0x26) {
        uses = 1u << vs | 1u << vt;
        vc = 7;
    } else if (f >= 0x30 && f <= 0x36) {
        uses = 1u << vt;
        o->vfake |= 1u << vs;
    } else if (f == 0x37) {
        o->vfake |= 1u << vd;
        o->flags |= VNOP_GROUP;
        return;
    } else {
        return;
    }
    o->v_def |= 1u << vd;
    o->v_use |= uses;
    o->vc_use |= vc;
    o->vc_def |= vc;
}

static op decode(uint32_t word)
{
    uint32_t rd = (word >> 11) & 31, rt = (word >> 16) & 31, rs = (word >> 21) & 31, vd = (word >> 6) & 31;
    op o;
    memset(&o, 0, sizeof o);
    switch (word >> 26) {
    case 0x00:
        switch (word & 0x3F) {
        case 0x00:
        case 0x02:
        case 0x03:
            alu(&o, rd, 1u << rt);
            break;
        case 0x04:
        case 0x06:
        case 0x07:
            alu(&o, rd, 1u << rt | 1u << rs);
            break;
        case 0x08:
            o.r_use |= 1u << rs;
            o.flags |= BRANCH;
            break;
        case 0x09:
            alu(&o, rd, 1u << rs);
            o.flags |= BRANCH;
            break;
        case 0x0D:
            o.flags |= BRANCH;
            break;
        case 0x20:
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x24:
        case 0x25:
        case 0x26:
        case 0x27:
        case 0x2A:
        case 0x2B:
            alu(&o, rd, 1u << rs | 1u << rt);
            break;
        default:
            break;
        }
        break;
    case 0x01:
        if (rt <= 0x01) {
            o.r_use |= 1u << rs;
            o.flags |= BRANCH;
        } else if (rt == 0x10 || rt == 0x11) {
            alu(&o, 31, 1u << rs);
            o.flags |= BRANCH;
        }
        break;
    case 0x02:
        o.flags |= BRANCH;
        break;
    case 0x03:
        alu(&o, 31, 0);
        o.flags |= BRANCH;
        break;
    case 0x04:
    case 0x05:
        o.r_use |= 1u << rs | 1u << rt;
        o.flags |= BRANCH;
        break;
    case 0x06:
    case 0x07:
        o.r_use |= 1u << rs;
        o.flags |= BRANCH;
        break;
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
    case 0x0E:
        alu(&o, rt, 1u << rs);
        break;
    case 0x0F:
        alu(&o, rt, 0);
        break;
    case 0x10:
        if (rs == 0x00) {
            o.r_def |= 1u << rt;
            o.flags |= LOAD | STORE;
        } else if (rs == 0x04) {
            o.r_use |= 1u << rt;
            o.flags |= LOAD | STORE;
        }
        break;
    case 0x12:
        cop2(word, &o, rs, rt, rd, vd);
        break;
    case 0x20:
    case 0x21:
    case 0x23:
    case 0x24:
    case 0x25:
    case 0x27:
        o.r_def |= 1u << rt;
        o.r_use |= 1u << rs;
        o.flags |= LOAD;
        break;
    case 0x28:
    case 0x29:
    case 0x2B:
        o.r_use |= 1u << rt | 1u << rs;
        o.flags |= STORE;
        break;
    case 0x32:
        if (rd <= 0x09) {
            o.v_def |= 1u << rt;
            o.r_use |= 1u << rs;
            o.flags |= LOAD;
        } else if (rd == 0x0B) {
            o.v_def |= 0xFFu << (rt & ~7u);
            o.r_use |= 1u << rs;
            o.flags |= LOAD | VNOP_GROUP;
        }
        break;
    case 0x3A:
        if (rd <= 0x0B) {
            o.v_use |= rd == 0x0B ? 0xFFu << (rt & ~7u) : 1u << rt;
            o.r_use |= 1u << rs;
            o.flags |= STORE;
        }
        break;
    default:
        break;
    }
    return o;
}

static uint32_t r_write(const op *o) { return (o->flags & BYPASS) ? 0 : o->r_def & ~1u; }

static bool pairs(const op *a, const op *b)
{
    return !(a->flags & VECTOR) != !(b->flags & VECTOR) && !(a->v_def & (b->v_use | b->v_def)) &&
           !(a->vc_def & (b->vc_use | b->vc_def)) &&
           !(((a->flags & VNOP_GROUP) || !(b->flags & VNOP_GROUP)) && (a->v_def & b->vfake));
}

static op word_op(const uint8_t *imem, uint32_t w)
{
    const uint8_t *p = imem + (w & 0x3FFu) * 4u;
    return decode((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
}

static void shift(rl_rsp_pipe_state *s, uint32_t r, uint32_t v, bool load)
{
    s->v[2] = s->v[1];
    s->v[1] = s->v[0];
    s->r[1] = s->r[0];
    s->load[1] = s->load[0];
    s->r[0] = r;
    s->v[0] = v;
    s->load[0] = load;
}

static uint32_t end(rl_rsp_pipe_state *s, const op *o)
{
    uint32_t cycles = 1, n;
    n = (o->r_use & s->r[0]) ? 2 : (o->r_use & s->r[1]) ? 1 : 0;
    for (uint32_t i = 0; i < n; i++)
        shift(s, 0, 0, false);
    cycles += n;
    n = (o->v_use & s->v[0]) ? 3 : (o->v_use & s->v[1]) ? 2 : (o->v_use & s->v[2]) ? 1 : 0;
    for (uint32_t i = 0; i < n; i++)
        shift(s, 0, 0, false);
    cycles += n;
    if (o->flags & STORE)
        while (s->load[1]) {
            shift(s, 0, 0, false);
            cycles++;
        }
    s->single = (o->flags & BRANCH) != 0;
    shift(s, r_write(o), o->v_def, (o->flags & LOAD) != 0);
    return cycles;
}

static uint32_t step(rl_rsp_pipe_state *s, uint32_t w, const uint8_t *imem)
{
    op a = word_op(imem, w);
    if (s->pend) {
        s->pend = false;
        op f = word_op(imem, w - 1u);
        op g;
        memset(&g, 0, sizeof g);
        g.flags = (uint8_t)((f.flags | a.flags) & ~BYPASS);
        g.r_use = f.r_use | a.r_use;
        g.r_def = r_write(&f) | r_write(&a);
        g.v_use = f.v_use | a.v_use;
        g.v_def = f.v_def | a.v_def;
        return end(s, &g);
    }
    if (!s->single && !(a.flags & BRANCH)) {
        op b = word_op(imem, w + 1u);
        if (pairs(&a, &b)) {
            s->pend = true;
            return 0;
        }
    }
    return end(s, &a);
}

static bool same(const rl_rsp_pipe_state *a, const rl_rsp_pipe_state *b)
{
    return a->r[0] == b->r[0] && a->r[1] == b->r[1] && a->v[0] == b->v[0] && a->v[1] == b->v[1] &&
           a->v[2] == b->v[2] && a->load[0] == b->load[0] && a->load[1] == b->load[1] && a->single == b->single &&
           a->pend == b->pend;
}

rl_rsp_pipe_state rl_rsp_pipe_unpack(uint64_t a, uint64_t b, uint64_t d)
{
    rl_rsp_pipe_state s;
    s.r[0] = (uint32_t)a;
    s.r[1] = (uint32_t)(a >> 32);
    s.v[0] = (uint32_t)b;
    s.v[1] = (uint32_t)(b >> 32);
    s.v[2] = (uint32_t)d;
    s.load[0] = (d >> 32) & 1;
    s.load[1] = (d >> 33) & 1;
    s.single = (d >> 34) & 1;
    s.pend = (d >> 35) & 1;
    return s;
}

void rl_rsp_pipe_taken(rl_rsp_pipe_state *s, uint32_t t)
{
    shift(s, 0, 0, false);
    s->single |= (t & 1u) != 0;
}

/** @brief The most words a delta follows (pipe::WINDOW). */
#define WINDOW 16u

int64_t rl_rsp_pipe_delta(rl_rsp_pipe_state x, rl_rsp_pipe_state e, uint32_t w, const uint8_t *imem, bool *met)
{
    int64_t d = 0;
    bool slot = false;
    for (uint32_t i = 0; i < WINDOW; i++) {
        if (same(&x, &e)) {
            *met = true;
            return d;
        }
        bool br = (word_op(imem, w).flags & BRANCH) != 0;
        d += (int64_t)step(&x, w, imem) - (int64_t)step(&e, w, imem);
        if (slot) {
            *met = same(&x, &e);
            return d;
        }
        slot = br;
        w = (w + 1u) & 0x3FFu;
    }
    *met = same(&x, &e);
    return d;
}
