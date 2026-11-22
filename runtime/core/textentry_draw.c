/**
 * @file textentry_draw.c
 * @brief The text entry panel's fonts, layout and pixels (textentry.h).
 *
 * The game's picture keeps its full size and place; a compact panel floats
 * over it: the game's prompt in full, wrapped onto as many lines as it needs
 * (and its sub text and any hint), over one line with the field (the text and
 * a blinking caret, or the guide text in gray when empty), the character
 * count, and OK (and Cancel when the game allows it). With a physical keyboard the line sits near the
 * bottom of the view; on a touch screen it sits right above the system
 * keyboard, which covers the lower part of the view.
 */
#include "core.h"
#include "textentry_int.h"
#include "ttf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONTS_MAX 8
#define SHIPPED_FONTS 5
#define BLINK_NS 530000000ull
/** @brief The part of a touch screen's height above the system keyboard, which the system draws over the app. */
#define TOUCH_KEYBOARD_SHARE 0.5f
/** @brief Prompt and sub text sizes and line heights at scale 1. */
#define PROMPT_PX 32.0f
#define PROMPT_LINE 42.0f
#define SUB_PX 26.0f
#define SUB_LINE 34.0f

static pthread_mutex_t g_font_lock = PTHREAD_MUTEX_INITIALIZER;
static rl_ttf g_fonts[FONTS_MAX];
static uint32_t g_nfonts;
static bool g_shipped;

void rl_text_entry_add_font(const uint8_t *data, size_t size)
{
    pthread_mutex_lock(&g_font_lock);
    if (g_nfonts < FONTS_MAX && rl_ttf_init(&g_fonts[g_nfonts], data, size)) {
        memmove(&g_fonts[1], &g_fonts[0], sizeof g_fonts[0] * g_nfonts);
        rl_ttf_init(&g_fonts[0], data, size);
        g_nfonts++;
    }
    pthread_mutex_unlock(&g_font_lock);
}

/** @brief Loads the fonts the program ships, once (font lock held). */
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
    if (!g_nfonts)
        rl_log(RL_LOG_INFO, "text", "text entry: no font beside the program (fonts/0.ttf); the panel is not shown");
}

bool rl_text_entry_can_draw(void)
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

/** @brief The width of n bytes of text at size px (font lock held); secret draws dots. */
static float width_of(const char *text, size_t n, float px, bool secret)
{
    float w = 0;
    const char *p = text, *end = text + n;
    while (p < end && *p) {
        uint32_t cp = next_cp(&p), g;
        const rl_ttf *f = glyph_for(secret ? 0x2022 : cp, &g);
        if (f)
            w += rl_ttf_advance(f, g, px);
    }
    return w;
}

static float text_width(const char *text, float px)
{
    pthread_mutex_lock(&g_font_lock);
    float w = width_of(text, strlen(text), px, false);
    pthread_mutex_unlock(&g_font_lock);
    return w;
}

typedef struct canvas {
    uint8_t *px;
    int32_t w, h;
} canvas;

static void blend(canvas *c, int32_t x, int32_t y, uint32_t rgb, float a)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h || a <= 0)
        return;
    uint8_t *p = c->px + ((size_t)y * (size_t)c->w + (size_t)x) * 3;
    if (a > 1)
        a = 1;
    float col[3] = { (float)(rgb >> 16 & 0xff), (float)(rgb >> 8 & 0xff), (float)(rgb & 0xff) };
    for (int i = 0; i < 3; i++)
        p[i] = (uint8_t)((float)p[i] + (col[i] - (float)p[i]) * a + 0.5f);
}

/** @brief A rounded rectangle: fill at alpha fa, then a stroke of width sw (0 for none) inside its edge. */
static void rrect(canvas *c, const float r[4], float radius, uint32_t fill, float fa, uint32_t stroke, float sw)
{
    float cx = r[0] + r[2] / 2, cy = r[1] + r[3] / 2, hx = r[2] / 2, hy = r[3] / 2;
    if (radius > hx)
        radius = hx;
    if (radius > hy)
        radius = hy;
    int32_t x0 = (int32_t)floorf(r[0]), x1 = (int32_t)ceilf(r[0] + r[2]);
    int32_t y0 = (int32_t)floorf(r[1]), y1 = (int32_t)ceilf(r[1] + r[3]);
    for (int32_t y = y0; y < y1; y++)
        for (int32_t x = x0; x < x1; x++) {
            float qx = fabsf((float)x + 0.5f - cx) - (hx - radius), qy = fabsf((float)y + 0.5f - cy) - (hy - radius);
            float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - radius;
            float in = fminf(fmaxf(0.5f - d, 0), 1);
            if (in <= 0)
                continue;
            blend(c, x, y, fill, in * fa);
            if (sw > 0) {
                float e = fminf(fmaxf(0.5f - (fabsf(d + sw / 2) - sw / 2), 0), 1);
                blend(c, x, y, stroke, e);
            }
        }
}

/** @brief Draws text with its baseline at y from x, clipped to clip (x0, y0, x1, y1) when given; returns its width. */
static float text(canvas *c, float x, float y, const char *s, float px, uint32_t rgb, const float *clip, bool secret)
{
    pthread_mutex_lock(&g_font_lock);
    float pen = x;
    while (*s) {
        uint32_t cp = next_cp(&s), g;
        const rl_ttf *f = glyph_for(secret ? 0x2022 : cp, &g);
        if (!f)
            break;
        float base = floorf(pen);
        rl_ttf_bitmap b;
        if (rl_ttf_render(f, g, px, pen - base, &b)) {
            int32_t ox = (int32_t)base + b.x, oy = (int32_t)floorf(y) + b.y;
            for (uint32_t j = 0; j < b.h; j++)
                for (uint32_t i = 0; i < b.w; i++) {
                    int32_t X = ox + (int32_t)i, Y = oy + (int32_t)j;
                    if (clip && ((float)X < clip[0] || (float)Y < clip[1] || (float)X >= clip[2] || (float)Y >= clip[3]))
                        continue;
                    uint8_t a = b.px[(size_t)j * b.w + i];
                    if (a)
                        blend(c, X, Y, rgb, (float)a / 255.0f);
                }
            free(b.px);
        }
        pen += rl_ttf_advance(f, g, px);
    }
    pthread_mutex_unlock(&g_font_lock);
    return pen - x;
}

/** @brief Draws under (RGBA8, uw by uh) scaled into r with bilinear sampling, times k. */
static void picture(canvas *c, const uint8_t *under, uint32_t uw, uint32_t uh, const float r[4], float k)
{
    if (!under || !uw || !uh || r[2] < 1 || r[3] < 1)
        return;
    int32_t x0 = (int32_t)r[0], y0 = (int32_t)r[1], w = (int32_t)r[2], h = (int32_t)r[3];
    for (int32_t y = 0; y < h; y++) {
        float sy = ((float)y + 0.5f) * (float)uh / (float)h - 0.5f;
        int32_t iy = (int32_t)floorf(sy);
        float fy = sy - (float)iy;
        int32_t ya = iy < 0 ? 0 : iy, yb = iy + 1 >= (int32_t)uh ? (int32_t)uh - 1 : iy + 1;
        if (ya >= (int32_t)uh)
            ya = (int32_t)uh - 1;
        for (int32_t x = 0; x < w; x++) {
            float sx = ((float)x + 0.5f) * (float)uw / (float)w - 0.5f;
            int32_t ix = (int32_t)floorf(sx);
            float fx = sx - (float)ix;
            int32_t xa = ix < 0 ? 0 : ix, xb = ix + 1 >= (int32_t)uw ? (int32_t)uw - 1 : ix + 1;
            if (xa >= (int32_t)uw)
                xa = (int32_t)uw - 1;
            int32_t X = x0 + x, Y = y0 + y;
            if (X < 0 || Y < 0 || X >= c->w || Y >= c->h)
                continue;
            const uint8_t *p00 = under + ((size_t)ya * uw + (size_t)xa) * 4, *p10 = under + ((size_t)ya * uw + (size_t)xb) * 4;
            const uint8_t *p01 = under + ((size_t)yb * uw + (size_t)xa) * 4, *p11 = under + ((size_t)yb * uw + (size_t)xb) * 4;
            uint8_t *o = c->px + ((size_t)Y * (size_t)c->w + (size_t)X) * 3;
            for (int i = 0; i < 3; i++) {
                float v = ((float)p00[i] * (1 - fx) + (float)p10[i] * fx) * (1 - fy) +
                          ((float)p01[i] * (1 - fx) + (float)p11[i] * fx) * fy;
                o[i] = (uint8_t)(v * k + 0.5f);
            }
        }
    }
}

static void set4(float *r, float x, float y, float w, float h)
{
    r[0] = x;
    r[1] = y;
    r[2] = w;
    r[3] = h;
}

/** @brief The count line beside the field: "n of max characters". */
static void count_line(char *out, size_t size, uint32_t n)
{
    if (rl_te.max_chars)
        snprintf(out, size, "%u of %u characters", n, rl_te.max_chars);
    else
        snprintf(out, size, "%u character%s", n, n == 1 ? "" : "s");
}

/**
 * @brief Splits UTF-8 text into lines no wider than width at size px: at the last space that fits, or between two
 * characters when a word alone is too wide. Fills starts and lens (at most max lines); returns the line count.
 */
static uint32_t wrap(const char *t, float width, float px, size_t *starts, size_t *lens, uint32_t max)
{
    uint32_t n = 0;
    size_t at = 0, len = strlen(t);
    while (at < len && n < max) {
        while (t[at] == ' ')
            at++;
        if (at >= len)
            break;
        size_t end = at, last_space = 0;
        while (end < len) {
            size_t next = end + rl_te_seq((unsigned char)t[end]);
            if (next > len)
                next = len;
            pthread_mutex_lock(&g_font_lock);
            float w = width_of(t + at, next - at, px, false);
            pthread_mutex_unlock(&g_font_lock);
            if (w > width && end > at)
                break;
            if (t[end] == ' ')
                last_space = end;
            end = next;
        }
        if (end < len && last_space > at)
            end = last_space;
        starts[n] = at;
        lens[n] = end - at;
        n++;
        at = end;
    }
    return n;
}

void rl_te_layout(uint32_t width, uint32_t height, bool touch, rl_te_place *out)
{
    memset(out, 0, sizeof *out);
    float W = (float)width, H = (float)height;
    float s = touch ? fminf(W, H) / 1080.0f : fminf(W / 1920.0f, H / 1080.0f);
    out->s = s;
    set4(out->picture, 0, 0, W, H);
    out->keyboard_top = touch ? floorf(H * TOUCH_KEYBOARD_SHARE) : H;
    float pad = 14 * s, fh = 72 * s, bw = 170 * s, gap = 14 * s, inset = 24 * s;
    float pw = fminf(W - 2 * inset, 1600 * s);
    float x0 = floorf((W - pw) / 2);
    float text_w = pw - 2 * pad - 12 * s;
    const char *header = rl_te.header[0] ? rl_te.header : "Enter text";
    out->nheader = wrap(header, text_w, PROMPT_PX * s, out->header_at, out->header_len, RL_TE_LINES);
    out->nsub = rl_te.sub[0] ? wrap(rl_te.sub, text_w, SUB_PX * s, out->sub_at, out->sub_len, RL_TE_LINES) : 0;
    uint32_t n = rl_te_chars(rl_te.text);
    out->hint = rl_te.message[0] || n < rl_te.min_chars;
    float strip = (float)out->nheader * PROMPT_LINE * s + (float)out->nsub * SUB_LINE * s + (out->hint ? SUB_LINE * s : 0);
    float ph = pad + strip + gap + fh + pad;
    float bottom = touch ? out->keyboard_top - 12 * s : H - 48 * s;
    float y0 = bottom - ph;
    set4(out->panel, x0, y0, pw, ph);
    out->strip_y = y0 + pad;
    float right = x0 + pw - pad, fy = bottom - pad - fh;
    set4(out->button[RL_TE_OK], right - bw, fy, bw, fh);
    out->has_button[RL_TE_OK] = true;
    right -= bw + gap;
    if (rl_te.can_cancel) {
        set4(out->button[RL_TE_CANCEL], right - bw, fy, bw, fh);
        out->has_button[RL_TE_CANCEL] = true;
        right -= bw + gap;
    }
    char count[64];
    count_line(count, sizeof count, n);
    right -= text_width(count, 28 * s) + gap;
    set4(out->field, x0 + pad, fy, right - (x0 + pad), fh);
}

#define WHITE 0xF0F2F6u
#define GRAY 0xAAB0BCu
#define DIM 0x6E727Cu
#define BLUE 0x60A5FAu
#define RED 0xF87171u
#define PANEL 0x181B21u
#define EDGE 0x5C626Eu
#define FIELD 0x0A0C10u
#define KEY 0x3A3E46u

/** @brief The hint left of the count: the guest's message, or the minimum when the text is shorter. */
static bool hint_line(char *out, size_t size, uint32_t n, uint32_t *rgb)
{
    if (rl_te.message[0]) {
        snprintf(out, size, "%s", rl_te.message);
        *rgb = RED;
        return true;
    }
    if (n < rl_te.min_chars) {
        snprintf(out, size, "At least %u character%s", rl_te.min_chars, rl_te.min_chars == 1 ? "" : "s");
        *rgb = GRAY;
        return true;
    }
    return false;
}

/** @brief The field: text or guide, scrolled to keep the caret in view, and the caret. */
static void field(canvas *c, const rl_te_place *pl, float px, bool caret_on)
{
    const float *f = pl->field;
    float s = pl->s;
    rrect(c, f, 14 * s, FIELD, 1, BLUE, fmaxf(3 * s, 1.5f));
    float inner = 28 * s, avail = f[2] - 2 * inner;
    float base = f[1] + f[3] / 2 + px * 0.36f;
    float clip[4] = { f[0] + 4 * s, f[1] + 4 * s, f[0] + f[2] - 4 * s, f[1] + f[3] - 4 * s };
    pthread_mutex_lock(&g_font_lock);
    float caret_x = width_of(rl_te.text, rl_te.caret, px, rl_te.secret);
    pthread_mutex_unlock(&g_font_lock);
    float shift = caret_x > avail ? caret_x - avail : 0;
    float tx = f[0] + inner - shift;
    if (rl_te.text[0])
        text(c, tx, base, rl_te.text, px, WHITE, clip, rl_te.secret);
    else if (rl_te.guide[0])
        text(c, tx, base, rl_te.guide, px, DIM, clip, false);
    if (caret_on) {
        float cx = tx + caret_x + 2 * s, half = px * 0.62f;
        float r[4] = { cx, f[1] + f[3] / 2 - half, fmaxf(4 * s, 2), 2 * half };
        rrect(c, r, 0, BLUE, 1, 0, 0);
    }
}

/** @brief The floating one-line panel: prompt, field, count, buttons, and a hint line under them when there is one. */
/** @brief The floating panel: the prompt (and sub text, and a hint) wrapped in a strip, over one line of field, count and buttons. */
static void floating_panel(canvas *c, const rl_te_place *pl, uint32_t n, bool caret_on)
{
    float s = pl->s;
    const float *p = pl->panel, *f = pl->field;
    rrect(c, p, 22 * s, PANEL, 0.92f, EDGE, fmaxf(2 * s, 1));
    float pad = 14 * s, gap = 14 * s, x = p[0] + pad + 6 * s, y = pl->strip_y;
    const char *header = rl_te.header[0] ? rl_te.header : "Enter text";
    char line[1100];
    for (uint32_t i = 0; i < pl->nheader; i++) {
        snprintf(line, sizeof line, "%.*s", (int)pl->header_len[i], header + pl->header_at[i]);
        text(c, x, y + PROMPT_LINE * s * 0.72f, line, PROMPT_PX * s, WHITE, NULL, false);
        y += PROMPT_LINE * s;
    }
    for (uint32_t i = 0; i < pl->nsub; i++) {
        snprintf(line, sizeof line, "%.*s", (int)pl->sub_len[i], rl_te.sub + pl->sub_at[i]);
        text(c, x, y + SUB_LINE * s * 0.72f, line, SUB_PX * s, GRAY, NULL, false);
        y += SUB_LINE * s;
    }
    uint32_t hc;
    if (pl->hint && hint_line(line, sizeof line, n, &hc))
        text(c, x, y + SUB_LINE * s * 0.72f, line, SUB_PX * s, hc, NULL, false);
    float mid = f[1] + f[3] / 2;
    field(c, pl, 40 * s, caret_on);
    count_line(line, sizeof line, n);
    text(c, f[0] + f[2] + gap, mid + 10 * s, line, 28 * s, GRAY, NULL, false);
    for (int b = 0; b <= RL_TE_CANCEL; b++) {
        if (!pl->has_button[b])
            continue;
        const float *r = pl->button[b];
        bool ok = b == RL_TE_OK, on = !ok || n >= rl_te.min_chars;
        rrect(c, r, r[3] / 2, ok ? (on ? BLUE : 0x464C58u) : KEY, 1, 0, 0);
        const char *label = ok ? rl_te.ok : "Cancel";
        float tw = text_width(label, 32 * s);
        text(c, r[0] + (r[2] - tw) / 2, mid + 11 * s, label, 32 * s, 0xFFFFFFu, NULL, false);
    }
}

bool rl_text_entry_draw(uint8_t *rgb, uint32_t width, uint32_t height, const uint8_t *under, uint32_t under_w,
                        uint32_t under_h, bool touch, uint64_t now_ns)
{
    if (!rl_text_entry_can_draw() || !width || !height)
        return false;
    pthread_mutex_lock(&rl_te_lock);
    if (!rl_te.open) {
        pthread_mutex_unlock(&rl_te_lock);
        return false;
    }
    canvas c = { rgb, (int32_t)width, (int32_t)height };
    memset(rgb, 0, (size_t)width * height * 3);
    rl_te_place pl;
    rl_te_layout(width, height, touch, &pl);
    picture(&c, under, under_w, under_h, pl.picture, 1.0f);
    uint32_t n = rl_te_chars(rl_te.text);
    bool caret_on = ((now_ns - rl_te.typed_ns) / BLINK_NS) % 2 == 0;
    floating_panel(&c, &pl, n, caret_on);
    pthread_mutex_unlock(&rl_te_lock);
    return true;
}
