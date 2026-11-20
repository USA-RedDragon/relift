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
    rl_present_slot_end(p, &s, w, h, true, g_w, g_h, NULL);
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
