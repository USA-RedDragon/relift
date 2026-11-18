/**
 * @file fxaa_ref.c
 * @brief The reference the FXAA pass is checked against: FXAA 3.11 Quality,
 * PC version, at preset 39, from NVIDIA's FXAA3_11.h by Timothy Lottes
 * (BSD-3-Clause; provenance and license text in runtime/third_party/fxaa),
 * ported to C on the CPU in pixel units: luma is the Rec. 601 weighting of
 * the color, computed once per pixel, and every texture fetch is a bilinear
 * sample with clamped edges. The nested edge search steps are one loop over
 * the preset's step table. runtime/core/shaders/fxaa.comp is the same
 * arithmetic line by line.
 */
#include "fxaa_ref.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FXAA_SUBPIX 0.75f
#define FXAA_EDGE_THRESHOLD 0.166f
#define FXAA_EDGE_THRESHOLD_MIN 0.0833f

static const float g_steps[] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.5f, 2.0f, 2.0f, 2.0f, 2.0f, 4.0f, 8.0f};
#define NSTEPS (sizeof g_steps / sizeof g_steps[0])

typedef struct img {
    const uint8_t *rgb;
    const float *luma;
    int w, h;
} img;

static int clampi(int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; }

static float luma_at(const img *m, int x, int y)
{
    return m->luma[(size_t)clampi(y, m->h - 1) * (size_t)m->w + (size_t)clampi(x, m->w - 1)];
}

/** @brief The corner and the weights of a bilinear sample at (x, y) in pixel units, pixel centers at .5. */
static void corners(float x, float y, int *x0, int *y0, float *fx, float *fy)
{
    float u = x - 0.5f, v = y - 0.5f;
    float ux = floorf(u), vy = floorf(v);
    *x0 = (int)ux;
    *y0 = (int)vy;
    *fx = u - ux;
    *fy = v - vy;
}

static float luma_lerp(const img *m, float x, float y)
{
    int x0, y0;
    float fx, fy;
    corners(x, y, &x0, &y0, &fx, &fy);
    float a = luma_at(m, x0, y0), b = luma_at(m, x0 + 1, y0);
    float c = luma_at(m, x0, y0 + 1), d = luma_at(m, x0 + 1, y0 + 1);
    float top = a + (b - a) * fx, bot = c + (d - c) * fx;
    return top + (bot - top) * fy;
}

static void rgb_lerp(const img *m, float x, float y, uint8_t *out)
{
    int x0, y0;
    float fx, fy;
    corners(x, y, &x0, &y0, &fx, &fy);
    const uint8_t *p[4];
    for (int i = 0; i < 4; i++) {
        int cx = clampi(x0 + (i & 1), m->w - 1), cy = clampi(y0 + (i >> 1), m->h - 1);
        p[i] = m->rgb + ((size_t)cy * (size_t)m->w + (size_t)cx) * 3;
    }
    for (int k = 0; k < 3; k++) {
        float top = p[0][k] + (p[1][k] - p[0][k]) * fx;
        float bot = p[2][k] + (p[3][k] - p[2][k]) * fx;
        float v = top + (bot - top) * fy + 0.5f;
        out[k] = (uint8_t)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v);
    }
}

static void fxaa_pixel(const img *m, int px, int py, uint8_t *out)
{
    float mx = (float)px + 0.5f, my = (float)py + 0.5f;
    float lumaM = luma_at(m, px, py);
    float lumaS = luma_at(m, px, py + 1), lumaE = luma_at(m, px + 1, py);
    float lumaN = luma_at(m, px, py - 1), lumaW = luma_at(m, px - 1, py);
    float rangeMax = fmaxf(fmaxf(lumaN, lumaW), fmaxf(lumaE, fmaxf(lumaS, lumaM)));
    float rangeMin = fminf(fminf(lumaN, lumaW), fminf(lumaE, fminf(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    if (range < fmaxf(FXAA_EDGE_THRESHOLD_MIN, rangeMax * FXAA_EDGE_THRESHOLD)) {
        memcpy(out, m->rgb + ((size_t)py * (size_t)m->w + (size_t)px) * 3, 3);
        return;
    }
    float lumaNW = luma_at(m, px - 1, py - 1), lumaSE = luma_at(m, px + 1, py + 1);
    float lumaNE = luma_at(m, px + 1, py - 1), lumaSW = luma_at(m, px - 1, py + 1);

    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0f / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0f * lumaM + lumaNS, edgeVert1 = -2.0f * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = -2.0f * lumaE + lumaNESE, edgeVert2 = -2.0f * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = fabsf(edgeHorz1) * 2.0f + fabsf(edgeHorz2);
    float edgeVert4 = fabsf(edgeVert1) * 2.0f + fabsf(edgeVert2);
    float edgeHorz3 = -2.0f * lumaW + lumaNWSW, edgeVert3 = -2.0f * lumaS + lumaSWSE;
    float edgeHorz = fabsf(edgeHorz3) + edgeHorz4, edgeVert = fabsf(edgeVert3) + edgeVert4;

    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = 1.0f;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0f + subpixNWSWNESE;
    if (!horzSpan) {
        lumaN = lumaW;
        lumaS = lumaE;
    }
    float subpixB = subpixA * (1.0f / 12.0f) - lumaM;
    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;
    bool pairN = fabsf(gradientN) >= fabsf(gradientS);
    float gradient = fmaxf(fabsf(gradientN), fabsf(gradientS));
    if (pairN)
        lengthSign = -lengthSign;
    float subpixC = fminf(fmaxf(fabsf(subpixB) * subpixRcpRange, 0.0f), 1.0f);

    float bx = mx, by = my;
    float ox = horzSpan ? 1.0f : 0.0f, oy = horzSpan ? 0.0f : 1.0f;
    if (!horzSpan)
        bx += lengthSign * 0.5f;
    else
        by += lengthSign * 0.5f;
    float nx = bx - ox * g_steps[0], ny = by - oy * g_steps[0];
    float qx = bx + ox * g_steps[0], qy = by + oy * g_steps[0];
    float subpixD = -2.0f * subpixC + 3.0f;
    float lumaEndN = luma_lerp(m, nx, ny);
    float subpixE = subpixC * subpixC;
    float lumaEndP = luma_lerp(m, qx, qy);
    if (!pairN)
        lumaNN = lumaSS;
    float gradientScaled = gradient * 1.0f / 4.0f;
    float lumaMM = lumaM - lumaNN * 0.5f;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0f;
    lumaEndN -= lumaNN * 0.5f;
    lumaEndP -= lumaNN * 0.5f;
    bool doneN = fabsf(lumaEndN) >= gradientScaled, doneP = fabsf(lumaEndP) >= gradientScaled;
    if (!doneN) {
        nx -= ox * g_steps[1];
        ny -= oy * g_steps[1];
    }
    if (!doneP) {
        qx += ox * g_steps[1];
        qy += oy * g_steps[1];
    }
    for (size_t i = 2; i < NSTEPS && (!doneN || !doneP); i++) {
        if (!doneN)
            lumaEndN = luma_lerp(m, nx, ny) - lumaNN * 0.5f;
        if (!doneP)
            lumaEndP = luma_lerp(m, qx, qy) - lumaNN * 0.5f;
        doneN = fabsf(lumaEndN) >= gradientScaled;
        doneP = fabsf(lumaEndP) >= gradientScaled;
        if (!doneN) {
            nx -= ox * g_steps[i];
            ny -= oy * g_steps[i];
        }
        if (!doneP) {
            qx += ox * g_steps[i];
            qy += oy * g_steps[i];
        }
    }
    float dstN = horzSpan ? mx - nx : my - ny;
    float dstP = horzSpan ? qx - mx : qy - my;
    bool goodSpanN = (lumaEndN < 0.0f) != lumaMLTZero;
    bool goodSpanP = (lumaEndP < 0.0f) != lumaMLTZero;
    float spanLengthRcp = 1.0f / (dstP + dstN);
    bool goodSpan = dstN < dstP ? goodSpanN : goodSpanP;
    float pixelOffset = fminf(dstN, dstP) * -spanLengthRcp + 0.5f;
    float subpixH = subpixF * subpixF * FXAA_SUBPIX;
    float off = fmaxf(goodSpan ? pixelOffset : 0.0f, subpixH);
    if (!horzSpan)
        mx += off * lengthSign;
    else
        my += off * lengthSign;
    rgb_lerp(m, mx, my, out);
}

bool fxaa_ref(const uint8_t *in, uint8_t *out, uint32_t width, uint32_t height)
{
    size_t n = (size_t)width * height;
    float *luma = malloc(n * sizeof *luma);
    if (!luma) {
        memcpy(out, in, n * 3);
        return false;
    }
    for (size_t i = 0; i < n; i++)
        luma[i] = (0.299f * in[3 * i] + 0.587f * in[3 * i + 1] + 0.114f * in[3 * i + 2]) * (1.0f / 255.0f);
    img m = {.rgb = in, .luma = luma, .w = (int)width, .h = (int)height};
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
            fxaa_pixel(&m, (int)x, (int)y, out + ((size_t)y * width + x) * 3);
    free(luma);
    return true;
}
