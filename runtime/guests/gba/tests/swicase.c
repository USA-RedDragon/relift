/* The SWI case generator the console probe ran: deterministic cases, a pure function of (record, call).
 * No division by non-constants, no libc, no mutable globals: every case is a pure function of (i, j).
 * See swicase.h for the record format. Memory map (all through swi_mem):
 *   0x02000000.. SWI_SRC  sources            0x02008000.. SWI_DST  EWRAM destinations
 *   0x0200E000.. SWI_INFO small structs      0x06013000.. SWI_VDST VRAM destinations
 * Every destination window is prefilled with a pattern and hashed with 16 bytes before and 48 bytes after
 * the nominal range, so overruns show up in the hash.
 */
#include "swicase.h"

#define SRC SWI_SRC
#define DST SWI_DST
#define INFO SWI_INFO
#define VDST SWI_VDST

/* ---- small PRNG seeded by (i, j) ---- */
typedef struct {
    uint32_t s;
} rng_t;

static uint32_t rnd(rng_t *r)
{
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}
static uint32_t rr(rng_t *r, uint32_t n) /* 0..n-1, n >= 1 */
{
    return (uint32_t)(((uint64_t)rnd(r) * n) >> 32);
}
static void rseed(rng_t *r, uint32_t i, uint32_t j)
{
    uint32_t s = (i + 1u) * 0x9E3779B1u ^ (j + 1u) * 0x85EBCA6Bu ^ 0xC2B2AE35u;
    r->s = s ? s : 1u;
    for (int k = 0; k < 4; k++)
        rnd(r);
}
static uint32_t rmag(rng_t *r) /* random value over all magnitudes */
{
    uint32_t v = rnd(r);
    return v >> rr(r, 32);
}
static uint32_t rsgn(rng_t *r, uint32_t v) { return (rnd(r) & 0x10000u) ? 0u - v : v; }

/* ---- memory helpers (no-ops when m == 0: the hash pass only needs the plan) ---- */
static void w8(const swi_mem *m, uint32_t a, uint32_t v)
{
    if (m)
        m->wr8(m->u, a, (uint8_t)v);
}
static void w16(const swi_mem *m, uint32_t a, uint32_t v)
{
    if (m)
        m->wr16(m->u, a, (uint16_t)v);
}
static void w32(const swi_mem *m, uint32_t a, uint32_t v)
{
    if (m)
        m->wr32(m->u, a, v);
}
static void fillw(const swi_mem *m, uint32_t a, uint32_t nbytes, rng_t *r) /* a aligned */
{
    for (uint32_t n = 0; n < nbytes + 16u; n += 4)
        w32(m, a + n, rnd(r));
}
static uint32_t pat(uint32_t a) { return ((a ^ (a >> 7) ^ (a >> 15)) * 29u + 0x5Bu) & 0xFFu; }

#define MAXW 5
typedef struct {
    swi_case c;
    uint32_t wa[MAXW], wl[MAXW];
    unsigned nw;
    unsigned special; /* 0 none, 1 irq, 2 ram reset */
    unsigned flags;
} plan_t;

static void addwin(plan_t *p, uint32_t a, uint32_t l)
{
    if (p->nw < MAXW) {
        p->wa[p->nw] = a;
        p->wl[p->nw] = l;
        p->nw++;
    }
    if ((a >> 24) >= 5u && (a >> 24) <= 7u)
        p->flags |= SWI_REC_BLANK;
}
/* prefill and register a destination window: [addr&~3 - 16, ((addr+len+3)&~3) + 48) */
static void dwin(plan_t *p, const swi_mem *m, uint32_t addr, uint32_t len)
{
    uint32_t lo = (addr & ~3u) - 16u, hi = ((addr + len + 3u) & ~3u) + 48u;
    for (uint32_t a = lo; a < hi; a += 4)
        w32(m, a, pat(a) | pat(a + 1) << 8 | pat(a + 2) << 16 | pat(a + 3) << 24);
    addwin(p, lo, hi - lo);
}

/* ---- kinds ---- */
enum { K_DIV, K_SQRT, K_ATAN, K_ATAN2, K_CPUSET, K_FAST, K_CKSUM, K_BGAFF, K_OBJAFF, K_BITUN, K_LZW, K_LZV, K_HUFF, K_RLW,
       K_RLV, K_D8W, K_D8V, K_D16, K_MIDI, K_RESET, K_IRQ, K_THUMB };

typedef struct {
    const char *name;
    uint8_t kind, swi, sub;
    uint16_t nrec, calls;
} grp_t;

static const uint32_t div_edge[][2] = {
    {0, 0}, {5, 0x7FFFFFFFu}, {0xFFFFFFFBu, 0x80000001u}, {0x7FFFFFFFu, 0x10000}, {0x80000000u, 0x10001},
    {0x80000000u, 0xFFFFFFFFu}, {0x80000000u, 1}, {0xFFFFFFFFu, 0x80000000u}, {1, 0x80000000u},
    {0x80000000u, 0x80000000u}, {0x7FFFFFFFu, 0x80000000u}, {0x80000000u, 0x7FFFFFFFu},
    {7, 7}, {0xFFFFFFF9u, 0xFFFFFFF9u}, {7, 0xFFFFFFF9u}, {0xFFFFFFF9u, 7},
    {3, 5}, {0xFFFFFFFDu, 5}, {3, 0xFFFFFFFBu}, {0xFFFFFFFDu, 0xFFFFFFFBu},
    {5, 3}, {0xFFFFFFFBu, 3}, {5, 0xFFFFFFFDu}, {0xFFFFFFFBu, 0xFFFFFFFDu},
    {0, 5}, {0, 0xFFFFFFFBu}, {1, 1}, {0xFFFFFFFFu, 0xFFFFFFFFu}, {1, 0xFFFFFFFFu}, {0xFFFFFFFFu, 1},
    {0x7FFFFFFFu, 1}, {0x7FFFFFFFu, 0xFFFFFFFFu}, {0x7FFFFFFFu, 2}, {0x7FFFFFFFu, 0x7FFFFFFFu},
    {0x80000000u, 2}, {0x80000000u, 0xFFFFFFFEu}, {0x100, 0x10000}, {0x1000, 0x1000}, {0xFFFFF000u, 0x1000},
    {100, 7}, {0xFFFFFF9Cu, 7}, {100, 0xFFFFFFF9u}, {0xFFFFFF9Cu, 0xFFFFFFF9u}, {1000000, 1000},
};
#define NDIVTAB ((unsigned)(sizeof div_edge / sizeof div_edge[0]))
#define NDIVEDGE (NDIVTAB + 16u)
#define NSQEDGE 68u
#define NA2EDGE 89u

static const grp_t groups[] = {
    {"Div.edge", K_DIV, 6, 0, NDIVEDGE, 1},
    {"Div.rand", K_DIV, 6, 1, 250, 1},
    {"Div.dense", K_DIV, 6, 2, 8, 256},
    {"DivArm.edge", K_DIV, 7, 0, NDIVEDGE, 1},
    {"DivArm.rand", K_DIV, 7, 1, 250, 1},
    {"DivArm.dense", K_DIV, 7, 2, 8, 256},
    {"Sqrt.edge", K_SQRT, 8, 0, NSQEDGE, 1},
    {"Sqrt.rand", K_SQRT, 8, 1, 200, 1},
    {"Sqrt.dense", K_SQRT, 8, 2, 8, 256},
    {"ArcTan", K_ATAN, 9, 1, 200, 1},
    {"ArcTan2.edge", K_ATAN2, 10, 0, NA2EDGE, 1},
    {"ArcTan2.rand", K_ATAN2, 10, 1, 300, 1},
    {"ArcTan2.dense", K_ATAN2, 10, 2, 8, 256},
    {"CpuSet", K_CPUSET, 11, 1, 450, 1},
    {"CpuFastSet", K_FAST, 12, 1, 300, 1},
    {"GetBiosChecksum", K_CKSUM, 13, 1, 2, 1},
    {"BgAffineSet", K_BGAFF, 14, 1, 150, 1},
    {"ObjAffineSet", K_OBJAFF, 15, 1, 150, 1},
    {"BitUnPack", K_BITUN, 16, 1, 150, 1},
    {"LZ77UnCompWram", K_LZW, 17, 1, 100, 1},
    {"LZ77UnCompVram", K_LZV, 18, 1, 100, 1},
    {"HuffUnComp", K_HUFF, 19, 1, 80, 1},
    {"RLUnCompWram", K_RLW, 20, 1, 100, 1},
    {"RLUnCompVram", K_RLV, 21, 1, 100, 1},
    {"Diff8bitUnFilterWram", K_D8W, 22, 1, 60, 1},
    {"Diff8bitUnFilterVram", K_D8V, 23, 1, 60, 1},
    {"Diff16bitUnFilter", K_D16, 24, 1, 60, 1},
    {"MidiKey2Freq", K_MIDI, 31, 1, 100, 1},
    {"RegisterRamReset", K_RESET, 1, 1, 8, 1},
    {"IrqWait", K_IRQ, 0, 1, 10, 1},
    {"Thumb", K_THUMB, 0, 1, 40, 1},
};
#define NGROUPS ((unsigned)(sizeof groups / sizeof groups[0]))

static const grp_t *find_group(unsigned i, unsigned *k)
{
    for (unsigned g = 0; g < NGROUPS; g++) {
        if (i < groups[g].nrec) {
            *k = i;
            return &groups[g];
        }
        i -= groups[g].nrec;
    }
    return 0;
}

unsigned swi_record_count(void)
{
    unsigned n = 0;
    for (unsigned g = 0; g < NGROUPS; g++)
        n += groups[g].nrec;
    return n;
}
unsigned swi_record_calls(unsigned i)
{
    unsigned k;
    const grp_t *g = find_group(i, &k);
    return g ? g->calls : 0;
}
const char *swi_record_name(unsigned i)
{
    unsigned k;
    const grp_t *g = find_group(i, &k);
    return g ? g->name : "?";
}

/* ---- generators ---- */
static void gen_div(plan_t *p, unsigned swi, unsigned sub, unsigned k, rng_t *r)
{
    uint32_t num, den;
    if (sub == 0) {
        if (k < NDIVTAB) {
            num = div_edge[k][0];
            den = div_edge[k][1];
        } else {
            unsigned kk = k - NDIVTAB;
            num = 1u << (kk * 2u);
            den = 1u << (15u - kk);
        }
    } else {
        num = rsgn(r, rmag(r));
        den = rsgn(r, rmag(r));
        if (sub == 2 && rr(r, 4) == 0)
            den = rsgn(r, rnd(r) >> 16);
        if (den == 0) /* the real BIOS hangs on a zero divisor (except 0/0): never generated */
            den = 1u + rr(r, 100);
    }
    if (swi == 7) {
        p->c.r[0] = den;
        p->c.r[1] = num;
    } else {
        p->c.r[0] = num;
        p->c.r[1] = den;
    }
}

static void gen_sqrt(plan_t *p, unsigned sub, unsigned k, rng_t *r)
{
    static const uint32_t sp[8] = {0, 1, 2, 3, 4, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu};
    uint32_t v;
    if (sub == 0) {
        if (k < 8)
            v = sp[k];
        else {
            unsigned t = k - 8, e = 1u + (t >> 2);
            uint32_t s = 1u << e, sq = s * s;
            switch (t & 3u) {
            case 0: v = sq; break;
            case 1: v = sq - 1u; break;
            case 2: v = (s - 1u) * (s - 1u); break;
            default: v = (s + 1u) * (s + 1u); break;
            }
        }
    } else {
        v = rmag(r);
        if (rr(r, 5) == 0) { /* near a perfect square */
            uint32_t s = rmag(r) & 0xFFFFu;
            v = s * s + (uint32_t)(int32_t)rr(r, 5) - 2u;
        }
    }
    p->c.r[0] = v;
}

static void gen_atan(plan_t *p, unsigned k, rng_t *r)
{
    static const uint32_t e[20] = {0, 0x4000, 0xFFFFC000u, 0x2000, 0xFFFFE000u, 1, 0xFFFFFFFFu, 0x7FFF, 0xFFFF8001u, 0xFFFF8000u,
                                   0x3FFF, 0x4001, 0xFFFFBFFFu, 0x12345678u, 0xFFFF0000u, 0x80000000u, 0x7FFFFFFFu, 0xFFFFFFFFu,
                                   0x10000, 0x8000};
    uint32_t v;
    if (k < 20)
        v = e[k];
    else
        switch (rr(r, 4)) {
        case 0: v = (uint32_t)(int32_t)(int16_t)rsgn(r, rr(r, 0x4001)); break; /* within the documented range */
        case 1: v = (uint32_t)(int32_t)(int16_t)rnd(r); break;
        case 2: v = rnd(r); break;
        default: v = rmag(r); break;
        }
    p->c.r[0] = v;
}

static void gen_atan2(plan_t *p, unsigned sub, unsigned k, rng_t *r)
{
    static const uint32_t s9[9] = {0, 1, 0xFFFFFFFFu, 0x100, 0xFFFFFF00u, 0x7FFF, 0xFFFF8001u, 0x8000, 0xFFFF8000u};
    static const uint32_t ex[8][2] = {{0x10000, 0}, {0, 0x10000}, {0x7FFFFFFFu, 1}, {1, 0x7FFFFFFFu}, {0x80000000u, 0x80000000u},
                                      {0x12345, 0x54321}, {0xFFFEDCBBu, 0x12345}, {0x7FFFFFFFu, 0x7FFFFFFFu}};
    uint32_t x, y;
    if (sub == 0) {
        if (k < 81) {
            x = s9[k / 9u];
            y = s9[k % 9u];
        } else {
            x = ex[k - 81][0];
            y = ex[k - 81][1];
        }
    } else {
        switch (rr(r, 5)) {
        case 0: /* 16-bit signed */
            x = (uint32_t)(int32_t)(int16_t)rnd(r);
            y = (uint32_t)(int32_t)(int16_t)rnd(r);
            break;
        case 1: /* all magnitudes */
            x = rsgn(r, rmag(r));
            y = rsgn(r, rmag(r));
            break;
        case 2: /* diagonal */
            x = rsgn(r, rmag(r));
            y = rr(r, 2) ? x : 0u - x;
            break;
        case 3: /* near the axes */
            x = rsgn(r, rmag(r));
            y = rsgn(r, rr(r, 4));
            if (rr(r, 2)) {
                uint32_t t = x;
                x = y;
                y = t;
            }
            break;
        default: /* big */
            x = rnd(r);
            y = rnd(r);
            break;
        }
    }
    p->c.r[0] = x;
    p->c.r[1] = y;
}

static void gen_cpuset(plan_t *p, const swi_mem *m, unsigned k, rng_t *r, int fast)
{
    static const uint16_t cs[8] = {0, 1, 2, 3, 7, 8, 9, 31};
    static const uint16_t cf[15] = {0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 39, 40};
    const unsigned nc = fast ? 15u : 8u;
    const uint16_t *cl = fast ? cf : cs;
    unsigned is32, fill, srcsel, dstsel;
    uint32_t cnt, src, dst, ctl, unit, len;
    if (k < nc * 4u) {
        is32 = k & 1u;
        fill = (k >> 1) & 1u;
        cnt = cl[k >> 2];
        srcsel = 0;
        dstsel = 0;
    } else {
        is32 = rnd(r) & 1u;
        fill = (rnd(r) >> 8) & 1u;
        unsigned c = rr(r, 16);
        cnt = c < 8 ? cl[rr(r, nc)] : c < 13 ? rr(r, 41) : rr(r, 601);
        srcsel = rr(r, 40);
        dstsel = rr(r, 10);
    }
    if (fast)
        is32 = 1;
    unit = is32 ? 4u : 2u;
    uint32_t so = rnd(r) & 0x3Cu, dof = rnd(r) & 0x3Cu;
    if (srcsel < 24 || srcsel >= 36)
        src = SRC + so;
    else if (srcsel < 28)
        src = SRC + so + 1u + rr(r, 3);
    else
        switch (srcsel) {
        case 28: src = 0x00000000u; break;
        case 29: src = 0x00000100u; break;
        case 30: src = 0x00003FF0u; break;
        case 31: src = 0x01FFFFF0u; break;
        case 32: src = 0x01FFFFFCu; break;
        case 33: src = 0x00004000u; break;
        case 34: src = 0x10000000u; break;
        default: src = 0x0F000000u; break;
        }
    if (dstsel < 6)
        dst = DST + dof;
    else if (dstsel == 6)
        dst = DST + dof + 1u + rr(r, 3);
    else
        dst = VDST + dof;
    uint32_t junk = 0;
    if (k >= nc * 4u && rr(r, 4) == 0)
        junk = rnd(r) & (fast ? 0xFEE00000u : 0xFAE00000u);
    ctl = cnt | (uint32_t)fill << 24 | (uint32_t)(fast ? 0 : is32) << 26 | junk;
    len = fast ? ((cnt + 7u) & ~7u) * 4u : cnt * unit;
    fillw(m, SRC, len + 0x100u, r);
    dwin(p, m, dst, len);
    p->c.r[0] = src;
    p->c.r[1] = dst;
    p->c.r[2] = ctl;
}

static void gen_bgaff(plan_t *p, const swi_mem *m, unsigned k, rng_t *r)
{
    uint32_t cnt = k < 4 ? k : rr(r, 4);
    for (uint32_t n = 0; n < 4; n++) {
        uint32_t a = SRC + n * 20u;
        int nice = rr(r, 4) == 0;
        {
            uint32_t ox = rmag(r), sx = rnd(r) & 0x80000000u, oy = rmag(r), sy = rnd(r) & 0x80000000u;
            w32(m, a + 0, ox ^ sx);
            w32(m, a + 4, oy ^ sy);
        }
        w16(m, a + 8, rsgn(r, rmag(r) & 0xFFFFu));
        w16(m, a + 10, rsgn(r, rmag(r) & 0xFFFFu));
        w16(m, a + 12, nice ? 0x100u : rnd(r));
        w16(m, a + 14, nice ? 0x100u : rnd(r));
        w16(m, a + 16, rnd(r));
        w16(m, a + 18, rnd(r));
    }
    dwin(p, m, DST, 64);
    p->c.r[0] = SRC;
    p->c.r[1] = DST;
    p->c.r[2] = cnt;
}

static void gen_objaff(plan_t *p, const swi_mem *m, unsigned k, rng_t *r)
{
    static const uint32_t offs[12] = {2, 8, 6, 4, 10, 16, 0xFFFFFFF8u, 0xFFFFFFFEu, 0, 1, 3, 32};
    uint32_t cnt = k < 4 ? k : rr(r, 4);
    uint32_t off = k < 12 ? offs[k] : (rr(r, 3) ? offs[rr(r, 3)] : offs[rr(r, 12)]);
    uint32_t dst = DST + 0x400u;
    for (uint32_t n = 0; n < 4; n++) {
        uint32_t a = SRC + n * 8u;
        int nice = rr(r, 4) == 0;
        w16(m, a + 0, nice ? 0x100u : rnd(r));
        w16(m, a + 2, nice ? 0x100u : rnd(r));
        w16(m, a + 4, rnd(r));
        w16(m, a + 6, rnd(r));
    }
    dwin(p, m, dst - 0x200u + 16u, 0x400u - 64u); /* exactly [dst-0x200, dst+0x200) */
    p->c.r[0] = SRC;
    p->c.r[1] = dst;
    p->c.r[2] = cnt;
    p->c.r[3] = off;
}

static void gen_bitun(plan_t *p, const swi_mem *m, unsigned k, rng_t *r)
{
    static const uint8_t sw[4] = {1, 2, 4, 8}, dw[6] = {1, 2, 4, 8, 16, 32};
    static const uint8_t ill[6][2] = {{3, 8}, {8, 3}, {16, 32}, {8, 4}, {4, 2}, {2, 1}};
    uint32_t sW, dW, srclen, off = 0;
    int legal = 1;
    if (k < 24) {
        sW = sw[k & 3u];
        dW = dw[k >> 2];
        srclen = 16;
        legal = dW >= sW;
    } else if (k < 30) {
        sW = ill[k - 24][0];
        dW = ill[k - 24][1];
        srclen = 8;
        legal = 0;
    } else {
        unsigned si = rr(r, 4);
        sW = sw[si];
        dW = dw[si + rr(r, 6 - si)];
        srclen = rr(r, 65);
        switch (rr(r, 6)) {
        case 0: off = 0; break;
        case 1: off = 0x80000000u; break;
        case 2: off = rr(r, 256); break;
        case 3: off = 0x80000000u | rr(r, 256); break;
        case 4: off = 1; break;
        default: off = 0x80000001u; break;
        }
    }
    fillw(m, SRC, srclen + 8u, r);
    w16(m, INFO, srclen);
    w8(m, INFO + 2, sW);
    w8(m, INFO + 3, dW);
    w32(m, INFO + 4, off);
    uint32_t bytes = 256;
    if (legal) {
        unsigned sh = sW == 1 ? 0u : sW == 2 ? 1u : sW == 4 ? 2u : 3u;
        bytes = ((((srclen * 8u) >> sh) * dW + 31u) >> 5) * 4u;
    }
    dwin(p, m, DST, bytes);
    p->c.r[0] = SRC;
    p->c.r[1] = DST;
    p->c.r[2] = INFO;
}

static uint32_t rsize(rng_t *r, uint32_t lo_max, uint32_t mid_max, uint32_t max)
{
    switch (rr(r, 8)) {
    case 0: return rr(r, lo_max + 1u);
    case 1:
    case 2: return rr(r, mid_max + 1u);
    default: return rr(r, max + 1u);
    }
}

static void lz_stream(const swi_mem *m, uint32_t size, uint32_t hdr, rng_t *r)
{
    uint32_t a = SRC, pos = 0;
    w32(m, a, hdr | size << 8);
    a += 4;
    while (pos < size) {
        uint32_t fl = a++, flags = 0;
        for (unsigned b = 0; b < 8 && pos < size; b++) {
            uint32_t rem = size - pos;
            if (pos >= 1 && rem >= 3 && rr(r, 100) < 55) {
                uint32_t len = 3u + rr(r, 16), disp;
                if (len > rem)
                    len = rem;
                uint32_t maxd = pos < 0x1000u ? pos : 0x1000u;
                disp = rr(r, 4) == 0 ? 1u : 1u + rr(r, maxd);
                w8(m, a++, (len - 3u) << 4 | (disp - 1u) >> 8);
                w8(m, a++, (disp - 1u) & 0xFFu);
                flags |= 0x80u >> b;
                pos += len;
            } else {
                w8(m, a++, rnd(r) >> 24);
                pos++;
            }
        }
        w8(m, fl, flags);
    }
    while (a & 3u)
        w8(m, a++, 0);
}

static void gen_lz(plan_t *p, const swi_mem *m, unsigned k, rng_t *r, int vram)
{
    uint32_t size = rsize(r, 20, 200, 2000), hdr = 0x10;
    uint32_t src = SRC;
    if (k % 25u == 11u)
        hdr = (rnd(r) & 0xF0u) == 0x10u ? 0x30u : (rnd(r) & 0xFFu); /* garbage type bits */
    if (k % 33u == 7u)
        src = (k & 1u) ? 0x100u : 0u; /* source in the BIOS region */
    lz_stream(m, size, hdr, r);
    dwin(p, m, vram ? VDST : DST, size);
    p->c.r[0] = src;
    p->c.r[1] = vram ? VDST : DST;
}

static void gen_rl(plan_t *p, const swi_mem *m, unsigned k, rng_t *r, int vram)
{
    uint32_t size = rsize(r, 20, 200, 1000), hdr = 0x30, a = SRC + 4, pos = 0;
    if (k % 25u == 11u)
        hdr = (rnd(r) & 0xF0u) == 0x30u ? 0x10u : (rnd(r) & 0xFFu);
    w32(m, SRC, hdr | size << 8);
    while (pos < size) {
        uint32_t rem = size - pos;
        if (rem >= 3 && rr(r, 3) == 0) {
            uint32_t len = 3u + rr(r, 128);
            if (len > rem)
                len = rem;
            w8(m, a++, 0x80u | (len - 3u));
            w8(m, a++, rnd(r) >> 24);
            pos += len;
        } else {
            uint32_t len = 1u + rr(r, rr(r, 4) ? 12u : 128u);
            if (len > rem)
                len = rem;
            w8(m, a++, len - 1u);
            for (uint32_t n = 0; n < len; n++)
                w8(m, a++, rnd(r) >> 24);
            pos += len;
        }
    }
    while (a & 3u)
        w8(m, a++, 0);
    dwin(p, m, vram ? VDST : DST, size);
    p->c.r[0] = SRC;
    p->c.r[1] = vram ? VDST : DST;
}

typedef struct {
    uint8_t leaf[32], val[32];
    int8_t ch[32][2];
    unsigned n;
} tree_t;

static int tree_build(tree_t *t, unsigned nl, uint32_t mask, rng_t *r)
{
    int id = (int)t->n++;
    if (nl == 1) {
        t->leaf[id] = 1;
        t->val[id] = (uint8_t)(rnd(r) & mask);
        t->ch[id][0] = t->ch[id][1] = -1;
        return id;
    }
    unsigned a = 1u + rr(r, nl - 1u);
    t->leaf[id] = 0;
    t->val[id] = 0;
    int c0 = tree_build(t, a, mask, r);
    int c1 = tree_build(t, nl - a, mask, r);
    t->ch[id][0] = (int8_t)c0;
    t->ch[id][1] = (int8_t)c1;
    return id;
}

static void gen_huff(plan_t *p, const swi_mem *m, unsigned k, rng_t *r)
{
    tree_t t;
    uint8_t tab[32], slot[32];
    int q[32], qh = 0, qt = 0;
    unsigned bits = 8, nl = 2u + 2u * rr(r, 7);
    switch (k % 16u) {
    case 3: bits = 2; break;
    case 9: bits = 5; break;
    default: bits = rr(r, 2) ? 8u : 4u; break;
    }
    uint32_t size = 4u * (1u + rr(r, 24));
    if (k % 8u == 5u)
        size = 1u + rr(r, 40); /* odd sizes */
    t.n = 0;
    int root = tree_build(&t, nl, (1u << bits) - 1u, r);
    for (unsigned n = 0; n < 32; n++)
        tab[n] = slot[n] = 0;
    tab[0] = (uint8_t)(nl - 1u);
    slot[root] = 1;
    unsigned np = 2;
    q[qt++] = root;
    while (qh < qt) {
        int nd = q[qh++];
        unsigned s = slot[nd], pr = np;
        np += 2;
        uint8_t byte = (uint8_t)((pr - (s & ~1u)) / 2u - 1u);
        for (int c = 0; c < 2; c++) {
            int ch = t.ch[nd][c];
            slot[ch] = (uint8_t)(pr + (unsigned)c);
            if (t.leaf[ch]) {
                tab[pr + (unsigned)c] = t.val[ch];
                byte |= c ? 0x40u : 0x80u;
            } else
                q[qt++] = ch;
        }
        tab[s] = byte;
    }
    w32(m, SRC, 0x20u | bits | size << 8);
    for (unsigned n = 0; n < 2u * nl; n++)
        w8(m, SRC + 4u + n, tab[n]);
    uint32_t a = (SRC + 4u + 2u * nl + 3u) & ~3u;
    /* bitstream: random walks, MSB first in each little-endian word */
    uint32_t nsym = 2u, acc = 0, nb = 0;
    for (uint32_t v = size * 8u; v >= bits; v -= bits)
        nsym++;
    for (uint32_t s = 0; s < nsym; s++) {
        int nd = root;
        while (!t.leaf[nd]) {
            unsigned b = rnd(r) >> 31;
            acc = acc << 1 | b;
            if (++nb == 32) {
                w32(m, a, acc);
                a += 4;
                acc = 0;
                nb = 0;
            }
            nd = t.ch[nd][b];
        }
    }
    if (nb) {
        w32(m, a, acc << (32u - nb));
        a += 4;
    }
    w32(m, a, 0);
    dwin(p, m, DST, (size + 3u) & ~3u);
    p->c.r[0] = SRC;
    p->c.r[1] = DST;
    p->c.r[2] = 0;
}

static void gen_diff(plan_t *p, const swi_mem *m, unsigned k, rng_t *r, int kind)
{
    /* kind 0: 8-bit to Wram, 1: 8-bit to Vram, 2: 16-bit */
    uint32_t size = rsize(r, 16, 100, 300), hdr = kind == 2 ? 0x82u : 0x81u;
    if (kind == 2 && rr(r, 8))
        size &= ~1u;
    if (k % 25u == 4u)
        hdr = kind == 2 ? 0x81u : 0x82u; /* data size field disagrees with the SWI */
    w32(m, SRC, hdr | size << 8);
    for (uint32_t n = 0; n < size + 8u; n += 4)
        w32(m, SRC + 4u + n, rnd(r));
    dwin(p, m, kind == 1 ? VDST : DST, size);
    p->c.r[0] = SRC;
    p->c.r[1] = kind == 1 ? VDST : DST;
}

static void gen_midi(plan_t *p, const swi_mem *m, unsigned k, rng_t *r)
{
    static const uint8_t keys[8] = {0, 1, 60, 127, 128, 150, 255, 69};
    uint32_t key = k < 8 ? keys[k] : rr(r, 3) ? rr(r, 128) : rr(r, 200);
    uint32_t fine = k < 8 ? (k * 37u) & 0xFFu : rr(r, 4) ? rr(r, 256) : rr(r, 300);
    uint32_t freq = rr(r, 3) ? rr(r, 0x20000u) : rmag(r);
    if (rr(r, 6) == 0)
        freq = 0x7C00u + rr(r, 0x300u);
    w16(m, INFO + 0x40, rnd(r));
    w16(m, INFO + 0x42, rnd(r));
    w32(m, INFO + 0x44, freq);
    w32(m, INFO + 0x48, rnd(r));
    w32(m, INFO + 0x4C, rnd(r));
    p->c.r[0] = INFO + 0x40u;
    p->c.r[1] = key;
    p->c.r[2] = fine;
}

static const struct {
    uint16_t off, val;
} ioinit[] = {
    {0x84, 0x0080}, {0x80, 0x7777}, {0x82, 0x0905}, {0x88, 0x42A0}, {0x08, 0x1F85}, {0x0A, 0x1E86}, {0x0C, 0x1D87},
    {0x0E, 0x1C88}, {0x48, 0x3F2B}, {0x4A, 0x3F37}, {0x50, 0x3F7F}, {0x52, 0x1F0F}, {0x120, 0x5678}, {0x122, 0x1234},
    {0x124, 0x4321}, {0x126, 0x8765}, {0x128, 0x1003}, {0x12A, 0x0F0F}, {0x132, 0x0123}, {0x134, 0x80F0},
};
static const uint16_t iohash[] = {0x00, 0x08, 0x0A, 0x0C, 0x0E, 0x48, 0x4A, 0x50, 0x52, 0x80, 0x82, 0x84, 0x88, 0xBA, 0xC6,
                                  0xD2, 0xDE, 0x120, 0x122, 0x124, 0x126, 0x128, 0x12A, 0x132, 0x134, 0x200, 0x204, 0x208};

static void gen_reset(plan_t *p, const swi_mem *m, unsigned k)
{
    static const uint8_t fl[8] = {0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1C, 0xE0};
    for (unsigned n = 0; n < sizeof ioinit / sizeof ioinit[0]; n++)
        w16(m, 0x04000000u + ioinit[n].off, ioinit[n].val);
    w16(m, 0x04000000u, 0x0080); /* DISPCNT: forced blank */
    static const uint32_t base[3] = {0x05000000u, 0x06000000u, 0x07000000u}, len[3] = {0x400u, 0x18000u, 0x400u};
    for (unsigned w = 0; w < 3; w++) {
        for (uint32_t a = base[w]; a < base[w] + len[w]; a += 4)
            w32(m, a, pat(a) | pat(a + 1) << 8 | pat(a + 2) << 16 | pat(a + 3) << 24 | 0x01010101u);
        addwin(p, base[w], len[w]);
    }
    p->special = 2;
    p->c.r[0] = fl[k & 7u];
}

static void gen_irq(plan_t *p, const swi_mem *m, unsigned k)
{
    static const struct {
        uint8_t swi, r0, r1, ie, vbl, tm0, ime;
        uint8_t flag;
    } T[10] = {
        {5, 0, 0, 1, 1, 0, 0, 0}, {5, 0, 0, 1, 1, 0, 0, 1}, {4, 0, 1, 1, 1, 0, 0, 1}, {4, 1, 1, 1, 1, 0, 0, 1},
        {4, 0, 1, 1, 1, 0, 0, 0}, {4, 1, 8, 8, 0, 1, 0, 0}, {4, 0, 8, 8, 0, 0, 0, 8}, {2, 0, 0, 1, 1, 0, 0, 0},
        {2, 0, 0, 1, 1, 0, 1, 0}, {4, 1, 9, 9, 1, 1, 0, 0},
    };
    unsigned t = k % 10u;
    w16(m, 0x04000208u, 0);       /* IME = 0 */
    w16(m, 0x04000200u, 0);       /* IE = 0 while the rest is programmed */
    w16(m, 0x04000102u, 0);       /* TM0 off */
    w16(m, 0x04000202u, 0xFFFF);  /* acknowledge everything */
    w16(m, 0x04000004u, T[t].vbl ? 0x0008u : 0u); /* DISPSTAT: VBlank IRQ enable */
    w32(m, SWI_FLAGWORD, T[t].flag);
    if (T[t].tm0) {
        w16(m, 0x04000100u, 0xD000u); /* reload: overflow after 0x3000 cycles */
        w16(m, 0x04000102u, 0x00C0u); /* enable + IRQ, prescaler 1 */
    }
    w16(m, 0x04000200u, T[t].ie);
    w16(m, 0x04000208u, T[t].ime);
    w16(m, 0x04000202u, 0xFFFF);
    p->special = 1;
    p->flags |= SWI_REC_IRQ;
    p->c.swi = T[t].swi;
    p->c.r[0] = T[t].r0;
    p->c.r[1] = T[t].r1;
    p->c.r[2] = 0;
    p->c.r[3] = 0;
}

static void gen_kind(plan_t *p, const swi_mem *m, unsigned kind, unsigned swi, unsigned sub, unsigned k, rng_t *r)
{
    switch (kind) {
    case K_DIV: gen_div(p, swi, sub, k, r); break;
    case K_SQRT: gen_sqrt(p, sub, k, r); break;
    case K_ATAN: gen_atan(p, k, r); break;
    case K_ATAN2: gen_atan2(p, sub, k, r); break;
    case K_CPUSET: gen_cpuset(p, m, k, r, 0); break;
    case K_FAST: gen_cpuset(p, m, k, r, 1); break;
    case K_CKSUM:
        if (k)
            for (int n = 0; n < 4; n++)
                p->c.r[n] = n ? rnd(r) : 0xBAAE187Fu;
        break;
    case K_BGAFF: gen_bgaff(p, m, k, r); break;
    case K_OBJAFF: gen_objaff(p, m, k, r); break;
    case K_BITUN: gen_bitun(p, m, k, r); break;
    case K_LZW: gen_lz(p, m, k, r, 0); break;
    case K_LZV: gen_lz(p, m, k, r, 1); break;
    case K_HUFF: gen_huff(p, m, k, r); break;
    case K_RLW: gen_rl(p, m, k, r, 0); break;
    case K_RLV: gen_rl(p, m, k, r, 1); break;
    case K_D8W: gen_diff(p, m, k, r, 0); break;
    case K_D8V: gen_diff(p, m, k, r, 1); break;
    case K_D16: gen_diff(p, m, k, r, 2); break;
    case K_MIDI: gen_midi(p, m, k, r); break;
    default: break;
    }
}

static const struct {
    uint8_t kind, swi;
} thumb_list[20] = {
    {K_DIV, 6}, {K_DIV, 7}, {K_SQRT, 8}, {K_ATAN, 9}, {K_ATAN2, 10}, {K_CPUSET, 11}, {K_FAST, 12}, {K_CKSUM, 13}, {K_BGAFF, 14},
    {K_OBJAFF, 15}, {K_BITUN, 16}, {K_LZW, 17}, {K_LZV, 18}, {K_HUFF, 19}, {K_RLW, 20}, {K_RLV, 21}, {K_D8W, 22}, {K_D8V, 23},
    {K_D16, 24}, {K_MIDI, 31},
};

static int plan_make(unsigned i, unsigned j, plan_t *p, const swi_mem *m)
{
    unsigned k;
    const grp_t *g = find_group(i, &k);
    rng_t r;
    if (!g)
        return 0;
    rseed(&r, i, j);
    p->c.swi = g->swi;
    p->c.thumb = 0;
    p->c.r[0] = 0x11111111u;
    p->c.r[1] = 0x22222222u;
    p->c.r[2] = 0x33333333u;
    p->c.r[3] = 0x44444444u;
    p->nw = 0;
    p->special = 0;
    p->flags = 0;
    if (g->kind == K_THUMB) {
        unsigned t = k % 20u;
        p->c.swi = thumb_list[t].swi;
        p->c.thumb = 1;
        gen_kind(p, m, thumb_list[t].kind, thumb_list[t].swi, 1, 5000u + k, &r);
    } else if (g->kind == K_RESET)
        gen_reset(p, m, k);
    else if (g->kind == K_IRQ)
        gen_irq(p, m, k);
    else {
        gen_kind(p, m, g->kind, g->swi, g->sub, k, &r);
    }
    return 1;
}

unsigned swi_record_flags(unsigned i)
{
    plan_t p;
    if (!plan_make(i, 0, &p, 0))
        return 0;
    return p.flags;
}

void swi_case_make(unsigned i, unsigned j, swi_case *c, const swi_mem *m)
{
    plan_t p;
    if (!plan_make(i, j, &p, m)) {
        c->swi = 0;
        c->thumb = 0;
        for (int n = 0; n < 4; n++)
            c->r[n] = 0;
        return;
    }
    *c = p.c;
}

uint32_t swi_case_hash(unsigned i, unsigned j, const swi_case *c, const swi_mem *m)
{
    plan_t p;
    uint32_t h = SWI_FNV_INIT;
    (void)c;
    if (!plan_make(i, j, &p, 0))
        return h;
    if (p.special == 1) {
        static const uint32_t a[3] = {0x04000200u, 0x04000202u, 0x04000208u};
        for (int n = 0; n < 3; n++)
            for (int b = 0; b < 2; b++)
                h = swi_fnv(h, m->rd8(m->u, a[n] + (uint32_t)b));
        for (int b = 0; b < 4; b++)
            h = swi_fnv(h, m->rd8(m->u, SWI_FLAGWORD + (uint32_t)b));
        return h;
    }
    for (unsigned w = 0; w < p.nw; w++)
        for (uint32_t n = 0; n < p.wl[w]; n++)
            h = swi_fnv(h, m->rd8(m->u, p.wa[w] + n));
    if (p.special == 2)
        for (unsigned n = 0; n < sizeof iohash / sizeof iohash[0]; n++)
            for (uint32_t b = 0; b < 2; b++)
                h = swi_fnv(h, m->rd8(m->u, 0x04000000u + iohash[n] + b));
    return h;
}
