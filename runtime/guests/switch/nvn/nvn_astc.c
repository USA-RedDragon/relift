/**
 * @file nvn_astc.c
 * @brief ASTC LDR decoding for hosts whose Vulkan device cannot sample ASTC textures.
 *
 * Written from the Khronos Data Format Specification's ASTC chapter. The decoder turns one 16 byte block into
 * bw x bh RGBA8 texels: both the UNORM and the sRGB decodings keep the top 8 bits of the 16 bit result (the
 * output of the reference encoder's 8 bit decoder), and the host's R8G8B8A8_SRGB format converts the sRGB ones. HDR endpoint modes and
 * every illegal encoding give the error color (opaque magenta).
 */
#include "nvn_astc.h"

#include <string.h>

#define MAX_TEXELS 144
#define MAX_PARTS 4

/** @brief Reads count bits of the 128 bit block from bit position pos (LSB first, at most 32 bits). */
static uint32_t get_bits(const uint8_t b[16], uint32_t pos, uint32_t count)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t p = pos + i;
        v |= (uint32_t)(b[p >> 3] >> (p & 7) & 1u) << i;
    }
    return v;
}

static void decode_trits(uint32_t t, uint8_t out[5])
{
    uint32_t c, t4, t3, t2, t1, t0;
    if ((t >> 2 & 7) == 7) {
        c = (t >> 5 & 7) << 2 | (t & 3);
        t4 = 2;
        t3 = 2;
    } else {
        c = t & 0x1f;
        if ((t >> 5 & 3) == 3) {
            t4 = 2;
            t3 = t >> 7 & 1;
        } else {
            t4 = t >> 7 & 1;
            t3 = t >> 5 & 3;
        }
    }
    if ((c & 3) == 3) {
        t2 = 2;
        t1 = c >> 4 & 1;
        t0 = (c >> 3 & 1) << 1 | ((c >> 2 & 1) & ~(c >> 3) & 1u);
    } else if ((c >> 2 & 3) == 3) {
        t2 = 2;
        t1 = 2;
        t0 = c & 3;
    } else {
        t2 = c >> 4 & 1;
        t1 = c >> 2 & 3;
        t0 = (c >> 1 & 1) << 1 | ((c & 1) & ~(c >> 1) & 1u);
    }
    out[0] = (uint8_t)t0;
    out[1] = (uint8_t)t1;
    out[2] = (uint8_t)t2;
    out[3] = (uint8_t)t3;
    out[4] = (uint8_t)t4;
}

static void decode_quints(uint32_t q, uint8_t out[3])
{
    uint32_t q2, q1, q0;
    if ((q >> 1 & 3) == 3 && (q >> 5 & 3) == 0) {
        q0 = 4;
        q1 = 4;
        q2 = (q & 1) << 2 | ((q >> 4 & 1) & ~(q & 1) & 1u) << 1 | ((q >> 3 & 1) & ~(q & 1) & 1u);
    } else {
        uint32_t c;
        if ((q >> 1 & 3) == 3) {
            q2 = 4;
            c = (q >> 3 & 3) << 3 | ((~(q >> 5) & 3u) << 1) | (q & 1);
        } else {
            q2 = q >> 5 & 3;
            c = q & 0x1f;
        }
        if ((c & 7) == 5) {
            q1 = 4;
            q0 = c >> 3 & 3;
        } else {
            q1 = c >> 3 & 3;
            q0 = c & 7;
        }
    }
    out[0] = (uint8_t)q0;
    out[1] = (uint8_t)q1;
    out[2] = (uint8_t)q2;
}

/** @brief The bit, trit and quint parts of the integer sequence encoding for a value range (max value). */
typedef struct ise_range {
    uint8_t max, bits, trit, quint;
} ise_range;

static const ise_range ranges[] = {
    { 1, 1, 0, 0 },  { 2, 0, 1, 0 },  { 3, 2, 0, 0 },  { 4, 0, 0, 1 },  { 5, 1, 1, 0 },   { 7, 3, 0, 0 },
    { 9, 1, 0, 1 },  { 11, 2, 1, 0 }, { 15, 4, 0, 0 }, { 19, 2, 0, 1 }, { 23, 3, 1, 0 },  { 31, 5, 0, 0 },
    { 39, 3, 0, 1 }, { 47, 4, 1, 0 }, { 63, 6, 0, 0 }, { 79, 4, 0, 1 }, { 95, 5, 1, 0 },  { 127, 7, 0, 0 },
    { 159, 5, 0, 1 }, { 191, 6, 1, 0 }, { 255, 8, 0, 0 },
};
#define NRANGES ((int)(sizeof ranges / sizeof ranges[0]))

static uint32_t ise_bits(const ise_range *r, uint32_t n)
{
    uint32_t b = r->bits * n;
    if (r->trit)
        b += (8 * n + 4) / 5;
    if (r->quint)
        b += (7 * n + 2) / 3;
    return b;
}

/** @brief Decodes n values of range r from bit position pos of b into out. */
static void ise_decode(const uint8_t b[16], uint32_t pos, const ise_range *r, uint32_t n, uint8_t *out)
{
    if (!r->trit && !r->quint) {
        for (uint32_t i = 0; i < n; i++, pos += r->bits)
            out[i] = (uint8_t)get_bits(b, pos, r->bits);
        return;
    }
    uint32_t group = r->trit ? 5 : 3;
    static const uint8_t tshift[5] = { 0, 2, 4, 5, 7 }, tbits[5] = { 2, 2, 1, 2, 1 };
    static const uint8_t qshift[3] = { 0, 3, 5 }, qbits[3] = { 3, 2, 2 };
    for (uint32_t i = 0; i < n; i += group) {
        uint32_t m[5], packed = 0, cnt = n - i < group ? n - i : group;
        for (uint32_t j = 0; j < cnt; j++) {
            m[j] = get_bits(b, pos, r->bits);
            pos += r->bits;
            uint32_t tb = r->trit ? tbits[j] : qbits[j], sh = r->trit ? tshift[j] : qshift[j];
            packed |= get_bits(b, pos, tb) << sh;
            pos += tb;
        }
        uint8_t d[5];
        if (r->trit)
            decode_trits(packed, d);
        else
            decode_quints(packed, d);
        for (uint32_t j = 0; j < cnt; j++)
            out[i + j] = (uint8_t)(m[j] | (uint32_t)d[j] << r->bits);
    }
}

/** @brief The color value v of range r unquantized to 0 to 255. */
static uint32_t unquant_color(const ise_range *r, uint32_t v)
{
    if (!r->trit && !r->quint) {
        uint32_t out = 0;
        for (uint32_t pos = 8; pos > 0;) {
            uint32_t take = r->bits < pos ? r->bits : pos;
            out |= (v >> (r->bits - take)) << (pos - take);
            pos -= take;
        }
        return out;
    }
    uint32_t a = (v & 1) ? 0x1ff : 0, b = 0, c, d = v >> r->bits, bb = v >> 1 & 1, cc = v >> 2 & 1, dd = v >> 3 & 1,
             ee = v >> 4 & 1, ff = v >> 5 & 1;
    if (r->trit) {
        switch (r->bits) {
        case 1: c = 204; b = 0; break;
        case 2: c = 93; b = bb << 8 | bb << 4 | bb << 2 | bb << 1; break;
        case 3: c = 44; b = cc << 8 | bb << 7 | cc << 3 | bb << 2 | cc << 1 | bb; break;
        case 4: c = 22; b = dd << 8 | cc << 7 | bb << 6 | dd << 2 | cc << 1 | bb; break;
        case 5: c = 11; b = ee << 8 | dd << 7 | cc << 6 | bb << 5 | ee << 1 | dd; break;
        default: c = 5; b = ff << 8 | ee << 7 | dd << 6 | cc << 5 | bb << 4 | ff; break;
        }
    } else {
        switch (r->bits) {
        case 1: c = 113; b = 0; break;
        case 2: c = 54; b = bb << 8 | bb << 3 | bb << 2; break;
        case 3: c = 26; b = cc << 8 | bb << 7 | cc << 2 | bb << 1 | cc; break;
        case 4: c = 13; b = dd << 8 | cc << 7 | bb << 6 | dd << 1 | cc; break;
        default: c = 6; b = ee << 8 | dd << 7 | cc << 6 | bb << 5 | ee; break;
        }
    }
    uint32_t t = d * c + b;
    t ^= a;
    return (a & 0x80) | t >> 2;
}

/** @brief The weight value v of range r unquantized to 0 to 64. */
static uint32_t unquant_weight(const ise_range *r, uint32_t v)
{
    uint32_t w;
    if (!r->trit && !r->quint) {
        switch (r->bits) {
        case 1: w = v ? 63 : 0; break;
        case 2: w = v * 21; break;
        case 3: w = v * 9; break;
        case 4: w = v << 2 | v >> 2; break;
        default: w = v << 1 | v >> 4; break;
        }
    } else {
        uint32_t a = (v & 1) ? 0x7f : 0, b, c, d = v >> r->bits, bb = v >> 1 & 1, cc = v >> 2 & 1;
        if (r->trit) {
            switch (r->bits) {
            case 0: return d * 32;
            case 1: c = 50; b = 0; break;
            case 2: c = 23; b = bb << 6 | bb << 2 | bb; break;
            default: c = 11; b = cc << 6 | bb << 5 | cc << 1 | bb; break;
            }
        } else {
            switch (r->bits) {
            case 0: return d * 16;
            case 1: c = 28; b = 0; break;
            default: c = 13; b = bb << 6 | bb << 1; break;
            }
        }
        uint32_t t = d * c + b;
        t ^= a;
        w = (a & 0x20) | t >> 2;
    }
    return w > 32 ? w + 1 : w;
}

static uint32_t hash52(uint32_t p)
{
    p ^= p >> 15;
    p -= p << 17;
    p += p << 7;
    p += p << 4;
    p ^= p >> 5;
    p += p << 16;
    p ^= p >> 7;
    p ^= p >> 3;
    p ^= p << 6;
    p ^= p >> 17;
    return p;
}

static uint32_t select_partition(uint32_t seed, uint32_t x, uint32_t y, uint32_t parts, bool small)
{
    if (small) {
        x <<= 1;
        y <<= 1;
    }
    seed += (parts - 1) * 1024;
    uint32_t rnum = hash52(seed), s[12];
    for (uint32_t i = 0; i < 8; i++)
        s[i] = rnum >> (4 * i) & 0xf;
    s[8] = rnum >> 18 & 0xf;
    s[9] = rnum >> 22 & 0xf;
    s[10] = rnum >> 26 & 0xf;
    s[11] = (rnum >> 30 | rnum << 2) & 0xf;
    for (uint32_t i = 0; i < 12; i++)
        s[i] *= s[i];
    uint32_t sh1, sh2, sh3;
    if (seed & 1) {
        sh1 = seed & 2 ? 4 : 5;
        sh2 = parts == 3 ? 6 : 5;
    } else {
        sh1 = parts == 3 ? 6 : 5;
        sh2 = seed & 2 ? 4 : 5;
    }
    sh3 = seed & 0x10 ? sh1 : sh2;
    for (uint32_t i = 0; i < 8; i++)
        s[i] >>= i & 1 ? sh2 : sh1;
    for (uint32_t i = 8; i < 12; i++)
        s[i] >>= sh3;
    uint32_t a = (s[0] * x + s[1] * y + (rnum >> 14)) & 0x3f, b = (s[2] * x + s[3] * y + (rnum >> 10)) & 0x3f,
             c = (s[4] * x + s[5] * y + (rnum >> 6)) & 0x3f, d = (s[6] * x + s[7] * y + (rnum >> 2)) & 0x3f;
    if (parts < 4)
        d = 0;
    if (parts < 3)
        c = 0;
    if (a >= b && a >= c && a >= d)
        return 0;
    if (b >= c && b >= d)
        return 1;
    return c >= d ? 2 : 3;
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/** @brief Moves one bit of b into a (the sign bit position) as the spec's bit transfer does. */
static void bit_transfer(int *a, int *b)
{
    *b = *b >> 1 | (*a & 0x80);
    *a = *a >> 1 & 0x3f;
    if (*a & 0x20)
        *a -= 0x40;
}

static void blue_contract(int c[4])
{
    c[0] = (c[0] + c[2]) >> 1;
    c[1] = (c[1] + c[2]) >> 1;
}

/** @brief Decodes the endpoint pair of color endpoint mode mode from v; false for the HDR modes. */
static bool endpoints(uint32_t mode, const int *v, int e0[4], int e1[4])
{
    int a[4], b[4];
    switch (mode) {
    case 0:
        e0[0] = e0[1] = e0[2] = v[0];
        e1[0] = e1[1] = e1[2] = v[1];
        e0[3] = e1[3] = 255;
        return true;
    case 1: {
        int l0 = v[0] >> 2 | (v[1] & 0xc0), l1 = clamp255(l0 + (v[1] & 0x3f));
        e0[0] = e0[1] = e0[2] = l0;
        e1[0] = e1[1] = e1[2] = l1;
        e0[3] = e1[3] = 255;
        return true;
    }
    case 4:
        e0[0] = e0[1] = e0[2] = v[0];
        e1[0] = e1[1] = e1[2] = v[1];
        e0[3] = v[2];
        e1[3] = v[3];
        return true;
    case 5: {
        int v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3];
        bit_transfer(&v1, &v0);
        bit_transfer(&v3, &v2);
        e0[0] = e0[1] = e0[2] = v0;
        e1[0] = e1[1] = e1[2] = clamp255(v0 + v1);
        e0[3] = v2;
        e1[3] = clamp255(v2 + v3);
        return true;
    }
    case 6:
    case 10:
        e0[0] = v[0] * v[3] >> 8;
        e0[1] = v[1] * v[3] >> 8;
        e0[2] = v[2] * v[3] >> 8;
        e1[0] = v[0];
        e1[1] = v[1];
        e1[2] = v[2];
        e0[3] = e1[3] = 255;
        if (mode == 10) {
            e0[3] = v[4];
            e1[3] = v[5];
        }
        return true;
    case 8:
    case 12: {
        bool alpha = mode == 12;
        int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
        for (int i = 0; i < 3; i++) {
            a[i] = v[2 * i];
            b[i] = v[2 * i + 1];
        }
        a[3] = alpha ? v[6] : 255;
        b[3] = alpha ? v[7] : 255;
        if (s1 >= s0) {
            memcpy(e0, a, sizeof a);
            memcpy(e1, b, sizeof b);
        } else {
            blue_contract(b);
            blue_contract(a);
            memcpy(e0, b, sizeof b);
            memcpy(e1, a, sizeof a);
        }
        return true;
    }
    case 9:
    case 13: {
        bool alpha = mode == 13;
        int x[8];
        for (int i = 0; i < (alpha ? 8 : 6); i++)
            x[i] = v[i];
        for (int i = 0; i < (alpha ? 4 : 3); i++)
            bit_transfer(&x[2 * i + 1], &x[2 * i]);
        for (int i = 0; i < 3; i++) {
            a[i] = x[2 * i];
            b[i] = x[2 * i] + x[2 * i + 1];
        }
        a[3] = alpha ? x[6] : 255;
        b[3] = alpha ? x[6] + x[7] : 255;
        if (x[1] + x[3] + x[5] < 0) {
            blue_contract(a);
            blue_contract(b);
            for (int i = 0; i < 4; i++) {
                e0[i] = clamp255(b[i]);
                e1[i] = clamp255(a[i]);
            }
        } else {
            for (int i = 0; i < 4; i++) {
                e0[i] = clamp255(a[i]);
                e1[i] = clamp255(b[i]);
            }
        }
        return true;
    }
    default:
        return false;
    }
}

static void error_block(uint32_t bw, uint32_t bh, uint8_t *out)
{
    for (uint32_t i = 0; i < bw * bh; i++) {
        out[4 * i] = 255;
        out[4 * i + 1] = 0;
        out[4 * i + 2] = 255;
        out[4 * i + 3] = 255;
    }
}

void nvn_astc_block(uint32_t bw, uint32_t bh, bool srgb, const uint8_t blk[16], uint8_t *out)
{
    uint32_t mode = get_bits(blk, 0, 11);
    if ((mode & 0x1ff) == 0x1fc) {
        if (mode >> 9 & 1) {
            error_block(bw, bh, out);
            return;
        }
        uint32_t lo_s = get_bits(blk, 12, 13), hi_s = get_bits(blk, 25, 13), lo_t = get_bits(blk, 38, 13),
                 hi_t = get_bits(blk, 51, 13);
        bool all_ones = lo_s == 0x1fff && hi_s == 0x1fff && lo_t == 0x1fff && hi_t == 0x1fff;
        if (get_bits(blk, 10, 2) != 3 || ((lo_s >= hi_s || lo_t >= hi_t) && !all_ones)) {
            error_block(bw, bh, out);
            return;
        }
        uint8_t px[4];
        for (int i = 0; i < 4; i++) {
            uint32_t c = blk[8 + 2 * i] | (uint32_t)blk[9 + 2 * i] << 8;
            px[i] = (uint8_t)(c >> 8);
        }
        for (uint32_t i = 0; i < bw * bh; i++)
            memcpy(out + 4 * i, px, 4);
        return;
    }

    uint32_t wx, wy, rbase = mode >> 4 & 1, hbit = mode >> 9 & 1, dual = mode >> 10 & 1, a = mode >> 5 & 3;
    if (mode & 3) {
        uint32_t b = mode >> 7 & 3;
        rbase |= (mode & 3) << 1;
        switch (mode >> 2 & 3) {
        case 0: wx = b + 4; wy = a + 2; break;
        case 1: wx = b + 8; wy = a + 2; break;
        case 2: wx = a + 2; wy = b + 8; break;
        default:
            b &= 1;
            if (mode & 0x100) {
                wx = b + 2;
                wy = a + 2;
            } else {
                wx = a + 2;
                wy = b + 6;
            }
            break;
        }
    } else {
        rbase |= (mode >> 2 & 3) << 1;
        if ((mode >> 2 & 3) == 0) {
            error_block(bw, bh, out);
            return;
        }
        uint32_t b = mode >> 9 & 3;
        switch (mode >> 7 & 3) {
        case 0: wx = 12; wy = a + 2; break;
        case 1: wx = a + 2; wy = 12; break;
        case 2:
            wx = a + 6;
            wy = b + 6;
            dual = 0;
            hbit = 0;
            break;
        default:
            if (a == 0) {
                wx = 6;
                wy = 10;
            } else if (a == 1) {
                wx = 10;
                wy = 6;
            } else {
                error_block(bw, bh, out);
                return;
            }
            break;
        }
    }
    if (rbase < 2) {
        error_block(bw, bh, out);
        return;
    }
    uint32_t nweights = wx * wy * (1 + dual);
    static const uint8_t wmax_lo[6] = { 1, 2, 3, 4, 5, 7 }, wmax_hi[6] = { 9, 11, 15, 19, 23, 31 };
    if (rbase > 7) {
        error_block(bw, bh, out);
        return;
    }
    uint32_t wmax = hbit ? wmax_hi[rbase - 2] : wmax_lo[rbase - 2];
    const ise_range *wr = &ranges[0];
    for (int i = 0; i < NRANGES; i++)
        if (ranges[i].max == wmax)
            wr = &ranges[i];
    uint32_t wbits = ise_bits(wr, nweights);
    if (nweights > 64 || wbits < 24 || wbits > 96 || wx > bw || wy > bh) {
        error_block(bw, bh, out);
        return;
    }

    uint32_t parts = get_bits(blk, 11, 2) + 1, pos = 13;
    if (dual && parts == 4) {
        error_block(bw, bh, out);
        return;
    }
    uint32_t seed = 0;
    if (parts > 1) {
        seed = get_bits(blk, 13, 10);
    }
    uint32_t cems[MAX_PARTS], cfg_bits, extra = 0;
    if (parts == 1) {
        cems[0] = get_bits(blk, pos, 4);
        cfg_bits = 17;
    } else {
        uint32_t sel = get_bits(blk, 23, 2);
        if (sel == 0) {
            uint32_t m = get_bits(blk, 25, 4);
            for (uint32_t i = 0; i < parts; i++)
                cems[i] = m;
            cfg_bits = 29;
        } else {
            extra = 3 * parts - 4;
            uint32_t enc = get_bits(blk, 23, 6) | get_bits(blk, 128 - wbits - extra, extra) << 6, cls = (enc & 3) - 1;
            enc >>= 2;
            for (uint32_t i = 0; i < parts; i++)
                cems[i] = (((enc >> i & 1) + cls) << 2) | (enc >> (parts + 2 * i) & 3);
            cfg_bits = 29;
        }
    }
    uint32_t cplane = 0;
    if (dual) {
        uint32_t at = 128 - wbits - extra - 2;
        cplane = get_bits(blk, at, 2);
        extra += 2;
    }
    uint32_t ncolor = 0;
    for (uint32_t i = 0; i < parts; i++)
        ncolor += ((cems[i] >> 2) + 1) * 2;
    if (128 < wbits + extra + cfg_bits || ncolor > 18) {
        error_block(bw, bh, out);
        return;
    }
    uint32_t cbits = 128 - wbits - extra - cfg_bits;
    int cq = -1;
    for (int i = NRANGES - 1; i >= 0; i--)
        if (ise_bits(&ranges[i], ncolor) <= cbits) {
            cq = i;
            break;
        }
    if (cq < 4) {
        error_block(bw, bh, out);
        return;
    }
    uint8_t cvals[32];
    ise_decode(blk, cfg_bits, &ranges[cq], ncolor, cvals);

    int e0[MAX_PARTS][4], e1[MAX_PARTS][4];
    uint32_t used = 0;
    for (uint32_t i = 0; i < parts; i++) {
        int v[8] = { 0 };
        uint32_t n = ((cems[i] >> 2) + 1) * 2;
        for (uint32_t j = 0; j < n; j++)
            v[j] = (int)unquant_color(&ranges[cq], cvals[used + j]);
        used += n;
        if (!endpoints(cems[i], v, e0[i], e1[i])) {
            static const int magenta[4] = { 255, 0, 255, 255 };
            memcpy(e0[i], magenta, sizeof magenta);
            memcpy(e1[i], magenta, sizeof magenta);
        }
    }

    uint8_t rev[16], wvals[128];
    for (int i = 0; i < 16; i++) {
        uint8_t x = blk[15 - i];
        x = (uint8_t)((x & 0xf0) >> 4 | (x & 0x0f) << 4);
        x = (uint8_t)((x & 0xcc) >> 2 | (x & 0x33) << 2);
        rev[i] = (uint8_t)((x & 0xaa) >> 1 | (x & 0x55) << 1);
    }
    ise_decode(rev, 0, wr, nweights, wvals);
    int w[2][64];
    for (uint32_t i = 0; i < wx * wy; i++)
        for (uint32_t p = 0; p < 1 + dual; p++)
            w[p][i] = (int)unquant_weight(wr, wvals[i * (1 + dual) + p]);

    uint32_t ds = (1024 + bw / 2) / (bw - 1), dt = (1024 + bh / 2) / (bh - 1);
    bool small = bw * bh < 31;
    for (uint32_t t = 0; t < bh; t++)
        for (uint32_t s = 0; s < bw; s++) {
            uint32_t gs = (ds * s * (wx - 1) + 32) >> 6, gt = (dt * t * (wy - 1) + 32) >> 6;
            uint32_t js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
            uint32_t w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            int wt[2];
            for (uint32_t p = 0; p < 1 + dual; p++) {
                uint32_t i00 = js + jt * wx, i01 = i00 + (js + 1 < wx ? 1 : 0), i10 = i00 + (jt + 1 < wy ? wx : 0),
                         i11 = i10 + (js + 1 < wx ? 1 : 0);
                wt[p] = (int)((uint32_t)(w[p][i00] * (int)w00 + w[p][i01] * (int)w01 + w[p][i10] * (int)w10 +
                                         w[p][i11] * (int)w11 + 8) >>
                              4);
            }
            uint32_t part = parts > 1 ? select_partition(seed, s, t, parts, small) : 0;
            for (uint32_t c = 0; c < 4; c++) {
                int wc = dual && c == cplane ? wt[1] : wt[0];
                int c0 = srgb ? e0[part][c] << 8 | 0x80 : e0[part][c] * 257;
                int c1 = srgb ? e1[part][c] << 8 | 0x80 : e1[part][c] * 257;
                uint32_t v = (uint32_t)((c0 * (64 - wc) + c1 * wc + 32) >> 6);
                out[4 * (t * bw + s) + c] = (uint8_t)(v >> 8);
            }
        }
}

void nvn_astc_image(uint32_t bw, uint32_t bh, bool srgb, uint32_t w, uint32_t h, const uint8_t *blocks, uint8_t *rgba)
{
    uint32_t nbx = (w + bw - 1) / bw, nby = (h + bh - 1) / bh;
    uint8_t texels[MAX_TEXELS * 4];
    for (uint32_t by = 0; by < nby; by++)
        for (uint32_t bx = 0; bx < nbx; bx++) {
            nvn_astc_block(bw, bh, srgb, blocks + 16 * (by * nbx + bx), texels);
            for (uint32_t t = 0; t < bh && by * bh + t < h; t++)
                for (uint32_t s = 0; s < bw && bx * bw + s < w; s++)
                    memcpy(rgba + 4 * ((by * bh + t) * w + bx * bw + s), texels + 4 * (t * bw + s), 4);
        }
}
