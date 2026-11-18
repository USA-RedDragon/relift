/**
 * @file picture.c
 * @brief The picture settings (n64.picture.*): how the renderer draws and
 * scans out, each one a parallel-rdp option. Their declared defaults are the
 * console's picture; a program replaces them with its developer's choices
 * (game.toml [n64.picture]) before declaring them, and a run overrides them
 * like any setting. With n64.picture.mode at "console" every other key is
 * ignored and the picture is the console's.
 */
#include "n64_int.h"

#include <relift/settings.h>

#include <stdlib.h>
#include <string.h>

static const char *const g_modes[] = {"console", "custom"};
static const char *const g_upscales[] = {"1x", "2x", "4x", "8x", "auto"};
static const char *const g_deinterlace[] = {"bob", "weave"};

#define BOOL_DEF(k, l, d, v) {.key = "n64.picture." k, .type = RL_SETTING_BOOL, .label = l, .description = d, .def_int = v}

static const rl_setting_def g_console[] = {
    {
        .key = "n64.picture.mode",
        .type = RL_SETTING_ENUM,
        .label = "Picture",
        .description = "Console: the console's picture at its own resolution, every other picture setting ignored. "
                       "Custom: the picture the other picture settings describe, which changes the picture from the console's",
        .choices = g_modes,
        .nchoices = 2,
    },
    {
        .key = "n64.picture.upscale",
        .type = RL_SETTING_ENUM,
        .label = "Internal resolution",
        .description = "The RDP draws at this multiple of the console's resolution; auto picks the one whose picture "
                       "is nearest the screen's height. Above 1x it changes the picture from the console's",
        .choices = g_upscales,
        .nchoices = 5,
    },
    {
        .key = "n64.picture.downscale_steps",
        .type = RL_SETTING_INT,
        .label = "Downscale steps",
        .description = "Halvings of an upscaled picture before it is shown; enough of them gives the console's "
                       "resolution with the upscaled picture averaged into it. It changes the picture from the console's",
        .min_int = 0,
        .max_int = 3,
        .step_int = 1,
    },
    BOOL_DEF("super_sampled_readback", "Super-sampled read back",
             "The game reads back the upscaled framebuffer averaged to its own resolution, not one drawn at it. On, it "
             "changes the picture from the console's",
             0),
    BOOL_DEF("readback_dither", "Dither the read back",
             "Dithers the averaged framebuffer a super-sampled read back gives the game. Off, it changes the picture from "
             "the console's",
             1),
    BOOL_DEF("native_tex_rect", "Texture rectangles at native resolution",
             "Draws texture rectangles without upscaling, as games that copy pixels one to one expect. Off, it changes the "
             "picture from the console's",
             1),
    BOOL_DEF("native_texture_lod", "Texture detail at native resolution",
             "Picks texture detail levels as at the console's resolution. On, it changes the picture from the console's",
             0),
    BOOL_DEF("vi_aa", "Video anti-aliasing",
             "The video interface smooths polygon edges when the game asks. Off, it changes the picture from the "
             "console's",
             1),
    BOOL_DEF("vi_resample", "Video resampling",
             "The video interface scales the framebuffer bilinearly when the game asks, else by repeating pixels. Off, it "
             "changes the picture from the console's",
             1),
    BOOL_DEF("vi_serrate", "Interlacing",
             "The video interface interlaces when the game asks. Off, it changes the picture from the console's", 1),
    BOOL_DEF("dither_filter", "Dither filter",
             "The video interface smooths the RDP's dither when the game asks. Off, it changes the picture from the "
             "console's",
             1),
    BOOL_DEF("divot", "Divot filter",
             "The video interface removes one-pixel gaps on edges when the game asks. Off, it changes the picture "
             "from the console's",
             1),
    BOOL_DEF("gamma_dither", "Gamma dither",
             "The video interface dithers its gamma when the game asks. Off, it changes the picture from the "
             "console's",
             1),
    {
        .key = "n64.picture.crop_overscan",
        .type = RL_SETTING_INT,
        .label = "Overscan crop",
        .description = "Lines cropped from the top and bottom, and a matching width from the sides. Above 0, it changes the "
                       "picture from the console's",
        .min_int = 0,
        .max_int = 64,
        .step_int = 1,
    },
    {
        .key = "n64.picture.deinterlace",
        .type = RL_SETTING_ENUM,
        .label = "Deinterlacing",
        .description = "Bob shows each field on its own; weave blends it with the field before, which changes the "
                       "picture from the console's",
        .choices = g_deinterlace,
        .nchoices = 2,
    },
};

#define NDEFS (sizeof g_console / sizeof g_console[0])

static rl_setting_def g_defs[NDEFS];
static uint32_t g_screen_h;

void rl_n64_picture_set_screen(uint32_t height) { __atomic_store_n(&g_screen_h, height, __ATOMIC_RELAXED); }

uint32_t rl_n64_auto_upscale(uint32_t screen_height)
{
    uint32_t s = 1;
    while (s < 8 && (uint64_t)screen_height * screen_height * 2 > (uint64_t)240 * s * 240 * s * 4)
        s *= 2;
    return s;
}

void rl_n64_picture_upscaled(uint32_t want, uint32_t got)
{
    if (got != want)
        rl_log(RL_LOG_WARN, "n64", "the Vulkan device cannot hold the RDP at %ux; it draws at %ux", want, got);
    if (got > 1)
        rl_log(RL_LOG_INFO, "n64", "the RDP draws at %ux the console's resolution", got);
}

static bool parse_int(const char *t, int64_t *out)
{
    char *end = NULL;
    long long v = strtoll(t, &end, 10);
    if (!*t || *end)
        return false;
    *out = v;
    return true;
}

/** @brief Sets the default of the def keyed n64.picture.<name> to text; false when it does not fit. */
static bool set_default(const char *name, size_t name_len, const char *text)
{
    for (size_t i = 0; i < NDEFS; i++) {
        rl_setting_def *d = &g_defs[i];
        const char *k = d->key + strlen("n64.picture.");
        if (strlen(k) != name_len || memcmp(k, name, name_len) != 0)
            continue;
        int64_t v = 0;
        switch (d->type) {
        case RL_SETTING_BOOL:
            if (!strcmp(text, "true"))
                v = 1;
            else if (strcmp(text, "false"))
                return false;
            break;
        case RL_SETTING_INT:
            if (!parse_int(text, &v) || v < d->min_int || v > d->max_int)
                return false;
            break;
        case RL_SETTING_ENUM:
            for (v = 0; v < (int64_t)d->nchoices && strcmp(d->choices[v], text); v++)
                ;
            if (v == (int64_t)d->nchoices)
                return false;
            break;
        default:
            return false;
        }
        d->def_int = v;
        return true;
    }
    return false;
}

void rl_n64_declare_picture(const char *const *defaults)
{
    memcpy(g_defs, g_console, sizeof g_defs);
    for (const char *const *p = defaults; p && *p; p++) {
        const char *eq = strchr(*p, '=');
        if (!eq || !set_default(*p, (size_t)(eq - *p), eq + 1))
            rl_log(RL_LOG_WARN, "n64", "the program's picture default %s is not valid; it is ignored", *p);
    }
    rl_settings_declare(g_defs, NDEFS);
}

void rl_n64_picture_get(rl_n64_picture *out)
{
    if (rl_setting_enum("n64.picture.mode") == 0) {
        *out = (rl_n64_picture){
            .upscale = 1,
            .readback_dither = true,
            .native_tex_rect = true,
            .vi_aa = true,
            .vi_resample = true,
            .vi_serrate = true,
            .dither_filter = true,
            .divot = true,
            .gamma_dither = true,
        };
        return;
    }
    uint32_t upscale = rl_setting_enum("n64.picture.upscale");
    *out = (rl_n64_picture){
        .upscale = upscale == 4 ? rl_n64_auto_upscale(__atomic_load_n(&g_screen_h, __ATOMIC_RELAXED)) : 1u << upscale,
        .downscale_steps = (uint32_t)rl_setting_int("n64.picture.downscale_steps"),
        .super_sampled_readback = rl_setting_bool("n64.picture.super_sampled_readback"),
        .readback_dither = rl_setting_bool("n64.picture.readback_dither"),
        .native_tex_rect = rl_setting_bool("n64.picture.native_tex_rect"),
        .native_texture_lod = rl_setting_bool("n64.picture.native_texture_lod"),
        .vi_aa = rl_setting_bool("n64.picture.vi_aa"),
        .vi_resample = rl_setting_bool("n64.picture.vi_resample"),
        .vi_serrate = rl_setting_bool("n64.picture.vi_serrate"),
        .dither_filter = rl_setting_bool("n64.picture.dither_filter"),
        .divot = rl_setting_bool("n64.picture.divot"),
        .gamma_dither = rl_setting_bool("n64.picture.gamma_dither"),
        .crop_overscan = (uint32_t)rl_setting_int("n64.picture.crop_overscan"),
        .weave = rl_setting_enum("n64.picture.deinterlace") == 1,
    };
}
