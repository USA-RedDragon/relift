/**
 * @file test_present.c
 * @brief The window presenter: where a picture sits in a window for each
 * picture.filter and in a turned swapchain, and, on a host whose Vulkan has headless surfaces, frames
 * shown through a real swapchain: placed and scaled as planned, sharp
 * bilinear blending only at pixel edges, the surface let go and made again,
 * a resize making a new swapchain, and a caller never waiting on the display.
 * The swapchain test runs on lavapipe (VK_LOADER_DRIVERS_SELECT), whose
 * queues present to headless surfaces as GPU drivers' may not.
 */
#include "runner.h"

#include "hostlib.h"
#include "present.h"
#include "vkmin.h"

#include <relift/post.h>
#include <relift/settings.h>
#include <relift/window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

TEST(present_place_integer_and_fill)
{
    rl_present_rect r;
    rl_present_place(256, 192, 1240, 1080, true, &r);
    CHECK_EQ(r.w, 1024u);
    CHECK_EQ(r.h, 768u);
    CHECK_EQ(r.x, 108u);
    CHECK_EQ(r.y, 156u);
    rl_present_place(256, 192, 1240, 1080, false, &r);
    CHECK_EQ(r.w, 1240u);
    CHECK_EQ(r.h, 930u);
    CHECK_EQ(r.x, 0u);
    CHECK_EQ(r.y, 75u);
    rl_present_place(256, 384, 1920, 1080, false, &r);
    CHECK_EQ(r.h, 1080u);
    CHECK_EQ(r.w, 720u);
    CHECK_EQ(r.x, 600u);
    rl_present_place(256, 192, 200, 100, true, &r);
    CHECK_EQ(r.h, 100u);
    CHECK_EQ(r.w, 133u);
    rl_present_place(0, 192, 200, 100, true, &r);
    CHECK_EQ(r.w, 0u);
    rl_present_place(256, 192, 0, 100, false, &r);
    CHECK_EQ(r.h, 0u);
}

TEST(present_turn_maps_the_picture_into_a_turned_swapchain)
{
    rl_present_rect in = { 10, 20, 100, 50 }, out;
    rl_present_turn(&in, 300, 200, RLVK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, &out);
    CHECK(out.x == 10 && out.y == 20 && out.w == 100 && out.h == 50);
    rl_present_turn(&in, 300, 200, RLVK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR, &out);
    CHECK(out.x == 130 && out.y == 10 && out.w == 50 && out.h == 100);
    rl_present_turn(&in, 300, 200, RLVK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR, &out);
    CHECK(out.x == 20 && out.y == 190 && out.w == 50 && out.h == 100);
    rl_present_turn(&in, 300, 200, RLVK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR, &out);
    CHECK(out.x == 190 && out.y == 130 && out.w == 100 && out.h == 50);
}

TEST(present_filter_setting_picks_the_scaling)
{
    rl_settings_reset();
    rl_settings_declare_runtime();
    CHECK(rl_post_integer(false));
    CHECK(!rl_post_integer(true));
    CHECK_EQ(rl_post_sampling(256, 192, 1024, 768), RL_POST_SAMPLE_NEAREST);
    CHECK_EQ(rl_post_sampling(256, 192, 1240, 930), RL_POST_SAMPLE_SHARP);
    CHECK_EQ(rl_post_sampling(256, 192, 512, 384), RL_POST_SAMPLE_NEAREST);
    CHECK_EQ(rl_post_sampling(256, 192, 400, 300), RL_POST_SAMPLE_LINEAR);
    CHECK_EQ(rl_post_sampling(256, 192, 512, 400), RL_POST_SAMPLE_LINEAR);
    CHECK_EQ(rl_post_sampling(256, 192, 520, 390), RL_POST_SAMPLE_SHARP);
    CHECK_EQ(rl_post_sampling(256, 192, 200, 150), RL_POST_SAMPLE_LINEAR);
    CHECK(rl_settings_set("picture.filter", "integer") == RL_SETTING_OK);
    CHECK(rl_post_integer(true));
    CHECK_EQ(rl_post_sampling(256, 192, 200, 150), RL_POST_SAMPLE_NEAREST);
    CHECK(rl_settings_set("picture.filter", "nearest") == RL_SETTING_OK);
    CHECK(!rl_post_integer(false));
    CHECK_EQ(rl_post_sampling(256, 192, 1240, 930), RL_POST_SAMPLE_NEAREST);
    CHECK(rl_settings_set("picture.filter", "sharp_bilinear") == RL_SETTING_OK);
    CHECK(!rl_post_integer(false));
    CHECK_EQ(rl_post_sampling(256, 192, 400, 300), RL_POST_SAMPLE_SHARP);
    CHECK_EQ(rl_post_sampling(256, 192, 768, 576), RL_POST_SAMPLE_NEAREST);
    CHECK(rl_settings_set("picture.filter", "bilinear") == RL_SETTING_INVALID);
    rl_settings_reset();
}

typedef struct headless_info {
    int32_t sType;
    const void *pNext;
    uint32_t flags;
} headless_info;

typedef rlvk_result(RLVK_CALL *headless_fn)(rlvk_handle, const headless_info *, const void *, rlvk_handle *);

static const char *const g_exts[] = { "VK_KHR_surface", "VK_EXT_headless_surface", NULL };
static uint32_t g_w = 300, g_h = 200;
static rl_window g_win;

static int h_init(void) { return 0; }

static void h_run(bool (*done)(void *user), void *user)
{
    while (!done(user))
        usleep(1000);
}

static void h_wake(void) {}

static void h_size(rl_window *w, uint32_t *width, uint32_t *height)
{
    (void)w;
    *width = __atomic_load_n(&g_w, __ATOMIC_RELAXED);
    *height = __atomic_load_n(&g_h, __ATOMIC_RELAXED);
}

static const char *const *h_exts(void) { return g_exts; }

static int32_t h_surface(rl_window *w, void *instance, uint64_t *surface)
{
    (void)w;
    rlvk_gipa_fn gipa = (rlvk_gipa_fn)rl_hostlib_vulkan_gipa();
    headless_fn make = gipa ? (headless_fn)gipa((rlvk_handle)(uintptr_t)instance, "vkCreateHeadlessSurfaceEXT") : NULL;
    headless_info info = { .sType = 1000256000 };
    return make ? make((rlvk_handle)(uintptr_t)instance, &info, NULL, surface) : -3;
}

static const rl_window_backend g_headless = {
    .name = "headless test",
    .init = h_init,
    .run = h_run,
    .wake = h_wake,
    .size = h_size,
    .vulkan_extensions = h_exts,
    .vulkan_surface = h_surface,
};

/** @brief A 4 by 2 frame of distinct colors. */
static const uint8_t g_frame[4 * 2 * 3] = {
    255, 0, 0,  0, 255, 0,  0, 0, 255,  255, 255, 0,
    0, 255, 255,  255, 0, 255,  255, 255, 255,  128, 64, 32,
};

static const uint8_t *px(const uint8_t *img, uint32_t w, uint32_t x, uint32_t y) { return img + ((size_t)y * w + x) * 4; }

static bool same(const uint8_t *a, const uint8_t *rgb) { return a[0] == rgb[0] && a[1] == rgb[1] && a[2] == rgb[2]; }

static uint8_t *capture(rl_present *p, uint32_t *w, uint32_t *h)
{
    rl_present_frame(p, g_frame, 4, 2, 4, 2, true, g_w, g_h, NULL);
    return rl_present_capture(p, 5000, w, h);
}

static int body(void *arg)
{
    int *failures = arg;
#define EXPECT(c)                                                                                                    \
    do {                                                                                                             \
        if (!(c)) {                                                                                                  \
            fprintf(stderr, "    %s:%d: %s\n", __FILE__, __LINE__, #c);                                              \
            (*failures)++;                                                                                           \
        }                                                                                                            \
    } while (0)
    rl_present *p = rl_present_open(&g_win);
    if (!p) {
        printf("    (no Vulkan with headless surfaces: the swapchain is not run)\n");
        return 0;
    }
    rl_settings_set("picture.filter", "integer");
    uint32_t w = 0, h = 0;
    uint8_t *img = capture(p, &w, &h);
    EXPECT(img && w == 300 && h == 200);
    if (img && w == 300 && h == 200) {
        static const uint8_t black[3] = { 0, 0, 0 };
        EXPECT(same(px(img, w, 150, 10), black));
        EXPECT(same(px(img, w, 150, 190), black));
        EXPECT(same(px(img, w, 10, 30), &g_frame[0]));
        EXPECT(same(px(img, w, 80, 30), &g_frame[3]));
        EXPECT(same(px(img, w, 299, 174), &g_frame[21]));
        EXPECT(same(px(img, w, 74, 99), &g_frame[0]));
        EXPECT(same(px(img, w, 75, 100), &g_frame[15]));
    }
    free(img);

    __atomic_store_n(&g_w, 310, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_win.generation, 1, __ATOMIC_RELEASE);
    rl_settings_set("picture.filter", "sharp_bilinear");
    img = capture(p, &w, &h);
    EXPECT(img && w == 310 && h == 200);
    if (img && w == 310) {
        EXPECT(same(px(img, w, 10, 30), &g_frame[0]));
        const uint8_t *edge = px(img, w, 77, 50);
        EXPECT(edge[0] > 0 && edge[0] < 255 && edge[1] > 0 && edge[1] < 255);
    }
    free(img);

    rl_present_surface(p, false);
    rl_present_stats st;
    rl_present_stats_get(p, &st);
    EXPECT(st.mode == -1);
    rl_present_surface(p, true);
    img = capture(p, &w, &h);
    EXPECT(img && w == 310);
    free(img);
    rl_present_stats_get(p, &st);
    EXPECT(st.swapchains == 3);
    EXPECT(st.presented >= 3);

    for (int i = 0; i < 200; i++)
        rl_present_frame(p, g_frame, 4, 2, 4, 2, true, g_w, g_h, NULL);
    rl_present_stats_get(p, &st);
    EXPECT(st.replaced > 0);
    rl_present_close(p);
    return 0;
}

TEST(present_headless_swapchain_shows_frames)
{
    setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);
    unsetenv(RL_WINDOW_ENV);
    rl_settings_reset();
    rl_settings_declare_runtime();
    g_win.b = &g_headless;
    rl_window_set_backend(&g_headless);
    int failures = 0;
    CHECK_EQ(rl_window_main(body, &failures), 0);
    CHECK_EQ(failures, 0);
    rl_window_set_backend(NULL);
    rl_settings_reset();
}
