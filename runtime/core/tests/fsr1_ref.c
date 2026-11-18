/**
 * @file fsr1_ref.c
 * @brief A C port of FidelityFX FSR 1's EASU and RCAS (ffx_fsr1.h, MIT, runtime/third_party/fsr1) in 32-bit floats,
 * with the header's bit-trick approximations, for the tests of the FSR 1 stage: the GPU pass includes the header
 * itself, so this port checks the stage's plumbing (gathers, rectangles, the half-float buffer between the passes).
 */
#include "fsr1_ref.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float from_bits(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static float rcp_lo(float a) { return from_bits(0x7ef07ebbu - bits(a)); }

static float rsq_lo(float a) { return from_bits(0x5f347d74u - (bits(a) >> 1)); }

static float rcp_med(float a)
{
    float b = from_bits(0x7ef19fffu - bits(a));
    return b * (-b * a + 2.0f);
}

static float sat(float a) { return a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a; }

static float min3(float a, float b, float c) { return fminf(a, fminf(b, c)); }

static float max3(float a, float b, float c) { return fmaxf(a, fmaxf(b, c)); }

/** @brief f rounded to the nearest half-precision value, as packHalf2x16 and unpackHalf2x16 leave it. */
static float half_round(float f)
{
    if (!(fabsf(f) < 65520.0f))
        return f;
    int e;
    frexpf(f, &e);
    if (e < -13)
        e = -13;
    float q = ldexpf(1.0f, e - 11);
    return rintf(f / q) * q;
}

typedef struct img {
    const uint32_t *px;
    uint32_t pitch;
    const rl_upscale_rect *r;
} img;

static void load(const img *im, int x, int y, float c[3])
{
    x = x < 0 ? 0 : x >= (int)im->r->width ? (int)im->r->width - 1 : x;
    y = y < 0 ? 0 : y >= (int)im->r->height ? (int)im->r->height - 1 : y;
    uint32_t w = im->px[(im->r->y + (uint32_t)y) * im->pitch + im->r->x + (uint32_t)x];
    for (int i = 0; i < 3; i++)
        c[i] = (float)((w >> (8 * i)) & 0xff) / 255.0f;
}

static float luma2(const float c[3]) { return c[2] * 0.5f + (c[0] * 0.5f + c[1]); }

static void easu_set(float dir[2], float *len, float px, float py, int which, float lA, float lB, float lC, float lD,
                     float lE)
{
    float w = which == 0 ? (1.0f - px) * (1.0f - py) : which == 1 ? px * (1.0f - py) : which == 2 ? (1.0f - px) * py : px * py;
    float dc = lD - lC, cb = lC - lB;
    float lenX = rcp_lo(fmaxf(fabsf(dc), fabsf(cb)));
    float dirX = lD - lB;
    dir[0] += dirX * w;
    lenX = sat(fabsf(dirX) * lenX);
    lenX *= lenX;
    *len += lenX * w;
    float ec = lE - lC, ca = lC - lA;
    float lenY = rcp_lo(fmaxf(fabsf(ec), fabsf(ca)));
    float dirY = lE - lA;
    dir[1] += dirY * w;
    lenY = sat(fabsf(dirY) * lenY);
    lenY *= lenY;
    *len += lenY * w;
}

static void easu_tap(float aC[3], float *aW, float ox, float oy, const float dir[2], const float len[2], float lob,
                     float clp, const float c[3])
{
    float vx = ox * dir[0] + oy * dir[1];
    float vy = ox * -dir[1] + oy * dir[0];
    vx *= len[0];
    vy *= len[1];
    float d2 = fminf(vx * vx + vy * vy, clp);
    float wB = 2.0f / 5.0f * d2 - 1.0f;
    float wA = lob * d2 - 1.0f;
    wB *= wB;
    wA *= wA;
    wB = 25.0f / 16.0f * wB - (25.0f / 16.0f - 1.0f);
    float w = wB * wA;
    for (int i = 0; i < 3; i++)
        aC[i] += c[i] * w;
    *aW += w;
}

static void easu(const img *in, uint32_t ow, uint32_t oh, uint32_t x, uint32_t y, float out[3])
{
    float sx = (float)in->r->width * (1.0f / (float)ow), sy = (float)in->r->height * (1.0f / (float)oh);
    float ppx = (float)x * sx + (0.5f * (float)in->r->width * (1.0f / (float)ow) - 0.5f);
    float ppy = (float)y * sy + (0.5f * (float)in->r->height * (1.0f / (float)oh) - 0.5f);
    float fx = floorf(ppx), fy = floorf(ppy);
    ppx -= fx;
    ppy -= fy;
    int bx = (int)fx, by = (int)fy;
    static const int tap[12][2] = { { 0, -1 }, { 1, -1 }, { -1, 1 }, { 0, 1 }, { 0, 0 }, { -1, 0 },
                                    { 1, 1 },  { 2, 1 },  { 2, 0 },  { 1, 0 }, { 1, 2 }, { 0, 2 } };
    enum { B, C, I, J, F, E, K, L, H, G, O, N };
    float c[12][3], l[12];
    for (int i = 0; i < 12; i++) {
        load(in, bx + tap[i][0], by + tap[i][1], c[i]);
        l[i] = luma2(c[i]);
    }
    float dir[2] = { 0, 0 }, len = 0;
    easu_set(dir, &len, ppx, ppy, 0, l[B], l[E], l[F], l[G], l[J]);
    easu_set(dir, &len, ppx, ppy, 1, l[C], l[F], l[G], l[H], l[K]);
    easu_set(dir, &len, ppx, ppy, 2, l[F], l[I], l[J], l[K], l[N]);
    easu_set(dir, &len, ppx, ppy, 3, l[G], l[J], l[K], l[L], l[O]);
    float dirR = dir[0] * dir[0] + dir[1] * dir[1];
    int zro = dirR < 1.0f / 32768.0f;
    dirR = rsq_lo(dirR);
    dirR = zro ? 1.0f : dirR;
    dir[0] = zro ? 1.0f : dir[0];
    dir[0] *= dirR;
    dir[1] *= dirR;
    len = len * 0.5f;
    len *= len;
    float stretch = (dir[0] * dir[0] + dir[1] * dir[1]) * rcp_lo(fmaxf(fabsf(dir[0]), fabsf(dir[1])));
    float len2[2] = { 1.0f + (stretch - 1.0f) * len, 1.0f + -0.5f * len };
    float lob = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * len;
    float clp = rcp_lo(lob);
    float mn[3], mx[3];
    for (int i = 0; i < 3; i++) {
        mn[i] = fminf(min3(c[F][i], c[G][i], c[J][i]), c[K][i]);
        mx[i] = fmaxf(max3(c[F][i], c[G][i], c[J][i]), c[K][i]);
    }
    float aC[3] = { 0, 0, 0 }, aW = 0;
    for (int i = 0; i < 12; i++)
        easu_tap(aC, &aW, (float)tap[i][0] - ppx, (float)tap[i][1] - ppy, dir, len2, lob, clp, c[i]);
    for (int i = 0; i < 3; i++)
        out[i] = fminf(mx[i], fmaxf(mn[i], aC[i] * (1.0f / aW)));
}

static void rcas(const float *work, uint32_t ow, uint32_t oh, int x, int y, float con, float out[3])
{
    const float *p[5];
    static const int off[5][2] = { { 0, -1 }, { -1, 0 }, { 0, 0 }, { 1, 0 }, { 0, 1 } };
    for (int i = 0; i < 5; i++) {
        int qx = x + off[i][0], qy = y + off[i][1];
        qx = qx < 0 ? 0 : qx >= (int)ow ? (int)ow - 1 : qx;
        qy = qy < 0 ? 0 : qy >= (int)oh ? (int)oh - 1 : qy;
        p[i] = work + ((size_t)qy * ow + (size_t)qx) * 3;
    }
    const float *b = p[0], *d = p[1], *e = p[2], *f = p[3], *h = p[4];
    float lobe3[3];
    for (int i = 0; i < 3; i++) {
        float mn4 = fminf(min3(b[i], d[i], f[i]), h[i]), mx4 = fmaxf(max3(b[i], d[i], f[i]), h[i]);
        float hitMin = fminf(mn4, e[i]) * (1.0f / (4.0f * mx4));
        float hitMax = (1.0f - fmaxf(mx4, e[i])) * (1.0f / (4.0f * mn4 + -4.0f));
        lobe3[i] = fmaxf(-hitMin, hitMax);
    }
    float lobe = fmaxf(-(0.25f - 1.0f / 16.0f), fminf(max3(lobe3[0], lobe3[1], lobe3[2]), 0.0f)) * con;
    float rcpL = rcp_med(4.0f * lobe + 1.0f);
    for (int i = 0; i < 3; i++)
        out[i] = (lobe * b[i] + lobe * d[i] + lobe * h[i] + lobe * f[i] + e[i]) * rcpL;
}

void fsr1_ref(const uint32_t *src, uint32_t sw, const rl_upscale_rect *in, uint32_t *dst, uint32_t dw,
              const rl_upscale_rect *out, float stops)
{
    img im = { src, sw, in };
    float *work = malloc((size_t)out->width * out->height * 3 * sizeof(float));
    if (!work)
        return;
    for (uint32_t y = 0; y < out->height; y++)
        for (uint32_t x = 0; x < out->width; x++) {
            float *c = work + ((size_t)y * out->width + x) * 3;
            easu(&im, out->width, out->height, x, y, c);
            for (int i = 0; i < 3; i++)
                c[i] = half_round(c[i]);
        }
    float con = exp2f(-stops);
    for (uint32_t y = 0; y < out->height; y++)
        for (uint32_t x = 0; x < out->width; x++) {
            float c[3];
            rcas(work, out->width, out->height, (int)x, (int)y, con, c);
            uint32_t w = 0xff000000u;
            for (int i = 0; i < 3; i++)
                w |= (uint32_t)lrintf(sat(c[i]) * 255.0f) << (8 * i);
            dst[(out->y + y) * dw + out->x + x] = w;
        }
    free(work);
}
