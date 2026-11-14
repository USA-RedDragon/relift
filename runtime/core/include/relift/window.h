/**
 * @file window.h
 * @brief Host windows in the core's own terms: a backend that opens windows,
 * makes Vulkan surfaces for them and runs the host event loop, and the
 * events a guest layer listens to. It knows no guest display model; a guest
 * layer maps its own display objects to windows.
 *
 * Event loops of most windowing systems must run on the process's first
 * thread. rl_window_main therefore runs the program on a second host thread
 * and the backend's loop on the calling one; backends run window work from
 * other threads on the loop thread.
 */
#ifndef RELIFT_WINDOW_H
#define RELIFT_WINDOW_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct rl_window_backend rl_window_backend;

/** @brief An open host window. A backend embeds this as the first member of its window. */
typedef struct rl_window {
    /** @brief The backend that opened the window (set by the backend). */
    const rl_window_backend *b;
    /** @brief Bumped by the backend each time the drawable size changes. */
    uint64_t generation;
} rl_window;

/** @brief Window events (rl_window_event_fn kind). */
enum rl_window_event {
    /** @brief The user asked to close the window (window NULL: to quit the program). */
    RL_WINDOW_CLOSE = 1,
    /** @brief The window got keyboard focus. */
    RL_WINDOW_FOCUS_GAINED = 2,
    /** @brief The window lost keyboard focus. */
    RL_WINDOW_FOCUS_LOST = 3,
    /** @brief The drawable size changed (generation was bumped). */
    RL_WINDOW_RESIZED = 4
};

/** @brief Receives window events, on the backend's loop thread. */
typedef void rl_window_event_fn(void *user, rl_window *w, int event);

/** @brief Operations of a window backend. */
struct rl_window_backend {
    /** @brief Name for logs. */
    const char *name;
    /** @brief Brings the backend up on the calling (first) thread; -1 when no display is available. */
    int (*init)(void);
    /** @brief Runs the event loop on the init thread until done(user) returns true; wake makes it check again. */
    void (*run)(bool (*done)(void *user), void *user);
    /** @brief Makes a running loop look at done again; any thread. */
    void (*wake)(void);
    /** @brief Opens a resizable window with a drawable of width by height pixels; any thread. NULL on failure. */
    rl_window *(*open)(const char *title, uint32_t width, uint32_t height);
    /** @brief Closes a window; any thread. */
    void (*close)(rl_window *w);
    /** @brief Current drawable size in pixels; any thread. */
    void (*size)(rl_window *w, uint32_t *width, uint32_t *height);
    /** @brief Host Vulkan instance extensions a surface needs, NULL-terminated. */
    const char *const *(*vulkan_extensions)(void);
    /** @brief The host vkGetInstanceProcAddr the backend loaded, or NULL. */
    void *(*vulkan_gipa)(void);
    /** @brief Creates a VkSurfaceKHR for w on host instance; returns a VkResult. */
    int32_t (*vulkan_surface)(rl_window *w, void *instance, uint64_t *surface);
};

/**
 * @brief Environment variable that picks the backend: "headless" (or "none")
 * opens no window, "sdl3" forces the SDL3 backend even without a display
 * variable (it still falls back to headless when SDL cannot start), anything
 * else or unset chooses automatically: the SDL3 backend when built in and a
 * display exists (WAYLAND_DISPLAY or DISPLAY on Linux, always elsewhere),
 * headless otherwise.
 */
#define RL_WINDOW_ENV "RELIFT_VIDEO"

/** @brief Sets the backend rl_window_main brings up; NULL selects the default. */
RL_RUNTIME_API void rl_window_set_backend(const rl_window_backend *b);
/** @brief The backend that is up, or NULL when the run is headless. */
RL_RUNTIME_API const rl_window_backend *rl_window_backend_current(void);
/** @brief Sets the function that receives window events (NULL removes it). */
RL_RUNTIME_API void rl_window_set_events(rl_window_event_fn *fn, void *user);
/** @brief Delivers an event to the listener; backends call it. */
RL_RUNTIME_API void rl_window_emit(rl_window *w, int event);
/**
 * @brief Runs body(arg) and returns its result. With a backend (built in and
 * chosen as RL_WINDOW_ENV describes) that comes up, body runs on a new host
 * thread while the calling thread runs the backend's event loop; otherwise
 * body runs on the calling thread and rl_window_backend_current is NULL.
 */
RL_RUNTIME_API int rl_window_main(int (*body)(void *arg), void *arg);

/** @brief The host backend built with VIDEO=sdl3. */
RL_RUNTIME_API const rl_window_backend *rl_host_window_backend(void);

#endif
