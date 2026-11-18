/**
 * @file upscale_ref.c
 * @brief The C reference of shaders/upscale.comp: the same mapping, filters and clamping in single precision.
 */
#include "upscale_ref.h"

#include <math.h>

#define LOBES 3.0f
#define MAX_TAPS 16
#define PI_F 3.14159265358979f

typedef struct px {
    float c[4];
} px;

typedef struct ctx {
    const uint32_t *src;
    uint32_t sw;
    const rl_upscale_rect *in;
} ctx;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static px fetch(const ctx *k, int x, int y)
{
    x = clampi(x, (int)k->in->x, (int)(k->in->x + k->in->width) - 1);
    y = clampi(y, (int)k->in->y, (int)(k->in->y + k->in->height) - 1);
    uint32_t w = k->src[(uint32_t)y * k->sw + (uint32_t)x];
    px p;
    for (int i = 0; i < 4; i++)
        p.c[i] = (float)((w >> (8 * i)) & 0xff) / 255.0f;
    return p;
}

static px mix(px a, px b, float t)
{
    for (int i = 0; i < 4; i++)
        a.c[i] = a.c[i] * (1.0f - t) + b.c[i] * t;
    return a;
}

static px bilinear(const ctx *k, float sx, float sy)
{
    float tx = sx - 0.5f, ty = sy - 0.5f, bx = floorf(tx), by = floorf(ty), fx = tx - bx, fy = ty - by;
    int ix = (int)bx, iy = (int)by;
    px top = mix(fetch(k, ix, iy), fetch(k, ix + 1, iy), fx);
    px bottom = mix(fetch(k, ix, iy + 1), fetch(k, ix + 1, iy + 1), fx);
    return mix(top, bottom, fy);
}

static float sharp_axis(float s, float ratio)
{
    float scale = floorf(ratio) > 1.0f ? floorf(ratio) : 1.0f;
    float base = floorf(s), d = s - base - 0.5f, range = 0.5f - 0.5f / scale;
    float c = d < -range ? -range : d > range ? range : d;
    return base + (d - c) * scale + 0.5f;
}

static float weight(float x)
{
    x = fabsf(x);
    if (x < 1e-5f)
        return 1.0f;
    if (x >= LOBES)
        return 0.0f;
    float p = PI_F * x;
    return LOBES * sinf(p) * sinf(p / LOBES) / (p * p);
}

static px lanczos(const ctx *k, float sx, float sy, const float step[2])
{
    float wx = step[0] > 1.0f ? step[0] : 1.0f, wy = step[1] > 1.0f ? step[1] : 1.0f;
    float tx = sx - 0.5f, ty = sy - 0.5f;
    int cx = (int)floorf(tx), cy = (int)floorf(ty);
    int r_x = (int)ceilf(LOBES * wx), r_y = (int)ceilf(LOBES * wy);
    r_x = r_x < MAX_TAPS / 2 ? r_x : MAX_TAPS / 2;
    r_y = r_y < MAX_TAPS / 2 ? r_y : MAX_TAPS / 2;
    px sum = { { 0 } };
    float wsum = 0.0f;
    for (int dy = 1 - r_y; dy <= r_y; dy++) {
        float w_y = weight((ty - (float)(cy + dy)) / wy);
        if (w_y == 0.0f)
            continue;
        for (int dx = 1 - r_x; dx <= r_x; dx++) {
            float w = w_y * weight((tx - (float)(cx + dx)) / wx);
            px f = fetch(k, cx + dx, cy + dy);
            for (int i = 0; i < 4; i++)
                sum.c[i] += w * f.c[i];
            wsum += w;
        }
    }
    px n[4] = { fetch(k, cx, cy), fetch(k, cx + 1, cy), fetch(k, cx, cy + 1), fetch(k, cx + 1, cy + 1) };
    for (int i = 0; i < 4; i++) {
        float lo = n[0].c[i], hi = n[0].c[i], v = sum.c[i] / wsum;
        for (int j = 1; j < 4; j++) {
            lo = n[j].c[i] < lo ? n[j].c[i] : lo;
            hi = n[j].c[i] > hi ? n[j].c[i] : hi;
        }
        sum.c[i] = v < lo ? lo : v > hi ? hi : v;
    }
    return sum;
}

static uint32_t pack(px p)
{
    uint32_t w = 0;
    for (int i = 0; i < 4; i++) {
        float v = p.c[i] < 0.0f ? 0.0f : p.c[i] > 1.0f ? 1.0f : p.c[i];
        w |= (uint32_t)lrintf(v * 255.0f) << (8 * i);
    }
    return w;
}

void upscale_ref(uint32_t choice, const uint32_t *src, uint32_t sw, const rl_upscale_rect *in, uint32_t *dst,
                 uint32_t dw, const rl_upscale_rect *out)
{
    ctx k = { src, sw, in };
    float step[2], ratio[2];
    rl_upscale_steps(in, out, step, ratio);
    float rx = ratio[0], ry = ratio[1];
    for (uint32_t y = 0; y < out->height; y++)
        for (uint32_t x = 0; x < out->width; x++) {
            float s[2];
            rl_upscale_map(in, out, x, y, s);
            px v;
            switch (choice) {
            case RL_UPSCALE_OFF:
                v = fetch(&k, (int)(x + in->x), (int)(y + in->y));
                break;
            case RL_UPSCALE_NEAREST:
                v = fetch(&k, (int)floorf(s[0]), (int)floorf(s[1]));
                break;
            case RL_UPSCALE_BILINEAR:
                v = bilinear(&k, s[0], s[1]);
                break;
            case RL_UPSCALE_SHARP_BILINEAR:
                v = bilinear(&k, sharp_axis(s[0], rx), sharp_axis(s[1], ry));
                break;
            default:
                v = lanczos(&k, s[0], s[1], step);
                break;
            }
            dst[(y + out->y) * dw + x + out->x] = pack(v);
        }
}
