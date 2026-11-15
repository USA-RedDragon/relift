/**
 * @file math.c
 * @brief Div, DivArm, Sqrt, ArcTan, ArcTan2, BgAffineSet and ObjAffineSet,
 * with the registers the console's routines leave.
 */
#include "bios_int.h"

const int16_t bios_sin[256] = {
    0x0000, 0x0192, 0x0323, 0x04B5, 0x0645, 0x07D5, 0x0964, 0x0AF1, 0x0C7C, 0x0E05, 0x0F8C, 0x1111, 0x1294,
    0x1413, 0x158F, 0x1708, 0x187D, 0x19EF, 0x1B5D, 0x1CC6, 0x1E2B, 0x1F8B, 0x20E7, 0x223D, 0x238E, 0x24DA,
    0x261F, 0x275F, 0x2899, 0x29CD, 0x2AFA, 0x2C21, 0x2D41, 0x2E5A, 0x2F6B, 0x3076, 0x3179, 0x3274, 0x3367,
    0x3453, 0x3536, 0x3612, 0x36E5, 0x37AF, 0x3871, 0x392A, 0x39DA, 0x3A82, 0x3B20, 0x3BB6, 0x3C42, 0x3CC5,
    0x3D3E, 0x3DAE, 0x3E14, 0x3E71, 0x3EC5, 0x3F0E, 0x3F4E, 0x3F84, 0x3FB1, 0x3FD3, 0x3FEC, 0x3FFB, 0x4000,
    0x3FFB, 0x3FEC, 0x3FD3, 0x3FB1, 0x3F84, 0x3F4E, 0x3F0E, 0x3EC5, 0x3E71, 0x3E14, 0x3DAE, 0x3D3E, 0x3CC5,
    0x3C42, 0x3BB6, 0x3B20, 0x3A82, 0x39DA, 0x392A, 0x3871, 0x37AF, 0x36E5, 0x3612, 0x3536, 0x3453, 0x3367,
    0x3274, 0x3179, 0x3076, 0x2F6B, 0x2E5A, 0x2D41, 0x2C21, 0x2AFA, 0x29CD, 0x2899, 0x275F, 0x261F, 0x24DA,
    0x238E, 0x223D, 0x20E7, 0x1F8B, 0x1E2B, 0x1CC6, 0x1B5D, 0x19EF, 0x187D, 0x1708, 0x158F, 0x1413, 0x1294,
    0x1111, 0x0F8C, 0x0E05, 0x0C7C, 0x0AF1, 0x0964, 0x07D5, 0x0645, 0x04B5, 0x0323, 0x0192, 0x0000, -0x0192,
    -0x0323, -0x04B5, -0x0645, -0x07D5, -0x0964, -0x0AF1, -0x0C7C, -0x0E05, -0x0F8C, -0x1111, -0x1294,
    -0x1413, -0x158F, -0x1708, -0x187D, -0x19EF, -0x1B5D, -0x1CC6, -0x1E2B, -0x1F8B, -0x20E7, -0x223D,
    -0x238E, -0x24DA, -0x261F, -0x275F, -0x2899, -0x29CD, -0x2AFA, -0x2C21, -0x2D41, -0x2E5A, -0x2F6B,
    -0x3076, -0x3179, -0x3274, -0x3367, -0x3453, -0x3536, -0x3612, -0x36E5, -0x37AF, -0x3871, -0x392A,
    -0x39DA, -0x3A82, -0x3B20, -0x3BB6, -0x3C42, -0x3CC5, -0x3D3E, -0x3DAE, -0x3E14, -0x3E71, -0x3EC5,
    -0x3F0E, -0x3F4E, -0x3F84, -0x3FB1, -0x3FD3, -0x3FEC, -0x3FFB, -0x4000, -0x3FFB, -0x3FEC, -0x3FD3,
    -0x3FB1, -0x3F84, -0x3F4E, -0x3F0E, -0x3EC5, -0x3E71, -0x3E14, -0x3DAE, -0x3D3E, -0x3CC5, -0x3C42,
    -0x3BB6, -0x3B20, -0x3A82, -0x39DA, -0x392A, -0x3871, -0x37AF, -0x36E5, -0x3612, -0x3536, -0x3453,
    -0x3367, -0x3274, -0x3179, -0x3076, -0x2F6B, -0x2E5A, -0x2D41, -0x2C21, -0x2AFA, -0x29CD, -0x2899,
    -0x275F, -0x261F, -0x24DA, -0x238E, -0x223D, -0x20E7, -0x1F8B, -0x1E2B, -0x1CC6, -0x1B5D, -0x19EF,
    -0x187D, -0x1708, -0x158F, -0x1413, -0x1294, -0x1111, -0x0F8C, -0x0E05, -0x0C7C, -0x0AF1, -0x0964,
    -0x07D5, -0x0645, -0x04B5, -0x0323, -0x0192,
};

static uint32_t asr(uint32_t v, unsigned n) { return (uint32_t)((int32_t)v >> n); }

bios_udiv bios_udiv_run(uint32_t n, uint32_t d, uint32_t q0)
{
    bios_udiv r = {.q = q0};
    uint32_t s = d;
    for (;;) {
        uint32_t h = n >> 1;
        bool more = s < h;
        if (s <= h)
            s <<= 1;
        r.steps++;
        if (!more)
            break;
        if (s == 0) {
            r.hang = true;
            return r;
        }
    }
    for (;;) {
        bool ge = n >= s;
        r.q = r.q * 2 + ge;
        if (ge)
            n -= s;
        r.steps++;
        if (s == d)
            break;
        s >>= 1;
    }
    r.rem = n;
    return r;
}

static void div_signed(bios_call *c, uint32_t num, uint32_t den)
{
    uint32_t d = den & 0x80000000u ? 0u - den : den;
    uint32_t n = num & 0x80000000u ? 0u - num : num;
    bios_udiv u = bios_udiv_run(n, d, den & 0x80000000u);
    bios_tick(c, 9 + 4 * u.steps);
    if (u.hang) {
        c->status = RL_GBA_SWI_HANG;
        return;
    }
    bool neg_q = ((den ^ num) & 0x80000000u) != 0;
    c->r[3] = u.q;
    c->r[0] = neg_q ? 0u - u.q : u.q;
    c->r[1] = num & 0x80000000u ? 0u - u.rem : u.rem;
}

void bios_div(bios_call *c) { div_signed(c, c->r[0], c->r[1]); }

void bios_div_arm(bios_call *c)
{
    bios_tick(c, 3);
    div_signed(c, c->r[1], c->r[0]);
}

void bios_sqrt(bios_call *c)
{
    bios_save(c, 4, 4, false);
    uint32_t v = c->r[0], x = v, g = 1;
    bios_tick(c, 4);
    while (x > g) {
        x >>= 1;
        g <<= 1;
        bios_tick(c, 4);
    }
    uint32_t prev, q;
    do {
        prev = g;
        bios_udiv u = bios_udiv_run(v, g, 0);
        q = u.q;
        g = (g + q) >> 1;
        bios_tick(c, 9 + 4 * u.steps);
    } while (g < prev);
    c->r[0] = prev;
    c->r[1] = g;
    c->r[3] = q;
}

static uint32_t arctan_core(bios_call *c, uint32_t x)
{
    static const uint32_t k[] = {0x390, 0x91C, 0xFB6, 0x16AA, 0x2081, 0x3651, 0xA2F9};
    uint32_t sq = 0u - asr(x * x, 14);
    uint32_t a = 0xA9;
    for (unsigned i = 0; i < 7; i++)
        a = asr(sq * a, 14) + k[i];
    c->r[1] = sq;
    c->r[3] = a;
    bios_tick(c, 40);
    return asr(x * a, 16);
}

void bios_arctan(bios_call *c) { c->r[0] = arctan_core(c, c->r[0]); }

static uint32_t sdiv_q(bios_call *c, uint32_t num, uint32_t den)
{
    div_signed(c, num, den);
    return c->r[0];
}

void bios_arctan2(bios_call *c)
{
    bios_save(c, 4, 7, true);
    int32_t x = (int32_t)c->r[0], y = (int32_t)c->r[1];
    bios_tick(c, 12);
    if (y == 0) {
        c->r[0] = x >= 0 ? 0 : 0x8000;
    } else if (x == 0) {
        c->r[0] = y >= 0 ? 0x4000 : 0xC000;
    } else {
        uint32_t xs = (uint32_t)x << 14, ys = (uint32_t)y << 14;
        uint32_t ux = (uint32_t)x, uy = (uint32_t)y;
        int32_t nx = (int32_t)(0u - ux), ny = (int32_t)(0u - uy);
        int kind;
        if (y >= 0) {
            if (x >= 0)
                kind = x >= y ? 0 : 1;
            else
                kind = nx < y ? 1 : 2;
        } else if (x > 0) {
            kind = x < ny ? 3 : 4;
        } else {
            kind = nx > ny ? 2 : 3;
        }
        uint32_t t;
        switch (kind) {
        case 0:
            c->r[0] = arctan_core(c, sdiv_q(c, ys, ux));
            break;
        case 1:
            t = arctan_core(c, sdiv_q(c, xs, uy));
            c->r[0] = 0x4000 - t;
            break;
        case 2:
            t = arctan_core(c, sdiv_q(c, ys, ux));
            c->r[0] = 0x8000 + t;
            break;
        case 3:
            t = arctan_core(c, sdiv_q(c, xs, uy));
            c->r[0] = 0xC000 - t;
            break;
        default:
            t = arctan_core(c, sdiv_q(c, ys, ux));
            c->r[0] = 0x10000 + t;
            break;
        }
    }
    c->r[3] = RL_GBA_BIOS_SWI_RETURN;
}

void bios_bg_affine(bios_call *c)
{
    bios_save(c, 4, 11, false);
    uint32_t src = c->r[0], dst = c->r[1];
    int32_t n = (int32_t)c->r[2];
    bios_tick(c, 12);
    while (--n >= 0) {
        uint32_t ang = (bios_ld16(c, src + 0x10, false) & 0xFFFFu) >> 8;
        uint32_t cs = (uint32_t)(int32_t)bios_sin[(ang + 0x40) & 0xFF];
        uint32_t sn = (uint32_t)(int32_t)bios_sin[ang];
        uint32_t sx = bios_ld16s(c, src + 0xC, false), sy = bios_ld16s(c, src + 0xE, true);
        uint32_t pa = asr(cs * sx, 14), pb = asr(sn * sx, 14), pc = asr(sn * sy, 14), pd = asr(cs * sy, 14);
        uint32_t ox = bios_ldm(c, src, false), oy = bios_ldm(c, src + 4, true), dxy = bios_ldm(c, src + 8, true);
        uint32_t dx = (uint32_t)(int32_t)(int16_t)dxy, dy = asr(dxy, 16);
        uint32_t startx = pa * (0u - dx) + ox + pb * dy;
        uint32_t starty = pc * (0u - dx) + oy + pd * (0u - dy);
        bios_st32(c, dst + 8, startx, false);
        bios_st32(c, dst + 12, starty, false);
        bios_st16(c, dst, pa, false);
        bios_st16(c, dst + 2, 0u - pb, false);
        bios_st16(c, dst + 4, pc, false);
        bios_st16(c, dst + 6, pd, false);
        bios_tick(c, 60);
        c->r[3] = pa;
        src += 20;
        dst += 16;
    }
    c->r[0] = src;
    c->r[1] = dst;
}

void bios_obj_affine(bios_call *c)
{
    bios_save(c, 8, 11, false);
    uint32_t src = c->r[0], dst = c->r[1], step = c->r[3];
    int32_t n = (int32_t)c->r[2];
    bios_tick(c, 10);
    while (--n >= 0) {
        uint32_t ang = (bios_ld16(c, src + 4, false) & 0xFFFFu) >> 8;
        uint32_t cs = (uint32_t)(int32_t)bios_sin[(ang + 0x40) & 0xFF];
        uint32_t sn = (uint32_t)(int32_t)bios_sin[ang];
        uint32_t sx = bios_ld16s(c, src, false), sy = bios_ld16s(c, src + 2, false);
        bios_st16(c, dst, asr(cs * sx, 14), false);
        dst += step;
        bios_st16(c, dst, 0u - asr(sn * sx, 14), false);
        dst += step;
        bios_st16(c, dst, asr(sn * sy, 14), false);
        dst += step;
        bios_st16(c, dst, asr(cs * sy, 14), false);
        dst += step;
        bios_tick(c, 50);
        src += 8;
    }
    c->r[0] = src;
    c->r[1] = dst;
}
