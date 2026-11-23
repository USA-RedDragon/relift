/**
 * @file warmup.h
 * @brief The shader compile screen: what a game shows while the host driver compiles its shaders, drawn by the
 * window's presenter on every host.
 *
 * The compile queue feeds it measured counters (rl_warmup_begin, rl_warmup_progress, rl_warmup_saving,
 * rl_warmup_end). Two layouts show them. The blocking screen is a card over the whole window, up while the game waits
 * for the compile; the corner card sits over the running game while the compile goes on beside it. The screen draws
 * only what was measured: the shaders finished and failed of the total, the time since the compile began, and, once
 * the first seconds have given a rate, the time left at that average rate. A host feeds it input (rl_warmup_key,
 * rl_warmup_tap), the window's theme and the game's name and icon, and lays rl_warmup_pixels over its picture the
 * way it does the on-screen controller (present.h overlay).
 *
 * Threads: the feed and the host calls may come from any thread; one lock covers the state.
 */
#ifndef RELIFT_WARMUP_H
#define RELIFT_WARMUP_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Where the compile stands. */
enum rl_warmup_phase {
    /** @brief Nothing to show: no compile began, or it ended and its screen went away. */
    RL_WARMUP_IDLE = 0,
    /** @brief The compile queue is running. */
    RL_WARMUP_COMPILING = 1,
    /** @brief Every shader is done and the cache is being written to disk. */
    RL_WARMUP_SAVING = 2,
    /** @brief The compile and the save are over; the screen shows how it went, then goes away. */
    RL_WARMUP_FINISHED = 3
};

/** @brief How the screen is laid out. */
enum rl_warmup_mode {
    /** @brief A card over the whole window: the game waits for the compile. */
    RL_WARMUP_BLOCKING = 0,
    /** @brief A small card over the running game. */
    RL_WARMUP_BACKGROUND = 1
};

/** @brief What a blocking screen waits for after the compile (rl_warmup_wait). */
enum rl_warmup_answer {
    /** @brief The player asked to start (or nothing needed asking): go on. */
    RL_WARMUP_START = 0,
    /** @brief The player asked to quit: the host was told to end the program. */
    RL_WARMUP_QUIT = 1
};

/** @brief The keys the screen takes. */
enum rl_warmup_key {
    /** @brief Escape, or the system's back key: quit from the blocking screen, dismiss a finished corner card. */
    RL_WARMUP_KEY_ESCAPE = 0,
    /** @brief Return: Start anyway after failures. */
    RL_WARMUP_KEY_ENTER = 1
};

/** @brief What the host does for the screen. Any member may be NULL. */
typedef struct rl_warmup_host {
    /**
     * @brief The blocking screen takes over the window's whole picture (the window has no game frame yet): the host
     * draws rl_warmup_pixels as the window's picture while on is true (the card is opaque over the whole view), and
     * shows the game again when false. Any thread.
     */
    void (*base)(bool on);
    /** @brief The player chose Quit: end the program as a window close would. */
    void (*quit)(void);
    /** @brief The screen changed: draw the overlay again (any thread). */
    void (*wake)(void);
} rl_warmup_host;

/** @brief Registers the host, or NULL for none. */
RL_RUNTIME_API void rl_warmup_set_host(const rl_warmup_host *host);
/** @brief The game's name (UTF-8) and its icon as RGBA8 rows of width by height pixels (copied; NULL for none). */
RL_RUNTIME_API void rl_warmup_set_game(const char *name, const uint8_t *icon_rgba, uint32_t width, uint32_t height);
/** @brief The GPU's name, shown under the game's. */
RL_RUNTIME_API void rl_warmup_set_gpu(const char *name);
/** @brief The window's theme: light or dark (dark until told). */
RL_RUNTIME_API void rl_warmup_set_dark(bool dark);
/** @brief Whether the host has a keyboard, so the screen says "Esc to quit" (a touch screen shows the Quit button only). */
RL_RUNTIME_API void rl_warmup_set_keyboard(bool keyboard);
/** @brief The file the failed shaders are listed in, shown when some failed; NULL for none (copied). */
RL_RUNTIME_API void rl_warmup_set_report(const char *path);

/**
 * @brief A compile of total shaders begins now; the time since is measured from this call. mode picks the layout.
 * A later call starts the screen over.
 */
RL_RUNTIME_API void rl_warmup_begin(uint32_t total, int mode);
/** @brief The queue's counts: done shaders finished, of which failed were turned down. Absolute, not a change. */
RL_RUNTIME_API void rl_warmup_progress(uint32_t done, uint32_t failed);
/**
 * @brief n more compiles are queued, whatever queued them (a late compile a draw waits for, a warm-up item). With no
 * compile pass running this starts one whose card shows after a second; its total is open, so no time left is given.
 * The pass ends by itself once every queued compile has been done for a moment.
 */
RL_RUNTIME_API void rl_warmup_queue(uint32_t n);
/** @brief n queued compiles are done, of which failed were turned down by the driver. */
RL_RUNTIME_API void rl_warmup_done(uint32_t n, uint32_t failed);
/** @brief True while a pass has run long enough to show but the game has no window yet: the host should open one. */
RL_RUNTIME_API bool rl_warmup_window_due(void);
/** @brief The queue is empty and the cache is being written. */
RL_RUNTIME_API void rl_warmup_saving(void);
/** @brief The cache is written: the screen shows the result and goes away (after the failures are answered). */
RL_RUNTIME_API void rl_warmup_end(void);
/**
 * @brief The game shows its first frame: a blocking screen gives way to the corner card for what is left. Call it
 * before the first picture is handed to the presenter.
 */
RL_RUNTIME_API void rl_warmup_game_started(void);
/**
 * @brief For the blocking screen, after rl_warmup_end: returns once the player has seen the result (a short pause when
 * all went well, until Start anyway or Quit when some shaders failed). RL_WARMUP_QUIT when the player quit.
 */
RL_RUNTIME_API int rl_warmup_wait(void);
/** @brief True while the screen shows something or may soon. */
RL_RUNTIME_API bool rl_warmup_active(void);
/** @brief True while the blocking screen is up, which takes the keys and taps. */
RL_RUNTIME_API bool rl_warmup_blocking(void);

/** @brief The window's size in pixels (the overlay's size). */
RL_RUNTIME_API void rl_warmup_view(uint32_t width, uint32_t height);
/**
 * @brief Moves the screen's time on to now_ns, draws it when it changed and returns it (premultiplied RGBA8, the
 * view's size, rows packed) with the changed rectangle in dirty (empty when nothing changed); NULL when it shows
 * nothing. Same contract as rl_touchpad_pixels.
 */
RL_RUNTIME_API const uint8_t *rl_warmup_pixels(uint64_t now_ns, uint32_t *width, uint32_t *height, int32_t dirty[4]);
/** @brief The rectangle the screen last drew on (x0, y0, x1, y1: one past the last), empty when it shows nothing. */
RL_RUNTIME_API void rl_warmup_extent(int32_t out[4]);
/** @brief A key; true when the screen took it (the host keeps it from the guest). */
RL_RUNTIME_API bool rl_warmup_key(int key);
/** @brief A tap or click at (x, y) in view pixels; true when the screen took it. */
RL_RUNTIME_API bool rl_warmup_tap(float x, float y);
/** @brief Forgets everything (tests). */
RL_RUNTIME_API void rl_warmup_reset(void);

#endif
