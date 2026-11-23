/**
 * @file warmup_draw.c
 * @brief The compile screen's drawing (warmup.h): the blocking card and the corner card, laid out in units of one
 * "rem" (a 90th of a 1080p window's height, a 54th of a phone's width) so one design fits every window. Shapes are
 * the on-screen controller's (touchpad_draw.c); text is a TrueType font from the files the program ships (the open
 * Noto fallback, as the text entry panel uses), or the controller's stroke font when none is there.
 */
#include "core.h"
#include "touchpad_int.h"
#include "ttf.h"
#include "warmup_int.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONTS_MAX 8
#define SHIPPED_FONTS 5
/** @brief The height of a line of text with no line height of its own, per em (Noto Sans). */
#define LINE_NORMAL 1.362f
/** @brief How much wider a bold glyph is than the regular one it is made from (the fonts shipped have one weight). */
#define BOLD_ADVANCE 1.06f

static pthread_mutex_t g_font_lock = PTHREAD_MUTEX_INITIALIZER;
static rl_ttf g_fonts[FONTS_MAX];
static uint32_t g_nfonts, g_added;
static bool g_shipped;

void rl_wu_add_font(const uint8_t *data, size_t size)
{
    pthread_mutex_lock(&g_font_lock);
    rl_ttf f;
    if (g_nfonts < FONTS_MAX && rl_ttf_init(&f, data, size)) {
        memmove(&g_fonts[1], &g_fonts[0], sizeof g_fonts[0] * g_nfonts);
        g_fonts[0] = f;
        g_nfonts++;
        g_added++;
    }
    pthread_mutex_unlock(&g_font_lock);
}

void rl_wu_reset_fonts(void)
{
    pthread_mutex_lock(&g_font_lock);
    memmove(&g_fonts[0], &g_fonts[g_added], sizeof g_fonts[0] * (g_nfonts - g_added));
    g_nfonts -= g_added;
    g_added = 0;
    pthread_mutex_unlock(&g_font_lock);
}

/** @brief Loads the shipped fonts once (font lock held). */
static void load_shipped(void)
{
    if (g_shipped)
        return;
    g_shipped = true;
    for (uint32_t i = 0; i < SHIPPED_FONTS && g_nfonts < FONTS_MAX; i++) {
        char name[32];
        snprintf(name, sizeof name, "fonts/%u.ttf", i);
        size_t n = 0;
        uint8_t *data = rl_asset_load(name, &n);
        if (!data)
            continue;
        if (rl_ttf_init(&g_fonts[g_nfonts], data, n))
            g_nfonts++;
        else
            free(data);
    }
}

bool rl_wu_has_font(void)
{
    pthread_mutex_lock(&g_font_lock);
    load_shipped();
    bool ok = g_nfonts > 0;
    pthread_mutex_unlock(&g_font_lock);
    return ok;
}

/** @brief Decodes the code point at s, moving s past it. */
static uint32_t next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    c &= more == 3 ? 0x07 : more == 2 ? 0x0f : more == 1 ? 0x1f : 0x7f;
    for (int k = 0; k < more && (*p & 0xc0) == 0x80; k++)
        c = (c << 6) | (*p++ & 0x3f);
    *s = (const char *)p;
    return c;
}

/** @brief The font and glyph that draw cp (font lock held). */
static const rl_ttf *glyph_for(uint32_t cp, uint32_t *g)
{
    for (uint32_t i = 0; i < g_nfonts; i++) {
        *g = rl_ttf_glyph(&g_fonts[i], cp);
        if (*g)
            return &g_fonts[i];
    }
    *g = 0;
    return g_nfonts ? &g_fonts[0] : NULL;
}

/** @brief The look: colors as 0xRRGGBB. */
typedef struct theme {
    uint32_t bg, card, border, text, muted, track, accent, onaccent, err, ok, btn, pill, pill_border;
    float pill_alpha;
} theme;

static const theme k_dark = { 0x0e0f12, 0x1b1d22, 0x555555, 0xffffff, 0x9aa1ad, 0x3a3f48, 0x8ab4ff,
                              0x10131a, 0xff5a5a, 0x5eead4, 0x2b2e35, 0x1b1d22, 0x666666, 0.94f };
static const theme k_light = { 0xeef0f4, 0xffffff, 0xc4c9d2, 0x14161a, 0x5b6270, 0xd8dce3, 0x2f62d0,
                               0xffffff, 0xc62828, 0x0f766e, 0xe4e7ed, 0xffffff, 0xaab0bb, 0.95f };

/** @brief One drawing: the canvas, the unit, the theme, and whether to only measure. */
typedef struct ui {
    rl_wu_canvas *c;
    float r;
    const theme *t;
    bool dry;
} ui;

static rl_tp_color col(uint32_t rgb, float a) { return rl_tp_rgb(rgb, a); }

/** @brief Widens the canvas's drawn rectangle by a rectangle (clamped to the canvas). */
static void grow(rl_wu_canvas *c, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > (int32_t)c->w)
        x1 = (int32_t)c->w;
    if (y1 > (int32_t)c->h)
        y1 = (int32_t)c->h;
    if (x1 <= x0 || y1 <= y0)
        return;
    if (c->x1 <= c->x0 || c->y1 <= c->y0) {
        c->x0 = x0;
        c->y0 = y0;
        c->x1 = x1;
        c->y1 = y1;
        return;
    }
    c->x0 = x0 < c->x0 ? x0 : c->x0;
    c->y0 = y0 < c->y0 ? y0 : c->y0;
    c->x1 = x1 > c->x1 ? x1 : c->x1;
    c->y1 = y1 > c->y1 ? y1 : c->y1;
}

void rl_wu_clear(rl_wu_canvas *c) { rl_tp_clear(c); }

/** @brief The width of n bytes of s at size px (font lock held for the font path). */
static float width_n(const char *s, size_t n, float px, bool bold)
{
    if (!n)
        return 0;
    pthread_mutex_lock(&g_font_lock);
    load_shipped();
    float w = 0;
    if (g_nfonts) {
        const char *p = s, *end = s + n;
        while (p < end && *p) {
            uint32_t cp = next_cp(&p), g;
            const rl_ttf *f = glyph_for(cp, &g);
            if (f)
                w += rl_ttf_advance(f, g, px) * (bold ? BOLD_ADVANCE : 1.0f);
        }
        pthread_mutex_unlock(&g_font_lock);
        return w;
    }
    pthread_mutex_unlock(&g_font_lock);
    char tmp[512];
    size_t k = n < sizeof tmp - 1 ? n : sizeof tmp - 1;
    memcpy(tmp, s, k);
    tmp[k] = 0;
    return rl_tp_text_width(tmp, px, bold);
}

static float width_of(const char *s, float px, bool bold) { return width_n(s, strlen(s), px, bold); }

/** @brief Blends coverage cov (0 to 1) of a straight color over the premultiplied pixel p. */
static void blend(uint8_t *p, uint32_t rgb, float a)
{
    float k = 1.0f - a;
    p[0] = (uint8_t)((float)((rgb >> 16) & 255) * a + (float)p[0] * k + 0.5f);
    p[1] = (uint8_t)((float)((rgb >> 8) & 255) * a + (float)p[1] * k + 0.5f);
    p[2] = (uint8_t)((float)(rgb & 255) * a + (float)p[2] * k + 0.5f);
    p[3] = (uint8_t)(255.0f * a + (float)p[3] * k + 0.5f);
}

/**
 * @brief Draws n bytes of s with its line box starting at top, lh high, from x; returns the width. Dry runs only
 * measure.
 */
static float text_n(ui *u, float x, float top, float lh, const char *s, size_t n, float px, bool bold, uint32_t rgb)
{
    float w = width_n(s, n, px, bold);
    if (u->dry || !n)
        return w;
    pthread_mutex_lock(&g_font_lock);
    if (g_nfonts) {
        float asc, desc;
        rl_ttf_metrics(&g_fonts[0], px, &asc, &desc);
        float base = top + (lh - (asc - desc)) / 2 + asc;
        float pen = x;
        const char *p = s, *end = s + n;
        while (p < end && *p) {
            uint32_t cp = next_cp(&p), g;
            const rl_ttf *f = glyph_for(cp, &g);
            if (!f)
                break;
            float ib = floorf(pen);
            for (int pass = 0; pass < (bold ? 3 : 1); pass++) {
                rl_ttf_bitmap b;
                float dx = pen - ib + (float)pass * px * 0.035f;
                float ibx = ib;
                if (dx >= 1.0f) {
                    ibx += floorf(dx);
                    dx -= floorf(dx);
                }
                if (!rl_ttf_render(f, g, px, dx, &b))
                    continue;
                int32_t ox = (int32_t)ibx + b.x, oy = (int32_t)floorf(base) + b.y;
                grow(u->c, ox, oy, ox + (int32_t)b.w, oy + (int32_t)b.h);
                for (uint32_t j = 0; j < b.h; j++) {
                    int32_t Y = oy + (int32_t)j;
                    if (Y < 0 || Y >= (int32_t)u->c->h)
                        continue;
                    for (uint32_t i = 0; i < b.w; i++) {
                        int32_t X = ox + (int32_t)i;
                        uint8_t a = b.px[(size_t)j * b.w + i];
                        if (a && X >= 0 && X < (int32_t)u->c->w)
                            blend(u->c->px + ((size_t)Y * u->c->w + (size_t)X) * 4, rgb, (float)a / 255.0f);
                    }
                }
                free(b.px);
            }
            pen += rl_ttf_advance(f, g, px) * (bold ? BOLD_ADVANCE : 1.0f);
        }
        pthread_mutex_unlock(&g_font_lock);
        return w;
    }
    pthread_mutex_unlock(&g_font_lock);
    char tmp[512];
    size_t k = n < sizeof tmp - 1 ? n : sizeof tmp - 1;
    memcpy(tmp, s, k);
    tmp[k] = 0;
    rl_tp_text(u->c, x, top + lh / 2, tmp, px, bold, RL_TP_LEFT, col(rgb, 1.0f), col(0, 0), 0);
    grow(u->c, (int32_t)x - 2, (int32_t)(top + lh / 2 - px), (int32_t)(x + w) + 4, (int32_t)(top + lh / 2 + px));
    return w;
}

static float text_line(ui *u, float x, float top, float lh, const char *s, float px, bool bold, uint32_t rgb)
{
    return text_n(u, x, top, lh, s, strlen(s), px, bold, rgb);
}

/** @brief A piece of a paragraph: text in one style. */
typedef struct run {
    const char *text;
    bool bold;
    uint32_t rgb;
} run;

/**
 * @brief Draws runs wrapped at width from x with the first line's box at top; returns the height used. Breaks at
 * spaces, and inside a word only when the word alone is wider than the line.
 */
static float para(ui *u, float x, float top, float width, float lh, float px, const run *runs, size_t nruns)
{
    float pen = 0, y = top;
    bool line_empty = true;
    for (size_t i = 0; i < nruns; i++) {
        const char *s = runs[i].text;
        while (*s) {
            size_t sp = 0;
            while (s[sp] == ' ')
                sp++;
            const char *w = s + sp;
            size_t wn = 0;
            while (w[wn] && w[wn] != ' ')
                wn++;
            if (!wn) {
                s = w;
                continue;
            }
            float space = sp && !line_empty ? width_n(" ", 1, px, runs[i].bold) : 0;
            float ww = width_n(w, wn, px, runs[i].bold);
            if (!line_empty && pen + space + ww > width + 0.5f) {
                y += lh;
                pen = 0;
                line_empty = true;
                space = 0;
            }
            size_t off = 0;
            while (off < wn) {
                size_t take = wn - off;
                if (pen + space + width_n(w + off, take, px, runs[i].bold) > width + 0.5f && take > 1) {
                    size_t lo = 1;
                    while (lo < take && pen + space + width_n(w + off, lo + 1, px, runs[i].bold) <= width + 0.5f)
                        lo++;
                    take = lo;
                    while (take < wn - off && ((unsigned char)w[off + take] & 0xc0) == 0x80)
                        take++;
                }
                float tw = text_n(u, x + pen + space, y, lh, w + off, take, px, runs[i].bold, runs[i].rgb);
                pen += space + tw;
                space = 0;
                line_empty = false;
                off += take;
                if (off < wn) {
                    y += lh;
                    pen = 0;
                    line_empty = true;
                }
            }
            s = w + wn;
        }
    }
    return y + lh - top;
}

static float para1(ui *u, float x, float top, float width, float lh, float px, const char *s, bool bold, uint32_t rgb)
{
    run r = { s, bold, rgb };
    return para(u, x, top, width, lh, px, &r, 1);
}

static void rrect(ui *u, float x0, float y0, float x1, float y1, float rad, uint32_t fill, float fa)
{
    if (!u->dry)
        rl_tp_rrect(u->c, x0, y0, x1, y1, rad, col(fill, fa), col(0, 0), 0, 1.0f);
}

/** @brief A bordered rounded box whose outer edge is (x0, y0) to (x1, y1): the border sits inside it. */
static void box(ui *u, float x0, float y0, float x1, float y1, float rad, uint32_t fill, float fa, uint32_t border,
                float bw)
{
    if (u->dry)
        return;
    rl_tp_rrect(u->c, x0 + bw / 2, y0 + bw / 2, x1 - bw / 2, y1 - bw / 2, rad - bw / 2, col(fill, fa),
                col(border, 1.0f), bw, 1.0f);
}

/** @brief The progress bar: ok part, failed sliver, and the rest as the track. */
static void bar(ui *u, float x, float y, float w, float h, const rl_wu_scene *s, bool all_ok)
{
    if (u->dry)
        return;
    const theme *t = u->t;
    rrect(u, x, y, x + w, y + h, h / 2, t->track, 1.0f);
    if (!s->total || !s->done)
        return;
    float done_w = w * (float)s->done / (float)s->total;
    if (done_w < h)
        done_w = h;
    float ok_w = w * (float)(s->done - s->failed) / (float)s->total;
    bool at_end = done_w >= w - 0.5f;
    if (s->failed) {
        float ew = w * (float)s->failed / (float)s->total;
        if (ew < u->r * 0.6f)
            ew = u->r * 0.6f;
        if (ok_w < h)
            ok_w = h;
        /* The whole done part in the failed color, its left cap and a flat-ended ok part over it. */
        rrect(u, x, y, x + done_w, y + h, h / 2, t->err, 1.0f);
        rrect(u, x, y, x + h, y + h, h / 2, t->accent, 1.0f);
        if (ok_w > h)
            rrect(u, x + h / 2, y, x + ok_w, y + h, 0, t->accent, 1.0f);
        (void)ew;
        return;
    }
    uint32_t fill = all_ok ? t->ok : t->accent;
    rrect(u, x, y, x + done_w, y + h, h / 2, fill, 1.0f);
    if (!at_end)
        rrect(u, x + done_w - h / 2, y, x + done_w, y + h, 0, fill, 1.0f);
}

/** @brief The game's icon in a rounded square of side side at (x, y), with the soft ring around it. */
static void icon(ui *u, float x, float y, float side, const rl_wu_scene *s)
{
    if (u->dry || !s->icon)
        return;
    float rad = u->r * 2.2f;
    rl_tp_rrect(u->c, x - u->r * 0.2f, y - u->r * 0.2f, x + side + u->r * 0.2f, y + side + u->r * 0.2f, rad + u->r * 0.2f,
                col(0x808080, 0.35f), col(0, 0), 0, 1.0f);
    float hx = side / 2, hy = side / 2, cx = x + hx, cy = y + hy;
    int32_t x0 = (int32_t)floorf(x), y0 = (int32_t)floorf(y), x1 = (int32_t)ceilf(x + side), y1 = (int32_t)ceilf(y + side);
    grow(u->c, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
    for (int32_t py = y0; py < y1; py++)
        for (int32_t px = x0; px < x1; px++) {
            if (px < 0 || py < 0 || px >= (int32_t)u->c->w || py >= (int32_t)u->c->h)
                continue;
            float qx = fabsf((float)px + 0.5f - cx) - (hx - rad), qy = fabsf((float)py + 0.5f - cy) - (hy - rad);
            float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - rad;
            float cov = fminf(fmaxf(0.5f - d, 0), 1);
            if (cov <= 0)
                continue;
            float sx = ((float)px + 0.5f - x) * (float)s->icon_w / side - 0.5f;
            float sy = ((float)py + 0.5f - y) * (float)s->icon_h / side - 0.5f;
            int32_t ix = (int32_t)floorf(sx), iy = (int32_t)floorf(sy);
            float fx = sx - (float)ix, fy = sy - (float)iy;
            float acc[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < 4; k++) {
                int32_t jx = ix + (k & 1), jy = iy + (k >> 1);
                jx = jx < 0 ? 0 : jx >= (int32_t)s->icon_w ? (int32_t)s->icon_w - 1 : jx;
                jy = jy < 0 ? 0 : jy >= (int32_t)s->icon_h ? (int32_t)s->icon_h - 1 : jy;
                const uint8_t *q = s->icon + ((size_t)jy * s->icon_w + (size_t)jx) * 4;
                float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                float a = (float)q[3] / 255.0f;
                acc[0] += (float)q[0] * a * wgt;
                acc[1] += (float)q[1] * a * wgt;
                acc[2] += (float)q[2] * a * wgt;
                acc[3] += a * wgt;
            }
            uint8_t *p = u->c->px + ((size_t)py * u->c->w + (size_t)px) * 4;
            float k = 1.0f - acc[3] * cov;
            p[0] = (uint8_t)(acc[0] * cov + (float)p[0] * k + 0.5f);
            p[1] = (uint8_t)(acc[1] * cov + (float)p[1] * k + 0.5f);
            p[2] = (uint8_t)(acc[2] * cov + (float)p[2] * k + 0.5f);
            p[3] = (uint8_t)(255.0f * acc[3] * cov + (float)p[3] * k + 0.5f);
        }
}

static void set4(float *o, float x, float y, float w, float h)
{
    o[0] = x;
    o[1] = y;
    o[2] = w;
    o[3] = h;
}

/** @brief A pill button with its label; returns its width. place gets its rectangle. */
static float button(ui *u, float right, float top, const char *label, bool primary, float *place)
{
    float r = u->r, px = 2.4f * r;
    float tw = width_of(label, px, true);
    float w = tw + 7.2f * r;
    if (w < 14.0f * r)
        w = 14.0f * r;
    float h = px * LINE_NORMAL + 2.8f * r;
    float x = right - w;
    rrect(u, x, top, x + w, top + h, h / 2, primary ? u->t->accent : u->t->btn, 1.0f);
    text_line(u, x + (w - tw) / 2, top + 1.4f * r, px * LINE_NORMAL, label, px, true,
              primary ? u->t->onaccent : u->t->text);
    if (place)
        set4(place, x, top, w, h);
    return w;
}

/** @brief The check mark in a circle of diameter 5 rem at (x, y) (its top left). */
static void check(ui *u, float x, float y)
{
    if (u->dry)
        return;
    float r = u->r, cx = x + 2.5f * r, cy = y + 2.5f * r;
    rl_tp_circle(u->c, cx, cy, 2.5f * r, col(u->t->ok, 1.0f), col(0, 0), 0, 1.0f);
    rl_tp_line(u->c, cx - 1.05f * r, cy + 0.05f * r, cx - 0.35f * r, cy + 0.85f * r, 0.6f * r, col(u->t->onaccent, 1));
    rl_tp_line(u->c, cx - 0.35f * r, cy + 0.85f * r, cx + 1.2f * r, cy - 0.95f * r, 0.6f * r, col(u->t->onaccent, 1));
}

/** @brief The words of the blocking screen's head for a state. */
static const char *head_of(int state)
{
    switch (state) {
    case RL_WU_FINISHING:
        return "Finishing up";
    case RL_WU_ERROR:
        return "Some graphics could not be prepared";
    case RL_WU_DONE:
        return "Graphics are ready";
    default:
        return "Preparing graphics for your GPU";
    }
}

/** @brief The right-hand words of a progress row: time left, or why none (out is set). Returns whether it is the strong color. */
static bool right_words(const rl_wu_scene *s, char *out, size_t n)
{
    switch (s->state) {
    case RL_WU_COMPILING:
        if (s->open) {
            out[0] = 0;
            return false;
        }
        rl_wu_eta_text(s->eta_s, out, n);
        return true;
    case RL_WU_FINISHING:
        snprintf(out, n, "Saving...");
        return false;
    default:
        snprintf(out, n, "Measuring speed...");
        return false;
    }
}

/** @brief The height a wrapped paragraph takes, without drawing it. */
static float para_height(ui *u, float width, float lh, float px, const run *runs, size_t n)
{
    bool dry = u->dry;
    u->dry = true;
    float h = para(u, 0, 0, width, lh, px, runs, n);
    u->dry = dry;
    return h;
}

/** @brief A two-item row (left strong, right as given) of font size px with its line boxes from top; returns its height. */
static float row(ui *u, float x, float top, float width, float px, const char *left, bool left_bold, uint32_t left_rgb,
                 const char *right, uint32_t right_rgb)
{
    float lh = px * LINE_NORMAL, gap = 2.0f * u->r;
    float lw = width_of(left, px, left_bold), rw = right[0] ? width_of(right, px, false) : 0;
    text_line(u, x, top, lh, left, px, left_bold, left_rgb);
    if (!right[0])
        return lh;
    if (lw + gap + rw <= width + 0.5f) {
        text_line(u, x + width - rw, top, lh, right, px, false, right_rgb);
        return lh;
    }
    text_line(u, x, top + lh + gap, lh, right, px, false, right_rgb);
    return 2 * lh + gap;
}

/** @brief The blocking card's paragraphs under the rule, for a state; returns the height. */
static float notes(ui *u, const rl_wu_scene *s, float x, float top, float width)
{
    float r = u->r, px = 2.1f * r, lh = px * 1.45f, y = top;
    const theme *t = u->t;
    if (s->starting) {
        run st[2] = { { "Starting now.", true, t->text },
                      { " Some scenes may pause briefly until this finishes.", false, t->muted } };
        y += para(u, x, y, width, lh, px, st, 2);
        y += r;
        run saved[2] = { { "Your progress is saved as it goes.", true, t->text },
                         { " If you quit now, the next launch continues from here.", false, t->muted } };
        y += para(u, x, y, width, lh, px, saved, 2);
        return y - top;
    }
    if (s->state == RL_WU_FINISHING) {
        return para1(u, x, y, width, lh, px, "Saving everything so the next launch is instant. Please wait a moment.", false,
                     t->muted);
    }
    if (s->state == RL_WU_DONE) {
        y += para1(u, x, y, width, lh, px, "Starting the game...", false, t->muted);
        y += r;
        y += para1(u, x, y, width, lh, px, "Next time it starts at once, until your GPU driver is updated.", false,
                   t->muted);
        return y - top;
    }
    y += para1(u, x, y, width, lh, px,
               "This happens once for each game and GPU driver. Later launches start at once.", false, t->muted);
    y += r;
    y += para1(u, x, y, width, lh, px, "The game starts when this finishes.", false, t->muted);
    y += r;
    run saved[2] = { { "Your progress is saved as it goes.", true, t->text },
                     { " If you quit now, the next launch continues from here.", false, t->muted } };
    y += para(u, x, y, width, lh, px, saved, 2);
    return y - top;
}

/**
 * @brief The blocking card with its top left at (x0, y0) and width cw; returns its height. u->dry measures; a real
 * pass fills the places of the buttons and the card in hits.
 */
static float blocking(ui *u, const rl_wu_scene *s, float x0, float y0, float cw, rl_wu_hits *hits)
{
    float r = u->r;
    const theme *t = u->t;
    float bw = 0.4f * r, pad_x = 4.4f * r, pad_y = 4.0f * r;
    float ix = x0 + bw + pad_x, iw = cw - 2 * (bw + pad_x);
    float y = y0 + bw + pad_y;
    char l[96], rt[96], tmp[96];
    bool dry = u->dry;

    /* Name, GPU and icon. */
    float side = s->icon ? 9.6f * r : 0;
    float tx = ix + (side ? side + 2.4f * r : 0), tw = iw - (side ? side + 2.4f * r : 0);
    float name_px = 3.8f * r, sub_px = 2.2f * r;
    run nr = { s->name, true, t->text }, gr = { s->gpu, false, t->muted };
    float name_h = s->name[0] ? para_height(u, tw, name_px * 1.15f, name_px, &nr, 1) : 0;
    float sub_h = s->gpu[0] ? para_height(u, tw, sub_px * LINE_NORMAL, sub_px, &gr, 1) : 0;
    float text_h = name_h + (name_h && sub_h ? 0.4f * r : 0) + sub_h;
    float row_h = text_h > side ? text_h : side;
    if (!dry && row_h > 0) {
        if (side)
            icon(u, ix, y + (row_h - side) / 2, side, s);
        float ty = y + (row_h - text_h) / 2;
        if (name_h)
            para(u, tx, ty, tw, name_px * 1.15f, name_px, &nr, 1);
        if (sub_h)
            para(u, tx, ty + name_h + (name_h ? 0.4f * r : 0), tw, sub_px * LINE_NORMAL, sub_px, &gr, 1);
    }
    y += row_h;

    /* Head. */
    int st = s->state;
    float head_px = 3.2f * r, head_lh = head_px * 1.2f;
    uint32_t head_rgb = st == RL_WU_ERROR ? t->err : t->text;
    float head_x = ix, head_w = iw;
    float head_top = y + (row_h > 0 ? 3.4f * r : 0);
    run hr = { head_of(st), true, head_rgb };
    if (st == RL_WU_DONE) {
        head_x = ix + 5.0f * r + 1.6f * r;
        head_w = iw - 6.6f * r;
    }
    float head_h = para_height(u, head_w, head_lh, head_px, &hr, 1);
    float head_row = st == RL_WU_DONE && head_h < 5.0f * r ? 5.0f * r : head_h;
    if (!dry) {
        if (st == RL_WU_DONE)
            check(u, ix, head_top + (head_row - 5.0f * r) / 2);
        para(u, head_x, head_top + (head_row - head_h) / 2, head_w, head_lh, head_px, &hr, 1);
    }
    y = head_top + head_row;

    /* Bar and the rows under it. */
    float bar_h = 2.0f * r;
    y += 2.2f * r;
    bar(u, ix, y, iw, bar_h, s, st == RL_WU_DONE);
    y += bar_h;
    float px = 2.4f * r;
    y += 1.6f * r;
    char done_t[32], total_t[32];
    rl_wu_count_text(st == RL_WU_ERROR ? s->done - s->failed : s->done, done_t, sizeof done_t);
    rl_wu_count_text(s->total, total_t, sizeof total_t);
    if (st == RL_WU_ERROR) {
        snprintf(l, sizeof l, "%s of %s shaders ready", done_t, total_t);
        snprintf(rt, sizeof rt, "%u failed", s->failed);
        y += row(u, ix, y, iw, px, l, true, t->text, rt, t->err);
    } else if (st == RL_WU_DONE) {
        snprintf(l, sizeof l, "%s of %s shaders", total_t, total_t);
        rl_wu_time_text(s->elapsed_ns / 1000000000ull, tmp, sizeof tmp);
        snprintf(rt, sizeof rt, "Took %s", tmp);
        y += row(u, ix, y, iw, px, l, true, t->text, rt, t->muted);
    } else {
        snprintf(l, sizeof l, "%s of %s shaders", done_t, total_t);
        bool strong = right_words(s, rt, sizeof rt);
        y += row(u, ix, y, iw, px, l, true, t->text, rt, strong ? t->text : t->muted);
        y += 0.6f * r;
        rl_wu_time_text(s->elapsed_ns / 1000000000ull, tmp, sizeof tmp);
        snprintf(l, sizeof l, "Time so far %s", tmp);
        if (s->open)
            rt[0] = 0;
        else
            snprintf(rt, sizeof rt, "%u%%", st == RL_WU_FINISHING ? 100u : rl_wu_percent(s->done, s->total));
        y += row(u, ix, y, iw, px, l, false, t->muted, rt, t->muted);
    }

    /* The result of a run with failures: what it means and where the details are. */
    if (st == RL_WU_ERROR) {
        y += 2.2f * r;
        char msg[256];
        snprintf(msg, sizeof msg,
                 "Your GPU driver turned down %u shader%s. The game can still start, but some effects may be missing "
                 "or look wrong. Updating your GPU driver may fix this.",
                 s->failed, s->failed == 1 ? "" : "s");
        y += para1(u, ix, y, iw, 2.3f * r * 1.45f, 2.3f * r, msg, false, t->text);
        if (s->report[0]) {
            y += 2.0f * r;
            y += para1(u, ix, y, iw, 2.1f * r * 1.45f, 2.1f * r, "Details are saved here:", true, t->text);
            y += r;
            float ppx = 1.9f * r, plh = ppx * 1.35f, pw = iw - 2.8f * r;
            run pr = { s->report, false, t->text };
            float ph = para_height(u, pw, plh, ppx, &pr, 1);
            if (!dry) {
                rrect(u, ix, y, ix + iw, y + ph + 2.0f * r, 1.2f * r, t->track, 1.0f);
                para(u, ix + 1.4f * r, y + r, pw, plh, ppx, &pr, 1);
            }
            y += ph + 2.0f * r;
        }
    } else {
        y += 3.0f * r;
        if (!dry)
            rrect(u, ix, y, ix + iw, y + 0.2f * r, 0.1f * r, t->track, 1.0f);
        y += 0.2f * r + 2.4f * r;
        y += notes(u, s, ix, y, iw);
    }

    /* Buttons. */
    bool quit = !s->starting && (st == RL_WU_MEASURING || st == RL_WU_COMPILING || st == RL_WU_ERROR);
    if (quit) {
        y += 3.0f * r;
        float bh = 2.4f * r * LINE_NORMAL + 2.8f * r, right = ix + iw;
        float *hq = hits ? hits->quit : NULL, *hs = hits ? hits->start : NULL;
        if (st == RL_WU_ERROR) {
            float sw = button(u, right, y, "Start anyway", true, hs);
            right -= sw + 1.8f * r;
        }
        button(u, right, y, "Quit", false, hq);
        if (s->keyboard && !dry) {
            float hp = 2.0f * r;
            text_line(u, ix, y + (bh - hp * LINE_NORMAL) / 2, hp * LINE_NORMAL, "Esc to quit", hp, false, t->muted);
        }
        y += bh;
    }
    y += pad_y + bw;
    if (!dry) {
        if (hits)
            set4(hits->card, x0, y0, cw, y - y0);
    }
    return y - y0;
}

/** @brief Paints the whole canvas in the theme's background color. */
static void fill_background(ui *u)
{
    rl_wu_canvas *c = u->c;
    uint8_t px[4] = { (uint8_t)((u->t->bg >> 16) & 255), (uint8_t)((u->t->bg >> 8) & 255), (uint8_t)(u->t->bg & 255), 255 };
    for (size_t i = 0; i < (size_t)c->w * c->h; i++)
        memcpy(c->px + i * 4, px, 4);
    grow(c, 0, 0, (int32_t)c->w, (int32_t)c->h);
}

/** @brief Draws the blocking screen: the whole canvas in the background color, the card centered. */
static void draw_blocking(ui *u, const rl_wu_scene *s, rl_wu_hits *hits)
{
    rl_wu_canvas *c = u->c;
    float r = u->r, W = (float)c->w, H = (float)c->h;
    bool portrait = H > W;
    float cw = (portrait ? 50.0f : 82.0f) * r;
    if (cw > W - 4 * r)
        cw = W - 4 * r;
    u->dry = true;
    float h = blocking(u, s, 0, 0, cw, NULL);
    u->dry = false;
    float x0 = floorf((W - cw) / 2), y0 = floorf((H - h) / 2);
    if (y0 < 0)
        y0 = 0;
    fill_background(u);
    box(u, x0, y0, x0 + cw, y0 + h, 3.6f * r, u->t->card, 1.0f, u->t->border, 0.4f * r);
    blocking(u, s, x0, y0, cw, hits);
}

/** @brief The corner card with its top left at (x0, y0) and width pw; returns its height. */
static float corner(ui *u, const rl_wu_scene *s, float x0, float y0, float pw)
{
    float r = u->r;
    const theme *t = u->t;
    float bw = 0.3f * r, padx = 2.2f * r, pady = 1.8f * r;
    float ix = x0 + bw + padx, iw = pw - 2 * (bw + padx);
    float y = y0 + bw + pady;
    int st = s->state;
    float tpx = 2.1f * r, tlh = tpx * 1.2f, mpx = 1.9f * r, mlh = mpx * LINE_NORMAL;
    char l[96], rt[96], done_t[32], total_t[32];
    rl_wu_count_text(st == RL_WU_ERROR ? s->done - s->failed : s->done, done_t, sizeof done_t);
    rl_wu_count_text(s->total, total_t, sizeof total_t);
    if (st == RL_WU_ERROR || st == RL_WU_DONE) {
        float dot = 1.6f * r;
        char title[96];
        if (st == RL_WU_ERROR)
            snprintf(title, sizeof title, "%u shader%s could not be prepared", s->failed, s->failed == 1 ? "" : "s");
        else
            snprintf(title, sizeof title, "Graphics are ready");
        run tr = { title, true, t->text };
        float th = para_height(u, iw - dot - 1.2f * r, tlh, tpx, &tr, 1);
        float rowh = th > dot ? th : dot;
        if (!u->dry) {
            rl_tp_circle(u->c, ix + dot / 2, y + rowh / 2, dot / 2, col(st == RL_WU_ERROR ? t->err : t->ok, 1.0f),
                         col(0, 0), 0, 1.0f);
            para(u, ix + dot + 1.2f * r, y + (rowh - th) / 2, iw - dot - 1.2f * r, tlh, tpx, &tr, 1);
        }
        y += rowh + r;
        y += para1(u, ix, y, iw, mlh, mpx,
                   st == RL_WU_ERROR ? "Some effects may look wrong. Details are saved to a report."
                                     : "Later launches start at once.",
                   false, t->muted);
        return y + pady + bw - y0;
    }
    y += para1(u, ix, y, iw, tlh, tpx, "Preparing graphics for your GPU", true, t->text);
    y += 1.2f * r;
    bar(u, ix, y, iw, 1.2f * r, s, false);
    y += 1.2f * r + r;
    snprintf(l, sizeof l, "%s of %s shaders", done_t, total_t);
    bool strong = right_words(s, rt, sizeof rt);
    float lw = width_of(l, mpx, false), rw = width_of(rt, mpx, false);
    text_line(u, ix, y, mlh, l, mpx, false, t->muted);
    if (lw + 1.4f * r + rw <= iw + 0.5f) {
        text_line(u, ix + iw - rw, y, mlh, rt, mpx, false, strong ? t->text : t->muted);
        y += mlh;
    } else {
        text_line(u, ix, y + mlh + r, mlh, rt, mpx, false, strong ? t->text : t->muted);
        y += 2 * mlh + r;
    }
    if (s->note) {
        y += 1.2f * r;
        if (!u->dry)
            rrect(u, ix, y, ix + iw, y + 0.2f * r, 0.1f * r, t->track, 1.0f);
        y += 0.2f * r + 1.2f * r;
        run nr[2] = { { "Starting now.", true, t->text }, { " Some scenes may pause briefly until this finishes.", false, t->muted } };
        y += para(u, ix, y, iw, mpx * 1.4f, mpx, nr, 2);
    }
    return y + pady + bw - y0;
}

/** @brief Draws the corner card with its soft shadow: top right of a wide window, top middle of a tall one. */
static void draw_corner(ui *u, const rl_wu_scene *s, rl_wu_hits *hits)
{
    rl_wu_canvas *c = u->c;
    float r = u->r, W = (float)c->w, H = (float)c->h;
    bool portrait = H > W;
    float pw = (portrait ? 48.0f : 46.0f) * r;
    if (pw > W - 2 * r)
        pw = W - 2 * r;
    u->dry = true;
    float h = corner(u, s, 0, 0, pw);
    u->dry = false;
    float x0 = floorf(portrait ? (W - pw) / 2 : W - 3 * r - pw), y0 = floorf(3 * r);
    for (int i = 8; i >= 1; i--) {
        float e = 2.4f * r * (float)i / 8.0f;
        rl_tp_rrect(c, x0 - e / 2, y0 + 0.6f * r - e / 2, x0 + pw + e / 2, y0 + 0.6f * r + h + e / 2, 2.6f * r + e / 2,
                    col(0x000000, 0.35f / 8.0f * 0.8f), col(0, 0), 0, 1.0f);
    }
    box(u, x0, y0, x0 + pw, y0 + h, 2.6f * r, u->t->pill, u->t->pill_alpha, u->t->pill_border, 0.3f * r);
    corner(u, s, x0, y0, pw);
    if (hits)
        set4(hits->card, x0, y0, pw, h);
}

void rl_wu_draw(rl_wu_canvas *c, const rl_wu_scene *s, rl_wu_hits *hits)
{
    if (hits)
        memset(hits, 0, sizeof *hits);
    if (!c || !c->px || !c->w || !c->h)
        return;
    float r = fminf((float)c->h / 90.0f, (float)c->w / 54.0f);
    if (r < 7.0f)
        r = 7.0f;
    ui u = { .c = c, .r = r, .t = s->dark ? &k_dark : &k_light, .dry = false };
    if (s->mode == RL_WARMUP_BLOCKING || s->starting)
        draw_blocking(&u, s, hits);
    else
        draw_corner(&u, s, hits);
}
