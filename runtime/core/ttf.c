/**
 * @file ttf.c
 * @brief The TrueType reader and rasterizer (ttf.h). Table layouts follow the
 * OpenType specification. Outlines are flattened into lines whose signed
 * area is accumulated per pixel and summed along each row, the exact-coverage
 * method of font-rs (Raph Levien, Apache-2.0 or MIT), written anew here.
 */
#include "ttf.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define COMPOSITE_DEPTH 8
#define POINTS_MAX 4096

static uint16_t u16(const rl_ttf *f, size_t o) { return o + 2 <= f->n ? (uint16_t)(f->d[o] << 8 | f->d[o + 1]) : 0; }
static int16_t s16(const rl_ttf *f, size_t o) { return (int16_t)u16(f, o); }
static uint32_t u32(const rl_ttf *f, size_t o) { return (uint32_t)u16(f, o) << 16 | u16(f, o + 2); }

static uint32_t table(const rl_ttf *f, const char tag[4])
{
    uint16_t count = u16(f, 4);
    for (uint32_t i = 0; i < count; i++) {
        size_t rec = 12 + (size_t)i * 16;
        if (rec + 16 > f->n)
            return 0;
        if (!memcmp(f->d + rec, tag, 4)) {
            uint32_t off = u32(f, rec + 8), len = u32(f, rec + 12);
            return (size_t)off + len <= f->n ? off : 0;
        }
    }
    return 0;
}

bool rl_ttf_init(rl_ttf *f, const uint8_t *data, size_t n)
{
    memset(f, 0, sizeof *f);
    f->d = data;
    f->n = n;
    if (n < 12 || u32(f, 0) != 0x00010000u)
        return false;
    uint32_t head = table(f, "head"), hhea = table(f, "hhea"), maxp = table(f, "maxp"), cmap = table(f, "cmap");
    f->glyf = table(f, "glyf");
    f->loca = table(f, "loca");
    f->hmtx = table(f, "hmtx");
    if (!head || !hhea || !maxp || !cmap || !f->glyf || !f->loca || !f->hmtx)
        return false;
    f->upem = u16(f, head + 18);
    f->loca_long = s16(f, head + 50) != 0;
    f->nglyphs = u16(f, maxp + 4);
    f->ascent = s16(f, hhea + 4);
    f->descent = s16(f, hhea + 6);
    f->nmetrics = u16(f, hhea + 34);
    uint16_t subs = u16(f, cmap + 2);
    uint32_t best = 0;
    int rank = 0;
    for (uint32_t i = 0; i < subs; i++) {
        size_t rec = cmap + 4 + (size_t)i * 8;
        uint16_t plat = u16(f, rec), enc = u16(f, rec + 2);
        uint32_t off = cmap + u32(f, rec + 4);
        uint16_t fmt = u16(f, off);
        int r = 0;
        if (fmt == 12 && (plat == 3 || plat == 0))
            r = 3;
        else if (fmt == 4 && ((plat == 3 && (enc == 1 || enc == 10)) || plat == 0))
            r = 2;
        if (r > rank) {
            rank = r;
            best = off;
            f->cmap_format = fmt;
        }
    }
    f->cmap = best;
    return f->upem && f->nglyphs && f->nmetrics && best;
}

uint32_t rl_ttf_glyph(const rl_ttf *f, uint32_t cp)
{
    size_t c = f->cmap;
    if (f->cmap_format == 12) {
        uint32_t groups = u32(f, c + 12);
        uint32_t lo = 0, hi = groups;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            size_t g = c + 16 + (size_t)mid * 12;
            uint32_t start = u32(f, g), end = u32(f, g + 4);
            if (cp < start)
                hi = mid;
            else if (cp > end)
                lo = mid + 1;
            else
                return u32(f, g + 8) + (cp - start);
        }
        return 0;
    }
    if (cp > 0xffff)
        return 0;
    uint16_t segs = u16(f, c + 6) / 2;
    size_t ends = c + 14, starts = ends + (size_t)segs * 2 + 2, deltas = starts + (size_t)segs * 2,
           ranges = deltas + (size_t)segs * 2;
    for (uint32_t i = 0; i < segs; i++) {
        if (cp > u16(f, ends + i * 2))
            continue;
        uint16_t start = u16(f, starts + i * 2);
        if (cp < start)
            return 0;
        uint16_t delta = u16(f, deltas + i * 2), range = u16(f, ranges + i * 2);
        if (!range)
            return (uint16_t)(cp + delta);
        uint16_t g = u16(f, ranges + i * 2 + range + (cp - start) * 2);
        return g ? (uint16_t)(g + delta) : 0;
    }
    return 0;
}

float rl_ttf_advance(const rl_ttf *f, uint32_t g, float px)
{
    uint32_t i = g < f->nmetrics ? g : f->nmetrics - 1u;
    return (float)u16(f, f->hmtx + (size_t)i * 4) * px / (float)f->upem;
}

void rl_ttf_metrics(const rl_ttf *f, float px, float *ascent, float *descent)
{
    *ascent = (float)f->ascent * px / (float)f->upem;
    *descent = (float)f->descent * px / (float)f->upem;
}

/** @brief The bytes of glyph g: offset and length in the font, false for none. */
static bool glyph_span(const rl_ttf *f, uint32_t g, size_t *off, size_t *len)
{
    if (g >= f->nglyphs)
        return false;
    size_t a, b;
    if (f->loca_long) {
        a = u32(f, f->loca + (size_t)g * 4);
        b = u32(f, f->loca + (size_t)g * 4 + 4);
    } else {
        a = (size_t)u16(f, f->loca + (size_t)g * 2) * 2;
        b = (size_t)u16(f, f->loca + (size_t)g * 2 + 2) * 2;
    }
    if (b <= a || f->glyf + b > f->n)
        return false;
    *off = f->glyf + a;
    *len = b - a;
    return true;
}

/** @brief Where lines go: an accumulation buffer of w by h cells, with a transform from font units. */
typedef struct sink {
    float *acc;
    int32_t w, h;
    float m[6];
} sink;

static void acc_line(sink *s, float x0, float y0, float x1, float y1)
{
    if (y0 == y1)
        return;
    float dir = 1;
    if (y0 > y1) {
        dir = -1;
        float t = x0;
        x0 = x1;
        x1 = t;
        t = y0;
        y0 = y1;
        y1 = t;
    }
    float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    if (y0 < 0) {
        x -= y0 * dxdy;
        y0 = 0;
    }
    int32_t yend = (int32_t)ceilf(y1);
    if (yend > s->h)
        yend = s->h;
    for (int32_t y = (int32_t)y0; y < yend; y++) {
        float *row = s->acc + (size_t)y * (size_t)s->w;
        float dy = fminf((float)y + 1, y1) - fmaxf((float)y, y0);
        float xnext = x + dxdy * dy;
        float d = dy * dir;
        float xa = fminf(x, xnext), xb = fmaxf(x, xnext);
        xa = fmaxf(xa, 0);
        xb = fminf(xb, (float)s->w - 2);
        if (xa > xb)
            xa = xb;
        float xaf = floorf(xa);
        int32_t ia = (int32_t)xaf;
        int32_t ib = (int32_t)ceilf(xb);
        if (ib <= ia + 1) {
            float xmf = 0.5f * (x + xnext) - xaf;
            xmf = fminf(fmaxf(xmf, 0), 1);
            row[ia] += d - d * xmf;
            row[ia + 1] += d * xmf;
        } else {
            float sc = 1.0f / (xb - xa);
            float x0f = xa - xaf;
            float a0 = 0.5f * sc * (1 - x0f) * (1 - x0f);
            float x1f = xb - (float)ib + 1;
            float am = 0.5f * sc * x1f * x1f;
            row[ia] += d * a0;
            if (ib == ia + 2) {
                row[ia + 1] += d * (1 - a0 - am);
            } else {
                float a1 = sc * (1.5f - x0f);
                row[ia + 1] += d * (a1 - a0);
                for (int32_t xi = ia + 2; xi < ib - 1; xi++)
                    row[xi] += d * sc;
                float a2 = a1 + (float)(ib - ia - 3) * sc;
                row[ib - 1] += d * (1 - a2 - am);
            }
            row[ib] += d * am;
        }
        x = xnext;
    }
}

static void map(const sink *s, float x, float y, float *ox, float *oy)
{
    *ox = s->m[0] * x + s->m[2] * y + s->m[4];
    *oy = s->m[1] * x + s->m[3] * y + s->m[5];
}

static void line(sink *s, float x0, float y0, float x1, float y1)
{
    float a, b, c, d;
    map(s, x0, y0, &a, &b);
    map(s, x1, y1, &c, &d);
    acc_line(s, a, b, c, d);
}

static void quad(sink *s, float x0, float y0, float cx, float cy, float x1, float y1)
{
    float a, b, c, d, e, g;
    map(s, x0, y0, &a, &b);
    map(s, cx, cy, &c, &d);
    map(s, x1, y1, &e, &g);
    float ddx = a - 2 * c + e, ddy = b - 2 * d + g;
    int n = 1 + (int)sqrtf(sqrtf(ddx * ddx + ddy * ddy) * 3);
    if (n > 16)
        n = 16;
    float px = a, py = b;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1 - t;
        float qx = u * u * a + 2 * u * t * c + t * t * e, qy = u * u * b + 2 * u * t * d + t * t * g;
        acc_line(s, px, py, qx, qy);
        px = qx;
        py = qy;
    }
}

/** @brief Draws the contours of a simple glyph at off. */
static bool simple(const rl_ttf *f, size_t off, int16_t ncont, sink *s)
{
    size_t ends = off + 10;
    uint16_t npts = (uint16_t)(u16(f, ends + (size_t)(ncont - 1) * 2) + 1);
    if (npts > POINTS_MAX)
        return false;
    size_t p = ends + (size_t)ncont * 2;
    p += 2 + u16(f, p);
    uint8_t *fl = malloc(npts);
    float *xs = malloc(npts * sizeof *xs), *ys = malloc(npts * sizeof *ys);
    bool ok = fl && xs && ys;
    for (uint32_t i = 0; ok && i < npts;) {
        if (p >= f->n) {
            ok = false;
            break;
        }
        uint8_t v = f->d[p++];
        uint32_t rep = 1;
        if (v & 8) {
            rep += p < f->n ? f->d[p++] : 0;
        }
        while (rep-- && i < npts)
            fl[i++] = v;
    }
    int32_t at = 0;
    for (uint32_t i = 0; ok && i < npts; i++) {
        if (fl[i] & 2) {
            int32_t dv = p < f->n ? f->d[p++] : 0;
            at += (fl[i] & 16) ? dv : -dv;
        } else if (!(fl[i] & 16)) {
            at += s16(f, p);
            p += 2;
        }
        xs[i] = (float)at;
    }
    at = 0;
    for (uint32_t i = 0; ok && i < npts; i++) {
        if (fl[i] & 4) {
            int32_t dv = p < f->n ? f->d[p++] : 0;
            at += (fl[i] & 32) ? dv : -dv;
        } else if (!(fl[i] & 32)) {
            at += s16(f, p);
            p += 2;
        }
        ys[i] = (float)at;
    }
    uint32_t first = 0;
    for (int32_t c = 0; ok && c < ncont; c++) {
        uint32_t last = u16(f, ends + (size_t)c * 2);
        if (last < first || last >= npts) {
            ok = false;
            break;
        }
        uint32_t n = last - first + 1;
        uint32_t k0 = first;
        while (k0 <= last && !(fl[k0] & 1))
            k0++;
        float sx, sy;
        if (k0 > last) {
            sx = (xs[first] + xs[first + (n > 1)]) / 2;
            sy = (ys[first] + ys[first + (n > 1)]) / 2;
            k0 = first;
        } else {
            sx = xs[k0];
            sy = ys[k0];
        }
        float cx = sx, cy = sy, qx = 0, qy = 0;
        bool have_q = false;
        for (uint32_t j = 1; j <= n; j++) {
            uint32_t k = first + (k0 - first + j) % n;
            float x = xs[k], y = ys[k];
            if (fl[k] & 1) {
                if (have_q)
                    quad(s, cx, cy, qx, qy, x, y);
                else
                    line(s, cx, cy, x, y);
                cx = x;
                cy = y;
                have_q = false;
            } else if (have_q) {
                float mx = (qx + x) / 2, my = (qy + y) / 2;
                quad(s, cx, cy, qx, qy, mx, my);
                cx = mx;
                cy = my;
                qx = x;
                qy = y;
            } else {
                qx = x;
                qy = y;
                have_q = true;
            }
        }
        if (have_q)
            quad(s, cx, cy, qx, qy, sx, sy);
        else if (cx != sx || cy != sy)
            line(s, cx, cy, sx, sy);
        first = last + 1;
    }
    free(fl);
    free(xs);
    free(ys);
    return ok;
}

static bool outline(const rl_ttf *f, uint32_t g, sink *s, int depth)
{
    size_t off, len;
    if (!glyph_span(f, g, &off, &len) || len < 10)
        return true;
    int16_t ncont = s16(f, off);
    if (ncont >= 0)
        return ncont == 0 || simple(f, off, ncont, s);
    if (depth >= COMPOSITE_DEPTH)
        return false;
    size_t p = off + 10;
    for (;;) {
        uint16_t flags = u16(f, p), sub = u16(f, p + 2);
        p += 4;
        float dx, dy;
        if (flags & 1) {
            dx = (float)s16(f, p);
            dy = (float)s16(f, p + 2);
            p += 4;
        } else {
            dx = (float)(int8_t)(p < f->n ? f->d[p] : 0);
            dy = (float)(int8_t)(p + 1 < f->n ? f->d[p + 1] : 0);
            p += 2;
        }
        if (!(flags & 2))
            dx = dy = 0;
        float a = 1, b = 0, c = 0, d = 1;
        if (flags & 8) {
            a = d = (float)s16(f, p) / 16384.0f;
            p += 2;
        } else if (flags & 0x40) {
            a = (float)s16(f, p) / 16384.0f;
            d = (float)s16(f, p + 2) / 16384.0f;
            p += 4;
        } else if (flags & 0x80) {
            a = (float)s16(f, p) / 16384.0f;
            b = (float)s16(f, p + 2) / 16384.0f;
            c = (float)s16(f, p + 4) / 16384.0f;
            d = (float)s16(f, p + 6) / 16384.0f;
            p += 8;
        }
        sink child = *s;
        const float *m = s->m;
        child.m[0] = m[0] * a + m[2] * b;
        child.m[1] = m[1] * a + m[3] * b;
        child.m[2] = m[0] * c + m[2] * d;
        child.m[3] = m[1] * c + m[3] * d;
        child.m[4] = m[0] * dx + m[2] * dy + m[4];
        child.m[5] = m[1] * dx + m[3] * dy + m[5];
        if (!outline(f, sub, &child, depth + 1))
            return false;
        if (!(flags & 0x20) || p >= f->n)
            return true;
    }
}

bool rl_ttf_render(const rl_ttf *f, uint32_t g, float px, float dx, rl_ttf_bitmap *out)
{
    memset(out, 0, sizeof *out);
    size_t off, len;
    if (!glyph_span(f, g, &off, &len) || len < 10)
        return false;
    float k = px / (float)f->upem;
    float x0 = (float)s16(f, off + 2) * k + dx, y0 = (float)s16(f, off + 4) * k;
    float x1 = (float)s16(f, off + 6) * k + dx, y1 = (float)s16(f, off + 8) * k;
    int32_t left = (int32_t)floorf(x0), right = (int32_t)ceilf(x1) + 1;
    int32_t top = (int32_t)floorf(-y1), bottom = (int32_t)ceilf(-y0) + 1;
    if (right - left > 4096 || bottom - top > 4096 || right <= left || bottom <= top)
        return false;
    sink s = { 0 };
    s.w = right - left + 2;
    s.h = bottom - top;
    s.acc = calloc((size_t)s.w * (size_t)s.h + 1, sizeof *s.acc);
    if (!s.acc)
        return false;
    s.m[0] = k;
    s.m[3] = -k;
    s.m[4] = dx - (float)left;
    s.m[5] = -(float)top;
    bool ok = outline(f, g, &s, 0);
    uint32_t w = (uint32_t)(right - left), h = (uint32_t)s.h;
    uint8_t *bm = ok ? malloc((size_t)w * h) : NULL;
    if (bm) {
        for (uint32_t y = 0; y < h; y++) {
            float sum = 0;
            const float *row = s.acc + (size_t)y * (size_t)s.w;
            for (uint32_t x = 0; x < w; x++) {
                sum += row[x];
                float a = fabsf(sum);
                bm[(size_t)y * w + x] = (uint8_t)(a >= 1 ? 255 : a * 255 + 0.5f);
            }
        }
    }
    free(s.acc);
    if (!bm)
        return false;
    out->px = bm;
    out->x = left;
    out->y = top;
    out->w = w;
    out->h = h;
    return true;
}
