/**
 * @file vic.c
 * @brief VIC, the video image compositor: reads the surfaces of the enabled
 * slots, converts them with each slot's color matrix, clamps them, places
 * them in the target rectangle over the background color and writes the
 * output surface.
 *
 * The method register offsets, the configuration structure, the pixel
 * format numbers, the 8-bit to 10-bit pixel path and the order of the steps
 * follow Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors)
 * Ryujinx.Graphics.Vic. Deinterlacing, scaling and the blending factors are
 * not implemented: slots must be progressive and unscaled.
 */
#include "host1x.h"

#include <stdlib.h>
#include <string.h>

enum {
    R_SURFACE_SLOT = 0x100,
    R_CONFIG_STRUCT = 0x1C2,
    R_OUTPUT_SURFACE = 0x1C8
};

enum {
    PF_A8B8G8R8 = 31,
    PF_A8R8G8B8 = 32,
    PF_X8B8G8R8 = 35,
    PF_Y8_U8V8_N420 = 67,
    PF_Y8_V8U8_N420 = 68
};

#define CONFIG_SIZE 0x610u
#define SLOT_BASE 0x90u
#define SLOT_SIZE 0xB0u
#define SLOTS 8u

typedef struct pixel {
    uint16_t r, g, b, a;
} pixel;

typedef struct matrix {
    int32_t c[3][4];
    uint32_t shift;
    bool enable;
} matrix;

typedef struct surface_config {
    uint32_t format, blk_kind, blk_height;
    uint32_t luma_w, luma_h, chroma_w, chroma_h;
    uint32_t surface_w, surface_h;
} surface_config;

static pixel *frame;
static size_t frame_n;

static uint64_t bits(const uint8_t *s, uint32_t lo, uint32_t n)
{
    uint64_t w;
    memcpy(&w, s + (lo / 64) * 8, 8);
    w >>= lo % 64;
    return n == 64 ? w : w & ((1ull << n) - 1);
}

static int32_t sbits(const uint8_t *s, uint32_t lo, uint32_t n)
{
    uint64_t v = bits(s, lo, n);
    return (int32_t)(v & (1ull << (n - 1)) ? v - (1ull << n) : v);
}

static matrix read_matrix(const uint8_t *m)
{
    matrix x;
    for (uint32_t col = 0; col < 4; col++)
        for (uint32_t row = 0; row < 3; row++)
            x.c[row][col] = sbits(m, col * 64 + row * 20, 20);
    x.shift = (uint32_t)bits(m, 60, 4);
    x.enable = bits(m, 127, 1);
    return x;
}

static surface_config read_surface_config(const uint8_t *s)
{
    return (surface_config){
        .format = (uint32_t)bits(s, 0, 7),
        .blk_kind = (uint32_t)bits(s, 11, 4),
        .blk_height = (uint32_t)bits(s, 15, 4),
        .surface_w = (uint32_t)bits(s, 32, 14) + 1,
        .surface_h = (uint32_t)bits(s, 46, 14) + 1,
        .luma_w = (uint32_t)bits(s, 64, 14) + 1,
        .luma_h = (uint32_t)bits(s, 78, 14) + 1,
        .chroma_w = (uint32_t)bits(s, 96, 14) + 1,
        .chroma_h = (uint32_t)bits(s, 110, 14) + 1,
    };
}

static uint32_t pitch_of(uint32_t width, uint32_t bpp) { return (width * bpp + 255) & ~255u; }

/** @brief One plane of a slot or the output: block-linear or pitch-linear. */
typedef struct plane {
    h1x_bl bl;
    uint32_t pitch;
    bool linear;
} plane;

static bool plane_of(plane *p, uint32_t addr, const surface_config *c, uint32_t w, uint32_t h, uint32_t bpp)
{
    p->linear = c->blk_kind == 0;
    p->pitch = pitch_of(w, bpp);
    p->bl = (h1x_bl){ NULL, w * bpp, h, c->blk_height };
    uint64_t size = p->linear ? (uint64_t)p->pitch * h : h1x_bl_size(w * bpp, h, c->blk_height);
    p->bl.base = addr ? h1x_ptr((uint64_t)addr << 8, size) : NULL;
    return p->bl.base != NULL;
}

static void plane_read(const plane *p, uint32_t y, uint8_t *dst, uint32_t n)
{
    if (p->linear)
        memcpy(dst, p->bl.base + (size_t)y * p->pitch, n);
    else
        h1x_bl_read_row(&p->bl, 0, y, dst, n);
}

static void plane_write(const plane *p, uint32_t y, const uint8_t *src, uint32_t n)
{
    if (p->linear)
        memcpy(p->bl.base + (size_t)y * p->pitch, src, n);
    else
        h1x_bl_write_row(&p->bl, 0, y, src, n);
}

static uint16_t up(uint8_t v) { return (uint16_t)(v << 2); }

static uint8_t down(uint16_t v) { return (uint8_t)(v >> 2); }

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static void blend_slot(const uint8_t *slot, const uint32_t *regs, uint32_t si, uint32_t out_w, uint32_t out_h, const uint8_t *out_cfg)
{
    surface_config sc = read_surface_config(slot + 0x40);
    if (sc.format != PF_Y8_V8U8_N420 && sc.format != PF_Y8_U8V8_N420) {
        rl_log(RL_LOG_WARN, "vic", "slot %u: pixel format %u is not supported", si, sc.format);
        return;
    }
    if (bits(slot, 16, 4) != 0)
        rl_log(RL_LOG_WARN, "vic", "slot %u: frame format %u is not progressive", si, (uint32_t)bits(slot, 16, 4));
    const uint32_t *offs = &regs[R_SURFACE_SLOT + si * 8 * 3];
    plane luma, chroma;
    if (!plane_of(&luma, offs[0], &sc, sc.luma_w, sc.luma_h, 1) || !plane_of(&chroma, offs[1], &sc, sc.chroma_w, sc.chroma_h, 2)) {
        rl_log(RL_LOG_WARN, "vic", "slot %u: surface not mapped", si);
        return;
    }
    matrix m = read_matrix(slot + 0x60);
    int32_t lo = (int32_t)bits(slot, 128, 10), hi = (int32_t)bits(slot, 138, 10);
    uint32_t x1 = (uint32_t)bits(out_cfg, 64, 14), x2 = (uint32_t)bits(out_cfg, 80, 14) + 1;
    uint32_t y1 = (uint32_t)bits(out_cfg, 96, 14), y2 = (uint32_t)bits(out_cfg, 112, 14) + 1;
    uint32_t tx = x1 < x2 ? x1 : x2, ty = y1 < y2 ? y1 : y2;
    uint32_t tw = x1 < x2 ? x2 - x1 : x1 - x2, th = y1 < y2 ? y2 - y1 : y1 - y2;
    if (tx >= out_w || ty >= out_h)
        return;
    if (tw > out_w - tx)
        tw = out_w - tx;
    if (th > out_h - ty)
        th = out_h - ty;
    uint32_t xe = tx + tw < sc.luma_w ? tx + tw : sc.luma_w, ye = ty + th < sc.luma_h ? ty + th : sc.luma_h;
    bool vu = sc.format == PF_Y8_V8U8_N420;
    uint8_t *yrow = malloc(sc.luma_w), *crow = malloc((size_t)sc.chroma_w * 2);
    if (!yrow || !crow) {
        free(yrow);
        free(crow);
        return;
    }
    for (uint32_t y = ty; y < ye; y++) {
        plane_read(&luma, y, yrow, sc.luma_w);
        plane_read(&chroma, y >> 1 < sc.chroma_h ? y >> 1 : sc.chroma_h - 1, crow, sc.chroma_w * 2);
        pixel *o = frame + (size_t)y * out_w;
        for (uint32_t x = tx; x < xe; x++) {
            uint32_t cx = (x >> 1) < sc.chroma_w ? (x >> 1) : sc.chroma_w - 1;
            int32_t in[3] = { up(yrow[x]), up(crow[2 * cx + (vu ? 0 : 1)]), up(crow[2 * cx + (vu ? 1 : 0)]) };
            int32_t c[3];
            for (int k = 0; k < 3; k++) {
                if (m.enable) {
                    int32_t v = in[0] * m.c[k][0] + in[1] * m.c[k][1] + in[2] * m.c[k][2];
                    v >>= m.shift;
                    v += m.c[k][3];
                    c[k] = v >> 8;
                } else {
                    c[k] = in[k];
                }
                c[k] = clamp(c[k], lo, hi);
            }
            o[x] = (pixel){ (uint16_t)c[0], (uint16_t)c[1], (uint16_t)c[2], 0x3FF };
        }
    }
    free(yrow);
    free(crow);
}

static void write_rgba(const plane *p, uint32_t w, uint32_t h, uint32_t format)
{
    uint8_t *row = malloc((size_t)w * 4);
    if (!row)
        return;
    bool bgra = format == PF_A8R8G8B8;
    for (uint32_t y = 0; y < h; y++) {
        const pixel *s = frame + (size_t)y * w;
        for (uint32_t x = 0; x < w; x++) {
            row[4 * x + 0] = down(bgra ? s[x].b : s[x].r);
            row[4 * x + 1] = down(s[x].g);
            row[4 * x + 2] = down(bgra ? s[x].r : s[x].b);
            row[4 * x + 3] = down(s[x].a);
        }
        plane_write(p, y, row, w * 4);
    }
    free(row);
}

static void write_nv12(const plane *luma, const plane *chroma, uint32_t w, uint32_t h, uint32_t cw, uint32_t ch, bool vu)
{
    uint8_t *row = malloc((size_t)(w > cw * 2 ? w : cw * 2));
    if (!row)
        return;
    for (uint32_t y = 0; y < h; y++) {
        const pixel *s = frame + (size_t)y * w;
        for (uint32_t x = 0; x < w; x++)
            row[x] = down(s[x].r);
        plane_write(luma, y, row, w);
    }
    for (uint32_t y = 0; y < ch && 2 * y < h; y++) {
        const pixel *s = frame + (size_t)(2 * y) * w;
        for (uint32_t x = 0; x < cw && 2 * x < w; x++) {
            row[2 * x + (vu ? 0 : 1)] = down(s[2 * x].g);
            row[2 * x + (vu ? 1 : 0)] = down(s[2 * x].b);
        }
        plane_write(chroma, y, row, cw * 2);
    }
    free(row);
}

void vic_execute(const uint32_t *regs)
{
    const uint8_t *cfg = regs[R_CONFIG_STRUCT] ? h1x_ptr((uint64_t)regs[R_CONFIG_STRUCT] << 8, CONFIG_SIZE) : NULL;
    if (!cfg) {
        rl_log(RL_LOG_WARN, "vic", "configuration not mapped");
        return;
    }
    const uint8_t *out_cfg = cfg + 0x10;
    surface_config oc = read_surface_config(cfg + 0x20);
    uint32_t w = oc.surface_w, h = oc.surface_h;
    size_t n = (size_t)w * h;
    if (n > frame_n) {
        pixel *p = realloc(frame, n * sizeof *p);
        if (!p)
            return;
        frame = p;
        frame_n = n;
    }
    pixel bg = { (uint16_t)bits(out_cfg, 16, 10), (uint16_t)bits(out_cfg, 26, 10), (uint16_t)bits(out_cfg, 36, 10), (uint16_t)bits(out_cfg, 6, 10) };
    for (size_t i = 0; i < n; i++)
        frame[i] = bg;
    for (uint32_t s = 0; s < SLOTS; s++) {
        const uint8_t *slot = cfg + SLOT_BASE + s * SLOT_SIZE;
        if (!bits(slot, 0, 1))
            continue;
        surface_config sc = read_surface_config(slot + 0x40);
        rl_log(RL_LOG_DEBUG, "vic", "slot %u: format %u %ux%u blk %u/%u, matrix %u; out format %u %ux%u blk %u/%u", s, sc.format, sc.luma_w,
               sc.luma_h, sc.blk_kind, sc.blk_height, (uint32_t)bits(slot, 0x60 * 8 + 127, 1), oc.format, w, h, oc.blk_kind, oc.blk_height);
        blend_slot(slot, regs, s, w, h, out_cfg);
    }
    const uint32_t *o = &regs[R_OUTPUT_SURFACE];
    plane p0, p1;
    switch (oc.format) {
    case PF_A8B8G8R8:
    case PF_X8B8G8R8:
    case PF_A8R8G8B8:
        if (plane_of(&p0, o[0], &oc, w, h, 4))
            write_rgba(&p0, w, h, oc.format);
        else
            rl_log(RL_LOG_WARN, "vic", "output surface not mapped");
        break;
    case PF_Y8_U8V8_N420:
    case PF_Y8_V8U8_N420:
        if (plane_of(&p0, o[0], &oc, oc.luma_w, oc.luma_h, 1) && plane_of(&p1, o[1], &oc, oc.chroma_w, oc.chroma_h, 2))
            write_nv12(&p0, &p1, w < oc.luma_w ? w : oc.luma_w, h < oc.luma_h ? h : oc.luma_h, oc.chroma_w, oc.chroma_h, oc.format == PF_Y8_V8U8_N420);
        else
            rl_log(RL_LOG_WARN, "vic", "output surface not mapped");
        break;
    default:
        rl_log(RL_LOG_WARN, "vic", "output pixel format %u is not supported", oc.format);
        break;
    }
}
