/**
 * @file settings.c
 * @brief The settings registry: declaration, typed validation, layered
 * values, the JSON file reader, the command-line flags, the schema writer
 * and the per-user folder.
 */
#include <relift/override.h>
#include <relift/settings.h>
#include <relift/ports.h>

#include "core.h"

#include <errno.h>
#include <math.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief The value of one setting. */
typedef struct value {
    bool set;
    int64_t i;
    double f;
    char *s;
} value;

/** @brief A declared setting and its value. */
typedef struct slot {
    const rl_setting_def *def;
    value val;
    value envval;
    char *env_text;
    bool env_warned;
} slot;

static const char *const g_video_choices[] = { "auto", "sdl3", "headless", "none" };
static const char *const g_present_choices[] = { "auto", "fifo", "relaxed", "latest_ready", "mailbox", "immediate" };
static const char *const g_aa_choices[] = { "off", "fxaa", "smaa" };
static const char *const g_aa_quality_choices[] = { "low", "medium", "high", "ultra" };
static const char *const g_filter_choices[] = { "default", "integer", "nearest", "sharp_bilinear" };
static const char *const g_gpu_math_choices[] = { "exact", "native" };
static const char *const g_panel_choices[] = { "auto", "always", "never" };
static const char *const g_upscaler_choices[] = {
    "auto", "game", "off", "nearest", "bilinear", "sharp_bilinear", "lanczos", "fsr1", "fsr2", "fsr3", "taa", "dlss",
};

static const rl_setting_def g_runtime_defs[] = {
    {
        .key = "video",
        .type = RL_SETTING_ENUM,
        .label = "Window backend",
        .description = "auto opens SDL3 when it is built in and a display exists; sdl3 skips the display check; "
                       "headless (or none) opens no window",
        .env = "RELIFT_VIDEO",
        .choices = g_video_choices,
        .nchoices = 4,
    },
    {
        .key = "language",
        .type = RL_SETTING_STRING,
        .label = "Title language",
        .description = "The language of the game's window title: a language name such as AmericanEnglish or French, "
                       "or a locale such as ja or fr_CA. Empty follows the host's language",
        .env = "RELIFT_LANGUAGE",
    },
    {
        .key = "video.present_mode",
        .type = RL_SETTING_ENUM,
        .label = "Present mode",
        .description = "How the host shows a finished frame. auto: fifo for the game's own window, mailbox where the "
                       "surface has it for windows on other displays. fifo: at the display's next refresh. relaxed: "
                       "at once when the frame is late (a variable refresh display shows it as it arrives; a fixed "
                       "one may tear). latest_ready: fifo that drops queued frames a newer one has passed. mailbox: "
                       "the newest frame at each refresh. immediate: at once, may tear. A mode the surface lacks "
                       "falls back to auto",
        .env = "RELIFT_PRESENT_MODE",
        .choices = g_present_choices,
        .nchoices = 6,
    },
    {
        .key = "video.present_trace",
        .type = RL_SETTING_STRING,
        .label = "Present trace",
        .description = "A file to write a line to for each frame the core presenter shows: when the guest handed the "
                       "frame over, when it was queued to the display and when the display showed it (nanoseconds of "
                       "the host clock), where the device can wait for presents. Empty: off",
        .env = "RELIFT_PRESENT_TRACE",
    },
    {
        .key = "video.fullscreen",
        .type = RL_SETTING_BOOL,
        .label = "Fullscreen",
        .description = "Open the game's window fullscreen. A compositor applies variable refresh (adaptive sync) only "
                       "to fullscreen windows, so frames that miss the display's refresh show as soon as they are "
                       "ready there. Off: a normal window",
        .env = "RELIFT_FULLSCREEN",
        .def_int = 0,
    },
    {
        .key = "video.gpu_driver",
        .type = RL_SETTING_STRING,
        .label = "GPU driver",
        .description = "Android: system uses the device's Vulkan driver; the name of an installed driver package "
                       "(a meta.json and its library under the app's gpu_drivers folder) loads that driver instead",
        .env = "RELIFT_GPU_DRIVER",
        .def_str = "system",
    },
    {
        .key = "gpu.math",
        .type = RL_SETTING_ENUM,
        .label = "GPU float math",
        .description = "exact gives the console GPU's float results bit for "
                       "bit; native uses this GPU's own "
                       "multiply-add, special functions and denormal handling, "
                       "which is faster on GPUs that are not "
                       "NVIDIA's and changes the picture from the console's. "
                       "Games that translate shaders read the tables of "
                       "the mode chosen",
        .env = "RELIFT_GPU_MATH",
        .choices = g_gpu_math_choices,
        .nchoices = 2,
    },
    {
        .key = "gpu.clock_floor",
        .type = RL_SETTING_BOOL,
        .label = "Keep GPU clock up",
        .description = "Android: keeps the GPU clock up while a frame's work runs, so the GPU does not clock down "
                       "between frames and stretch the next one: steadier frame times, more battery. Through "
                       "VK_QCOM_queue_perf_hint on Qualcomm's driver, else an ADPF performance hint session told each "
                       "frame's work time against 60 frames a second. Other hosts: no effect",
        .env = "RELIFT_GPU_CLOCK_FLOOR",
        .def_int = 1,
    },
    {
        .key = "video.render_resolution",
        .type = RL_SETTING_INT,
        .label = "Render resolution",
        .description = "The height in lines the game draws its picture at, one of the sizes the game itself offers (a \
guest lists them with rl_render_sizes_register, from facts measured or read from the game's data). 0: the console's own \
size. A game that offers no sizes ignores it, and its settings screen shows no row. A size is a lighter or sharper \
picture the game makes itself; no image is scaled by relift",
        .env = "RELIFT_RENDER_RESOLUTION",
        .def_int = 0,
        .min_int = 0,
        .max_int = 4320,
    },
    {
        .key = "gpu.profile",
        .type = RL_SETTING_BOOL,
        .label = "GPU profile",
        .description = "Times the GPU work of every batch with timestamps: render passes, dispatches, copies, clears, "
                       "uploads and the layer's own work (the present copy, the upscaler), plus the time between "
                       "batches and the presenter's own GPU work, and logs a ranking every 5 seconds. Each timestamp "
                       "waits for the work before it, so the game runs slower while it is on. Off: no timestamps",
        .env = "RELIFT_GPU_PROFILE",
        .def_int = 0,
    },
    {
        .key = "picture.aa",
        .type = RL_SETTING_ENUM,
        .label = "Anti-aliasing",
        .apply = RL_APPLY_LIVE,
        .description = "Smooths the edges of the picture the window shows: fxaa is FXAA 3.11, smaa is SMAA 1x. Anything "
                       "but off changes the picture from the console's",
        .choices = g_aa_choices,
        .nchoices = 3,
    },
    {
        .key = "picture.aa_quality",
        .type = RL_SETTING_ENUM,
        .label = "Anti-aliasing quality",
        .apply = RL_APPLY_LIVE,
        .description = "SMAA's preset: higher finds longer and diagonal edges and costs more GPU time. With smaa on it "
                       "changes the picture from the console's",
        .def_int = 2,
        .choices = g_aa_quality_choices,
        .nchoices = 4,
    },
    {
        .key = "picture.filter",
        .type = RL_SETTING_ENUM,
        .label = "Scaling filter",
        .apply = RL_APPLY_LIVE,
        .description = "How windows scale and sample the picture. default: the window's own scaling (whole "
                       "multiples, or filling the window where a setting such as video.nds_integer_scale asks), "
                       "nearest at whole multiples, sharp_bilinear above 2x when the scale is not whole, bilinear below 2x. integer: whole "
                       "multiples only, sharp. nearest: fill the window, sharp uneven pixels. sharp_bilinear: fill "
                       "the window with even pixels whose edges blend over one screen pixel. Anything but nearest "
                       "sampling changes the picture from the console's",
        .env = "RELIFT_PICTURE_FILTER",
        .choices = g_filter_choices,
        .nchoices = 4,
    },
    {
        .key = "picture.upscaler",
        .type = RL_SETTING_ENUM,
        .label = "Upscaler",
        .apply = RL_APPLY_LIVE,
        .description = "The upscaler a game's own upscaler call (such as DLSS) runs inside the frame, before the "
                       "game's UI: game is the game's own, translated; auto is the game's own where this device "
                       "meets its requirements, else picked by GPU: taa on integrated and mobile GPUs, and on "
                       "discrete ones dlss where it runs, else fsr2, and taa where FSR 2 does not start; off copies the "
                       "picture at its render size; "
                       "nearest, bilinear, sharp_bilinear and lanczos are relift's; fsr1 is AMD FSR 1 (EASU and "
                       "RCAS); fsr2 and fsr3 are AMD FSR 2 and the FSR 3 upscaler, temporal, where the game gives "
                       "depth and motion vectors, else FSR 1; taa is relift's plain temporal upscaler, else "
                       "bilinear; dlss is NVIDIA DLSS, temporal, on an NVIDIA GPU with the driver's NGX library and "
                       "NVIDIA's DLSS library beside the program, else fsr2. Every choice but game changes the "
                       "picture from the console's",
        .env = "RELIFT_UPSCALER",
        .choices = g_upscaler_choices,
        .nchoices = 12,
    },
    {
        .key = "picture.upscale_scale",
        .type = RL_SETTING_INT,
        .label = "Window upscale factor",
        .apply = RL_APPLY_LIVE,
        .description = "For guests that do not upscale inside their frame: with picture.upscaler set to one of "
                       "relift's upscalers, the window's picture is upscaled to this many times its size ahead of "
                       "anti-aliasing and the window's own scaling",
        .def_int = 2,
        .min_int = 1,
        .max_int = 8,
    },
    {
        .key = "picture.taa_sharpness",
        .type = RL_SETTING_FLOAT,
        .label = "TAA sharpening",
        .apply = RL_APPLY_LIVE,
        .description = "How strongly AMD's RCAS sharpens the picture the taa upscaler writes, from 0 (no sharpening "
                       "pass) to 1 (its strongest). Anything but 0 changes the picture further from the console's",
        .env = "RELIFT_TAA_SHARPNESS",
        .def_float = 0.5,
        .min_float = 0.0,
        .max_float = 1.0,
        .step_float = 0.05,
    },
    /* docgen:region debug-settings */
    {
        .key = "renderdoc.capture",
        .type = RL_SETTING_STRING,
        .label = "RenderDoc capture",
        .description = "<present>[:<frames>]: when the program runs under RenderDoc, capture that many frames "
                       "(default 1) from the numbered guest present (counting from 1), including runs with no "
                       "window. file: capture the next present each time a file named capture-next appears in "
                       "the capture folder (the program removes it). Only Vulkan work is captured. Empty: off",
        .env = "RELIFT_RENDERDOC_CAPTURE",
    },
    {
        .key = "renderdoc.capture_dir",
        .type = RL_SETTING_STRING,
        .label = "RenderDoc capture folder",
        .description = "Where RenderDoc writes the captures renderdoc.capture asks for. Empty: the captures folder of "
                       "the user data folder",
        .env = "RELIFT_RENDERDOC_DIR",
    },
    {
        .key = "present.capture",
        .type = RL_SETTING_STRING,
        .label = "Window capture (debugging)",
        .description = "Debugging: <folder>[:<every>][:<from>-<to>] writes the window's pictures exactly as they reach "
                       "the screen, after relift's scaling, picture passes and overlays, as "
                       "<folder>/present-<frame>-d<draw>.png (frame: the number of the guest frame handed to the "
                       "window; draw: the number of the swapchain image drawn, counting from 1), every <every>th draw "
                       "(default 1), only draws of the frames from <from> to before <to> when given, with a log line "
                       "each saying what the presenter drew. Each capture waits for the GPU. Empty: off",
        .env = "RELIFT_PRESENT_CAPTURE",
    },
    {
        .key = "present.flash",
        .type = RL_SETTING_STRING,
        .label = "Flash watch (debugging)",
        .description = "Debugging: <folder>[:<threshold>] reads back every new frame the window shows and, when one "
                       "frame differs sharply from the frames on both sides while those two agree, writes the three as "
                       "<folder>/flash-<frame>/present-<n>.png with flash.json (threshold in luminance steps, default "
                       "10). Each new frame waits for the GPU. Empty: off",
        .env = "RELIFT_PRESENT_FLASH",
    },
    {
        .key = "debug.gdb",
        .type = RL_SETTING_INT,
        .label = "GDB server port",
        .description = "A port on 127.0.0.1 where a GDB remote-protocol server waits for gdb or lldb, which then see "
                       "the guest's threads, registers and memory by guest address. 0: off",
        .env = "RELIFT_DEBUG_GDB",
        .def_int = 0,
        .min_int = 0,
        .max_int = 65535,
    },
    {
        .key = "debug.gdb_wait",
        .type = RL_SETTING_BOOL,
        .label = "Wait for the debugger",
        .description = "With debug.gdb set, the game starts only once a debugger attached and continued",
        .env = "RELIFT_DEBUG_GDB_WAIT",
        .def_int = 0,
    },
    {
        .key = "debug.snapshot",
        .type = RL_SETTING_INT,
        .label = "Diagnostic snapshot presents",
        .description = "In a development build, F12, holding Minus while clicking the right stick, or a "
                       "snapshot-now file in the save folder writes a snapshot folder (memory, GPU, batches, "
                       "threads, the picture and the settings) covering that many presents after the trigger. "
                       "0: no trigger works",
        .env = "RELIFT_DEBUG_SNAPSHOT",
        .def_int = 3,
        .min_int = 0,
        .max_int = 600,
    },
    /* docgen:end */
    {
        .key = "text_entry.text",
        .type = RL_SETTING_STRING,
        .label = "Typed text",
        .description = "When set, the answer to every text the game asks for (a name, for one), given without showing "
                       "the text entry panel. Empty: a run with a window and no input script shows the panel",
        .env = "RELIFT_TEXT_ENTRY_TEXT",
    },
    {
        .key = "text_entry.default",
        .type = RL_SETTING_STRING,
        .label = "Default typed text",
        .description = "The answer when the game asks for text, no panel can be shown (no window, or an input script "
                       "drives the run) and the game offers no text of its own",
        .def_str = "Player",
    },
    {
        .key = "text_entry.panel",
        .type = RL_SETTING_ENUM,
        .label = "Text entry panel",
        .description = "When the panel that takes typed text is shown: auto in a run with a window and no input script; "
                       "always also when an input script drives the run (the run waits for the player); never answers "
                       "from the settings at once",
        .env = "RELIFT_TEXT_ENTRY_PANEL",
        .choices = g_panel_choices,
        .nchoices = 3,
    },    {
        .key = "save.dir",
        .type = RL_SETTING_STRING,
        .label = "Save folder",
        .description = "Where the game's save data is kept. Empty: <user folder>/save when the program names an app, "
                       "else next to the ROM for a cartridge and ./save for other programs",
        .env = "RELIFT_SAVE",
    },
    {
        .key = "vk.device",
        .type = RL_SETTING_STRING,
        .label = "Graphics device",
        .description = "A part of the name of the host graphics device the game uses. Empty: the first device that "
                       "can present",
        .env = "RELIFT_VK_DEVICE",
    },
    {
        .key = "stats.every",
        .type = RL_SETTING_INT,
        .label = "Frame stats",
        .description = "Every this many guest frames, log the frames per second, the process CPU and the CPU the "
                       "presenting thread spent per frame over them. 0: off",
        .env = "RELIFT_STATS",
        .def_int = 0,
        .min_int = 0,
        .max_int = 1000000,
    },
    {
        .key = "stats.trace",
        .type = RL_SETTING_STRING,
        .label = "Frame trace",
        .description = "A file to write the frame trace to: vsyncs, presents and the points each guest layer traces, "
                       "for finding why a vsync went without a new frame. Empty: off",
        .env = "RELIFT_FRAME_TRACE",
    },
};

static char *g_mods[RL_SETTINGS_MODS_MAX];
static size_t g_nmods;
static slot g_slots[RL_SETTINGS_MAX];
static size_t g_nslots;
static char *g_app;
static char *g_file;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void value_free(value *v)
{
    free(v->s);
    memset(v, 0, sizeof *v);
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static bool key_ok(const char *k)
{
    if (!k || !*k || *k == '.' || *k == '_' || (*k >= '0' && *k <= '9'))
        return false;
    for (; *k; k++)
        if (!((*k >= 'a' && *k <= 'z') || (*k >= '0' && *k <= '9') || *k == '_' || *k == '.'))
            return false;
    return true;
}

static bool def_ok(const rl_setting_def *d, const char **why)
{
    if (!key_ok(d->key)) {
        *why = "the key must be lowercase letters, digits, '_' and '.', starting with a letter";
        return false;
    }
    switch (d->type) {
    case RL_SETTING_BOOL:
        if (d->def_int != 0 && d->def_int != 1)
            return *why = "the default of a bool is 0 or 1", false;
        break;
    case RL_SETTING_INT:
        if (d->min_int > d->max_int || d->def_int < d->min_int || d->def_int > d->max_int || d->step_int < 0)
            return *why = "the int range is empty, the step is negative or the default is outside it", false;
        break;
    case RL_SETTING_FLOAT:
        if (!(d->min_float <= d->max_float) || !(d->def_float >= d->min_float && d->def_float <= d->max_float) ||
            d->step_float < 0 || !isfinite(d->min_float) || !isfinite(d->max_float))
            return *why = "the float range is empty or not finite, the step is negative or the default is outside it",
                   false;
        break;
    case RL_SETTING_ENUM:
        if (!d->choices || d->nchoices == 0 || d->def_int < 0 || d->def_int >= (int64_t)d->nchoices)
            return *why = "an enum needs choices and a default that is one of them", false;
        break;
    case RL_SETTING_STRING:
    case RL_SETTING_BINDING:
        break;
    default:
        return *why = "unknown type", false;
    }
    return true;
}

static slot *find(const char *key)
{
    for (size_t i = 0; key && i < g_nslots; i++)
        if (!strcmp(g_slots[i].def->key, key))
            return &g_slots[i];
    return NULL;
}

int rl_settings_declare(const rl_setting_def *defs, size_t n)
{
    int added = 0;
    pthread_mutex_lock(&g_lock);
    size_t fresh = 0;
    for (size_t i = 0; i < n; i++) {
        const char *why = NULL;
        slot *s = find(defs[i].key);
        if (s && s->def == &defs[i])
            continue;
        if (s) {
            rl_log(RL_LOG_ERROR, "settings", "%s is declared twice", defs[i].key);
            goto fail;
        }
        for (size_t j = 0; j < i; j++)
            if (defs[j].key && !strcmp(defs[j].key, defs[i].key)) {
                rl_log(RL_LOG_ERROR, "settings", "%s is declared twice", defs[i].key);
                goto fail;
            }
        if (!def_ok(&defs[i], &why)) {
            rl_log(RL_LOG_ERROR, "settings", "%s: %s", defs[i].key ? defs[i].key : "(no key)", why);
            goto fail;
        }
        fresh++;
    }
    if (g_nslots + fresh > RL_SETTINGS_MAX) {
        rl_log(RL_LOG_ERROR, "settings", "more than %u settings", RL_SETTINGS_MAX);
        goto fail;
    }
    for (size_t i = 0; i < n; i++) {
        slot *s = find(defs[i].key);
        if (s)
            continue;
        g_slots[g_nslots++] = (slot){ .def = &defs[i] };
        added++;
    }
    pthread_mutex_unlock(&g_lock);
    return added;
fail:
    pthread_mutex_unlock(&g_lock);
    return -1;
}

static bool parse_bool(const char *t, bool *out)
{
    static const char *const yes[] = { "true", "1", "yes", "on" };
    static const char *const no[] = { "false", "0", "no", "off" };
    for (size_t i = 0; i < 4; i++) {
        if (!strcmp(t, yes[i]))
            return *out = true, true;
        if (!strcmp(t, no[i]))
            return *out = false, true;
    }
    return false;
}

/** @brief Parses and checks text against def; on failure writes why into err. */
static bool parse_value(const rl_setting_def *d, const char *t, value *out, char *err, size_t errsz)
{
    char *end;
    value v = { .set = true };
    switch (d->type) {
    case RL_SETTING_BOOL: {
        bool b;
        if (!parse_bool(t, &b)) {
            snprintf(err, errsz, "'%s' is not true or false", t);
            return false;
        }
        v.i = b;
        break;
    }
    case RL_SETTING_INT: {
        errno = 0;
        long long x = strtoll(t, &end, 0);
        if (!*t || *end || errno) {
            snprintf(err, errsz, "'%s' is not an integer", t);
            return false;
        }
        if (x < d->min_int || x > d->max_int) {
            snprintf(err, errsz, "%lld is outside %lld to %lld", x, (long long)d->min_int, (long long)d->max_int);
            return false;
        }
        if (d->step_int > 0 && (x - d->min_int) % d->step_int != 0) {
            snprintf(err, errsz, "%lld is not a multiple of %lld from %lld", x, (long long)d->step_int,
                     (long long)d->min_int);
            return false;
        }
        v.i = x;
        break;
    }
    case RL_SETTING_FLOAT: {
        errno = 0;
        double x = strtod(t, &end);
        if (!*t || *end || errno || !isfinite(x)) {
            snprintf(err, errsz, "'%s' is not a number", t);
            return false;
        }
        if (x < d->min_float || x > d->max_float) {
            snprintf(err, errsz, "%g is outside %g to %g", x, d->min_float, d->max_float);
            return false;
        }
        if (d->step_float > 0) {
            double q = (x - d->min_float) / d->step_float;
            if (fabs(q - (double)(long long)(q + 0.5)) > 1e-6) {
                snprintf(err, errsz, "%g is not a multiple of %g from %g", x, d->step_float, d->min_float);
                return false;
            }
        }
        v.f = x;
        break;
    }
    case RL_SETTING_ENUM: {
        uint32_t i = 0;
        while (i < d->nchoices && strcmp(d->choices[i], t))
            i++;
        if (i == d->nchoices) {
            size_t n = (size_t)snprintf(err, errsz, "'%s' is not one of", t);
            for (uint32_t k = 0; k < d->nchoices && n < errsz; k++)
                n += (size_t)snprintf(err + n, errsz - n, "%s%s", k ? ", " : " ", d->choices[k]);
            return false;
        }
        v.i = i;
        break;
    }
    case RL_SETTING_STRING:
        v.s = dup_str(t);
        break;
    case RL_SETTING_BINDING:
        if (!rl_ports_valid_binding(t)) {
            snprintf(err, errsz, "'%s' is not a comma separated list of host inputs such as gamepad:south,key:z", t);
            return false;
        }
        v.s = dup_str(t);
        break;
    }
    if ((d->type == RL_SETTING_STRING || d->type == RL_SETTING_BINDING) && !v.s) {
        snprintf(err, errsz, "out of memory");
        return false;
    }
    *out = v;
    return true;
}

rl_setting_status rl_settings_set(const char *key, const char *val)
{
    char err[256];
    value v;
    pthread_mutex_lock(&g_lock);
    slot *s = find(key);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        rl_log(RL_LOG_WARN, "settings", "unknown setting %s; ignored", key ? key : "(null)");
        return RL_SETTING_UNKNOWN;
    }
    if (!parse_value(s->def, val ? val : "", &v, err, sizeof err)) {
        pthread_mutex_unlock(&g_lock);
        rl_log(RL_LOG_WARN, "settings", "%s: %s; ignored", key, err);
        return RL_SETTING_INVALID;
    }
    value_free(&s->val);
    s->val = v;
    const bool port_binding = s->def->type == RL_SETTING_BINDING && !strncmp(key, "bind.", 5);
    pthread_mutex_unlock(&g_lock);
    if (port_binding)
        rl_ports_apply_setting(key, val ? val : "");
    return RL_SETTING_OK;
}

typedef struct jp {
    const char *p;
} jp;

static void skip_ws(jp *j)
{
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')
        j->p++;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= c - '0';
        else if (c >= 'a' && c <= 'f')
            v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v |= c - 'A' + 10;
        else
            return -1;
    }
    return v;
}

static size_t utf8(unsigned c, char *o)
{
    if (c < 0x80) {
        o[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        o[0] = (char)(0xC0 | (c >> 6));
        o[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    o[0] = (char)(0xE0 | (c >> 12));
    o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    o[2] = (char)(0x80 | (c & 0x3F));
    return 3;
}

/** @brief Reads a JSON string at j into a new buffer; NULL when malformed. */
static char *jstring(jp *j)
{
    if (*j->p != '"')
        return NULL;
    j->p++;
    size_t cap = strlen(j->p) + 1, n = 0;
    char *o = malloc(cap);
    if (!o)
        return NULL;
    while (*j->p && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c < 0x20)
            goto bad;
        if (c != '\\') {
            o[n++] = (char)c;
            continue;
        }
        c = (unsigned char)*j->p++;
        switch (c) {
        case '"':
        case '\\':
        case '/':
            o[n++] = (char)c;
            break;
        case 'b':
            o[n++] = '\b';
            break;
        case 'f':
            o[n++] = '\f';
            break;
        case 'n':
            o[n++] = '\n';
            break;
        case 'r':
            o[n++] = '\r';
            break;
        case 't':
            o[n++] = '\t';
            break;
        case 'u': {
            int u = hex4(j->p);
            if (u < 0)
                goto bad;
            j->p += 4;
            if (u >= 0xD800 && u < 0xE000)
                u = 0xFFFD;
            n += utf8((unsigned)u, o + n);
            break;
        }
        default:
            goto bad;
        }
    }
    if (*j->p != '"')
        goto bad;
    j->p++;
    o[n] = 0;
    return o;
bad:
    free(o);
    return NULL;
}

static bool add_mod(const char *path)
{
    if (!path || !*path)
        return false;
    if (g_nmods >= RL_SETTINGS_MODS_MAX) {
        rl_log(RL_LOG_WARN, "settings", "more than %u mods; %s ignored", RL_SETTINGS_MODS_MAX, path);
        return false;
    }
    char *p = dup_str(path);
    if (!p)
        return false;
    g_mods[g_nmods++] = p;
    return true;
}

/** @brief Reads a JSON array of strings at j into the mod list; false when malformed. */
static bool jmods(jp *j, int *applied)
{
    j->p++;
    skip_ws(j);
    if (*j->p == ']') {
        j->p++;
        return true;
    }
    for (;;) {
        skip_ws(j);
        char *s = jstring(j);
        if (!s)
            return false;
        pthread_mutex_lock(&g_lock);
        if (add_mod(s))
            ++*applied;
        pthread_mutex_unlock(&g_lock);
        free(s);
        skip_ws(j);
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p++ == ']')
            return true;
        return false;
    }
}

int rl_settings_load_json(const char *text, const char *origin)
{
    jp j = { text };
    int applied = 0;
    skip_ws(&j);
    if (*j.p++ != '{')
        goto bad;
    skip_ws(&j);
    if (*j.p == '}')
        j.p++;
    else
        for (;;) {
            skip_ws(&j);
            char *key = jstring(&j);
            if (!key)
                goto bad;
            skip_ws(&j);
            if (*j.p++ != ':') {
                free(key);
                goto bad;
            }
            skip_ws(&j);
            char *val = NULL;
            bool null = false;
            if (!strcmp(key, "mods") && *j.p == '[') {
                bool ok = jmods(&j, &applied);
                free(key);
                if (!ok)
                    goto bad;
                skip_ws(&j);
                if (*j.p == ',') {
                    j.p++;
                    continue;
                }
                if (*j.p++ != '}')
                    goto bad;
                break;
            }
            if (*j.p == '"') {
                val = jstring(&j);
            } else {
                const char *b = j.p;
                while (*j.p && *j.p != ',' && *j.p != '}' && *j.p != ' ' && *j.p != '\n' && *j.p != '\r' &&
                       *j.p != '\t')
                    j.p++;
                size_t n = (size_t)(j.p - b);
                if (n == 4 && !strncmp(b, "null", 4))
                    null = true;
                else if (n && *b != '{' && *b != '[') {
                    val = malloc(n + 1);
                    if (val) {
                        memcpy(val, b, n);
                        val[n] = 0;
                    }
                }
            }
            if (!val && !null) {
                free(key);
                goto bad;
            }
            if (val && rl_settings_set(key, val) == RL_SETTING_OK)
                applied++;
            free(key);
            free(val);
            skip_ws(&j);
            if (*j.p == ',') {
                j.p++;
                continue;
            }
            if (*j.p++ != '}')
                goto bad;
            break;
        }
    skip_ws(&j);
    if (*j.p)
        goto bad;
    return applied;
bad:
    rl_log(RL_LOG_WARN, "settings", "%s is not a JSON object of settings; ignored", origin ? origin : "settings");
    return -1;
}

int rl_settings_load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        rl_log(RL_LOG_WARN, "settings", "cannot read %s: %s; ignored", path, strerror(errno));
        return -1;
    }
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    while (buf) {
        size_t got = fread(buf + n, 1, cap - n - 1, f);
        n += got;
        if (n + 1 < cap)
            break;
        if (cap > (size_t)1 << 24) {
            free(buf);
            buf = NULL;
            break;
        }
        char *bigger = realloc(buf, cap * 2);
        if (!bigger) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = bigger;
        cap *= 2;
    }
    bool bad = ferror(f) || !buf;
    fclose(f);
    if (bad) {
        free(buf);
        rl_log(RL_LOG_WARN, "settings", "cannot read %s; ignored", path);
        return -1;
    }
    buf[n] = 0;
    int r = rl_settings_load_json(buf, path);
    free(buf);
    return r;
}

/** @brief The end of the JSON value at p (string, array, object or bare token); NULL when malformed. */
static const char *json_end(const char *p)
{
    int depth = 0;
    do {
        if (*p == '"') {
            p++;
            while (*p && *p != '"')
                p += *p == '\\' && p[1] ? 2 : 1;
            if (!*p)
                return NULL;
            p++;
        } else if (*p == '[' || *p == '{') {
            depth++;
            p++;
        } else if (*p == ']' || *p == '}') {
            if (!depth)
                return p;
            depth--;
            p++;
        } else if (depth) {
            if (!*p)
                return NULL;
            p++;
        } else {
            while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t')
                p++;
            return p;
        }
    } while (depth);
    return p;
}

typedef struct entry {
    char *key;
    char *raw;
} entry;

static void entries_free(entry *e, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(e[i].key);
        free(e[i].raw);
    }
    free(e);
}

static char *slice(const char *b, const char *e)
{
    char *s = malloc((size_t)(e - b) + 1);
    if (s) {
        memcpy(s, b, (size_t)(e - b));
        s[e - b] = 0;
    }
    return s;
}

/** @brief The keys of a flat JSON object with each value's text as written; false when malformed. */
static bool entries_parse(const char *text, entry **out, size_t *count)
{
    jp j = { text };
    entry *e = NULL;
    size_t n = 0;
    skip_ws(&j);
    if (*j.p++ != '{')
        return false;
    skip_ws(&j);
    if (*j.p == '}') {
        j.p++;
        goto done;
    }
    for (;;) {
        skip_ws(&j);
        char *key = jstring(&j);
        if (!key)
            goto bad;
        skip_ws(&j);
        const char *b = *j.p == ':' ? ++j.p : NULL;
        if (b) {
            skip_ws(&j);
            b = j.p;
        }
        const char *end = b ? json_end(b) : NULL;
        entry *ne = end && end > b ? realloc(e, (n + 1) * sizeof *e) : NULL;
        if (!ne) {
            free(key);
            goto bad;
        }
        e = ne;
        e[n].key = key;
        e[n].raw = slice(b, end);
        n++;
        j.p = end;
        skip_ws(&j);
        if (*j.p == ',') {
            j.p++;
            continue;
        }
        if (*j.p++ != '}')
            goto bad;
        break;
    }
done:
    skip_ws(&j);
    if (*j.p)
        goto bad;
    *out = e;
    *count = n;
    return true;
bad:
    entries_free(e, n);
    return false;
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long len = ftell(f);
        if (len >= 0 && len < (1L << 24) && fseek(f, 0, SEEK_SET) == 0) {
            buf = malloc((size_t)len + 1);
            if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) {
                free(buf);
                buf = NULL;
            }
            if (buf)
                buf[len] = 0;
        }
    }
    fclose(f);
    return buf;
}

static void make_dirs(const char *path)
{
    char d[4096];
    size_t n = strlen(path);
    if (n >= sizeof d)
        return;
    memcpy(d, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (d[i] != '/' && d[i] != '\\')
            continue;
        char c = d[i];
        d[i] = 0;
#if defined(_WIN32)
        _mkdir(d);
#else
        mkdir(d, 0755);
#endif
        d[i] = c;
    }
}

static void json_quote(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c < 0x20)
            fprintf(f, "\\u%04x", c);
        else
            fputc(c, f);
    }
    fputc('"', f);
}

bool rl_settings_file(char *out, size_t size)
{
    pthread_mutex_lock(&g_lock);
    bool ok = false;
    if (g_file) {
        int n = snprintf(out, size, "%s", g_file);
        ok = n > 0 && (size_t)n < size;
    }
    pthread_mutex_unlock(&g_lock);
    return ok || rl_settings_user_file(out, size);
}

/** @brief The JSON text of a parsed value of d. */
static char *json_value(const rl_setting_def *d, const value *v)
{
    char num[64];
    switch (d->type) {
    case RL_SETTING_BOOL:
        return dup_str(v->i ? "true" : "false");
    case RL_SETTING_INT:
        snprintf(num, sizeof num, "%lld", (long long)v->i);
        return dup_str(num);
    case RL_SETTING_FLOAT:
        snprintf(num, sizeof num, "%.17g", v->f);
        return dup_str(num);
    default:
        break;
    }
    const char *s = d->type == RL_SETTING_ENUM ? d->choices[v->i] : v->s ? v->s : "";
    size_t cap = strlen(s) * 6 + 3;
    char *o = malloc(cap);
    if (!o)
        return NULL;
    size_t n = 0;
    o[n++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            o[n++] = '\\';
            o[n++] = (char)c;
        } else if (c < 0x20) {
            n += (size_t)snprintf(o + n, cap - n, "\\u%04x", c);
        } else {
            o[n++] = (char)c;
        }
    }
    o[n++] = '"';
    o[n] = 0;
    return o;
}

bool rl_settings_save(const char *key, const char *val)
{
    char err[256];
    value v;
    memset(&v, 0, sizeof v);
    pthread_mutex_lock(&g_lock);
    slot *s = find(key);
    if (!s || (val && !parse_value(s->def, val, &v, err, sizeof err))) {
        pthread_mutex_unlock(&g_lock);
        rl_log(RL_LOG_WARN, "settings", "%s: %s; not saved", key ? key : "(null)", s ? err : "unknown setting");
        return false;
    }
    char *raw = val ? json_value(s->def, &v) : NULL;
    value_free(&s->val);
    s->val = v;
    const bool port_binding = s->def->type == RL_SETTING_BINDING && !strncmp(key, "bind.", 5);
    const char *def_str = s->def->def_str;
    pthread_mutex_unlock(&g_lock);
    if (port_binding)
        rl_ports_apply_setting(key, val ? val : def_str ? def_str : "");
    char path[4096];
    if (!rl_settings_file(path, sizeof path)) {
        free(raw);
        return false;
    }
    char *text = read_text(path);
    entry *e = NULL;
    size_t n = 0;
    if (text && !entries_parse(text, &e, &n)) {
        rl_log(RL_LOG_WARN, "settings", "%s is not a JSON object of settings; %s not saved", path, key);
        free(text);
        free(raw);
        return false;
    }
    free(text);
    size_t at = n;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(e[i].key, key))
            at = i;
    if (raw && at == n) {
        entry *ne = realloc(e, (n + 1) * sizeof *e);
        if (!ne) {
            entries_free(e, n);
            free(raw);
            return false;
        }
        e = ne;
        e[n].key = dup_str(key);
        e[n].raw = NULL;
        n++;
    }
    if (at < n) {
        free(e[at].raw);
        e[at].raw = raw;
        raw = NULL;
    }
    free(raw);
    make_dirs(path);
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f != NULL;
    if (f) {
        fputs("{", f);
        bool first = true;
        for (size_t i = 0; i < n; i++) {
            if (!e[i].raw)
                continue;
            fputs(first ? "\n  " : ",\n  ", f);
            first = false;
            json_quote(f, e[i].key);
            fprintf(f, ": %s", e[i].raw);
        }
        fputs(first ? "}\n" : "\n}\n", f);
        ok = !ferror(f);
        if (fclose(f) != 0)
            ok = false;
    }
    entries_free(e, n);
#if defined(_WIN32)
    if (ok)
        remove(path);
#endif
    if (ok && rename(tmp, path) != 0)
        ok = false;
    if (!ok) {
        remove(tmp);
        rl_log(RL_LOG_WARN, "settings", "cannot write %s; %s not saved", path, key);
    }
    return ok;
}

/** @brief The value of `--name value` or `--name=value` at argv[*i]; NULL when the flag is another. */
static const char *flag_value(int argc, char *const *argv, int *i, const char *name, bool *missing)
{
    size_t n = strlen(name);
    const char *a = argv[*i];
    if (strncmp(a, name, n))
        return NULL;
    if (a[n] == '=')
        return a + n + 1;
    if (a[n])
        return NULL;
    if (*i + 1 >= argc) {
        *missing = true;
        return NULL;
    }
    return argv[++*i];
}

int rl_settings_args(int argc, char *const *argv)
{
    int applied = 0, n;
    rl_settings_declare_runtime();
    bool file_flag = false;
    for (int i = 1; i < argc; i++) {
        bool missing = false;
        const char *v = flag_value(argc, argv, &i, "--relift-settings", &missing);
        if (missing)
            rl_log(RL_LOG_WARN, "settings", "--relift-settings needs a file; ignored");
        if (!v)
            continue;
        file_flag = true;
        pthread_mutex_lock(&g_lock);
        free(g_file);
        g_file = dup_str(v);
        pthread_mutex_unlock(&g_lock);
        if ((n = rl_settings_load_file(v)) > 0)
            applied += n;
    }
    char user[4096];
    if (!file_flag && rl_settings_user_file(user, sizeof user)) {
        FILE *f = fopen(user, "rb");
        if (f) {
            fclose(f);
            if ((n = rl_settings_load_file(user)) > 0)
                applied += n;
        }
    }
    for (int i = 1; i < argc; i++) {
        bool missing = false;
        const char *v = flag_value(argc, argv, &i, "--relift-mods", &missing);
        if (missing)
            rl_log(RL_LOG_WARN, "settings", "--relift-mods needs a path; ignored");
        if (!v)
            continue;
        pthread_mutex_lock(&g_lock);
        if (add_mod(v))
            applied++;
        pthread_mutex_unlock(&g_lock);
    }
    for (int i = 1; i < argc; i++) {
        bool missing = false;
        const char *v = flag_value(argc, argv, &i, "--relift-set", &missing);
        if (missing)
            rl_log(RL_LOG_WARN, "settings", "--relift-set needs key=value; ignored");
        if (!v)
            continue;
        const char *eq = strchr(v, '=');
        if (!eq || eq == v) {
            rl_log(RL_LOG_WARN, "settings", "--relift-set %s is not key=value; ignored", v);
            continue;
        }
        char key[128];
        size_t kn = (size_t)(eq - v);
        if (kn >= sizeof key) {
            rl_log(RL_LOG_WARN, "settings", "--relift-set: the key is too long; ignored");
            continue;
        }
        memcpy(key, v, kn);
        key[kn] = 0;
        if (rl_settings_set(key, eq + 1) == RL_SETTING_OK)
            applied++;
    }
    return applied;
}

void rl_settings_declare_runtime(void)
{
    rl_settings_declare(g_runtime_defs, sizeof g_runtime_defs / sizeof g_runtime_defs[0]);
}

void rl_settings_reset(void)
{
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_nslots; i++) {
        value_free(&g_slots[i].val);
        value_free(&g_slots[i].envval);
        free(g_slots[i].env_text);
    }
    for (size_t i = 0; i < g_nmods; i++)
        free(g_mods[i]);
    g_nmods = 0;
    memset(g_slots, 0, sizeof g_slots);
    g_nslots = 0;
    free(g_app);
    g_app = NULL;
    free(g_file);
    g_file = NULL;
    pthread_mutex_unlock(&g_lock);
}

/** @brief The slot of key when it has the type, else NULL (with a debug log). */
static slot *typed(const char *key, rl_setting_type type)
{
    slot *s = find(key);
    if (!s || s->def->type != type) {
        rl_log(RL_LOG_DEBUG, "settings", "%s is not a declared %s setting", key ? key : "(null)",
               type == RL_SETTING_BOOL    ? "bool"
               : type == RL_SETTING_INT   ? "int"
               : type == RL_SETTING_FLOAT ? "float"
               : type == RL_SETTING_ENUM  ? "enum"
               : type == RL_SETTING_STRING ? "string"
                                           : "binding");
        return NULL;
    }
    return s;
}

/** @brief The value that wins for s: the environment, the set value, the default. Lock held. */
static const value *effective(slot *s, value *dflt)
{
    const rl_setting_def *d = s->def;
    const char *e = d->env ? getenv(d->env) : NULL;
    if (e && *e) {
        if (!s->env_text || strcmp(s->env_text, e)) {
            char err[256];
            value_free(&s->envval);
            free(s->env_text);
            s->env_text = dup_str(e);
            s->env_warned = false;
            if (!parse_value(d, e, &s->envval, err, sizeof err) && !s->env_warned) {
                s->env_warned = true;
                rl_log(RL_LOG_WARN, "settings", "%s=%s: %s; ignored", d->env, e, err);
            }
        }
        if (s->envval.set)
            return &s->envval;
    }
    if (s->val.set)
        return &s->val;
    memset(dflt, 0, sizeof *dflt);
    dflt->i = d->def_int;
    dflt->f = d->def_float;
    dflt->s = (char *)(d->def_str ? d->def_str : "");
    return dflt;
}

bool rl_setting_bool(const char *key)
{
    pthread_mutex_lock(&g_lock);
    slot *s = typed(key, RL_SETTING_BOOL);
    value d;
    bool r = s && effective(s, &d)->i != 0;
    pthread_mutex_unlock(&g_lock);
    return r;
}

int64_t rl_setting_int(const char *key)
{
    pthread_mutex_lock(&g_lock);
    slot *s = typed(key, RL_SETTING_INT);
    value d;
    int64_t r = s ? effective(s, &d)->i : 0;
    pthread_mutex_unlock(&g_lock);
    return r;
}

double rl_setting_float(const char *key)
{
    pthread_mutex_lock(&g_lock);
    slot *s = typed(key, RL_SETTING_FLOAT);
    value d;
    double r = s ? effective(s, &d)->f : 0;
    pthread_mutex_unlock(&g_lock);
    return r;
}

uint32_t rl_setting_enum(const char *key)
{
    pthread_mutex_lock(&g_lock);
    slot *s = typed(key, RL_SETTING_ENUM);
    value d;
    uint32_t r = s ? (uint32_t)effective(s, &d)->i : 0;
    pthread_mutex_unlock(&g_lock);
    return r;
}

const char *rl_setting_str(const char *key)
{
    pthread_mutex_lock(&g_lock);
    slot *s = typed(key, RL_SETTING_STRING);
    value d;
    const char *r = s ? effective(s, &d)->s : "";
    pthread_mutex_unlock(&g_lock);
    return r;
}

const char *rl_setting_binding(const char *key)
{
    pthread_mutex_lock(&g_lock);
    slot *s = typed(key, RL_SETTING_BINDING);
    value d;
    const char *r = s ? effective(s, &d)->s : "";
    pthread_mutex_unlock(&g_lock);
    return r;
}

void rl_settings_dump(FILE *out)
{
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_nslots; i++) {
        slot *s = &g_slots[i];
        value d;
        const value *v = effective(s, &d);
        const char *src = s->envval.set ? "env" : s->val.set ? "set" : "default";
        switch (s->def->type) {
        case RL_SETTING_BOOL:
            fprintf(out, "%s = %s (%s)\n", s->def->key, v->i ? "true" : "false", src);
            break;
        case RL_SETTING_INT:
            fprintf(out, "%s = %lld (%s)\n", s->def->key, (long long)v->i, src);
            break;
        case RL_SETTING_FLOAT:
            fprintf(out, "%s = %g (%s)\n", s->def->key, v->f, src);
            break;
        case RL_SETTING_ENUM:
            fprintf(out, "%s = %s (%s)\n", s->def->key,
                    v->i >= 0 && (size_t)v->i < s->def->nchoices ? s->def->choices[v->i] : "?", src);
            break;
        default:
            fprintf(out, "%s = %s (%s)\n", s->def->key, v->s ? v->s : "", src);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

typedef struct sink {
    char *buf;
    size_t size, len;
} sink;

static void put(sink *k, const char *s, size_t n)
{
    if (k->len < k->size && k->buf) {
        size_t room = k->size - k->len - 1;
        memcpy(k->buf + k->len, s, n < room ? n : room);
    }
    k->len += n;
}

static void puts_(sink *k, const char *s) { put(k, s, strlen(s)); }

static void put_json_str(sink *k, const char *s)
{
    puts_(k, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char tmp[8];
        if (c == '"' || c == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)c;
            put(k, tmp, 2);
        } else if (c < 0x20) {
            put(k, tmp, (size_t)snprintf(tmp, sizeof tmp, "\\u%04x", c));
        } else {
            put(k, (const char *)&c, 1);
        }
    }
    puts_(k, "\"");
}

static void put_fmt(sink *k, const char *fmt, ...) RL_PRINTF(2, 3);
static void put_fmt(sink *k, const char *fmt, ...)
{
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    put(k, tmp, n > 0 ? (size_t)n : 0);
}

static void put_field(sink *k, const char *name, const char *val)
{
    put_fmt(k, ",\"%s\":", name);
    put_json_str(k, val ? val : "");
}

size_t rl_settings_schema_json(char *buf, size_t size)
{
    static const char *const types[] = { "bool", "int", "float", "enum", "string", "key_binding" };
    sink k = { buf, size, 0 };
    pthread_mutex_lock(&g_lock);
    puts_(&k, "[");
    for (size_t i = 0; i < g_nslots; i++) {
        const rl_setting_def *d = g_slots[i].def;
        puts_(&k, i ? ",{\"key\":" : "{\"key\":");
        put_json_str(&k, d->key);
        put_fmt(&k, ",\"type\":\"%s\"", types[d->type]);
        put_field(&k, "label", d->label);
        put_field(&k, "description", d->description);
        put_fmt(&k, ",\"apply\":\"%s\"", d->apply == RL_APPLY_LIVE ? "live" : "start");
        switch (d->type) {
        case RL_SETTING_BOOL:
            put_fmt(&k, ",\"default\":%s", d->def_int ? "true" : "false");
            break;
        case RL_SETTING_INT:
            put_fmt(&k, ",\"default\":%lld,\"min\":%lld,\"max\":%lld,\"step\":%lld", (long long)d->def_int,
                    (long long)d->min_int, (long long)d->max_int, (long long)d->step_int);
            break;
        case RL_SETTING_FLOAT:
            put_fmt(&k, ",\"default\":%.17g,\"min\":%.17g,\"max\":%.17g,\"step\":%.17g", d->def_float,
                    d->min_float, d->max_float, d->step_float);
            break;
        case RL_SETTING_ENUM:
            puts_(&k, ",\"default\":");
            put_json_str(&k, d->choices[d->def_int]);
            puts_(&k, ",\"choices\":[");
            for (uint32_t c = 0; c < d->nchoices; c++) {
                if (c)
                    puts_(&k, ",");
                put_json_str(&k, d->choices[c]);
            }
            puts_(&k, "]");
            break;
        case RL_SETTING_STRING:
            put_field(&k, "default", d->def_str);
            break;
        case RL_SETTING_BINDING:
            put_field(&k, "default", d->def_str);
            break;
        }
        if (d->env)
            put_field(&k, "env", d->env);
        puts_(&k, "}");
    }
    puts_(&k, "]");
    pthread_mutex_unlock(&g_lock);
    if (k.buf && k.size)
        k.buf[k.len < k.size ? k.len : k.size - 1] = 0;
    return k.len;
}

static bool absolute(const char *p, rl_dir_os os)
{
    if (!p || !*p)
        return false;
    if (os == RL_DIR_WINDOWS)
        return (p[0] && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) || (p[0] == '\\' && p[1] == '\\');
    return p[0] == '/';
}

bool rl_settings_dir_for(char *out, size_t size, rl_dir_os os, const char *app, const char *home,
                         const char *xdg_data, const char *appdata)
{
    char sep = os == RL_DIR_WINDOWS ? '\\' : '/';
    char base[4096];
    int n;
    if (!app || !*app || strpbrk(app, "/\\") || !strcmp(app, ".") || !strcmp(app, ".."))
        return false;
    switch (os) {
    case RL_DIR_MACOS:
        if (!absolute(home, os))
            return false;
        n = snprintf(base, sizeof base, "%s/Library/Application Support", home);
        break;
    case RL_DIR_WINDOWS:
        if (absolute(appdata, os))
            n = snprintf(base, sizeof base, "%s", appdata);
        else if (absolute(home, os))
            n = snprintf(base, sizeof base, "%s\\AppData\\Roaming", home);
        else
            return false;
        break;
    default:
        if (absolute(xdg_data, os))
            n = snprintf(base, sizeof base, "%s", xdg_data);
        else if (absolute(home, os))
            n = snprintf(base, sizeof base, "%s/.local/share", home);
        else
            return false;
        break;
    }
    while (n > 1 && (base[n - 1] == '/' || base[n - 1] == '\\'))
        base[--n] = 0;
    n = snprintf(out, size, "%s%c%s", base, sep, app);
    return n > 0 && (size_t)n < size;
}

bool rl_settings_user_dir(char *out, size_t size, const char *app)
{
#if defined(_WIN32)
    return rl_settings_dir_for(out, size, RL_DIR_WINDOWS, app, getenv("USERPROFILE"), NULL, getenv("APPDATA"));
#elif defined(__APPLE__)
    return rl_settings_dir_for(out, size, RL_DIR_MACOS, app, getenv("HOME"), NULL, NULL);
#else
    return rl_settings_dir_for(out, size, RL_DIR_XDG, app, getenv("HOME"), getenv("XDG_DATA_HOME"), NULL);
#endif
}

void rl_settings_set_app(const char *app)
{
    pthread_mutex_lock(&g_lock);
    free(g_app);
    g_app = app && *app ? dup_str(app) : NULL;
    pthread_mutex_unlock(&g_lock);
}

const char *rl_settings_app(void) { return g_app; }

bool rl_settings_user_file(char *out, size_t size)
{
    char dir[4096];
    if (!g_app || !rl_settings_user_dir(dir, sizeof dir, g_app))
        return false;
    int n = snprintf(out, size, "%s/settings.json", dir);
    return n > 0 && (size_t)n < size;
}

bool relift_setting_bool(const char *key) { return rl_setting_bool(key); }
int64_t relift_setting_int(const char *key) { return rl_setting_int(key); }
double relift_setting_float(const char *key) { return rl_setting_float(key); }
uint32_t relift_setting_enum(const char *key) { return rl_setting_enum(key); }
const char *relift_setting_str(const char *key) { return rl_setting_str(key); }
const char *relift_setting_binding(const char *key)
{
    return rl_setting_binding(key);
}

float relift_setting_binding_value(const char *key)
{
    return rl_ports_read_binding(rl_setting_binding(key), 0);
}

size_t rl_settings_mod_count(void)
{
    pthread_mutex_lock(&g_lock);
    size_t n = g_nmods;
    pthread_mutex_unlock(&g_lock);
    return n;
}

const char *rl_settings_mod(size_t index)
{
    pthread_mutex_lock(&g_lock);
    const char *p = index < g_nmods ? g_mods[index] : NULL;
    pthread_mutex_unlock(&g_lock);
    return p;
}

uint32_t rl_render_resolution_lines(void)
{
    int64_t v = rl_setting_int("video.render_resolution");
    return v > 0 ? (uint32_t)v : 0;
}

static rl_render_sizes_fn g_render_sizes;

void rl_render_sizes_register(rl_render_sizes_fn fn) { g_render_sizes = fn; }

size_t rl_render_sizes(rl_render_size *out, size_t max)
{
    return g_render_sizes ? g_render_sizes(out, max) : 0;
}
