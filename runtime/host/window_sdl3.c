/**
 * @file window_sdl3.c
 * @brief Host windows, Vulkan surfaces and input through SDL3 (zlib
 * license). Built with VIDEO=sdl3.
 *
 * SDL's video calls run on the thread that called init; open, close and
 * surface creation from other threads go through SDL_RunOnMainThread, which
 * the event loop in run serves. Keyboard, mouse and gamepads become
 * rl_input snapshots; with no gamepad connected, pad 0 follows the keyboard.
 * Fingers, the gamepad count, the view size and the back gesture go to the
 * on-screen controller (touchpad.h); when it is in use, frames are drawn at
 * the picture place it names with its overlay on top.
 */
#include <relift/appinfo.h>
#include <relift/input.h>
#include <relift/settings.h>
#include <relift/touchpad.h>
#include <relift/window.h>
#ifdef __ANDROID__
#include <relift/android.h>
#endif

#include "core.h"

#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include "hostlib.h"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct sdl_win {
    rl_window head;
    SDL_Window *w;
    SDL_WindowID id;
    uint32_t width, height;
    SDL_Renderer *r;
    SDL_Texture *t;
    uint32_t tw, th;
    SDL_Texture *ov;
    uint32_t ow, oh;
    bool logical;
    bool no_renderer;
    struct sdl_win *next;
} sdl_win;

static const rl_window_backend g_sdl;
static SDL_ThreadID g_loop;
static Uint32 g_wake_event;
static const char **g_exts;
static SDL_Mutex *g_lock;
static sdl_win *g_wins;
static SDL_Gamepad *g_pads[RL_INPUT_PADS];
static uint64_t g_keys[4];

static void leave(void)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

static const rl_touchpad_host g_touch_host = {
#ifdef __ANDROID__
    .haptic = rl_android_haptic,
#endif
    .leave = leave,
};

static int sdl_init(void)
{
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
    rl_touchpad_set_host(&g_touch_host);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        rl_log(RL_LOG_INFO, "window", "SDL video init failed: %s", SDL_GetError());
        return -1;
    }
    g_loop = SDL_GetCurrentThreadID();
    g_lock = SDL_CreateMutex();
    g_wake_event = SDL_RegisterEvents(1);
    if (!g_lock || !g_wake_event) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        return -1;
    }
    if (!SDL_Vulkan_LoadLibrary(getenv("RELIFT_VULKAN_LIBRARY"))) {
        rl_log(RL_LOG_WARN, "window", "SDL could not load Vulkan: %s", SDL_GetError());
        return 0;
    }
    Uint32 n = 0;
    const char *const *e = SDL_Vulkan_GetInstanceExtensions(&n);
    g_exts = calloc(n + 1, sizeof *g_exts);
    if (!g_exts)
        abort();
    for (Uint32 i = 0; e && i < n; i++)
        g_exts[i] = e[i];
    rl_log(RL_LOG_INFO, "window", "SDL %s video driver", SDL_GetCurrentVideoDriver());
    return 0;
}

static sdl_win *find(SDL_WindowID id)
{
    SDL_LockMutex(g_lock);
    sdl_win *w = g_wins;
    while (w && w->id != id)
        w = w->next;
    SDL_UnlockMutex(g_lock);
    return w;
}

static void pixel_size(sdl_win *w)
{
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(w->w, &pw, &ph);
    __atomic_store_n(&w->width, (uint32_t)(pw > 0 ? pw : 0), __ATOMIC_RELAXED);
    __atomic_store_n(&w->height, (uint32_t)(ph > 0 ? ph : 0), __ATOMIC_RELAXED);
    float density = SDL_GetWindowPixelDensity(w->w), scale = SDL_GetWindowDisplayScale(w->w);
    if (density <= 0)
        density = 1;
    if (scale <= 0)
        scale = density;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(w->w, &ww, &wh);
    SDL_Rect safe = { 0, 0, ww, wh };
    int32_t in[4] = { 0, 0, 0, 0 };
    if (SDL_GetWindowSafeArea(w->w, &safe)) {
        in[0] = (int32_t)((float)safe.x * density);
        in[1] = (int32_t)((float)safe.y * density);
        in[2] = (int32_t)((float)(ww - safe.x - safe.w) * density);
        in[3] = (int32_t)((float)(wh - safe.y - safe.h) * density);
    }
    rl_touchpad_view((uint32_t)(pw > 0 ? pw : 0), (uint32_t)(ph > 0 ? ph : 0), scale, in);
}

static uint32_t pad_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < RL_INPUT_PADS; i++)
        n += g_pads[i] != NULL;
    return n;
}

static void window_event(const SDL_Event *e)
{
    sdl_win *w = find(e->window.windowID);
    if (!w)
        return;
    switch (e->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        rl_window_emit(&w->head, RL_WINDOW_CLOSE);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        rl_window_emit(&w->head, RL_WINDOW_FOCUS_GAINED);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        rl_window_emit(&w->head, RL_WINDOW_FOCUS_LOST);
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        pixel_size(w);
        __atomic_add_fetch(&w->head.generation, 1, __ATOMIC_RELEASE);
        rl_window_emit(&w->head, RL_WINDOW_RESIZED);
        break;
    default:
        break;
    }
}

static void pad_event(const SDL_Event *e)
{
    char name[128] = "";
    if (e->type == SDL_EVENT_GAMEPAD_ADDED) {
        for (int i = 0; i < RL_INPUT_PADS; i++)
            if (!g_pads[i]) {
                g_pads[i] = SDL_OpenGamepad(e->gdevice.which);
                if (!g_pads[i])
                    return;
                const char *n = SDL_GetGamepadName(g_pads[i]);
                SDL_strlcpy(name, n ? n : "", sizeof name);
                rl_touchpad_gamepad(true, name, pad_count());
                return;
            }
        return;
    }
    for (int i = 0; i < RL_INPUT_PADS; i++)
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == e->gdevice.which) {
            const char *n = SDL_GetGamepadName(g_pads[i]);
            SDL_strlcpy(name, n ? n : "", sizeof name);
            SDL_CloseGamepad(g_pads[i]);
            g_pads[i] = NULL;
            rl_touchpad_gamepad(false, name, pad_count());
        }
}

static void finger_event(const SDL_Event *e)
{
    sdl_win *w = find(e->tfinger.windowID);
    if (!w)
        w = g_wins;
    if (!w)
        return;
    int phase = e->type == SDL_EVENT_FINGER_DOWN     ? RL_TOUCHPAD_DOWN
                : e->type == SDL_EVENT_FINGER_MOTION ? RL_TOUCHPAD_MOVE
                : e->type == SDL_EVENT_FINGER_UP     ? RL_TOUCHPAD_UP
                                                     : RL_TOUCHPAD_CANCEL;
    float x = e->tfinger.x * (float)__atomic_load_n(&w->width, __ATOMIC_RELAXED);
    float y = e->tfinger.y * (float)__atomic_load_n(&w->height, __ATOMIC_RELAXED);
    rl_touchpad_finger((uint64_t)e->tfinger.fingerID, phase, x, y);
}

static void key_event(const SDL_Event *e)
{
    if (e->key.scancode == SDL_SCANCODE_AC_BACK) {
        if (e->key.down && !e->key.repeat) {
            if (rl_touchpad_current())
                rl_touchpad_back();
            else
                rl_window_emit(NULL, RL_WINDOW_CLOSE);
        }
        return;
    }
    unsigned sc = (unsigned)e->key.scancode;
    if (sc >= 256)
        return;
    if (e->key.down)
        g_keys[sc / 64] |= 1ull << (sc % 64);
    else
        g_keys[sc / 64] &= ~(1ull << (sc % 64));
}

static const struct {
    SDL_GamepadButton sdl;
    int rl;
} g_buttons[] = {
    { SDL_GAMEPAD_BUTTON_SOUTH, RL_BTN_SOUTH },
    { SDL_GAMEPAD_BUTTON_EAST, RL_BTN_EAST },
    { SDL_GAMEPAD_BUTTON_WEST, RL_BTN_WEST },
    { SDL_GAMEPAD_BUTTON_NORTH, RL_BTN_NORTH },
    { SDL_GAMEPAD_BUTTON_LEFT_STICK, RL_BTN_LEFT_STICK },
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK, RL_BTN_RIGHT_STICK },
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, RL_BTN_LEFT_SHOULDER },
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, RL_BTN_RIGHT_SHOULDER },
    { SDL_GAMEPAD_BUTTON_START, RL_BTN_START },
    { SDL_GAMEPAD_BUTTON_BACK, RL_BTN_BACK },
    { SDL_GAMEPAD_BUTTON_DPAD_UP, RL_BTN_DPAD_UP },
    { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, RL_BTN_DPAD_RIGHT },
    { SDL_GAMEPAD_BUTTON_DPAD_DOWN, RL_BTN_DPAD_DOWN },
    { SDL_GAMEPAD_BUTTON_DPAD_LEFT, RL_BTN_DPAD_LEFT },
    { SDL_GAMEPAD_BUTTON_GUIDE, RL_BTN_GUIDE },
    { SDL_GAMEPAD_BUTTON_MISC1, RL_BTN_MISC },
};

static int16_t axis(SDL_Gamepad *g, SDL_GamepadAxis a, bool flip)
{
    int v = SDL_GetGamepadAxis(g, a);
    if (flip)
        v = -v;
    return (int16_t)(v < -32767 ? -32767 : v > 32767 ? 32767 : v);
}

static void publish(void)
{
    rl_input_snapshot s;
    memset(&s, 0, sizeof s);
    unsigned n = 0;
    for (int i = 0; i < RL_INPUT_PADS; i++) {
        SDL_Gamepad *g = g_pads[i];
        if (!g)
            continue;
        rl_input_pad *p = &s.pads[n++];
        p->connected = true;
        for (size_t b = 0; b < sizeof g_buttons / sizeof g_buttons[0]; b++)
            if (SDL_GetGamepadButton(g, g_buttons[b].sdl))
                p->buttons |= 1u << g_buttons[b].rl;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384)
            p->buttons |= 1u << RL_BTN_LEFT_TRIGGER;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384)
            p->buttons |= 1u << RL_BTN_RIGHT_TRIGGER;
        p->lx = axis(g, SDL_GAMEPAD_AXIS_LEFTX, false);
        p->ly = axis(g, SDL_GAMEPAD_AXIS_LEFTY, true);
        p->rx = axis(g, SDL_GAMEPAD_AXIS_RIGHTX, false);
        p->ry = axis(g, SDL_GAMEPAD_AXIS_RIGHTY, true);
    }
    if (!n)
        s.pads[0].connected = true;
    memcpy(s.keys, g_keys, sizeof s.keys);
    SDL_Keymod m = SDL_GetModState();
    s.modifiers = ((m & SDL_KMOD_CTRL) ? 1u : 0) | ((m & SDL_KMOD_SHIFT) ? 2u : 0) | ((m & SDL_KMOD_ALT) ? 4u : 0) |
                  ((m & SDL_KMOD_GUI) ? 8u : 0) | ((m & SDL_KMOD_CAPS) ? 16u : 0) | ((m & SDL_KMOD_NUM) ? 32u : 0) |
                  ((m & SDL_KMOD_SCROLL) ? 64u : 0);
    float x = 0, y = 0;
    SDL_MouseButtonFlags mb = SDL_GetMouseState(&x, &y);
    s.pointer.x = (int32_t)x;
    s.pointer.y = (int32_t)y;
    s.pointer.buttons = ((mb & SDL_BUTTON_LMASK) ? 1u << RL_PTR_LEFT : 0) | ((mb & SDL_BUTTON_RMASK) ? 1u << RL_PTR_RIGHT : 0) |
                        ((mb & SDL_BUTTON_MMASK) ? 1u << RL_PTR_MIDDLE : 0);
    s.touch_count = rl_touchpad_game_touches(s.touches, RL_INPUT_TOUCHES);
    rl_input_publish(&s);
}

static void handle(const SDL_Event *e)
{
    if (e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST)
        window_event(e);
    else if (e->type == SDL_EVENT_GAMEPAD_ADDED || e->type == SDL_EVENT_GAMEPAD_REMOVED)
        pad_event(e);
    else if (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP)
        key_event(e);
    else if (e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_FINGER_MOTION ||
             e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED)
        finger_event(e);
    else if (e->type == SDL_EVENT_QUIT)
        rl_window_emit(NULL, RL_WINDOW_CLOSE);
}

static void sdl_run(bool (*done)(void *user), void *user)
{
    while (!done(user)) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100))
            continue;
        handle(&e);
        while (SDL_PollEvent(&e))
            handle(&e);
        publish();
    }
}

static void sdl_wake(void)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = g_wake_event;
    SDL_PushEvent(&e);
}

static void on_loop(SDL_MainThreadCallback fn, void *arg)
{
    if (SDL_GetCurrentThreadID() == g_loop)
        fn(arg);
    else
        SDL_RunOnMainThread(fn, arg, true);
}

#define TAG_MAX 8
#define TAG_LEN 64

/** @brief The game's title for this host: the language setting, then the host's preferred languages. */
static const char *app_title(char tags[TAG_MAX][TAG_LEN])
{
    const char *list[TAG_MAX];
    size_t n = 0;
    const char *setting = rl_setting_str("language");
    if (setting && *setting) {
        snprintf(tags[n], TAG_LEN, "%s", setting);
        list[n] = tags[n];
        n++;
    }
    int count = 0;
    SDL_Locale **loc = SDL_GetPreferredLocales(&count);
    for (int i = 0; loc && i < count && n < TAG_MAX; i++) {
        snprintf(tags[n], TAG_LEN, "%s%s%s", loc[i]->language, loc[i]->country ? "_" : "", loc[i]->country ? loc[i]->country : "");
        list[n] = tags[n];
        n++;
    }
    SDL_free(loc);
    return rl_app_title(list, n);
}

static void set_icon(SDL_Window *sw)
{
    size_t n = 0;
    const uint8_t *png = rl_app_icon_png(&n);
    if (!png)
        return;
    SDL_IOStream *io = SDL_IOFromConstMem(png, n);
    SDL_Surface *icon = io ? SDL_LoadPNG_IO(io, true) : NULL;
    if (!icon) {
        rl_log(RL_LOG_WARN, "window", "the game's icon could not be read: %s", SDL_GetError());
        return;
    }
    if (!SDL_SetWindowIcon(sw, icon))
        rl_log(RL_LOG_WARN, "window", "SDL could not set the window icon: %s", SDL_GetError());
    SDL_DestroySurface(icon);
}

typedef struct open_req {
    const char *title;
    uint32_t width, height;
    sdl_win *out;
} open_req;

static void do_open(void *p)
{
    open_req *r = p;
    char tags[TAG_MAX][TAG_LEN];
    const char *title = app_title(tags);
    if (!title)
        title = r->title;
    SDL_SetAppMetadata(title, NULL, NULL);
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
#ifdef __ANDROID__
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
    SDL_Window *sw = SDL_CreateWindow(title, (int)r->width, (int)r->height, flags);
    if (!sw) {
        rl_log(RL_LOG_ERROR, "window", "SDL could not open a window: %s", SDL_GetError());
        return;
    }
    set_icon(sw);
    sdl_win *w = calloc(1, sizeof *w);
    if (!w)
        abort();
    w->head.b = &g_sdl;
    w->w = sw;
    w->id = SDL_GetWindowID(sw);
    pixel_size(w);
    SDL_LockMutex(g_lock);
    w->next = g_wins;
    g_wins = w;
    SDL_UnlockMutex(g_lock);
    r->out = w;
}

static rl_window *sdl_open(const char *title, uint32_t width, uint32_t height)
{
    open_req r = { .title = title, .width = width, .height = height };
    on_loop(do_open, &r);
    return r.out ? &r.out->head : NULL;
}

static void do_close(void *p)
{
    sdl_win *w = p;
    SDL_LockMutex(g_lock);
    for (sdl_win **pp = &g_wins; *pp; pp = &(*pp)->next)
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    SDL_UnlockMutex(g_lock);
    if (w->t)
        SDL_DestroyTexture(w->t);
    if (w->ov)
        SDL_DestroyTexture(w->ov);
    if (w->r)
        SDL_DestroyRenderer(w->r);
    SDL_DestroyWindow(w->w);
    free(w);
}

static void sdl_close(rl_window *h)
{
    if (h)
        on_loop(do_close, h);
}

static void sdl_size(rl_window *h, uint32_t *width, uint32_t *height)
{
    sdl_win *w = (sdl_win *)h;
    *width = __atomic_load_n(&w->width, __ATOMIC_RELAXED);
    *height = __atomic_load_n(&w->height, __ATOMIC_RELAXED);
}

static const char *const *sdl_vulkan_extensions(void) { return g_exts; }

static void *sdl_vulkan_gipa(void)
{
#ifdef __ANDROID__
    return rl_hostlib_vulkan_gipa();
#else
    SDL_FunctionPointer f = SDL_Vulkan_GetVkGetInstanceProcAddr();
    void *p;
    memcpy(&p, &f, sizeof p);
    return p;
#endif
}

typedef struct surface_req {
    sdl_win *w;
    void *instance;
    uint64_t surface;
    bool ok;
} surface_req;

static void do_surface(void *p)
{
    surface_req *r = p;
    VkSurfaceKHR s = 0;
#ifdef __ANDROID__
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(uintptr_t)rl_hostlib_vulkan_gipa();
    PFN_vkCreateAndroidSurfaceKHR create =
        gipa ? (PFN_vkCreateAndroidSurfaceKHR)gipa((VkInstance)r->instance, "vkCreateAndroidSurfaceKHR") : NULL;
    VkAndroidSurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = SDL_GetPointerProperty(SDL_GetWindowProperties(r->w->w), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER,
                                         NULL),
    };
    r->ok = create && info.window && create((VkInstance)r->instance, &info, NULL, &s) == VK_SUCCESS;
    if (!r->ok)
        SDL_SetError("vkCreateAndroidSurfaceKHR failed");
#else
    r->ok = SDL_Vulkan_CreateSurface(r->w->w, (VkInstance)r->instance, NULL, &s);
#endif
    if (!r->ok)
        rl_log(RL_LOG_ERROR, "window", "SDL could not create a Vulkan surface: %s", SDL_GetError());
    memcpy(&r->surface, &s, sizeof r->surface);
}

static int32_t sdl_vulkan_surface(rl_window *h, void *instance, uint64_t *surface)
{
    surface_req r = { .w = (sdl_win *)h, .instance = instance };
    on_loop(do_surface, &r);
    *surface = r.surface;
    return r.ok ? 0 : -3;
}

typedef struct present_req {
    sdl_win *w;
    const uint8_t *rgb;
    uint32_t width, height;
} present_req;

static void do_present(void *p)
{
    present_req *q = p;
    sdl_win *w = q->w;
    if (w->no_renderer)
        return;
    if (!w->r) {
        w->r = SDL_CreateRenderer(w->w, NULL);
        if (!w->r) {
            rl_log(RL_LOG_ERROR, "window", "SDL could not make a renderer: %s", SDL_GetError());
            w->no_renderer = true;
            return;
        }
    }
    if (!w->t || w->tw != q->width || w->th != q->height) {
        if (w->t)
            SDL_DestroyTexture(w->t);
        w->t = SDL_CreateTexture(w->r, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, (int)q->width, (int)q->height);
        if (!w->t) {
            rl_log(RL_LOG_ERROR, "window", "SDL could not make a texture: %s", SDL_GetError());
            w->no_renderer = true;
            return;
        }
        SDL_SetTextureScaleMode(w->t, SDL_SCALEMODE_NEAREST);
        w->tw = q->width;
        w->th = q->height;
        w->logical = false;
    }
    SDL_UpdateTexture(w->t, NULL, q->rgb, (int)(q->width * 3));
    bool pad = rl_touchpad_wanted();
    if (w->logical == pad) {
        SDL_SetRenderLogicalPresentation(w->r, pad ? 0 : (int)q->width, pad ? 0 : (int)q->height,
                                         pad ? SDL_LOGICAL_PRESENTATION_DISABLED
                                             : SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
        w->logical = !pad;
    }
    SDL_SetRenderDrawColor(w->r, 0, 0, 0, 255);
    SDL_RenderClear(w->r);
    if (!pad) {
        SDL_RenderTexture(w->r, w->t, NULL, NULL);
        SDL_RenderPresent(w->r);
        return;
    }
    rl_touchpad_tick(rl_clock_ns());
    int32_t pic[4];
    rl_touchpad_picture(pic);
    SDL_FRect dst = { (float)pic[0], (float)pic[1], (float)pic[2], (float)pic[3] };
    SDL_RenderTexture(w->r, w->t, NULL, &dst);
    uint32_t ow = 0, oh = 0;
    int32_t dirty[4];
    const uint8_t *px = rl_touchpad_pixels(&ow, &oh, dirty);
    if (px && (!w->ov || w->ow != ow || w->oh != oh)) {
        if (w->ov)
            SDL_DestroyTexture(w->ov);
        w->ov = SDL_CreateTexture(w->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, (int)ow, (int)oh);
        w->ow = ow;
        w->oh = oh;
        if (w->ov) {
            SDL_SetTextureBlendMode(w->ov, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            SDL_UpdateTexture(w->ov, NULL, px, (int)(ow * 4));
        }
        dirty[2] = 0;
    }
    if (px && w->ov && dirty[2] > 0 && dirty[3] > 0) {
        SDL_Rect r = { dirty[0], dirty[1], dirty[2], dirty[3] };
        SDL_UpdateTexture(w->ov, &r, px + ((size_t)dirty[1] * ow + (size_t)dirty[0]) * 4, (int)(ow * 4));
    }
    if (px && w->ov)
        SDL_RenderTexture(w->r, w->ov, NULL, NULL);
    SDL_RenderPresent(w->r);
}

static void sdl_present_rgb(rl_window *h, const uint8_t *rgb, uint32_t width, uint32_t height)
{
    present_req q = { .w = (sdl_win *)h, .rgb = rgb, .width = width, .height = height };
    on_loop(do_present, &q);
}

static const rl_window_backend g_sdl = {
    .name = "SDL3",
    .init = sdl_init,
    .run = sdl_run,
    .wake = sdl_wake,
    .open = sdl_open,
    .close = sdl_close,
    .size = sdl_size,
    .vulkan_extensions = sdl_vulkan_extensions,
    .vulkan_gipa = sdl_vulkan_gipa,
    .vulkan_surface = sdl_vulkan_surface,
    .present_rgb = sdl_present_rgb,
};

const rl_window_backend *rl_host_window_backend(void) { return &g_sdl; }
