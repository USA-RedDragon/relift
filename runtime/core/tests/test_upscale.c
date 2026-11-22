/**
 * @file test_upscale.c
 * @brief relift's spatial upscaler choices (upscale.h) as the window pipeline's upscaler stage: every choice keeps a
 * picture at its own size byte for byte, bilinear and nearest give the known values of a two-pixel ramp, and every
 * choice matches the C reference (upscale_ref.c) when it enlarges and when it shrinks, over subrectangles too, and
 * FSR 1 keeps flat pictures flat and matches a C port of its EASU and RCAS (fsr1_ref.c): exactly on lavapipe; on
 * devices that fuse multiplies and adds a few edge pixels differ, where EASU's weights nearly cancel, so up to 2% of
 * the channels may differ by more than 2, within the clamp to the four nearest pixels.
 */
#include "runner.h"

#include "dlss.h"
#include "hostlib.h"
#include "fsr1_ref.h"
#include "upscale_ref.h"

#include <relift/post.h>
#include <relift/settings.h>
#include <relift/upscale.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/** @brief A window-pipeline stage that runs one relift choice at a fixed output size. */
typedef struct test_stage {
    uint32_t choice, ow, oh;
    rl_upscaler *u;
    int records;
} test_stage;

static bool ts_start(void *user, const rl_post_device *d)
{
    test_stage *t = user;
    t->u = rl_upscaler_new(d, t->choice, RL_UPSCALE_BUFFERS);
    return t->u != NULL;
}

static bool ts_plan(void *user, uint32_t w, uint32_t h, uint32_t *ow, uint32_t *oh)
{
    test_stage *t = user;
    (void)w, (void)h;
    *ow = t->ow;
    *oh = t->oh;
    return true;
}

static void ts_record(void *user, uint64_t cmd, uint64_t in, uint64_t out, uint32_t w, uint32_t h, uint32_t ow,
                      uint32_t oh)
{
    test_stage *t = user;
    rl_upscaler_retire(t->u);
    rl_upscale_inputs ui = {
        .color = { .handle = in, .width = w, .height = h },
        .in = { 0, 0, w, h },
        .out = { .handle = out, .width = ow, .height = oh },
        .out_rect = { 0, 0, ow, oh },
        .pre_exposure = 1.0f,
    };
    t->records += rl_upscaler_record(t->u, cmd, &ui);
}

static const rl_post_stage g_stage = { "test", ts_start, ts_plan, ts_record };

/**
 * @brief Runs choice on w by h RGB888 pixels to ow by oh through the window pipeline into out; false without a
 * Vulkan device or when the stage did not run.
 */
static bool run(uint32_t choice, const uint8_t *rgb, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, uint8_t *out)
{
    rl_settings_reset();
    rl_settings_declare_runtime();
    test_stage t = { .choice = choice, .ow = ow, .oh = oh };
    rl_post_set_upscaler(&g_stage, &t);
    uint32_t gw = 0, gh = 0;
    const uint8_t *got = rl_hostlib_vulkan_gipa() ? rl_post_apply(rgb, w, h, &gw, &gh) : rgb;
    bool ok = got != rgb && gw == ow && gh == oh && t.records == 1;
    if (ok)
        memcpy(out, got, (size_t)ow * oh * 3);
    rl_post_set_upscaler(NULL, NULL);
    rl_upscaler_free(t.u);
    rl_settings_reset();
    return ok;
}

static bool have_vulkan(void)
{
    static bool tried, ok;
    if (!tried) {
        tried = true;
        uint8_t px[3] = { 1, 2, 3 }, out[3];
        ok = run(RL_UPSCALE_NEAREST, px, 1, 1, 1, 1, out) && !memcmp(px, out, 3);
        if (!ok)
            printf("    (no Vulkan device: the upscaler is not run)\n");
    }
    return ok;
}

static void scene(uint8_t *rgb, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = rgb + (y * w + x) * 3;
            int dx = (int)x - (int)w / 2, dy = (int)y - (int)h / 3;
            bool disc = dx * dx + dy * dy < (int)(w * w / 16);
            p[0] = disc ? 250 : (uint8_t)(x * 255 / w);
            p[1] = disc ? 30 : (uint8_t)((x ^ y) & 4 ? 200 : 40);
            p[2] = (uint8_t)(y * 255 / h);
        }
}

static void words_of(const uint8_t *rgb, uint32_t *w, size_t n)
{
    for (size_t i = 0; i < n; i++)
        w[i] = rgb[3 * i] | (uint32_t)rgb[3 * i + 1] << 8 | (uint32_t)rgb[3 * i + 2] << 16 | 0xff000000u;
}

TEST(upscale_names_and_spatial_siblings)
{
    CHECK(!strcmp(rl_upscale_name(RL_UPSCALE_GAME), "game"));
    CHECK(!strcmp(rl_upscale_name(RL_UPSCALE_SHARP_BILINEAR), "sharp_bilinear"));
    CHECK(!strcmp(rl_upscale_name(RL_UPSCALE_LANCZOS), "lanczos"));
    CHECK_EQ(rl_upscale_spatial(RL_UPSCALE_BILINEAR), RL_UPSCALE_BILINEAR);
    CHECK_EQ(rl_upscale_spatial(RL_UPSCALE_GAME), RL_UPSCALE_OFF);
    CHECK_EQ(rl_upscale_spatial(RL_UPSCALE_AUTO), RL_UPSCALE_OFF);
    CHECK_EQ(rl_upscale_resolve(RL_UPSCALE_AUTO, true, true), RL_UPSCALE_GAME);
    CHECK_EQ(rl_upscale_resolve(RL_UPSCALE_AUTO, true, false), RL_UPSCALE_DLSS);
    CHECK(rl_upscale_temporal(RL_UPSCALE_FALLBACK_TEMPORAL));
    CHECK_EQ(rl_upscale_spatial(RL_UPSCALE_DLSS), RL_UPSCALE_FSR1);
    CHECK(rl_upscale_temporal(rl_upscale_resolve(RL_UPSCALE_AUTO, true, false)));
    CHECK(!rl_upscale_temporal(RL_UPSCALE_FSR1));
    CHECK_EQ(rl_upscale_resolve(RL_UPSCALE_AUTO, false, true), RL_UPSCALE_OFF);
    CHECK_EQ(rl_upscale_resolve(RL_UPSCALE_GAME, false, true), RL_UPSCALE_OFF);
    CHECK_EQ(rl_upscale_resolve(RL_UPSCALE_BILINEAR, false, true), RL_UPSCALE_BILINEAR);
    CHECK_EQ(rl_upscale_resolve(RL_UPSCALE_LANCZOS, true, true), RL_UPSCALE_LANCZOS);
    CHECK(rl_upscaler_new(NULL, RL_UPSCALE_BILINEAR, RL_UPSCALE_IMAGES) == NULL);
}

TEST(upscale_every_choice_keeps_a_picture_at_its_own_size)
{
    if (!have_vulkan())
        return;
    enum { W = 29, H = 17 };
    static uint8_t in[W * H * 3], out[W * H * 3];
    scene(in, W, H);
    for (uint32_t c = RL_UPSCALE_OFF; c <= RL_UPSCALE_LANCZOS; c++) {
        CHECK(run(c, in, W, H, W, H, out));
        CHECK(memcmp(in, out, sizeof in) == 0);
    }
}

TEST(upscale_bilinear_and_nearest_give_the_known_ramp)
{
    if (!have_vulkan())
        return;
    static const uint8_t ramp[6] = { 0, 0, 0, 255, 255, 255 };
    uint8_t out[12];
    CHECK(run(RL_UPSCALE_BILINEAR, ramp, 2, 1, 4, 1, out));
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[3], 64);
    CHECK_EQ(out[6], 191);
    CHECK_EQ(out[9], 255);
    CHECK(run(RL_UPSCALE_NEAREST, ramp, 2, 1, 4, 1, out));
    CHECK_EQ(out[3], 0);
    CHECK_EQ(out[6], 255);
    CHECK(run(RL_UPSCALE_SHARP_BILINEAR, ramp, 2, 1, 4, 1, out));
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[9], 255);
}

TEST(upscale_every_choice_matches_the_c_reference)
{
    if (!have_vulkan())
        return;
    static const uint32_t sizes[][4] = { { 37, 23, 64, 41 }, { 40, 30, 120, 90 }, { 64, 41, 37, 23 } };
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        uint32_t w = sizes[s][0], h = sizes[s][1], ow = sizes[s][2], oh = sizes[s][3];
        uint8_t *in = malloc((size_t)w * h * 3), *got = malloc((size_t)ow * oh * 3);
        uint32_t *src = malloc((size_t)w * h * 4), *want = malloc((size_t)ow * oh * 4);
        CHECK(in && got && src && want);
        if (!in || !got || !src || !want)
            return;
        scene(in, w, h);
        words_of(in, src, (size_t)w * h);
        for (uint32_t c = RL_UPSCALE_NEAREST; c <= RL_UPSCALE_LANCZOS; c++) {
            rl_upscale_rect ir = { 0, 0, w, h }, orc = { 0, 0, ow, oh };
            upscale_ref(c, src, w, &ir, want, ow, &orc);
            CHECK(run(c, in, w, h, ow, oh, got));
            int worst = 0;
            for (size_t i = 0; i < (size_t)ow * oh * 3; i++) {
                int d = abs((int)got[i] - (int)((want[i / 3] >> (8 * (i % 3))) & 0xff));
                worst = d > worst ? d : worst;
            }
            printf("    %s %ux%u -> %ux%u: worst difference %d\n", rl_upscale_name(c), w, h, ow, oh, worst);
            CHECK(worst <= 1);
        }
        free(in);
        free(got);
        free(src);
        free(want);
    }
}

TEST(upscale_reference_reads_only_the_input_rectangle)
{
    enum { W = 8, H = 6 };
    uint32_t src[W * H], a[16 * 12], b[16 * 12];
    for (int i = 0; i < W * H; i++)
        src[i] = 0xff000000u | (uint32_t)(i * 5);
    rl_upscale_rect ir = { 2, 1, 4, 3 }, orc = { 0, 0, 16, 12 };
    upscale_ref(RL_UPSCALE_LANCZOS, src, W, &ir, a, 16, &orc);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (x < 2 || x >= 6 || y < 1 || y >= 4)
                src[y * W + x] = 0xffffffffu;
    upscale_ref(RL_UPSCALE_LANCZOS, src, W, &ir, b, 16, &orc);
    CHECK(memcmp(a, b, sizeof a) == 0);
}

TEST(upscale_window_runs_relifts_choice_at_the_scale_setting)
{
    if (!have_vulkan())
        return;
    static const uint8_t ramp[6] = { 0, 0, 0, 255, 255, 255 };
    rl_settings_reset();
    rl_settings_declare_runtime();
    uint32_t w = 0, h = 0;
    CHECK(rl_post_apply(ramp, 2, 1, &w, &h) == ramp);
    CHECK(rl_settings_set("picture.upscaler", "game") == RL_SETTING_OK);
    CHECK(rl_post_apply(ramp, 2, 1, &w, &h) == ramp);
    CHECK(rl_settings_set("picture.upscaler", "bilinear") == RL_SETTING_OK);
    const uint8_t *out = rl_post_apply(ramp, 2, 1, &w, &h);
    CHECK_EQ(w, 4);
    CHECK_EQ(h, 2);
    CHECK(out != ramp && out[3] == 64 && out[6] == 191);
    CHECK(rl_settings_set("picture.upscale_scale", "3") == RL_SETTING_OK);
    out = rl_post_apply(ramp, 2, 1, &w, &h);
    CHECK_EQ(w, 6);
    CHECK(out != ramp && out[15] == 255);
    rl_settings_reset();
}

TEST(upscale_fsr1_keeps_flat_pictures_and_matches_its_c_port)
{
    if (!have_vulkan())
        return;
    enum { W = 40, H = 24 };
    static uint8_t flat[W * H * 3], out[2 * W * 2 * H * 3];
    for (size_t i = 0; i < sizeof flat; i++)
        flat[i] = (uint8_t)(i % 3 == 0 ? 200 : i % 3 == 1 ? 90 : 30);
    CHECK(run(RL_UPSCALE_FSR1, flat, W, H, 2 * W, 2 * H, out));
    bool same = true;
    for (size_t i = 0; i < sizeof out; i++)
        same = same && out[i] == flat[i % 3];
    CHECK(same);
    static const uint32_t sizes[][4] = { { 37, 23, 64, 41 }, { 40, 30, 60, 45 }, { 32, 18, 32, 18 } };
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        uint32_t w = sizes[s][0], h = sizes[s][1], ow = sizes[s][2], oh = sizes[s][3];
        uint8_t *in = malloc((size_t)w * h * 3), *got = malloc((size_t)ow * oh * 3);
        uint32_t *src = malloc((size_t)w * h * 4), *want = malloc((size_t)ow * oh * 4);
        CHECK(in && got && src && want);
        if (!in || !got || !src || !want)
            return;
        scene(in, w, h);
        words_of(in, src, (size_t)w * h);
        rl_upscale_rect ir = { 0, 0, w, h }, orc = { 0, 0, ow, oh };
        fsr1_ref(src, w, &ir, want, ow, &orc, 0.25f);
        CHECK(run(RL_UPSCALE_FSR1, in, w, h, ow, oh, got));
        int worst = 0;
        size_t off = 0, over = 0;
        for (size_t i = 0; i < (size_t)ow * oh * 3; i++) {
            int d = abs((int)got[i] - (int)((want[i / 3] >> (8 * (i % 3))) & 0xff));
            over += d > 2;
            if (d > worst)
                worst = d, off = i;
        }
        printf("    fsr1 %ux%u -> %ux%u: worst difference %d at pixel %zu, %zu channels over 2\n", w, h, ow, oh, worst,
               off / 3, over);
        CHECK(over * 50 <= (size_t)ow * oh * 3);
        CHECK(worst <= 48);
        free(in);
        free(got);
        free(src);
        free(want);
    }
}

TEST(upscale_dlss_looks_beside_the_program_and_names_the_folder)
{
    const char *base = getenv("RELIFT_REPORT_DIR");
    char root[512], lib[700];
    snprintf(root, sizeof root, "%s/dlss-find", base && *base ? base : ".");
    mkdir(base && *base ? base : ".", 0755);
    mkdir(root, 0755);
    snprintf(lib, sizeof lib, "%s/libnvidia-ngx-dlss.so.310.9.16", root);
    remove(lib);
    char found[1200] = "", why[1600] = "";
    CHECK(!rl_dlss_find(root, found, sizeof found, why, sizeof why));
    CHECK(strstr(why, root) != NULL);
    CHECK(!rl_dlss_find(NULL, found, sizeof found, why, sizeof why));
    FILE *f = fopen(lib, "wb");
    CHECK(f != NULL);
    if (f)
        fclose(f);
    CHECK(rl_dlss_find(root, found, sizeof found, why, sizeof why));
    CHECK(!strcmp(found, lib));
    remove(lib);
}

TEST(upscale_auto_falls_back_from_dlss_to_fsr2)
{
    CHECK_EQ(rl_upscale_resolve(RL_UPSCALE_AUTO, true, false), RL_UPSCALE_DLSS);
    CHECK_EQ(RL_UPSCALE_FALLBACK_TEMPORAL, RL_UPSCALE_FSR2);
}
