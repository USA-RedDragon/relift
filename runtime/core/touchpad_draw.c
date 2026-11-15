/**
 * @file touchpad_draw.c
 * @brief The overlay canvas: shapes from signed distance functions with
 * one-pixel antialiasing, composited over premultiplied RGBA8, and text in
 * the stroke font.
 */
#include "touchpad_int.h"

#include <math.h>
#include <string.h>

enum { SDF_CIRCLE, SDF_RRECT, SDF_CROSS, SDF_TRI, SDF_LINE, SDF_GLYPH };

typedef struct sdf {
    int kind;
    float a, b, c, d, e, f;
    float r;
    const int8_t (*pts)[2];
    uint32_t n;
    float ox, oy, scale;
} sdf;

rl_tp_color rl_tp_rgb(uint32_t rgb, float a)
{
    rl_tp_color c = { (float)((rgb >> 16) & 255) / 255.0f, (float)((rgb >> 8) & 255) / 255.0f,
                      (float)(rgb & 255) / 255.0f, a };
    return c;
}

void rl_tp_clear(rl_tp_canvas *c)
{
    if (c->x1 > c->x0 && c->y1 > c->y0)
        for (int32_t y = c->y0; y < c->y1; y++)
            memset(c->px + ((size_t)y * c->w + (size_t)c->x0) * 4, 0, (size_t)(c->x1 - c->x0) * 4);
    c->x0 = c->y0 = c->x1 = c->y1 = 0;
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static float box(float px, float py, float cx, float cy, float hx, float hy, float r)
{
    float qx = fabsf(px - cx) - (hx - r), qy = fabsf(py - cy) - (hy - r);
    float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(mx * mx + my * my) + (in < 0 ? in : 0) - r;
}

static float seg(float px, float py, float x0, float y0, float x1, float y1)
{
    float dx = x1 - x0, dy = y1 - y0, l = dx * dx + dy * dy;
    float t = l > 0 ? ((px - x0) * dx + (py - y0) * dy) / l : 0;
    t = clamp01(t);
    float ex = px - (x0 + t * dx), ey = py - (y0 + t * dy);
    return sqrtf(ex * ex + ey * ey);
}

static float edge(float px, float py, float x0, float y0, float x1, float y1)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    return l > 0 ? ((px - x0) * dy - (py - y0) * dx) / l : 0;
}

static float eval(const sdf *s, float px, float py)
{
    switch (s->kind) {
    case SDF_CIRCLE: {
        float dx = px - s->a, dy = py - s->b;
        return sqrtf(dx * dx + dy * dy) - s->r;
    }
    case SDF_RRECT:
        return box(px, py, s->a, s->b, s->c, s->d, s->r);
    case SDF_CROSS: {
        float v = box(px, py, s->a, s->b, s->d, s->c, s->r), h = box(px, py, s->a, s->b, s->c, s->d, s->r);
        return v < h ? v : h;
    }
    case SDF_TRI: {
        float e0 = edge(px, py, s->a, s->b, s->c, s->d), e1 = edge(px, py, s->c, s->d, s->e, s->f),
              e2 = edge(px, py, s->e, s->f, s->a, s->b);
        float m = e0 > e1 ? e0 : e1;
        return m > e2 ? m : e2;
    }
    case SDF_LINE:
        return seg(px, py, s->a, s->b, s->c, s->d) - s->r;
    default: {
        float best = 1e9f;
        float gx = (px - s->ox) / s->scale, gy = (s->oy - py) / s->scale;
        bool have = false;
        float lx = 0, ly = 0;
        for (uint32_t i = 0; i < s->n; i++) {
            if (s->pts[i][0] == -128) {
                have = false;
                continue;
            }
            float x = s->pts[i][0], y = s->pts[i][1];
            float d = have ? seg(gx, gy, lx, ly, x, y) : sqrtf((gx - x) * (gx - x) + (gy - y) * (gy - y));
            if (d < best)
                best = d;
            lx = x;
            ly = y;
            have = true;
        }
        return best * s->scale - s->r;
    }
    }
}

static void touch_box(rl_tp_canvas *c, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (c->x1 <= c->x0 || c->y1 <= c->y0) {
        c->x0 = x0;
        c->y0 = y0;
        c->x1 = x1;
        c->y1 = y1;
        return;
    }
    if (x0 < c->x0)
        c->x0 = x0;
    if (y0 < c->y0)
        c->y0 = y0;
    if (x1 > c->x1)
        c->x1 = x1;
    if (y1 > c->y1)
        c->y1 = y1;
}

/**
 * Draws s over the bounds: inside (d < 0) in fill, the band within `inner` of the edge in stroke, out to `ext` past
 * the edge in stroke too; opacity scales the result.
 */
static void raster(rl_tp_canvas *c, const sdf *s, float bx0, float by0, float bx1, float by1, rl_tp_color fill,
                   rl_tp_color stroke, float ext, float inner, float opacity)
{
    float m = ext + 2.0f;
    int32_t x0 = (int32_t)floorf(bx0 - m), y0 = (int32_t)floorf(by0 - m), x1 = (int32_t)ceilf(bx1 + m),
            y1 = (int32_t)ceilf(by1 + m);
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > (int32_t)c->w)
        x1 = (int32_t)c->w;
    if (y1 > (int32_t)c->h)
        y1 = (int32_t)c->h;
    if (x1 <= x0 || y1 <= y0 || opacity <= 0.0f)
        return;
    touch_box(c, x0, y0, x1, y1);
    bool stroked = stroke.a > 0.0f && (ext > 0.0f || inner > 0.0f);
    for (int32_t y = y0; y < y1; y++) {
        uint8_t *p = c->px + ((size_t)y * c->w + (size_t)x0) * 4;
        for (int32_t x = x0; x < x1; x++, p += 4) {
            float d = eval(s, (float)x + 0.5f, (float)y + 0.5f);
            float cov = clamp01(0.5f - d + ext);
            if (cov <= 0.0f)
                continue;
            float t = stroked ? clamp01(0.5f + d + inner) : 0.0f;
            float r = fill.r + (stroke.r - fill.r) * t, g = fill.g + (stroke.g - fill.g) * t,
                  b = fill.b + (stroke.b - fill.b) * t, a = fill.a + (stroke.a - fill.a) * t;
            a *= cov * opacity;
            if (a <= 0.0f)
                continue;
            float k = 1.0f - a;
            p[0] = (uint8_t)(r * a * 255.0f + p[0] * k + 0.5f);
            p[1] = (uint8_t)(g * a * 255.0f + p[1] * k + 0.5f);
            p[2] = (uint8_t)(b * a * 255.0f + p[2] * k + 0.5f);
            p[3] = (uint8_t)(a * 255.0f + p[3] * k + 0.5f);
        }
    }
}

void rl_tp_circle(rl_tp_canvas *c, float cx, float cy, float r, rl_tp_color fill, rl_tp_color stroke, float sw,
                  float opacity)
{
    sdf s = { .kind = SDF_CIRCLE, .a = cx, .b = cy, .r = r };
    raster(c, &s, cx - r, cy - r, cx + r, cy + r, fill, stroke, sw / 2, sw / 2, opacity);
}

void rl_tp_rrect(rl_tp_canvas *c, float x0, float y0, float x1, float y1, float radius, rl_tp_color fill,
                 rl_tp_color stroke, float sw, float opacity)
{
    float hx = (x1 - x0) / 2, hy = (y1 - y0) / 2;
    float r = radius > hx ? hx : radius;
    r = r > hy ? hy : r;
    sdf s = { .kind = SDF_RRECT, .a = x0 + hx, .b = y0 + hy, .c = hx, .d = hy, .r = r };
    raster(c, &s, x0, y0, x1, y1, fill, stroke, sw / 2, sw / 2, opacity);
}

void rl_tp_cross(rl_tp_canvas *c, float cx, float cy, float sz, float a, rl_tp_color fill, rl_tp_color stroke,
                 float sw, float opacity)
{
    sdf s = { .kind = SDF_CROSS, .a = cx, .b = cy, .c = sz / 2, .d = a / 2, .r = sw / 2 };
    raster(c, &s, cx - sz / 2, cy - sz / 2, cx + sz / 2, cy + sz / 2, fill, stroke, sw / 2, sw / 2, opacity);
}

void rl_tp_triangle(rl_tp_canvas *c, const float p[6], rl_tp_color fill, rl_tp_color outline, float ow)
{
    sdf s = { .kind = SDF_TRI, .a = p[0], .b = p[1], .c = p[2], .d = p[3], .e = p[4], .f = p[5] };
    float lx = fminf(p[0], fminf(p[2], p[4])), hx = fmaxf(p[0], fmaxf(p[2], p[4]));
    float ly = fminf(p[1], fminf(p[3], p[5])), hy = fmaxf(p[1], fmaxf(p[3], p[5]));
    if (edge(p[4], p[5], p[0], p[1], p[2], p[3]) > 0) {
        s.c = p[4];
        s.d = p[5];
        s.e = p[2];
        s.f = p[3];
    }
    raster(c, &s, lx, ly, hx, hy, fill, outline, ow, 0, 1.0f);
}

void rl_tp_line(rl_tp_canvas *c, float x0, float y0, float x1, float y1, float width, rl_tp_color color)
{
    sdf s = { .kind = SDF_LINE, .a = x0, .b = y0, .c = x1, .d = y1, .r = width / 2 };
    raster(c, &s, fminf(x0, x1) - width, fminf(y0, y1) - width, fmaxf(x0, x1) + width, fmaxf(y0, y1) + width, color,
           color, 0, 0, 1.0f);
}

static void dashes(rl_tp_canvas *c, float x0, float y0, float x1, float y1, float width, float dash, float gap,
                   rl_tp_color color)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l <= 0)
        return;
    for (float t = 0; t < l; t += dash + gap) {
        float e = t + dash < l ? t + dash : l;
        rl_tp_line(c, x0 + dx * t / l, y0 + dy * t / l, x0 + dx * e / l, y0 + dy * e / l, width, color);
    }
}

void rl_tp_dashed_rect(rl_tp_canvas *c, float x0, float y0, float x1, float y1, float width, float dash, float gap,
                       rl_tp_color color)
{
    dashes(c, x0, y0, x1, y0, width, dash, gap, color);
    dashes(c, x1, y0, x1, y1, width, dash, gap, color);
    dashes(c, x1, y1, x0, y1, width, dash, gap, color);
    dashes(c, x0, y1, x0, y0, width, dash, gap, color);
}

static const rl_tp_glyph *glyph(char ch)
{
    unsigned u = (unsigned char)ch;
    return &rl_tp_font_glyphs[u >= 32 && u < 127 ? u - 32 : '?' - 32];
}

static float stroke_of(float size, bool bold) { return size * (bold ? 0.105f : 0.075f); }

float rl_tp_text_width(const char *text, float size, bool bold)
{
    float scale = size * 0.72f / 40.0f, w = 0;
    for (const char *t = text; *t; t++)
        w += ((float)glyph(*t)->advance + 9.0f) * scale;
    if (*text)
        w -= 9.0f * scale;
    return w + stroke_of(size, bold);
}

void rl_tp_text(rl_tp_canvas *c, float x, float y, const char *text, float size, bool bold, int align,
                rl_tp_color color, rl_tp_color outline, float ow)
{
    float scale = size * 0.72f / 40.0f, sw = stroke_of(size, bold);
    float w = rl_tp_text_width(text, size, bold);
    float pen = align == RL_TP_CENTER ? x - w / 2 : align == RL_TP_RIGHT ? x - w : x;
    pen += sw / 2;
    float base = y + size * 0.36f;
    for (const char *t = text; *t; t++) {
        const rl_tp_glyph *g = glyph(*t);
        if (g->count) {
            sdf s = { .kind = SDF_GLYPH, .pts = rl_tp_font_points + g->first, .n = g->count, .ox = pen, .oy = base,
                      .scale = scale, .r = sw / 2 };
            raster(c, &s, pen - 4 * scale, base - 56 * scale, pen + ((float)g->advance + 4) * scale,
                   base + 16 * scale, color, outline, ow, 0, 1.0f);
        }
        pen += ((float)g->advance + 9.0f) * scale;
    }
}
