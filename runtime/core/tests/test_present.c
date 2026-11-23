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
#include "vkdev.h"
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

TEST(present_mode_choice_follows_the_setting_and_falls_back)
{
    const int32_t wayland[] = { RLVK_PRESENT_MODE_MAILBOX_KHR, RLVK_PRESENT_MODE_FIFO_LATEST_READY_KHR,
                                RLVK_PRESENT_MODE_FIFO_KHR, RLVK_PRESENT_MODE_IMMEDIATE_KHR };
    const int32_t relaxed[] = { RLVK_PRESENT_MODE_FIFO_KHR, RLVK_PRESENT_MODE_FIFO_RELAXED_KHR };
    const int32_t fifo_only[] = { RLVK_PRESENT_MODE_FIFO_KHR };
    /* auto takes the caller's base where offered, else FIFO */
    CHECK_EQ(rl_present_mode_choose(RL_PRESENT_MODE_AUTO, RLVK_PRESENT_MODE_FIFO_KHR, wayland, 4),
             RLVK_PRESENT_MODE_FIFO_KHR);
    CHECK_EQ(rl_present_mode_choose(RL_PRESENT_MODE_AUTO, RLVK_PRESENT_MODE_MAILBOX_KHR, wayland, 4),
             RLVK_PRESENT_MODE_MAILBOX_KHR);
    CHECK_EQ(rl_present_mode_choose(RL_PRESENT_MODE_AUTO, RLVK_PRESENT_MODE_MAILBOX_KHR, relaxed, 2),
             RLVK_PRESENT_MODE_FIFO_KHR);
    /* a named mode is used where offered */
    CHECK_EQ(rl_present_mode_choose(RL_PRESENT_MODE_RELAXED, RLVK_PRESENT_MODE_FIFO_KHR, relaxed, 2),
             RLVK_PRESENT_MODE_FIFO_RELAXED_KHR);
    CHECK_EQ(rl_present_mode_choose(RL_PRESENT_MODE_LATEST_READY, RLVK_PRESENT_MODE_FIFO_KHR, wayland, 4),
             RLVK_PRESENT_MODE_FIFO_LATEST_READY_KHR);
    CHECK_EQ(rl_present_mode_choose(RL_PRESENT_MODE_IMMEDIATE, RLVK_PRESENT_MODE_FIFO_KHR, wayland, 4),
             RLVK_PRESENT_MODE_IMMEDIATE_KHR);
    CHECK_EQ(rl_present_mode_choose(RL_PRESENT_MODE_FIFO, RLVK_PRESENT_MODE_MAILBOX_KHR, wayland, 4),
             RLVK_PRESENT_MODE_FIFO_KHR);
    /* a named mode the surface lacks falls back to auto, then FIFO */
    CHECK_EQ(rl_present_mode_choose(RL_PRESENT_MODE_RELAXED, RLVK_PRESENT_MODE_FIFO_KHR, wayland, 4),
             RLVK_PRESENT_MODE_FIFO_KHR);
    CHECK_EQ(rl_present_mode_choose(RL_PRESENT_MODE_MAILBOX, RLVK_PRESENT_MODE_MAILBOX_KHR, fifo_only, 1),
             RLVK_PRESENT_MODE_FIFO_KHR);
    CHECK_EQ(rl_present_mode_choose(RL_PRESENT_MODE_RELAXED, RLVK_PRESENT_MODE_FIFO_KHR, NULL, 0),
             RLVK_PRESENT_MODE_FIFO_KHR);
    /* a choice past the list is auto */
    CHECK_EQ(rl_present_mode_choose(99, RLVK_PRESENT_MODE_FIFO_KHR, wayland, 4), RLVK_PRESENT_MODE_FIFO_KHR);
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

/** @brief Plays a guest GPU layer: copies a w by h RGBA8 picture into the slot's image and waits for the GPU. */
static bool fill_slot(const rl_present_slot *s, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    const rlvk_dev *d = rl_vkdev();
    rlvk_buf stage = { 0 };
    if (!d || !rl_vkdev_buf_make(d, &stage, (rlvk_size)w * h * 4, RLVK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, false))
        return false;
    memcpy(stage.map, rgba, (size_t)w * h * 4);
    rl_vkdev_buf_flush(d, &stage);
    rlvk_handle pool = 0, cmd = 0, fence = 0;
    rlvk_command_pool_create_info pi = { .sType = RLVK_ST_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = d->family };
    d->vkCreateCommandPool(d->device, &pi, NULL, &pool);
    rlvk_command_buffer_allocate_info ai = {
        .sType = RLVK_ST_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1
    };
    d->vkAllocateCommandBuffers(d->device, &ai, &cmd);
    rlvk_fence_create_info fi = { .sType = RLVK_ST_FENCE_CREATE_INFO };
    d->vkCreateFence(d->device, &fi, NULL, &fence);
    rlvk_command_buffer_begin_info bi = { .sType = RLVK_ST_COMMAND_BUFFER_BEGIN_INFO };
    d->vkBeginCommandBuffer(cmd, &bi);
    rlvk_image_memory_barrier b = {
        .sType = RLVK_ST_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = RLVK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = RLVK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = RLVK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = RLVK_QUEUE_FAMILY_IGNORED,
        .image = s->image,
        .subresourceRange = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    d->vkCmdPipelineBarrier(cmd, RLVK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, RLVK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                            NULL, 1, &b);
    rlvk_buffer_image_copy c = { .imageSubresource = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
    d->vkCmdCopyBufferToImage(cmd, stage.buffer, s->image, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    b.srcAccessMask = RLVK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = RLVK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    d->vkCmdPipelineBarrier(cmd, RLVK_PIPELINE_STAGE_TRANSFER_BIT, RLVK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                            1, &b);
    d->vkEndCommandBuffer(cmd);
    rlvk_submit_info si = { .sType = RLVK_ST_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    rl_vkdev_guest_queue_lock();
    bool ok = d->vkQueueSubmit(d->guest_queue, 1, &si, fence) == RLVK_SUCCESS;
    rl_vkdev_guest_queue_unlock();
    ok = ok && d->vkWaitForFences(d->device, 1, &fence, 1, ~(uint64_t)0) == RLVK_SUCCESS;
    d->vkDestroyFence(d->device, fence, NULL);
    d->vkDestroyCommandPool(d->device, pool, NULL);
    rl_vkdev_buf_free(d, &stage);
    return ok;
}

/** @brief Shows a w by h RGB picture through a slot, as a GPU guest layer would, and captures the window. */
static uint8_t *capture_slot(rl_present *p, const uint8_t *rgb, uint32_t w, uint32_t h, uint32_t *cw, uint32_t *ch)
{
    uint8_t *rgba = malloc((size_t)w * h * 4);
    rl_present_slot s;
    if (!rgba || !rl_present_slot_begin(p, w, h, &s)) {
        free(rgba);
        return NULL;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        memcpy(rgba + i * 4, rgb + i * 3, 3);
        rgba[i * 4 + 3] = 255;
    }
    if (!fill_slot(&s, rgba, w, h)) {
        rl_present_slot_abort(p, &s);
        free(rgba);
        return NULL;
    }
    free(rgba);
    rl_present_set_fit(p, true);
    rl_present_slot_end(p, &s, w, h);
    return rl_present_capture(p, 5000, cw, ch);
}

/** @brief A w by h picture with a diagonal edge, to give the anti-aliasing passes something to change. */
static uint8_t *diagonal(uint32_t w, uint32_t h)
{
    uint8_t *rgb = malloc((size_t)w * h * 3);
    for (uint32_t y = 0; rgb && y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            memset(rgb + ((size_t)y * w + x) * 3, x * h > y * w ? 255 : 0, 3);
    return rgb;
}

#define EXPECT(c)                                                                                                    \
    do {                                                                                                             \
        if (!(c)) {                                                                                                  \
            fprintf(stderr, "    %s:%d: %s\n", __FILE__, __LINE__, #c);                                              \
            (*failures)++;                                                                                           \
        }                                                                                                            \
    } while (0)

/** @brief The test overlay: where it puts the picture and its pixels, the view's size, changed by the test. */
static uint8_t *g_ov;
static int32_t g_ov_place[4] = { 10, 20, 100, 50 };
static uint32_t g_ov_ver, g_ov_seen;

static bool ov_place(void *user, int32_t rect[4])
{
    (void)user;
    memcpy(rect, g_ov_place, sizeof g_ov_place);
    return true;
}

static const uint8_t *ov_pixels(void *user, uint64_t now_ns, uint32_t *width, uint32_t *height, int32_t dirty[4])
{
    (void)user;
    (void)now_ns;
    *width = g_w;
    *height = g_h;
    uint32_t v = __atomic_load_n(&g_ov_ver, __ATOMIC_ACQUIRE);
    bool changed = v != g_ov_seen;
    g_ov_seen = v;
    dirty[0] = dirty[1] = 0;
    dirty[2] = changed ? (int32_t)g_w : 0;
    dirty[3] = changed ? (int32_t)g_h : 0;
    return g_ov;
}

/** @brief Paints the test overlay: a half-covering red block over the picture's corner and an opaque blue square. */
static void ov_paint(uint32_t blue_x)
{
    memset(g_ov, 0, (size_t)g_w * g_h * 4);
    for (uint32_t y = 0; y < 30; y++)
        for (uint32_t x = 0; x < 50; x++)
            memcpy(g_ov + ((size_t)y * g_w + x) * 4, (const uint8_t[4]){ 64, 0, 0, 128 }, 4);
    for (uint32_t y = 150; y < 160; y++)
        for (uint32_t x = blue_x; x < blue_x + 10; x++)
            memcpy(g_ov + ((size_t)y * g_w + x) * 4, (const uint8_t[4]){ 0, 0, 255, 255 }, 4);
    __atomic_add_fetch(&g_ov_ver, 1, __ATOMIC_RELEASE);
}

static uint32_t word(const uint8_t *rgb) { return (uint32_t)rgb[0] | (uint32_t)rgb[1] << 8 | (uint32_t)rgb[2] << 16; }

/** @brief Whether a captured pixel is the overlay's blend over the frame color rgb (RGB only). */
static bool blended(const uint8_t *got, const uint8_t *rgb, uint32_t x, uint32_t y)
{
    uint32_t want = rl_present_overlay_blend(word(rgb), *(const uint32_t *)(g_ov + ((size_t)y * g_w + x) * 4));
    const uint8_t w8[3] = { (uint8_t)want, (uint8_t)(want >> 8), (uint8_t)(want >> 16) };
    return same(got, w8);
}

/** @brief The overlay on a presenter: placed picture, blended corner, opaque square, a change shown with no new frame. */
static void overlay_checks(rl_present *p, int *failures)
{
    g_ov = calloc((size_t)g_w * g_h, 4);
    if (!g_ov)
        return;
    ov_paint(200);
    rl_present_overlay ov = { .place = ov_place, .pixels = ov_pixels };
    rl_present_set_overlay(p, &ov);
    rl_settings_set("picture.filter", "integer");
    uint32_t w = 0, h = 0, sw = 0, sh = 0;
    uint8_t *cpu = capture(p, &w, &h);
    uint8_t *gpu = capture_slot(p, g_frame, 4, 2, &sw, &sh);
    EXPECT(cpu && gpu && w == g_w && h == g_h && sw == w && sh == h);
    static const uint8_t black[3] = { 0, 0, 0 }, blue[3] = { 0, 0, 255 };
    for (int i = 0; i < 2 && cpu && gpu && w == g_w && h == g_h; i++) {
        const uint8_t *img = i ? gpu : cpu;
        EXPECT(same(px(img, w, 12, 35), &g_frame[0]));
        EXPECT(same(px(img, w, 108, 68), &g_frame[21]));
        EXPECT(same(px(img, w, 120, 45), black));
        EXPECT(blended(px(img, w, 5, 5), black, 5, 5));
        EXPECT(blended(px(img, w, 30, 25), &g_frame[0], 30, 25));
        EXPECT(!same(px(img, w, 30, 25), &g_frame[0]));
        EXPECT(same(px(img, w, 205, 155), blue));
    }
    if (cpu && gpu && sw == w && sh == h)
        EXPECT(!memcmp(cpu, gpu, (size_t)w * h * 4));
    free(cpu);
    free(gpu);
    rl_present_stats before, now;
    rl_present_stats_get(p, &before);
    ov_paint(250);
    for (int i = 0; i < 100; i++) {
        usleep(10000);
        rl_present_stats_get(p, &now);
        if (now.presented > before.presented)
            break;
    }
    EXPECT(now.presented > before.presented);
    uint8_t *moved = rl_present_snapshot(p, 5000, &w, &h);
    EXPECT(moved && w == g_w && same(px(moved, w, 255, 155), blue));
    EXPECT(moved && w == g_w && same(px(moved, w, 205, 155), black));
    free(moved);
    rl_present_set_overlay(p, NULL);
    free(g_ov);
    g_ov = NULL;
}

static int body(void *arg)
{
    int *failures = arg;
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

    rl_settings_set("picture.filter", "integer");
    uint32_t sw = 0, sh = 0;
    uint8_t *cpu = capture(p, &w, &h);
    uint8_t *gpu = capture_slot(p, g_frame, 4, 2, &sw, &sh);
    EXPECT(cpu && gpu && w == sw && h == sh);
    EXPECT(cpu && gpu && w == sw && h == sh && !memcmp(cpu, gpu, (size_t)w * h * 4));
    free(cpu);
    free(gpu);

    uint8_t *edge = diagonal(48, 32);
    rl_settings_set("picture.aa", "off");
    uint8_t *plain = NULL, *aa_cpu = NULL, *aa_gpu = NULL;
    rl_present_frame(p, edge, 48, 32, 48, 32, true, g_w, g_h, NULL);
    plain = rl_present_capture(p, 5000, &w, &h);
    EXPECT(rl_settings_set("picture.aa", "fxaa") == RL_SETTING_OK);
    rl_present_frame(p, edge, 48, 32, 48, 32, true, g_w, g_h, NULL);
    aa_cpu = rl_present_capture(p, 5000, &w, &h);
    aa_gpu = capture_slot(p, edge, 48, 32, &sw, &sh);
    EXPECT(plain && aa_cpu && aa_gpu && sw == w && sh == h);
    if (plain && aa_cpu && aa_gpu && sw == w && sh == h) {
        EXPECT(memcmp(plain, aa_cpu, (size_t)w * h * 4) != 0);
        EXPECT(!memcmp(aa_cpu, aa_gpu, (size_t)w * h * 4));
    }
    rl_settings_set("picture.aa", "off");
    EXPECT(rl_settings_set("picture.upscaler", "bilinear") == RL_SETTING_OK);
    EXPECT(rl_settings_set("picture.upscale_scale", "2") == RL_SETTING_OK);
    rl_present_frame(p, edge, 48, 32, 48, 32, true, g_w, g_h, NULL);
    uint8_t *up_cpu = rl_present_capture(p, 5000, &w, &h);
    uint8_t *up_gpu = capture_slot(p, edge, 48, 32, &sw, &sh);
    EXPECT(up_cpu && up_gpu && sw == w && sh == h);
    if (up_cpu && up_gpu && sw == w && sh == h)
        EXPECT(!memcmp(up_cpu, up_gpu, (size_t)w * h * 4));
    if (up_cpu && plain)
        EXPECT(memcmp(up_cpu, plain, (size_t)w * h * 4) != 0);
    free(up_cpu);
    free(up_gpu);
    rl_settings_set("picture.upscaler", "off");
    free(edge);
    free(plain);
    free(aa_cpu);
    free(aa_gpu);

    overlay_checks(p, failures);

    rl_present_slot s[4];
    uint32_t got = 0;
    for (; got < 4 && rl_present_slot_begin(p, 4, 2, &s[got]); got++)
        ;
    EXPECT(got == 2);
    for (uint32_t i = 0; i < got; i++)
        rl_present_slot_abort(p, &s[i]);
    rl_present_close(p);
    return 0;
}

/** @brief A still the compile screen put up and took away before the game's first frame must not stop that frame. */
static int still_body(void *arg)
{
    int *failures = arg;
    rl_present *p = rl_present_open(&g_win);
    if (!p) {
        printf("    (no Vulkan with headless surfaces: the swapchain is not run)\n");
        return 0;
    }
    static uint8_t card[16 * 16 * 3];
    memset(card, 0x40, sizeof card);
    rl_present_still(p, card, 16, 16);
    usleep(200000);
    rl_present_still(p, NULL, 0, 0);
    usleep(200000);
    uint32_t w = 0, h = 0;
    uint8_t *img = capture(p, &w, &h);
    EXPECT(img != NULL);
    free(img);
    return 0;
}

TEST(a_still_taken_away_before_the_first_frame_lets_the_frame_through)
{
    setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);
    unsetenv(RL_WINDOW_ENV);
    rl_settings_reset();
    rl_settings_declare_runtime();
    g_win.b = &g_headless;
    rl_window_set_backend(&g_headless);
    int failures = 0;
    CHECK_EQ(rl_window_main(still_body, &failures), 0);
    CHECK_EQ(failures, 0);
    rl_window_set_backend(NULL);
    rl_settings_reset();
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

#ifdef RL_HOST_VIDEO
static int sdl_body(void *arg)
{
    int *failures = arg;
    const rl_window_backend *b = rl_window_backend_current();
    if (!b || !b->open) {
        printf("    (no window backend: the SDL window is not run)\n");
        return 0;
    }
    rl_window *win = b->open("relift test", 320, 240);
    EXPECT(win != NULL);
    rl_present *p = win ? rl_window_presenter(win) : NULL;
    if (!p) {
        printf("    (the window has no presenter: not run)\n");
        return 0;
    }
    rl_settings_set("picture.filter", "integer");
    EXPECT(rl_window_present_rgb(win, g_frame, 4, 2));
    uint32_t w = 0, h = 0;
    uint8_t *img = rl_present_capture(p, 5000, &w, &h);
    EXPECT(img && w && h);
    rl_present_rect r;
    rl_present_place(4, 2, w, h, true, &r);
    uint32_t fx = 99, fy = 99;
    if (img && w && h && r.w && r.h) {
        EXPECT(same(px(img, w, r.x + 1, r.y + 1), &g_frame[0]));
        EXPECT(same(px(img, w, r.x + r.w - 2, r.y + r.h - 2), &g_frame[21]));
        EXPECT(rl_window_frame_point(win, (int32_t)(r.x + 1), (int32_t)(r.y + 1), &fx, &fy));
        EXPECT(fx == 0 && fy == 0);
        EXPECT(rl_window_frame_point(win, (int32_t)(r.x + r.w - 2), (int32_t)(r.y + r.h - 2), &fx, &fy));
        EXPECT(fx == 3 && fy == 1);
        EXPECT(!rl_window_frame_point(win, 0, 0, &fx, &fy));
    }
    free(img);
    uint32_t sw = 0, sh = 0;
    uint8_t *gpu = capture_slot(p, g_frame, 4, 2, &sw, &sh);
    EXPECT(gpu && sw == w && sh == h);
    if (gpu && sw == w && sh == h)
        EXPECT(same(px(gpu, sw, r.x + 1, r.y + 1), &g_frame[0]));
    free(gpu);
    b->close(win);
    return 0;
}

TEST(present_sdl_window_shows_cpu_and_gpu_frames)
{
    setenv(RL_WINDOW_ENV, "sdl3", 1);
    rl_settings_reset();
    rl_settings_declare_runtime();
    rl_window_set_backend(NULL);
    int failures = 0;
    CHECK_EQ(rl_window_main(sdl_body, &failures), 0);
    CHECK_EQ(failures, 0);
    unsetenv(RL_WINDOW_ENV);
    rl_settings_reset();
}
#endif

TEST(vkdev_chain_links_a_whole_chain_in_front_of_what_was_there)
{
    struct link {
        int32_t stype;
        void *next;
    } a = { 1, NULL }, b = { 2, NULL }, c = { 3, NULL };
    a.next = &b;
    rl_vkdev_build build;
    memset(&build, 0, sizeof build);
    build.pnext = &c;
    rl_vkdev_chain(&build, &a);
    CHECK(build.pnext == &a);
    CHECK(a.next == &b);
    CHECK(b.next == &c);
    CHECK(c.next == NULL);
    rl_vkdev_chain(&build, NULL);
    CHECK(build.pnext == &a);
}

TEST(present_headless_swapchain_frames_pass_validation)
{
    const char *dir = getenv("RELIFT_REPORT_DIR");
    char log[1024];
    snprintf(log, sizeof log, "%s/present_validation.txt", dir && *dir ? dir : ".");
    remove(log);
    setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);
    setenv("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation", 1);
    setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
    setenv("VK_KHRONOS_VALIDATION_REPORT_FLAGS", "error", 1);
    setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_LOG_MSG", 1);
    setenv("VK_KHRONOS_VALIDATION_LOG_FILENAME", log, 1);
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
    FILE *f = fopen(log, "r");
    if (!f) {
        printf("    (no validation layer: not checked)\n");
        return;
    }
    static char text[1 << 16];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    if (strstr(text, "Validation Error"))
        printf("%s\n", text);
    CHECK(strstr(text, "SYNC-HAZARD") == NULL);
    CHECK(strstr(text, "Validation Error") == NULL);
}
