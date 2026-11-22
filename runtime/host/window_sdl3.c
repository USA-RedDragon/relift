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
 * the picture place it names with its overlay on top. While a text entry
 * panel is open (textentry.h), the window's presenter shows the panel over
 * the last picture, typed text and keys go to the panel and the guest gets
 * no input.
 */
#include <relift/appinfo.h>
#include <relift/input.h>
#include <relift/post.h>
#include <relift/settings.h>
#include <relift/textentry.h>
#include <relift/touchpad.h>
#include <relift/window.h>

#ifdef __ANDROID__
#include <relift/android.h>
#endif

#include "core.h"
#include "present.h"

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

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define RL_MOBILE 1
#endif

/** @brief SDL's sharp bilinear sampling: PIXELART from SDL 3.4 (blends only at texel edges), else linear. */
#if SDL_VERSION_ATLEAST(3, 4, 0)
#define SHARP_SCALE SDL_SCALEMODE_PIXELART
#else
#define SHARP_SCALE SDL_SCALEMODE_LINEAR
#endif

typedef struct sdl_win {
    rl_window head;
    SDL_Window *w;
    SDL_WindowID id;
    uint32_t width, height;
    uint32_t screen_w, screen_h;
    SDL_Renderer *r;
    SDL_Texture *t;
    uint32_t tw, th;
    uint32_t lw, lh;
    SDL_Texture *ov;
    uint32_t ow, oh;
    SDL_Texture *te;
    uint32_t te_w, te_h;
    bool logical;
    bool fit;
    bool integer;
    int sampling;
    bool no_renderer;
    rl_present *pres;
    bool pres_tried;
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

/** @brief The text entry panel: the window it is shown in, the picture under it and the panel's pixels. */
static struct {
    SDL_Mutex *lock;
    sdl_win *w;
    uint8_t *under;
    uint32_t uw, uh;
    uint8_t *rgb;
    size_t cap;
    bool on;
    bool dirty;
    uint64_t drawn_ns;
} g_te;

/** @brief How often the panel is drawn again for its caret's blink. */
#define TE_BLINK_NS 265000000ull

#ifdef RL_MOBILE
#define TE_TOUCH true
#else
#define TE_TOUCH false
#endif

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
    .settings = rl_android_settings,
#endif
    .leave = leave,
};

static void on_loop(SDL_MainThreadCallback fn, void *arg);

static void sdl_wake(void);

/** @brief Shows the panel's pixels with the window's SDL renderer (loop thread, panel lock held). */
static void te_render(sdl_win *w, uint32_t W, uint32_t H)
{
    if (!w->r)
        return;
    if (!w->te || w->te_w != W || w->te_h != H) {
        if (w->te)
            SDL_DestroyTexture(w->te);
        w->te = SDL_CreateTexture(w->r, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, (int)W, (int)H);
        w->te_w = W;
        w->te_h = H;
        if (!w->te)
            return;
    }
    SDL_UpdateTexture(w->te, NULL, g_te.rgb, (int)(W * 3));
    SDL_SetRenderLogicalPresentation(w->r, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    w->logical = false;
    SDL_RenderTexture(w->r, w->te, NULL, NULL);
    SDL_RenderPresent(w->r);
}

/**
 * @brief Draws the panel: into the window's presenter, or, for a window the SDL renderer draws (the on-screen
 * controller's), with the renderer on the loop thread.
 */
static void te_draw(void)
{
    SDL_LockMutex(g_te.lock);
    sdl_win *w = g_te.w;
    if (g_te.on && w && !w->pres) {
        if (SDL_GetCurrentThreadID() != g_loop) {
            g_te.dirty = true;
            SDL_UnlockMutex(g_te.lock);
            sdl_wake();
            return;
        }
        g_te.dirty = false;
    }
    if (g_te.on && w) {
        uint32_t W = __atomic_load_n(&w->width, __ATOMIC_RELAXED), H = __atomic_load_n(&w->height, __ATOMIC_RELAXED);
        size_t need = (size_t)W * H * 3;
        if (need > g_te.cap) {
            uint8_t *g = realloc(g_te.rgb, need);
            if (g) {
                g_te.rgb = g;
                g_te.cap = need;
            }
        }
        uint64_t now = rl_clock_ns();
        __atomic_store_n(&g_te.drawn_ns, now, __ATOMIC_RELAXED);
        if (W && H && need <= g_te.cap &&
            rl_text_entry_draw(g_te.rgb, W, H, g_te.under, g_te.uw, g_te.uh, TE_TOUCH, now)) {
            if (w->pres)
                rl_present_still(w->pres, g_te.rgb, W, H);
            else
                te_render(w, W, H);
        }
    }
    SDL_UnlockMutex(g_te.lock);
}

static void te_start_input(void *arg)
{
    sdl_win *w = arg;
    memset(g_keys, 0, sizeof g_keys);
    int32_t f[4];
    rl_text_entry_field(__atomic_load_n(&w->width, __ATOMIC_RELAXED), __atomic_load_n(&w->height, __ATOMIC_RELAXED),
                        TE_TOUCH, f);
    float density = SDL_GetWindowPixelDensity(w->w);
    if (density <= 0)
        density = 1;
    SDL_Rect area = { (int)((float)f[0] / density), (int)((float)f[1] / density), (int)((float)f[2] / density),
                      (int)((float)f[3] / density) };
    SDL_SetTextInputArea(w->w, &area, 0);
    SDL_StartTextInput(w->w);
}

static void te_stop_input(void *arg)
{
    sdl_win *w = arg;
    SDL_StopTextInput(w->w);
    memset(g_keys, 0, sizeof g_keys);
}

/**
 * @brief Shows the panel in the first window that has shown a frame, over the picture it shows now: read back from
 * its presenter, or for a window the SDL renderer draws, taken from the next frame it gets.
 */
static bool te_show(void)
{
    SDL_LockMutex(g_lock);
    sdl_win *w = g_wins;
    while (w && !w->pres && !w->r)
        w = w->next;
    SDL_UnlockMutex(g_lock);
    if (!w)
        return false;
    uint32_t uw = 0, uh = 0;
    uint8_t *under = NULL;
    if (w->pres) {
        rl_present_stats st;
        rl_present_stats_get(w->pres, &st);
        if (!st.presented)
            return false;
        under = rl_present_snapshot(w->pres, 1000, &uw, &uh);
        if (!under)
            return false;
    }
    SDL_LockMutex(g_te.lock);
    free(g_te.under);
    g_te.under = under;
    g_te.uw = uw;
    g_te.uh = uh;
    g_te.w = w;
    g_te.on = true;
    SDL_UnlockMutex(g_te.lock);
    on_loop(te_start_input, w);
    te_draw();
    return true;
}

static void te_hide(void)
{
    SDL_LockMutex(g_te.lock);
    sdl_win *w = g_te.w;
    g_te.on = false;
    g_te.w = NULL;
    free(g_te.under);
    g_te.under = NULL;
    SDL_UnlockMutex(g_te.lock);
    if (!w)
        return;
    if (w->pres)
        rl_present_still(w->pres, NULL, 0, 0);
    on_loop(te_stop_input, w);
}

static const rl_text_entry_host g_te_host = { te_show, te_hide, te_draw };

/** @brief While the panel is open, takes the events it uses and keeps keys, buttons and touches from the guest. */
static bool te_event(const SDL_Event *e)
{
    if (!rl_text_entry_active())
        return false;
    switch (e->type) {
    case SDL_EVENT_TEXT_INPUT:
        rl_text_entry_input(e->text.text);
        return true;
    case SDL_EVENT_KEY_DOWN: {
        int k = -1;
        switch (e->key.key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            k = RL_TEXT_ENTRY_KEY_CONFIRM;
            break;
        case SDLK_ESCAPE:
        case SDLK_AC_BACK:
            k = RL_TEXT_ENTRY_KEY_CANCEL;
            break;
        case SDLK_BACKSPACE:
            k = RL_TEXT_ENTRY_KEY_BACKSPACE;
            break;
        case SDLK_DELETE:
            k = RL_TEXT_ENTRY_KEY_DELETE;
            break;
        case SDLK_LEFT:
            k = RL_TEXT_ENTRY_KEY_LEFT;
            break;
        case SDLK_RIGHT:
            k = RL_TEXT_ENTRY_KEY_RIGHT;
            break;
        case SDLK_HOME:
            k = RL_TEXT_ENTRY_KEY_HOME;
            break;
        case SDLK_END:
            k = RL_TEXT_ENTRY_KEY_END;
            break;
        default:
            break;
        }
        if (k >= 0)
            rl_text_entry_key(k);
        return true;
    }
    case SDL_EVENT_KEY_UP:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START)
            rl_text_entry_key(RL_TEXT_ENTRY_KEY_CONFIRM);
        else if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK)
            rl_text_entry_key(RL_TEXT_ENTRY_KEY_CANCEL);
        return true;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        SDL_LockMutex(g_te.lock);
        sdl_win *w = g_te.w;
        uint32_t W = w ? __atomic_load_n(&w->width, __ATOMIC_RELAXED) : 0;
        uint32_t H = w ? __atomic_load_n(&w->height, __ATOMIC_RELAXED) : 0;
        float density = w ? SDL_GetWindowPixelDensity(w->w) : 1;
        SDL_UnlockMutex(g_te.lock);
        float x, y;
        if (e->type == SDL_EVENT_FINGER_DOWN) {
            x = e->tfinger.x * (float)W;
            y = e->tfinger.y * (float)H;
        } else {
            x = e->button.x * (density > 0 ? density : 1);
            y = e->button.y * (density > 0 ? density : 1);
        }
        if (W && H)
            rl_text_entry_tap(x, y, W, H, TE_TOUCH);
        return true;
    }
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return true;
    default:
        return false;
    }
}

static int sdl_init(void)
{
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    /* SDL's Android wait re-pushes the poll sentinel on every pump, and each push wakes the wait at once: with
     * sentinels on, sdl_run would spin a core. */
    SDL_SetHint(SDL_HINT_POLL_SENTINEL, "0");
#endif
#ifdef RL_MOBILE
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
    rl_touchpad_set_host(&g_touch_host);
    rl_text_entry_set_host(&g_te_host);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        rl_log(RL_LOG_INFO, "window", "SDL video init failed: %s", SDL_GetError());
        return -1;
    }
    g_loop = SDL_GetCurrentThreadID();
    g_lock = SDL_CreateMutex();
    g_wake_event = SDL_RegisterEvents(1);
    g_te.lock = SDL_CreateMutex();
    if (!g_lock || !g_wake_event || !g_te.lock) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        return -1;
    }
#if defined(__APPLE__) && TARGET_OS_IPHONE && !defined(RL_VULKAN_LINKED)
    rl_log(RL_LOG_INFO, "window", "SDL %s video driver, no Vulkan", SDL_GetCurrentVideoDriver());
    return 0;
#endif
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
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w->w));
    float md = mode && mode->pixel_density > 0 ? mode->pixel_density : 1;
    __atomic_store_n(&w->screen_w, mode && mode->w > 0 ? (uint32_t)((float)mode->w * md) : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&w->screen_h, mode && mode->h > 0 ? (uint32_t)((float)mode->h * md) : 0, __ATOMIC_RELAXED);
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
    if (rl_text_entry_active()) {
        rl_input_snapshot quiet;
        memset(&quiet, 0, sizeof quiet);
        for (int i = 0; i < RL_INPUT_PADS; i++)
            quiet.pads[i].connected = s.pads[i].connected;
        s = quiet;
    }
    rl_input_publish(&s);
}

/** @brief Counts e when it is input from a real device (rl_input_host_event). */
static void count_host(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        rl_input_host_event(RL_INPUT_HOST_KEY);
        break;
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        if (e->type != SDL_EVENT_MOUSE_MOTION || e->motion.which != SDL_TOUCH_MOUSEID)
            rl_input_host_event(RL_INPUT_HOST_MOUSE);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        rl_input_host_event(RL_INPUT_HOST_PAD);
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
        rl_input_host_event(RL_INPUT_HOST_TOUCH);
        break;
    default:
        break;
    }
}

static void handle(const SDL_Event *e)
{
    count_host(e);
    if (te_event(e))
        return;
    if (e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST) {
        window_event(e);
        if (e->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && rl_text_entry_active())
            te_draw();
    }
    else if (e->type == SDL_EVENT_GAMEPAD_ADDED || e->type == SDL_EVENT_GAMEPAD_REMOVED)
        pad_event(e);
    else if (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP)
        key_event(e);
    else if (e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_FINGER_MOTION ||
             e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED)
        finger_event(e);
    else if (e->type == SDL_EVENT_QUIT)
        rl_window_emit(NULL, RL_WINDOW_CLOSE);
    else if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND || e->type == SDL_EVENT_TERMINATING)
        rl_window_emit(NULL, RL_WINDOW_BACKGROUND);
}

/**
 * @brief How long sdl_run waits for an event, in ms. SDL polls HIDAPI
 * controllers only when it pumps, and its Android wait does not wake for
 * their reports (other hosts poll by themselves), so Android pumps every
 * millisecond while a controller is connected, as SDL does elsewhere.
 */
static Sint32 wait_ms(void)
{
#ifdef __ANDROID__
    if (SDL_HasJoystick())
        return 1;
#endif
    return 100;
}

static void sdl_run(bool (*done)(void *user), void *user)
{
    while (!done(user)) {
        SDL_Event e;
        bool got = SDL_WaitEventTimeout(&e, wait_ms());
        if (got) {
            handle(&e);
            while (SDL_PollEvent(&e))
                handle(&e);
            publish();
        }
        if (rl_text_entry_active() && (g_te.dirty || rl_clock_ns() - __atomic_load_n(&g_te.drawn_ns, __ATOMIC_RELAXED) >=
                                                            TE_BLINK_NS))
            te_draw();
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
    SDL_WindowFlags flags = (g_exts ? SDL_WINDOW_VULKAN : 0) | SDL_WINDOW_RESIZABLE;
#ifdef RL_MOBILE
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
    flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
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
    if (w->te)
        SDL_DestroyTexture(w->te);
    if (w->r)
        SDL_DestroyRenderer(w->r);
    SDL_DestroyWindow(w->w);
    free(w);
}

static void sdl_close(rl_window *h)
{
    if (!h)
        return;
    sdl_win *w = (sdl_win *)h;
    rl_present_close(w->pres);
    w->pres = NULL;
    on_loop(do_close, h);
}

static void sdl_size(rl_window *h, uint32_t *width, uint32_t *height)
{
    sdl_win *w = (sdl_win *)h;
    *width = __atomic_load_n(&w->width, __ATOMIC_RELAXED);
    *height = __atomic_load_n(&w->height, __ATOMIC_RELAXED);
}

static void sdl_screen(rl_window *h, uint32_t *width, uint32_t *height)
{
    sdl_win *w = (sdl_win *)h;
    *width = __atomic_load_n(&w->screen_w, __ATOMIC_RELAXED);
    *height = __atomic_load_n(&w->screen_h, __ATOMIC_RELAXED);
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
    /** @brief Android: the window has no surface now (the app is in the background); a later try can work. */
    bool later;
} surface_req;

static void do_surface(void *p)
{
    surface_req *r = p;
    VkSurfaceKHR s = 0;
#ifdef __ANDROID__
    void *nw = SDL_GetPointerProperty(SDL_GetWindowProperties(r->w->w), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
    uint64_t made = 0;
    r->ok = nw && rl_android_vulkan_surface(nw, r->instance, &made) == VK_SUCCESS;
    memcpy(&s, &made, sizeof s);
    if (!r->ok)
        SDL_SetError("vkCreateAndroidSurfaceKHR failed");
    if (!nw)
        r->later = true;
#else
    r->ok = SDL_Vulkan_CreateSurface(r->w->w, (VkInstance)r->instance, NULL, &s);
#endif
    if (!r->ok && !r->later)
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
    uint32_t width, height, shown_w, shown_h;
} present_req;

/** @brief Sets the texture's sampling by rl_post_sampling for a w by h texture drawn at dw by dh (float pixels). */
static void sample(sdl_win *w, uint32_t tw, uint32_t th, float dw, float dh)
{
    int m = rl_post_sampling(tw, th, dw > 0 ? (uint32_t)(dw + 0.5f) : 0, dh > 0 ? (uint32_t)(dh + 0.5f) : 0);
    if (m == w->sampling)
        return;
    w->sampling = m;
    SDL_SetTextureScaleMode(w->t, m == RL_POST_SAMPLE_SHARP    ? SHARP_SCALE
                                  : m == RL_POST_SAMPLE_LINEAR ? SDL_SCALEMODE_LINEAR
                                                               : SDL_SCALEMODE_NEAREST);
}

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
        w->sampling = RL_POST_SAMPLE_NEAREST;
        w->tw = q->width;
        w->th = q->height;
        w->logical = false;
    }
    if (w->lw != q->shown_w || w->lh != q->shown_h) {
        w->lw = q->shown_w;
        w->lh = q->shown_h;
        w->logical = false;
    }
    bool integer = rl_post_integer(w->fit);
    if (integer != w->integer) {
        w->integer = integer;
        w->logical = false;
    }
    SDL_UpdateTexture(w->t, NULL, q->rgb, (int)(q->width * 3));
    SDL_LockMutex(g_te.lock);
    bool te_here = g_te.on && g_te.w == w;
    if (te_here && !g_te.under) {
        g_te.under = malloc((size_t)q->width * q->height * 4);
        if (g_te.under) {
            for (size_t i = 0; i < (size_t)q->width * q->height; i++) {
                memcpy(g_te.under + i * 4, q->rgb + i * 3, 3);
                g_te.under[i * 4 + 3] = 255;
            }
            g_te.uw = q->width;
            g_te.uh = q->height;
            g_te.dirty = true;
        }
    }
    SDL_UnlockMutex(g_te.lock);
    if (te_here) {
        te_draw();
        return;
    }
    bool pad = rl_touchpad_wanted();
    if (w->logical == pad) {
        SDL_SetRenderLogicalPresentation(w->r, pad ? 0 : (int)q->shown_w, pad ? 0 : (int)q->shown_h,
                                         pad     ? SDL_LOGICAL_PRESENTATION_DISABLED
                                         : w->integer ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                                      : SDL_LOGICAL_PRESENTATION_LETTERBOX);
        w->logical = !pad;
    }
    SDL_FRect at = { 0 };
    if (pad) {
        int32_t pic[4];
        rl_touchpad_picture(pic);
        at.w = (float)pic[2];
        at.h = (float)pic[3];
    } else {
        SDL_GetRenderLogicalPresentationRect(w->r, &at);
    }
    sample(w, q->width, q->height, at.w, at.h);
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

/**
 * @brief The window's presenter, opened with the first ask unless the on-screen controller is in use (its overlay is
 * still drawn by the SDL renderer) or the host has no Vulkan device with swapchains (the renderer shows the frames).
 */
static rl_present *sdl_presenter(rl_window *h)
{
    sdl_win *w = (sdl_win *)h;
    if (!w->pres_tried) {
        w->pres_tried = true;
        if (g_exts && !rl_touchpad_wanted()) {
            w->pres = rl_present_open(h);
            if (w->pres)
                rl_present_set_fit(w->pres, w->fit);
        }
    }
    return w->pres;
}

static void sdl_present_rgb(rl_window *h, const uint8_t *rgb, uint32_t width, uint32_t height, uint32_t shown_w,
                            uint32_t shown_h)
{
    sdl_win *w = (sdl_win *)h;
    rl_present *p = sdl_presenter(h);
    if (p) {
        uint32_t ww = 0, wh = 0;
        sdl_size(h, &ww, &wh);
        rl_present_frame(p, rgb, width, height, shown_w, shown_h, w->fit, ww, wh, NULL);
        return;
    }
    present_req q = {
        .w = w, .rgb = rgb, .width = width, .height = height, .shown_w = shown_w, .shown_h = shown_h
    };
    on_loop(do_present, &q);
}

typedef struct point_req {
    sdl_win *w;
    float x, y;
    uint32_t fx, fy;
    bool ok;
} point_req;

static void do_point(void *p)
{
    point_req *q = p;
    sdl_win *w = q->w;
    float lx = 0, ly = 0;
    rl_present_rect at;
    uint32_t fw = 0, fh = 0;
    if (w->pres) {
        int pw = 0, ph = 0;
        SDL_GetWindowSize(w->w, &pw, &ph);
        float sx = pw > 0 ? (float)w->width / (float)pw : 1.0f, sy = ph > 0 ? (float)w->height / (float)ph : 1.0f;
        if (!rl_present_shown(w->pres, &at, &fw, &fh))
            return;
        float px = q->x * sx - (float)at.x, py = q->y * sy - (float)at.y;
        if (px < 0 || py < 0 || px >= (float)at.w || py >= (float)at.h)
            return;
        q->fx = (uint32_t)(px * (float)fw / (float)at.w);
        q->fy = (uint32_t)(py * (float)fh / (float)at.h);
        q->ok = q->fx < fw && q->fy < fh;
        return;
    }
    if (!w->r || !w->logical || !w->lw || !w->lh || !SDL_RenderCoordinatesFromWindow(w->r, q->x, q->y, &lx, &ly))
        return;
    if (lx < 0 || ly < 0 || lx >= (float)w->lw || ly >= (float)w->lh)
        return;
    q->fx = (uint32_t)(lx * (float)w->tw / (float)w->lw);
    q->fy = (uint32_t)(ly * (float)w->th / (float)w->lh);
    q->ok = q->fx < w->tw && q->fy < w->th;
}

static bool sdl_frame_point(rl_window *h, int32_t x, int32_t y, uint32_t *fx, uint32_t *fy)
{
    point_req q = { .w = (sdl_win *)h, .x = (float)x + 0.5f, .y = (float)y + 0.5f };
    on_loop(do_point, &q);
    if (q.ok) {
        *fx = q.fx;
        *fy = q.fy;
    }
    return q.ok;
}

typedef struct fit_req {
    sdl_win *w;
    bool fit;
} fit_req;

static void do_fit(void *p)
{
    fit_req *q = p;
    if (q->w->fit != q->fit) {
        q->w->fit = q->fit;
        q->w->logical = false;
        if (q->w->pres)
            rl_present_set_fit(q->w->pres, q->fit);
    }
}

static void sdl_set_fit(rl_window *h, bool fit)
{
    fit_req q = { .w = (sdl_win *)h, .fit = fit };
    on_loop(do_fit, &q);
}

#ifdef __ANDROID__
static uint32_t sdl_display_count(void) { return rl_android_display_count(); }

static rl_window *sdl_open_display(uint32_t index) { return rl_android_display_open(index); }
#endif

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
    .presenter = sdl_presenter,
    .screen = sdl_screen,
    .frame_point = sdl_frame_point,
    .set_fit = sdl_set_fit,
#ifdef __ANDROID__
    .display_count = sdl_display_count,
    .open_display = sdl_open_display,
#endif
};

const rl_window_backend *rl_host_window_backend(void) { return &g_sdl; }
