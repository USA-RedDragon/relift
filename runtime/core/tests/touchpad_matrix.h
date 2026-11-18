/**
 * @file touchpad_matrix.h
 * @brief Checks of a guest's fitted on-screen controller over a matrix of
 * view sizes, densities, orientations and picture shapes: no two touch areas
 * meet, none meets the picture, every one lies in the safe area, sizes stay
 * above a minimum in dp, and the picture keeps a fair share of the view (a
 * whole-multiple picture may lose a step or two of scale).
 * RL_TOUCHPAD_PNG names a folder to draw a few of the views to as PNG files:
 * the picture as flat rectangles, the overlay, and the touch areas outlined.
 */
#ifndef RELIFT_TOUCHPAD_MATRIX_H
#define RELIFT_TOUCHPAD_MATRIX_H

#include "runner.h"

#include <relift/settings.h>
#include <relift/touchpad.h>

#include "touchpad_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct tpm_view {
    const char *name;
    uint32_t w, h;
    float ppd;
    int32_t safe[4];
} tpm_view;

typedef struct tpm_picture {
    uint32_t w, h;
    bool whole;
} tpm_picture;

static const tpm_view g_tpm_views[] = {
    { "phone-720x1280", 720, 1280, 2.0f, { 0, 48, 0, 0 } },
    { "phone-720x1560", 720, 1560, 2.0f, { 0, 48, 0, 48 } },
    { "phone-1080x1920", 1080, 1920, 2.625f, { 0, 63, 0, 0 } },
    { "phone-1080x2400", 1080, 2400, 2.625f, { 0, 96, 0, 63 } },
    { "phone-1080x2520", 1080, 2520, 2.625f, { 0, 96, 0, 63 } },
    { "phone-1440x3200", 1440, 3200, 3.5f, { 0, 120, 0, 84 } },
    { "phone-landscape-1280x720", 1280, 720, 2.0f, { 48, 0, 0, 0 } },
    { "phone-landscape-1560x720", 1560, 720, 2.0f, { 96, 0, 0, 48 } },
    { "phone-landscape-1920x1080", 1920, 1080, 2.625f, { 63, 0, 0, 0 } },
    { "phone-landscape-2400x1080", 2400, 1080, 2.625f, { 96, 0, 63, 48 } },
    { "phone-landscape-2520x1080", 2520, 1080, 2.625f, { 96, 0, 63, 48 } },
    { "phone-landscape-3200x1440", 3200, 1440, 3.5f, { 120, 0, 84, 63 } },
    { "tablet-4x3-1536x2048", 1536, 2048, 2.0f, { 0, 48, 0, 0 } },
    { "tablet-4x3-2048x1536", 2048, 1536, 2.0f, { 0, 48, 0, 0 } },
    { "tablet-16x10-1600x2560", 1600, 2560, 2.0f, { 0, 48, 0, 48 } },
    { "tablet-16x10-2560x1600", 2560, 1600, 2.0f, { 0, 48, 0, 48 } },
    { "tablet-small-1200x1920", 1200, 1920, 1.5f, { 0, 36, 0, 0 } },
    { "tablet-small-1920x1200", 1920, 1200, 1.5f, { 0, 36, 0, 0 } },
    { "squarish-1240x1080", 1240, 1080, 2.0f, { 0, 0, 0, 0 } },
    { "squarish-1080x1240", 1080, 1240, 2.0f, { 0, 0, 0, 0 } },
    { "desktop-1280x720", 1280, 720, 1.0f, { 0, 0, 0, 0 } },
    { "desktop-1920x1080", 1920, 1080, 1.0f, { 0, 0, 0, 0 } },
    { "desktop-1920x1080-hidpi", 1920, 1080, 1.5f, { 0, 0, 0, 0 } },
    { "desktop-1000x1000", 1000, 1000, 1.0f, { 0, 0, 0, 0 } },
};

typedef struct tpm_shape {
    bool round;
    float x0, y0, x1, y1;
} tpm_shape;

static bool tpm_round(uint8_t shape) { return shape == RL_TOUCHPAD_BUTTON || shape == RL_TOUCHPAD_STICK; }

static bool tpm_meet(const tpm_shape *a, const tpm_shape *b)
{
    float ax = (a->x0 + a->x1) / 2, ay = (a->y0 + a->y1) / 2, bx = (b->x0 + b->x1) / 2, by = (b->y0 + b->y1) / 2;
    if (a->round && b->round)
        return hypotf(ax - bx, ay - by) < (a->x1 - a->x0) / 2 + (b->x1 - b->x0) / 2 - 0.01f;
    if (!a->round && !b->round)
        return a->x0 < b->x1 - 0.01f && b->x0 < a->x1 - 0.01f && a->y0 < b->y1 - 0.01f && b->y0 < a->y1 - 0.01f;
    const tpm_shape *c = a->round ? a : b, *r = a->round ? b : a;
    float cx = (c->x0 + c->x1) / 2, cy = (c->y0 + c->y1) / 2, rad = (c->x1 - c->x0) / 2;
    float nx = fminf(fmaxf(cx, r->x0), r->x1), ny = fminf(fmaxf(cy, r->y0), r->y1);
    return hypotf(cx - nx, cy - ny) < rad - 0.01f;
}

static void tpm_png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint8_t len[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    fwrite(len, 1, 4, f);
    fwrite(type, 1, 4, f);
    uint32_t crc = 0xffffffffu;
    for (int i = 0; i < 4; i++)
        crc = table[(crc ^ (uint8_t)type[i]) & 255] ^ (crc >> 8);
    for (uint32_t i = 0; i < n; i++)
        crc = table[(crc ^ data[i]) & 255] ^ (crc >> 8);
    crc = ~crc;
    if (n)
        fwrite(data, 1, n, f);
    uint8_t c[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc };
    fwrite(c, 1, 4, f);
}

/** Writes RGB8 pixels as a PNG with stored (uncompressed) deflate blocks. */
static void tpm_png_write(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24),
                         (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
    tpm_png_chunk(f, "IHDR", ihdr, 13);
    size_t raw = (size_t)h * (1 + (size_t)w * 3), blocks = (raw + 65534) / 65535;
    uint8_t *z = malloc(2 + raw + blocks * 5 + 4);
    if (!z) {
        fclose(f);
        return;
    }
    size_t n = 0;
    z[n++] = 0x78;
    z[n++] = 0x01;
    uint32_t a = 1, b = 0;
    size_t at = 0;
    while (at < raw) {
        size_t len = raw - at < 65535 ? raw - at : 65535;
        z[n++] = at + len >= raw;
        z[n++] = (uint8_t)len;
        z[n++] = (uint8_t)(len >> 8);
        z[n++] = (uint8_t)~len;
        z[n++] = (uint8_t)(~len >> 8);
        for (size_t i = 0; i < len; i++, at++) {
            size_t row = at / (1 + (size_t)w * 3), col = at % (1 + (size_t)w * 3);
            uint8_t v = col == 0 ? 0 : rgb[row * (size_t)w * 3 + col - 1];
            z[n++] = v;
            a = (a + v) % 65521u;
            b = (b + a) % 65521u;
        }
    }
    uint32_t ad = (b << 16) | a;
    z[n++] = (uint8_t)(ad >> 24);
    z[n++] = (uint8_t)(ad >> 16);
    z[n++] = (uint8_t)(ad >> 8);
    z[n++] = (uint8_t)ad;
    tpm_png_chunk(f, "IDAT", z, (uint32_t)n);
    tpm_png_chunk(f, "IEND", NULL, 0);
    free(z);
    fclose(f);
}

static void tpm_fill(uint8_t *rgb, uint32_t w, uint32_t h, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t r,
                     uint8_t g, uint8_t b)
{
    for (int32_t y = y0 < 0 ? 0 : y0; y < y1 && y < (int32_t)h; y++)
        for (int32_t x = x0 < 0 ? 0 : x0; x < x1 && x < (int32_t)w; x++) {
            uint8_t *p = rgb + ((size_t)y * w + (size_t)x) * 3;
            p[0] = r;
            p[1] = g;
            p[2] = b;
        }
}

static void tpm_ring(uint8_t *rgb, uint32_t w, uint32_t h, float cx, float cy, float rad, uint8_t r, uint8_t g, uint8_t b)
{
    for (int32_t y = (int32_t)(cy - rad) - 1; y <= (int32_t)(cy + rad) + 1; y++)
        for (int32_t x = (int32_t)(cx - rad) - 1; x <= (int32_t)(cx + rad) + 1; x++) {
            float d = hypotf((float)x + 0.5f - cx, (float)y + 0.5f - cy);
            if (d >= rad - 0.7f && d < rad + 0.7f && x >= 0 && y >= 0 && x < (int32_t)w && y < (int32_t)h) {
                uint8_t *p = rgb + ((size_t)y * w + (size_t)x) * 3;
                p[0] = r;
                p[1] = g;
                p[2] = b;
            }
        }
}

static void tpm_frame(uint8_t *rgb, uint32_t w, uint32_t h, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t r,
                      uint8_t g, uint8_t b)
{
    tpm_fill(rgb, w, h, x0, y0, x1, y0 + 1, r, g, b);
    tpm_fill(rgb, w, h, x0, y1 - 1, x1, y1, r, g, b);
    tpm_fill(rgb, w, h, x0, y0, x0 + 1, y1, r, g, b);
    tpm_fill(rgb, w, h, x1 - 1, y0, x1, y1, r, g, b);
}

/** Draws the view: the picture as flat screens, the overlay over it, the touch areas and the safe area outlined. */
static void tpm_draw(const char *dir, const char *guest, const tpm_view *v, const tpm_picture *pic, const char *tag)
{
    uint32_t ow, oh;
    int32_t dirty[4];
    const uint8_t *ov = rl_touchpad_pixels(&ow, &oh, dirty);
    if (!ov || ow != v->w || oh != v->h)
        return;
    uint8_t *rgb = calloc((size_t)v->w * v->h, 3);
    if (!rgb)
        return;
    int32_t r[4];
    rl_touchpad_picture(r);
    bool stacked = pic->w * 3 == pic->h * 2;
    if (stacked) {
        tpm_fill(rgb, v->w, v->h, r[0], r[1], r[0] + r[2], r[1] + r[3] / 2, 40, 90, 140);
        tpm_fill(rgb, v->w, v->h, r[0], r[1] + r[3] / 2, r[0] + r[2], r[1] + r[3], 50, 130, 80);
    } else {
        tpm_fill(rgb, v->w, v->h, r[0], r[1], r[0] + r[2], r[1] + r[3], 40, 90, 140);
        if (pic->w > pic->h * 3 / 2 && !strcmp(guest, "nds"))
            tpm_fill(rgb, v->w, v->h, r[0] + r[2] / 2, r[1], r[0] + r[2], r[1] + r[3], 50, 130, 80);
    }
    for (size_t i = 0; i < (size_t)v->w * v->h; i++) {
        const uint8_t *p = ov + i * 4;
        uint32_t k = 255 - p[3];
        rgb[i * 3] = (uint8_t)(p[0] + rgb[i * 3] * k / 255);
        rgb[i * 3 + 1] = (uint8_t)(p[1] + rgb[i * 3 + 1] * k / 255);
        rgb[i * 3 + 2] = (uint8_t)(p[2] + rgb[i * 3 + 2] * k / 255);
    }
    rl_touchpad_rect rc[32];
    uint32_t n = rl_touchpad_placed(rc, 32);
    for (uint32_t i = 0; i < n; i++) {
        if (tpm_round(rc[i].shape))
            tpm_ring(rgb, v->w, v->h, (rc[i].hit_x0 + rc[i].hit_x1) / 2, (rc[i].hit_y0 + rc[i].hit_y1) / 2,
                     (rc[i].hit_x1 - rc[i].hit_x0) / 2, 230, 70, 70);
        else
            tpm_frame(rgb, v->w, v->h, (int32_t)rc[i].hit_x0, (int32_t)rc[i].hit_y0, (int32_t)rc[i].hit_x1,
                      (int32_t)rc[i].hit_y1, 230, 70, 70);
    }
    tpm_frame(rgb, v->w, v->h, v->safe[0], v->safe[1], (int32_t)v->w - v->safe[2], (int32_t)v->h - v->safe[3], 240,
              200, 60);
    tpm_frame(rgb, v->w, v->h, r[0], r[1], r[0] + r[2], r[1] + r[3], 255, 255, 255);
    char path[700];
    snprintf(path, sizeof path, "%s/touchpad-%s-%s-%s.png", dir, guest, tag, v->name);
    tpm_png_write(path, rgb, v->w, v->h);
    free(rgb);
}

/** Why the controls of the current view are wrong, or NULL. */
static const char *tpm_problem(const tpm_view *v, float full_scale, float picture_scale, bool whole, const int32_t pic[4])
{
    static char why[200];
    rl_touchpad_rect rc[32];
    uint32_t n = rl_touchpad_placed(rc, 32);
    tpm_shape hs[32], ds[32];
    if (n == 0)
        return "no controls placed";
    for (uint32_t i = 0; i < n; i++) {
        bool round = tpm_round(rc[i].shape);
        hs[i] = (tpm_shape){ round, rc[i].hit_x0, rc[i].hit_y0, rc[i].hit_x1, rc[i].hit_y1 };
        ds[i] = (tpm_shape){ round, rc[i].x0, rc[i].y0, rc[i].x1, rc[i].y1 };
        const char *name = rc[i].label ? rc[i].label : rc[i].control ? rc[i].control : "shape";
        float dw = (rc[i].x1 - rc[i].x0) / v->ppd, dh = (rc[i].y1 - rc[i].y0) / v->ppd;
        float hw = (rc[i].hit_x1 - rc[i].hit_x0) / v->ppd, hh = (rc[i].hit_y1 - rc[i].hit_y0) / v->ppd;
        if (rc[i].hit_x0 < (float)v->safe[0] - 0.5f || rc[i].hit_y0 < (float)v->safe[1] - 0.5f ||
            rc[i].hit_x1 > (float)(v->w - (uint32_t)v->safe[2]) + 0.5f ||
            rc[i].hit_y1 > (float)(v->h - (uint32_t)v->safe[3]) + 0.5f) {
            snprintf(why, sizeof why, "%s leaves the safe area", name);
            return why;
        }
        if (rc[i].hit_x0 > rc[i].x0 + 0.01f || rc[i].hit_y0 > rc[i].y0 + 0.01f || rc[i].hit_x1 < rc[i].x1 - 0.01f ||
            rc[i].hit_y1 < rc[i].y1 - 0.01f) {
            snprintf(why, sizeof why, "%s takes touches on less than it draws", name);
            return why;
        }
        float min_draw = round ? 36.0f : rc[i].shape == RL_TOUCHPAD_DPAD ? 90.0f : 24.0f;
        if ((round ? dw : dh) < min_draw || hw < 32.0f || hh < 32.0f) {
            snprintf(why, sizeof why, "%s is too small: %.1f by %.1f dp, touch area %.1f by %.1f", name, dw, dh, hw, hh);
            return why;
        }
        for (uint32_t j = 0; j < i; j++)
            if (tpm_meet(&hs[i], &hs[j])) {
                const char *other = rc[j].label ? rc[j].label : rc[j].control ? rc[j].control : "shape";
                snprintf(why, sizeof why, "touch areas of %s and %s meet", name, other);
                return why;
            }
        tpm_shape p = { false, (float)pic[0], (float)pic[1], (float)(pic[0] + pic[2]), (float)(pic[1] + pic[3]) };
        if (pic[2] > 0 && tpm_meet(&hs[i], &p)) {
            snprintf(why, sizeof why, "%s meets the picture", name);
            return why;
        }
    }
    if (picture_scale < full_scale * (whole ? 0.3f : 0.45f)) {
        snprintf(why, sizeof why, "the picture shrinks to %.2f of its full size", picture_scale / full_scale);
        return why;
    }
    return NULL;
}

static void tpm_setup(const rl_touchpad_guest *guest, const tpm_view *v, const tpm_picture *pic)
{
    rl_touchpad_reset();
    rl_settings_reset();
    rl_touchpad_declare(guest);
    rl_settings_set("input.touch_pad.show", "always");
    rl_touchpad_set_picture(pic->w, pic->h, pic->whole);
    rl_touchpad_view(v->w, v->h, v->ppd, v->safe);
}

/**
 * Runs every view of the matrix with each picture shape and the default
 * settings, then with the button size at its smallest and largest; returns
 * the number of views with a problem after printing each.
 */
static int tpm_run(const rl_touchpad_guest *guest, const tpm_picture *pics, size_t npics)
{
    int bad = 0;
    const char *dir = getenv("RL_TOUCHPAD_PNG");
    static const char *const sizes[] = { "100", "70", "150" };
    for (size_t z = 0; z < 3; z++)
        for (size_t pi = 0; pi < npics; pi++)
            for (size_t vi = 0; vi < sizeof g_tpm_views / sizeof g_tpm_views[0]; vi++) {
                const tpm_view *v = &g_tpm_views[vi];
                tpm_setup(guest, v, &pics[pi]);
                rl_settings_set("input.touch_pad.size", sizes[z]);
                int32_t pic[4];
                rl_touchpad_picture(pic);
                float full = fminf((float)v->w / (float)pics[pi].w, (float)v->h / (float)pics[pi].h);
                if (pics[pi].whole && full >= 1.0f)
                    full = floorf(full);
                float got = pics[pi].w ? (float)pic[2] / (float)pics[pi].w : 0.0f;
                const char *why = tpm_problem(v, full, got, pics[pi].whole, pic);
                if (why) {
                    fprintf(stderr, "  %s %s picture %ux%u size %s%%: %s\n", guest->name, v->name, pics[pi].w,
                            pics[pi].h, sizes[z], why);
                    bad++;
                }
                if (dir && z == 0 && pi == 0)
                    tpm_draw(dir, guest->name, v, &pics[pi], "default");
            }
    rl_touchpad_reset();
    return bad;
}

#endif
