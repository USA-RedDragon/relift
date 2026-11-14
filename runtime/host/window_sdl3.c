/**
 * @file window_sdl3.c
 * @brief Host windows, Vulkan surfaces and input through SDL3 (zlib
 * license). Built with VIDEO=sdl3.
 *
 * SDL's video calls run on the thread that called init; open, close and
 * surface creation from other threads go through SDL_RunOnMainThread, which
 * the event loop in run serves. Keyboard, mouse and gamepads become
 * rl_input snapshots; with no gamepad connected, pad 0 follows the keyboard.
 */
#include <relift/input.h>
#include <relift/window.h>

#include "core.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdlib.h>
#include <string.h>

typedef struct sdl_win {
    rl_window head;
    SDL_Window *w;
    SDL_WindowID id;
    uint32_t width, height;
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

static int sdl_init(void)
{
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
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
    if (e->type == SDL_EVENT_GAMEPAD_ADDED) {
        for (int i = 0; i < RL_INPUT_PADS; i++)
            if (!g_pads[i]) {
                g_pads[i] = SDL_OpenGamepad(e->gdevice.which);
                return;
            }
        return;
    }
    for (int i = 0; i < RL_INPUT_PADS; i++)
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == e->gdevice.which) {
            SDL_CloseGamepad(g_pads[i]);
            g_pads[i] = NULL;
        }
}

static void key_event(const SDL_Event *e)
{
    unsigned sc = (unsigned)e->key.scancode;
    if (sc >= 256)
        return;
    if (e->key.down)
        g_keys[sc / 64] |= 1ull << (sc % 64);
    else
        g_keys[sc / 64] &= ~(1ull << (sc % 64));
}

static bool key(unsigned sc) { return (g_keys[sc / 64] >> (sc % 64)) & 1; }

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

static const struct {
    SDL_Scancode sc;
    int rl;
} g_keypad[] = {
    { SDL_SCANCODE_Z, RL_BTN_SOUTH },
    { SDL_SCANCODE_X, RL_BTN_EAST },
    { SDL_SCANCODE_A, RL_BTN_WEST },
    { SDL_SCANCODE_S, RL_BTN_NORTH },
    { SDL_SCANCODE_Q, RL_BTN_LEFT_SHOULDER },
    { SDL_SCANCODE_W, RL_BTN_RIGHT_SHOULDER },
    { SDL_SCANCODE_1, RL_BTN_LEFT_TRIGGER },
    { SDL_SCANCODE_3, RL_BTN_RIGHT_TRIGGER },
    { SDL_SCANCODE_RETURN, RL_BTN_START },
    { SDL_SCANCODE_BACKSPACE, RL_BTN_BACK },
    { SDL_SCANCODE_UP, RL_BTN_DPAD_UP },
    { SDL_SCANCODE_RIGHT, RL_BTN_DPAD_RIGHT },
    { SDL_SCANCODE_DOWN, RL_BTN_DPAD_DOWN },
    { SDL_SCANCODE_LEFT, RL_BTN_DPAD_LEFT },
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
    if (!n) {
        rl_input_pad *p = &s.pads[0];
        p->connected = true;
        for (size_t k = 0; k < sizeof g_keypad / sizeof g_keypad[0]; k++)
            if (key(g_keypad[k].sc))
                p->buttons |= 1u << g_keypad[k].rl;
    }
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

typedef struct open_req {
    const char *title;
    uint32_t width, height;
    sdl_win *out;
} open_req;

static void do_open(void *p)
{
    open_req *r = p;
    SDL_Window *sw = SDL_CreateWindow(r->title, (int)r->width, (int)r->height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!sw) {
        rl_log(RL_LOG_ERROR, "window", "SDL could not open a window: %s", SDL_GetError());
        return;
    }
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
    SDL_FunctionPointer f = SDL_Vulkan_GetVkGetInstanceProcAddr();
    void *p;
    memcpy(&p, &f, sizeof p);
    return p;
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
    r->ok = SDL_Vulkan_CreateSurface(r->w->w, (VkInstance)r->instance, NULL, &s);
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
};

const rl_window_backend *rl_host_window_backend(void) { return &g_sdl; }
