/**
 * @file av1_dav1d.c
 * @brief AV1 decoding through dav1d (BSD-2-Clause license, see host/dav1d/NOTICE).
 *
 * dav1d decodes temporal units as they come out of a stream. This file feeds it one unit at a time on a single
 * thread (so frames come out in decode order, with no frame delay) and converts the shown picture to 8-bit 4:2:0:
 * higher bit depths are rounded down, 4:2:2 and 4:4:4 chroma is decimated, and monochrome gets neutral chroma
 * (inferred: the decoded surfaces a game asks for are 8-bit NV12).
 */
#include <relift/video.h>

#include <dav1d/dav1d.h>

#include <stdlib.h>
#include <string.h>

typedef struct av1_dec {
    Dav1dContext *ctx;
    uint8_t *planes;
    size_t cap;
} av1_dec;

static void *av1_open(void)
{
    av1_dec *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    Dav1dSettings s;
    dav1d_default_settings(&s);
    s.n_threads = 1;
    s.max_frame_delay = 1;
    s.logger.callback = NULL;
    if (dav1d_open(&d->ctx, &s) < 0) {
        free(d);
        return NULL;
    }
    return d;
}

static void av1_close(void *h)
{
    av1_dec *d = h;
    if (!d)
        return;
    dav1d_close(&d->ctx);
    free(d->planes);
    free(d);
}

static uint32_t sample(const Dav1dPicture *p, int plane, uint32_t x, uint32_t y)
{
    const uint8_t *row = (const uint8_t *)p->data[plane] + (ptrdiff_t)y * p->stride[plane ? 1 : 0];
    if (p->p.bpc == 8)
        return row[x];
    uint32_t v = ((const uint16_t *)(const void *)row)[x];
    unsigned shift = (unsigned)p->p.bpc - 8;
    v = (v + (1u << (shift - 1))) >> shift;
    return v > 255 ? 255 : v;
}

static int av1_convert(av1_dec *d, const Dav1dPicture *p, rl_video_image *out)
{
    uint32_t w = (uint32_t)p->p.w, h = (uint32_t)p->p.h;
    uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    size_t need = (size_t)w * h + 2 * (size_t)cw * ch;
    if (need > d->cap) {
        uint8_t *n = realloc(d->planes, need);
        if (!n)
            return -1;
        d->planes = n;
        d->cap = need;
    }
    uint8_t *y = d->planes, *u = y + (size_t)w * h, *v = u + (size_t)cw * ch;
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t c = 0; c < w; c++)
            y[(size_t)r * w + c] = (uint8_t)sample(p, 0, c, r);
    for (uint32_t r = 0; r < ch; r++)
        for (uint32_t c = 0; c < cw; c++) {
            uint8_t uu = 128, vv = 128;
            if (p->p.layout != DAV1D_PIXEL_LAYOUT_I400) {
                uint32_t px = p->p.layout == DAV1D_PIXEL_LAYOUT_I444 ? c * 2 : c;
                uint32_t py = p->p.layout == DAV1D_PIXEL_LAYOUT_I420 ? r : r * 2;
                uu = (uint8_t)sample(p, 1, px, py);
                vv = (uint8_t)sample(p, 2, px, py);
            }
            u[(size_t)r * cw + c] = uu;
            v[(size_t)r * cw + c] = vv;
        }
    out->width = w;
    out->height = h;
    out->y = y;
    out->u = u;
    out->v = v;
    out->y_stride = w;
    out->uv_stride = cw;
    return 1;
}

static int av1_decode(void *h, const uint8_t *data, size_t size, rl_video_image *out)
{
    av1_dec *d = h;
    if (!d || !data || size == 0)
        return d ? 0 : -1;
    Dav1dData in;
    uint8_t *buf = dav1d_data_create(&in, size);
    if (!buf)
        return -1;
    memcpy(buf, data, size);
    int shown = 0;
    while (in.sz > 0) {
        int rc = dav1d_send_data(d->ctx, &in);
        if (rc < 0 && rc != DAV1D_ERR(EAGAIN)) {
            dav1d_data_unref(&in);
            return -1;
        }
        Dav1dPicture pic;
        memset(&pic, 0, sizeof pic);
        rc = dav1d_get_picture(d->ctx, &pic);
        if (rc == 0) {
            int ok = av1_convert(d, &pic, out);
            dav1d_picture_unref(&pic);
            if (ok < 0) {
                dav1d_data_unref(&in);
                return -1;
            }
            shown = 1;
        } else if (rc != DAV1D_ERR(EAGAIN)) {
            dav1d_data_unref(&in);
            return -1;
        }
    }
    return shown;
}

static const rl_av1_stream_backend g_av1 = {
    .name = "dav1d",
    .open = av1_open,
    .close = av1_close,
    .decode = av1_decode,
};

const rl_av1_stream_backend *rl_host_av1_stream_backend(void) { return &g_av1; }
