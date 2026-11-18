/**
 * @file test_screens.c
 * @brief The layouts of two screens in a window, their composition, the
 * mapping of a touch on a picture back to a screen's pixel (wherever the
 * screen is drawn and however it is scaled), the split over displays, and
 * presenting to a main window and a second display through a backend.
 */
#include "runner.h"

#include <relift/screens.h>
#include <relift/touchpad.h>
#include <relift/window.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint32_t g_sizes[RL_SCREENS][2] = {{256, 192}, {256, 192}};

TEST(stacked_and_side_by_side_place_the_screens_edge_to_edge)
{
    rl_screens_picture p;
    rl_screens_plan(g_sizes, RL_SCREENS_STACKED, -1, false, &p);
    CHECK_EQ(p.width, 256);
    CHECK_EQ(p.height, 384);
    CHECK_EQ(p.place[0].y, 0);
    CHECK_EQ(p.place[1].y, 192);
    rl_screens_plan(g_sizes, RL_SCREENS_SIDE_BY_SIDE, -1, true, &p);
    CHECK_EQ(p.width, 512);
    CHECK_EQ(p.height, 192);
    CHECK_EQ(p.place[1].x, 256);
}

TEST(a_large_screen_is_twice_the_size_beside_or_below_the_small_one)
{
    rl_screens_picture p;
    rl_screens_plan(g_sizes, RL_SCREENS_LARGE_TOP, -1, false, &p);
    CHECK_EQ(p.width, 512 + 256);
    CHECK_EQ(p.height, 384);
    CHECK_EQ(p.place[0].w, 512);
    CHECK_EQ(p.place[1].x, 512);
    CHECK_EQ(p.place[1].y, 384 - 192);
    rl_screens_plan(g_sizes, RL_SCREENS_LARGE_BOTTOM, -1, true, &p);
    CHECK_EQ(p.width, 512);
    CHECK_EQ(p.height, 384 + 192);
    CHECK_EQ(p.place[1].y, 0);
    CHECK_EQ(p.place[1].w, 512);
    CHECK_EQ(p.place[0].y, 384);
    CHECK_EQ(p.place[0].x, 128);
}

TEST(one_screen_alone_fills_the_picture)
{
    rl_screens_picture p;
    rl_screens_plan(g_sizes, RL_SCREENS_TOP_ONLY, -1, false, &p);
    CHECK(p.place[0].shown && !p.place[1].shown);
    CHECK_EQ(p.width, 256);
    rl_screens_plan(g_sizes, RL_SCREENS_BOTTOM_ONLY, -1, false, &p);
    CHECK(!p.place[0].shown && p.place[1].shown);
    rl_screens_plan(g_sizes, RL_SCREENS_STACKED, 1, false, &p);
    CHECK(!p.place[0].shown && p.place[1].shown);
    CHECK_EQ(p.height, 192);
    rl_screens_plan(g_sizes, RL_SCREENS_TOP_ONLY, 1, false, &p);
    CHECK(!p.place[0].shown && p.place[1].shown);
}

TEST(touches_map_back_through_every_layout)
{
    static const rl_screens_layout layouts[] = {RL_SCREENS_STACKED,    RL_SCREENS_SIDE_BY_SIDE, RL_SCREENS_LARGE_TOP,
                                                RL_SCREENS_LARGE_BOTTOM, RL_SCREENS_TOP_ONLY,     RL_SCREENS_BOTTOM_ONLY};
    for (unsigned l = 0; l < sizeof layouts / sizeof layouts[0]; l++)
        for (int portrait = 0; portrait < 2; portrait++) {
            rl_screens_picture p;
            rl_screens_plan(g_sizes, layouts[l], -1, portrait, &p);
            if (!p.place[1].shown)
                continue;
            const rl_screens_place *b = &p.place[1];
            uint32_t s, x, y;
            CHECK(rl_screens_hit(&p, g_sizes, b->x, b->y, &s, &x, &y));
            CHECK_EQ(s, 1);
            CHECK_EQ(x, 0);
            CHECK_EQ(y, 0);
            CHECK(rl_screens_hit(&p, g_sizes, b->x + b->w - 1, b->y + b->h - 1, &s, &x, &y));
            CHECK_EQ(s, 1);
            CHECK_EQ(x, 255);
            CHECK_EQ(y, 191);
            CHECK(rl_screens_hit(&p, g_sizes, b->x + b->w / 2, b->y + b->h / 2, &s, &x, &y));
            CHECK(x >= 127 && x <= 128 && y >= 95 && y <= 96);
            CHECK(rl_screens_hit(&p, g_sizes, p.place[0].x, p.place[0].y, &s, &x, &y) || !p.place[0].shown);
        }
}

TEST(a_point_on_no_screen_hits_nothing)
{
    rl_screens_picture p;
    rl_screens_plan(g_sizes, RL_SCREENS_LARGE_TOP, -1, false, &p);
    uint32_t s, x, y;
    CHECK(!rl_screens_hit(&p, g_sizes, 600, 10, &s, &x, &y));
    CHECK(!rl_screens_hit(&p, g_sizes, 10000, 10, &s, &x, &y));
}

TEST(composition_scales_each_screen_by_whole_multiples)
{
    uint8_t top[256 * 192 * 3], bottom[256 * 192 * 3];
    for (uint32_t i = 0; i < sizeof top; i += 3) {
        top[i] = 10;
        top[i + 1] = 20;
        top[i + 2] = (uint8_t)(i / 3 % 256);
        bottom[i] = 200;
        bottom[i + 1] = 100;
        bottom[i + 2] = (uint8_t)(i / 3 / 256);
    }
    rl_screens_frame f[RL_SCREENS] = {{top, 256, 192}, {bottom, 256, 192}};
    rl_screens_picture p;
    rl_screens_plan(g_sizes, RL_SCREENS_LARGE_BOTTOM, -1, false, &p);
    uint8_t *rgb = malloc((size_t)p.width * p.height * 3);
    rl_screens_compose(&p, f, rgb);
    const rl_screens_place *b = &p.place[1];
    CHECK_EQ(b->w, 512);
    const uint8_t *px = rgb + ((size_t)(b->y + 11) * p.width + b->x + 25) * 3;
    CHECK_EQ(px[0], 200);
    CHECK_EQ(px[2], 5);
    const rl_screens_place *t = &p.place[0];
    px = rgb + ((size_t)(t->y + 3) * p.width + t->x + 7) * 3;
    CHECK_EQ(px[0], 10);
    CHECK_EQ(px[2], 7);
    px = rgb + ((size_t)0 * p.width + 400) * 3 + 0;
    CHECK_EQ(px[0], 200);
    free(rgb);
}

TEST(auto_stacks_a_tall_window_and_takes_no_room_for_the_controller)
{
    CHECK_EQ(rl_screens_resolve(g_sizes, RL_SCREENS_AUTO, 1080, 2400, false), RL_SCREENS_STACKED);
    CHECK_EQ(rl_screens_resolve(g_sizes, RL_SCREENS_AUTO, 2400, 1080, true), RL_SCREENS_SIDE_BY_SIDE);
    CHECK_EQ(rl_screens_resolve(g_sizes, RL_SCREENS_AUTO, 2400, 1080, false), RL_SCREENS_SIDE_BY_SIDE);
    CHECK_EQ(rl_screens_resolve(g_sizes, RL_SCREENS_AUTO, 1000, 1000, false), RL_SCREENS_STACKED);
    CHECK_EQ(rl_screens_resolve(g_sizes, RL_SCREENS_AUTO, 1360, 900, false), RL_SCREENS_SIDE_BY_SIDE);
    CHECK_EQ(rl_screens_resolve(g_sizes, RL_SCREENS_AUTO, 700, 900, false), RL_SCREENS_STACKED);
    CHECK_EQ(rl_screens_resolve(g_sizes, RL_SCREENS_AUTO, 1536, 768, false), RL_SCREENS_LARGE_TOP);
    CHECK_EQ(rl_screens_resolve(g_sizes, RL_SCREENS_LARGE_TOP, 1000, 1000, false), RL_SCREENS_LARGE_TOP);
}

TEST(displays_split_the_screens_by_the_setting)
{
    CHECK_EQ(rl_screens_content(RL_SCREENS_DISPLAYS_AUTO, 0, 0), -1);
    CHECK_EQ(rl_screens_content(RL_SCREENS_DISPLAYS_AUTO, 0, 1), 0);
    CHECK_EQ(rl_screens_content(RL_SCREENS_DISPLAYS_AUTO, 1, 1), 1);
    CHECK_EQ(rl_screens_content(RL_SCREENS_DISPLAYS_SWAPPED, 0, 1), 1);
    CHECK_EQ(rl_screens_content(RL_SCREENS_DISPLAYS_SWAPPED, 1, 1), 0);
    CHECK_EQ(rl_screens_content(RL_SCREENS_DISPLAYS_TOP_BOTH, 1, 1), 0);
    CHECK_EQ(rl_screens_content(RL_SCREENS_DISPLAYS_BOTTOM_BOTH, 0, 1), 1);
    CHECK_EQ(rl_screens_content(RL_SCREENS_DISPLAYS_SINGLE, 0, 1), -1);
    CHECK(rl_screens_content(RL_SCREENS_DISPLAYS_SINGLE, 1, 1) < -1);
}

typedef struct fake_window {
    rl_window head;
    uint32_t width, height;
    uint32_t shown_w, shown_h;
    uint8_t first[3];
    int presents;
    rl_window_touch touch;
    bool touching;
} fake_window;

static int fake_init(void) { return 0; }

static fake_window g_main_w = {.width = 1080, .height = 2000};
static fake_window g_second_w = {.width = 1200, .height = 1000};
static uint32_t g_displays;
static rl_window_backend g_back;

static void fake_present(rl_window *w, const uint8_t *rgb, uint32_t width, uint32_t height, uint32_t sw, uint32_t sh)
{
    (void)sw;
    (void)sh;
    fake_window *f = (fake_window *)w;
    f->shown_w = width;
    f->shown_h = height;
    memcpy(f->first, rgb, 3);
    f->presents++;
}

static void fake_size(rl_window *w, uint32_t *width, uint32_t *height)
{
    *width = ((fake_window *)w)->width;
    *height = ((fake_window *)w)->height;
}

static int g_opens, g_closes;

static uint32_t fake_count(void) { return g_displays; }

static rl_window *fake_open(uint32_t index)
{
    (void)index;
    g_opens++;
    g_second_w.head.b = &g_back;
    return &g_second_w.head;
}

static void fake_close(rl_window *w)
{
    (void)w;
    g_closes++;
}

static uint32_t fake_touches(rl_window *w, rl_window_touch *out, uint32_t max)
{
    fake_window *f = (fake_window *)w;
    if (!f->touching || !max)
        return 0;
    out[0] = f->touch;
    return 1;
}

static bool fake_frame_point(rl_window *w, int32_t x, int32_t y, uint32_t *fx, uint32_t *fy)
{
    (void)w;
    *fx = (uint32_t)x;
    *fy = (uint32_t)y;
    return true;
}

typedef struct snap {
    uint32_t main_w, main_h, second_presents;
    uint32_t main_w2, main_h2, main_first, second_h, second_first;
    bool idle_active;
    rl_screens_pen touch_pen, mouse_pen;
    uint32_t swapped_main_first, swapped_second_first;
    uint32_t single_h, single_swapped_first, single_swapped_h;
    rl_screens_pen single_swapped_pen;
    uint32_t unplugged_h, unplugged_presents, unplugged_closes, back_h, back_presents, back_opens;
} snap;

static snap g_snap;

static void fake_wake(void) {}

static void fake_run(bool (*done)(void *user), void *user)
{
    while (!done(user))
        usleep(1000);
}

static int present_body(void *arg)
{
    (void)arg;
    uint8_t top[256 * 192 * 3], bottom[256 * 192 * 3];
    memset(top, 1, sizeof top);
    memset(bottom, 2, sizeof bottom);
    rl_screens_frame f[RL_SCREENS] = {{top, 256, 192}, {bottom, 256, 192}};
    rl_screens_declare(g_sizes, 1);
    g_main_w.head.b = &g_back;
    rl_screens_config cfg = {.layout = RL_SCREENS_STACKED, .displays = RL_SCREENS_DISPLAYS_AUTO};
    rl_screens_present(&g_main_w.head, &cfg, f);
    g_snap.main_w = g_main_w.shown_w;
    g_snap.main_h = g_main_w.shown_h;
    g_snap.second_presents = (uint32_t)g_second_w.presents;
    g_displays = 1;
    rl_screens_present(&g_main_w.head, &cfg, f);
    g_snap.main_w2 = g_main_w.shown_w;
    g_snap.main_h2 = g_main_w.shown_h;
    g_snap.main_first = g_main_w.first[0];
    g_snap.second_h = g_second_w.shown_h;
    g_snap.second_first = g_second_w.first[0];
    rl_input_snapshot s;
    memset(&s, 0, sizeof s);
    g_snap.idle_active = rl_screens_touch(&g_main_w.head, &s).active;
    g_second_w.touching = true;
    g_second_w.touch = (rl_window_touch){1, 100, 50};
    g_snap.touch_pen = rl_screens_touch(&g_main_w.head, &s);
    g_second_w.touching = false;
    s.pointer.buttons = 1u << RL_PTR_LEFT;
    s.pointer.x = 10;
    s.pointer.y = 10;
    g_snap.mouse_pen = rl_screens_touch(&g_main_w.head, &s);
    cfg.swap = true;
    rl_screens_present(&g_main_w.head, &cfg, f);
    g_snap.swapped_main_first = g_main_w.first[0];
    g_snap.swapped_second_first = g_second_w.first[0];
    cfg.swap = false;
    cfg.displays = RL_SCREENS_DISPLAYS_SINGLE;
    rl_screens_present(&g_main_w.head, &cfg, f);
    g_snap.single_h = g_main_w.shown_h;
    cfg.swap = true;
    rl_screens_present(&g_main_w.head, &cfg, f);
    g_snap.single_swapped_first = g_main_w.first[0];
    g_snap.single_swapped_h = g_main_w.shown_h;
    s.pointer.x = 10;
    s.pointer.y = 20;
    g_snap.single_swapped_pen = rl_screens_touch(&g_main_w.head, &s);
    cfg.swap = false;
    cfg.displays = RL_SCREENS_DISPLAYS_AUTO;
    rl_screens_present(&g_main_w.head, &cfg, f);
    int before = g_second_w.presents;
    g_displays = 0;
    rl_screens_present(&g_main_w.head, &cfg, f);
    g_snap.unplugged_h = g_main_w.shown_h;
    g_snap.unplugged_presents = (uint32_t)(g_second_w.presents - before);
    g_snap.unplugged_closes = (uint32_t)g_closes;
    g_displays = 1;
    rl_screens_present(&g_main_w.head, &cfg, f);
    g_snap.back_h = g_main_w.shown_h;
    g_snap.back_presents = (uint32_t)(g_second_w.presents - before);
    g_snap.back_opens = (uint32_t)g_opens;
    rl_screens_reset();
    return 0;
}

TEST(two_displays_each_show_a_screen_touches_on_the_second_reach_the_pen_and_unplugging_falls_back)
{
    memset(&g_back, 0, sizeof g_back);
    g_back.name = "fake";
    g_back.init = fake_init;
    g_back.run = fake_run;
    g_back.wake = fake_wake;
    g_back.present_rgb = fake_present;
    g_back.size = fake_size;
    g_back.display_count = fake_count;
    g_back.open_display = fake_open;
    g_back.close = fake_close;
    g_back.touches = fake_touches;
    g_back.frame_point = fake_frame_point;
    unsetenv(RL_WINDOW_ENV);
    rl_window_set_backend(&g_back);
    CHECK_EQ(rl_window_main(present_body, NULL), 0);
    rl_window_set_backend(NULL);
    CHECK_EQ(g_snap.main_w, 256);
    CHECK_EQ(g_snap.main_h, 384);
    CHECK_EQ(g_snap.second_presents, 0);
    CHECK_EQ(g_snap.main_w2, 256);
    CHECK_EQ(g_snap.main_h2, 192);
    CHECK_EQ(g_snap.main_first, 1);
    CHECK_EQ(g_snap.second_h, 192);
    CHECK_EQ(g_snap.second_first, 2);
    CHECK(!g_snap.idle_active);
    CHECK(g_snap.touch_pen.down);
    CHECK_EQ(g_snap.touch_pen.x, 100);
    CHECK_EQ(g_snap.touch_pen.y, 50);
    CHECK(g_snap.mouse_pen.active && !g_snap.mouse_pen.down);
    CHECK_EQ(g_snap.swapped_main_first, 2);
    CHECK_EQ(g_snap.swapped_second_first, 1);
    CHECK_EQ(g_snap.single_h, 384);
    CHECK_EQ(g_snap.single_swapped_first, 2);
    CHECK_EQ(g_snap.single_swapped_h, 384);
    CHECK(g_snap.single_swapped_pen.down);
    CHECK_EQ(g_snap.single_swapped_pen.y, 20);
    CHECK_EQ(g_snap.unplugged_h, 384);
    CHECK_EQ(g_snap.unplugged_presents, 0);
    CHECK_EQ(g_snap.unplugged_closes, 2);
    CHECK_EQ(g_snap.back_h, 192);
    CHECK_EQ(g_snap.back_presents, 1);
    CHECK_EQ(g_snap.back_opens, 3);
}