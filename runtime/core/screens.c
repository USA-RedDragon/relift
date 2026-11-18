/**
 * @file screens.c
 * @brief The layouts of a two-screen guest's pictures, their composition,
 * the split over displays and the mapping of touches back to guest pixels.
 */
#include <relift/screens.h>
#include <relift/touchpad.h>

#include "core.h"

#include <stdlib.h>
#include <string.h>

#define NO_WINDOW (-2)

static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }

static void put(rl_screens_picture *p, uint32_t s, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    p->place[s] = (rl_screens_place){true, x, y, w, h};
}

void rl_screens_plan(const uint32_t sizes[RL_SCREENS][2], rl_screens_layout layout, int only, bool portrait,
                     rl_screens_picture *out)
{
    memset(out, 0, sizeof *out);
    uint32_t w0 = sizes[0][0], h0 = sizes[0][1], w1 = sizes[1][0], h1 = sizes[1][1];
    if (only < 0 && layout == RL_SCREENS_TOP_ONLY)
        only = 0;
    else if (only < 0 && layout == RL_SCREENS_BOTTOM_ONLY)
        only = 1;
    if (only >= 0) {
        uint32_t s = (uint32_t)only % RL_SCREENS;
        out->width = sizes[s][0];
        out->height = sizes[s][1];
        put(out, s, 0, 0, out->width, out->height);
        return;
    }
    switch (layout) {
    case RL_SCREENS_SIDE_BY_SIDE:
        out->width = w0 + w1;
        out->height = umax(h0, h1);
        put(out, 0, 0, (out->height - h0) / 2, w0, h0);
        put(out, 1, w0, (out->height - h1) / 2, w1, h1);
        break;
    case RL_SCREENS_LARGE_TOP:
    case RL_SCREENS_LARGE_BOTTOM: {
        uint32_t big = layout == RL_SCREENS_LARGE_TOP ? 0 : 1, small = 1 - big;
        uint32_t bw = 2 * sizes[big][0], bh = 2 * sizes[big][1], sw = sizes[small][0], sh = sizes[small][1];
        if (portrait) {
            out->width = umax(bw, sw);
            out->height = bh + sh;
            put(out, big, (out->width - bw) / 2, 0, bw, bh);
            put(out, small, (out->width - sw) / 2, bh, sw, sh);
        } else {
            out->width = bw + sw;
            out->height = umax(bh, sh);
            put(out, big, 0, 0, bw, bh);
            put(out, small, bw, out->height - sh, sw, sh);
        }
        break;
    }
    case RL_SCREENS_AUTO:
    case RL_SCREENS_STACKED:
    default:
        out->width = umax(w0, w1);
        out->height = h0 + h1;
        put(out, 0, (out->width - w0) / 2, 0, w0, h0);
        put(out, 1, (out->width - w1) / 2, h0, w1, h1);
        break;
    }
}

void rl_screens_compose(const rl_screens_picture *plan, const rl_screens_frame frames[RL_SCREENS], uint8_t *rgb)
{
    memset(rgb, 0, (size_t)plan->width * plan->height * 3u);
    static uint32_t xmap[RL_SCREENS_MAX_SIDE];
    for (uint32_t s = 0; s < RL_SCREENS; s++) {
        const rl_screens_place *p = &plan->place[s];
        const rl_screens_frame *f = &frames[s];
        if (!p->shown || !f->rgb || !f->width || !f->height || !p->w || !p->h || p->w > RL_SCREENS_MAX_SIDE)
            continue;
        for (uint32_t x = 0; x < p->w; x++)
            xmap[x] = (uint32_t)((uint64_t)x * f->width / p->w);
        for (uint32_t y = 0; y < p->h; y++) {
            uint32_t sy = (uint32_t)((uint64_t)y * f->height / p->h);
            const uint8_t *src = f->rgb + (size_t)sy * f->width * 3u;
            uint8_t *dst = rgb + ((size_t)(p->y + y) * plan->width + p->x) * 3u;
            if (p->w == f->width) {
                memcpy(dst, src, (size_t)p->w * 3u);
                continue;
            }
            for (uint32_t x = 0; x < p->w; x++) {
                const uint8_t *px = src + (size_t)xmap[x] * 3u;
                dst[x * 3u] = px[0];
                dst[x * 3u + 1] = px[1];
                dst[x * 3u + 2] = px[2];
            }
        }
    }
}

bool rl_screens_hit(const rl_screens_picture *plan, const uint32_t sizes[RL_SCREENS][2], uint32_t x, uint32_t y,
                    uint32_t *screen, uint32_t *sx, uint32_t *sy)
{
    for (uint32_t s = 0; s < RL_SCREENS; s++) {
        const rl_screens_place *p = &plan->place[s];
        if (!p->shown || x < p->x || y < p->y || x - p->x >= p->w || y - p->y >= p->h)
            continue;
        *screen = s;
        *sx = (uint32_t)((uint64_t)(x - p->x) * sizes[s][0] / p->w);
        *sy = (uint32_t)((uint64_t)(y - p->y) * sizes[s][1] / p->h);
        return true;
    }
    return false;
}

static uint64_t scale_millis(const rl_screens_picture *p, uint32_t w, uint32_t h)
{
    if (!p->width || !p->height)
        return 0;
    uint64_t a = (uint64_t)w * 1000u / p->width, b = (uint64_t)h * 1000u / p->height;
    return a < b ? a : b;
}

rl_screens_layout rl_screens_resolve(const uint32_t sizes[RL_SCREENS][2], rl_screens_layout layout, uint32_t width,
                                     uint32_t height, bool controller)
{
    if (layout != RL_SCREENS_AUTO)
        return layout;
    if (height > width || controller)
        return RL_SCREENS_STACKED;
    rl_screens_picture a, b;
    rl_screens_plan(sizes, RL_SCREENS_STACKED, -1, false, &a);
    rl_screens_plan(sizes, RL_SCREENS_SIDE_BY_SIDE, -1, false, &b);
    return scale_millis(&b, width, height) > scale_millis(&a, width, height) ? RL_SCREENS_SIDE_BY_SIDE
                                                                             : RL_SCREENS_STACKED;
}

int rl_screens_content(rl_screens_displays displays, uint32_t display, uint32_t display_count)
{
    if (display_count == 0 || displays == RL_SCREENS_DISPLAYS_SINGLE)
        return display == 0 ? -1 : NO_WINDOW;
    switch (displays) {
    case RL_SCREENS_DISPLAYS_SWAPPED:
        return display == 0 ? 1 : display == 1 ? 0 : NO_WINDOW;
    case RL_SCREENS_DISPLAYS_TOP_BOTH:
        return display < 2 ? 0 : NO_WINDOW;
    case RL_SCREENS_DISPLAYS_BOTTOM_BOTH:
        return display < 2 ? 1 : NO_WINDOW;
    default:
        return display == 0 ? 0 : display == 1 ? 1 : NO_WINDOW;
    }
}

typedef struct output {
    rl_window *win;
    rl_screens_picture plan;
    uint8_t *rgb;
    size_t cap;
    bool drawn;
} output;

static struct {
    uint32_t sizes[RL_SCREENS][2];
    uint32_t touch;
    bool declared;
    output out[2];
} S;

void rl_screens_declare(const uint32_t sizes[RL_SCREENS][2], uint32_t touch_screen)
{
    memcpy(S.sizes, sizes, sizeof S.sizes);
    S.touch = touch_screen % RL_SCREENS;
    S.declared = true;
}

void rl_screens_reset(void)
{
    if (S.out[1].win)
        rl_window_close_display(S.out[1].win);
    for (int i = 0; i < 2; i++)
        free(S.out[i].rgb);
    memset(&S, 0, sizeof S);
}

const rl_screens_picture *rl_screens_last_plan(uint32_t display)
{
    return display < 2 && S.out[display].drawn ? &S.out[display].plan : NULL;
}

static rl_screens_layout swapped(rl_screens_layout l)
{
    switch (l) {
    case RL_SCREENS_LARGE_TOP:
        return RL_SCREENS_LARGE_BOTTOM;
    case RL_SCREENS_LARGE_BOTTOM:
        return RL_SCREENS_LARGE_TOP;
    case RL_SCREENS_TOP_ONLY:
        return RL_SCREENS_BOTTOM_ONLY;
    case RL_SCREENS_BOTTOM_ONLY:
        return RL_SCREENS_TOP_ONLY;
    default:
        return l;
    }
}

static void draw(output *o, rl_window *win, const rl_screens_config *cfg, int content,
                 const rl_screens_frame frames[RL_SCREENS])
{
    uint32_t pw, ph;
    rl_window_size(win, &pw, &ph);
    rl_screens_layout l = rl_screens_resolve(S.sizes, cfg->layout, pw, ph, !win->extra && rl_touchpad_wanted());
    if (cfg->swap)
        l = swapped(l);
    rl_screens_plan(S.sizes, l, content, ph > pw, &o->plan);
    if (cfg->swap && (l == RL_SCREENS_STACKED || l == RL_SCREENS_SIDE_BY_SIDE) && content < 0) {
        rl_screens_place t = o->plan.place[0];
        o->plan.place[0] = o->plan.place[1];
        o->plan.place[1] = t;
        if (l == RL_SCREENS_STACKED)
            o->plan.place[0].y = 0, o->plan.place[1].y = o->plan.place[0].h;
        else
            o->plan.place[0].x = 0, o->plan.place[1].x = o->plan.place[0].w;
    }
    size_t need = (size_t)o->plan.width * o->plan.height * 3u;
    if (need > o->cap) {
        uint8_t *g = realloc(o->rgb, need);
        if (!g)
            return;
        o->rgb = g;
        o->cap = need;
    }
    o->win = win;
    o->drawn = true;
    rl_screens_compose(&o->plan, frames, o->rgb);
    rl_window_set_fit(win, !cfg->integer_scale);
}

void rl_screens_present(rl_window *main, const rl_screens_config *cfg, const rl_screens_frame frames[RL_SCREENS])
{
    if (!S.declared || !main)
        return;
    uint32_t count = cfg->displays == RL_SCREENS_DISPLAYS_SINGLE ? 0 : rl_window_display_count();
    rl_screens_displays dsp = cfg->displays;
    if (cfg->swap && dsp == RL_SCREENS_DISPLAYS_AUTO)
        dsp = RL_SCREENS_DISPLAYS_SWAPPED;
    else if (cfg->swap && dsp == RL_SCREENS_DISPLAYS_SWAPPED)
        dsp = RL_SCREENS_DISPLAYS_AUTO;
    int second = rl_screens_content(dsp, 1, count);
    output *o1 = &S.out[1];
    if (second == NO_WINDOW && o1->win) {
        rl_window_close_display(o1->win);
        o1->win = NULL;
        o1->drawn = false;
    } else if (second != NO_WINDOW && !o1->win) {
        o1->win = rl_window_open_display(0);
    }
    if (o1->win && second != NO_WINDOW) {
        draw(o1, o1->win, cfg, second, frames);
        rl_window_present_rgb(o1->win, o1->rgb, o1->plan.width, o1->plan.height);
    }
    output *o0 = &S.out[0];
    draw(o0, main, cfg, rl_screens_content(dsp, 0, count), frames);
    rl_touchpad_set_picture(o0->plan.width, o0->plan.height, cfg->integer_scale);
    rl_window_present_rgb(main, o0->rgb, o0->plan.width, o0->plan.height);
}

static bool on_touch_screen(const output *o, uint32_t x, uint32_t y, rl_screens_pen *pen)
{
    uint32_t s, sx, sy;
    if (!o->drawn || !rl_screens_hit(&o->plan, S.sizes, x, y, &s, &sx, &sy) || s != S.touch)
        return false;
    *pen = (rl_screens_pen){true, true, sx, sy};
    return true;
}

rl_screens_pen rl_screens_touch(rl_window *main, const rl_input_snapshot *s)
{
    rl_screens_pen pen = {0};
    const output *o0 = &S.out[0];
    if (main && o0->drawn) {
        if (rl_touchpad_wanted()) {
            for (uint32_t i = 0; i < s->touch_count; i++) {
                pen.active = true;
                if (on_touch_screen(o0, (uint32_t)((uint64_t)s->touches[i].x * o0->plan.width / 65536u),
                                    (uint32_t)((uint64_t)s->touches[i].y * o0->plan.height / 65536u), &pen))
                    return pen;
            }
        } else if (s->pointer.buttons & (1u << RL_PTR_LEFT)) {
            uint32_t fx, fy;
            pen.active = true;
            if (rl_window_frame_point(main, s->pointer.x, s->pointer.y, &fx, &fy) && on_touch_screen(o0, fx, fy, &pen))
                return pen;
        }
    }
    const output *o1 = &S.out[1];
    if (o1->win) {
        rl_window_touch t[4];
        uint32_t n = rl_window_touches(o1->win, t, 4);
        for (uint32_t i = 0; i < n; i++) {
            pen.active = true;
            if (on_touch_screen(o1, t[i].x, t[i].y, &pen))
                return pen;
        }
    }
    return pen;
}
