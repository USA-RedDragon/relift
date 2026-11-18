/**
 * @file screens.h
 * @brief Presents a two-screen guest (the DS; any console with a top and a
 * bottom screen) in the host's windows. A layout setting chooses how the
 * screens share one window (stacked, side by side, one large and one small,
 * one only); when the host has a second display, the screens split over the
 * displays. The module composes each window's picture from the guest's
 * frames, tells the on-screen controller (touchpad.h) the picture's size and
 * maps the touches on a picture, wherever it is drawn and however scaled, to
 * the pixel of the guest screen under it.
 *
 * A guest layer declares the screens once (rl_screens_declare), calls
 * rl_screens_present with each frame and rl_screens_touch once per input
 * poll. Threads: the guest's run thread calls all of these.
 */
#ifndef RELIFT_SCREENS_H
#define RELIFT_SCREENS_H

#include <relift/abi_core.h>
#include <relift/input.h>
#include <relift/window.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief Number of screens: the top one is 0, the bottom one 1. */
#define RL_SCREENS 2u
/** @brief Largest side of a composed picture in pixels. */
#define RL_SCREENS_MAX_SIDE 2048u

/** @brief How the screens share a window. */
typedef enum rl_screens_layout {
    /**
     * @brief Chosen for each window: stacked in a portrait window and where the on-screen controller is in use;
     * else stacked or side by side, whichever fills the window better.
     */
    RL_SCREENS_AUTO = 0,
    /** @brief The top screen above the bottom one, as the console holds them. */
    RL_SCREENS_STACKED = 1,
    /** @brief The top screen left of the bottom one. */
    RL_SCREENS_SIDE_BY_SIDE = 2,
    /** @brief The top screen large, the bottom one small beside it (landscape) or below it (portrait). */
    RL_SCREENS_LARGE_TOP = 3,
    /** @brief The bottom screen large, the top one small. */
    RL_SCREENS_LARGE_BOTTOM = 4,
    /** @brief The top screen alone. */
    RL_SCREENS_TOP_ONLY = 5,
    /** @brief The bottom screen alone. */
    RL_SCREENS_BOTTOM_ONLY = 6
} rl_screens_layout;

/** @brief What the displays show when the host has more than one. */
typedef enum rl_screens_displays {
    /** @brief One display: the layout. Two or more: the top screen on the first, the bottom one on the second. */
    RL_SCREENS_DISPLAYS_AUTO = 0,
    /** @brief The bottom screen on the first display, the top one on the second. */
    RL_SCREENS_DISPLAYS_SWAPPED = 1,
    /** @brief The top screen on both displays. */
    RL_SCREENS_DISPLAYS_TOP_BOTH = 2,
    /** @brief The bottom screen on both displays. */
    RL_SCREENS_DISPLAYS_BOTTOM_BOTH = 3,
    /** @brief Only the first display is used, with the layout. */
    RL_SCREENS_DISPLAYS_SINGLE = 4
} rl_screens_displays;

/** @brief The settings of one run. */
typedef struct rl_screens_config {
    rl_screens_layout layout;
    rl_screens_displays displays;
    /** @brief Scale the pictures by whole multiples only; false fills the window with the shape kept. */
    bool integer_scale;
    /**
     * @brief The player's swap: the screens exchange places (a single screen shows the other one, a large one
     * becomes the small one, the displays exchange their screens).
     */
    bool swap;
} rl_screens_config;

/** @brief One guest screen's frame: RGB888, rows packed. */
typedef struct rl_screens_frame {
    const uint8_t *rgb;
    uint32_t width, height;
} rl_screens_frame;

/** @brief Where one screen sits in a picture, in the picture's pixels. */
typedef struct rl_screens_place {
    bool shown;
    uint32_t x, y, w, h;
} rl_screens_place;

/** @brief A window's picture: its size and where each screen sits in it. */
typedef struct rl_screens_picture {
    uint32_t width, height;
    rl_screens_place place[RL_SCREENS];
} rl_screens_picture;

/** @brief The pen: down at pixel (x, y) of the touch screen, or up. */
typedef struct rl_screens_pen {
    /** @brief A finger or mouse button is on a window (the pen is up unless it is on the touch screen). */
    bool active;
    bool down;
    uint32_t x, y;
} rl_screens_pen;

/**
 * @brief Plans the picture of layout in a window that is portrait (taller than wide) or not. only is -1 for both
 * screens or the index of the one screen shown. sizes are the screens' pixel sizes. Large screens are drawn at
 * twice their size next to small ones at their own.
 */
RL_RUNTIME_API void rl_screens_plan(const uint32_t sizes[RL_SCREENS][2], rl_screens_layout layout, int only,
                                    bool portrait, rl_screens_picture *out);
/**
 * @brief The layout rl_screens_present draws in a window of width by height when the setting says
 * RL_SCREENS_AUTO (one of the explicit layouts; any other layout comes back unchanged). controller: the
 * on-screen controller is in use.
 */
RL_RUNTIME_API rl_screens_layout rl_screens_resolve(const uint32_t sizes[RL_SCREENS][2], rl_screens_layout layout,
                                                    uint32_t width, uint32_t height, bool controller);
/** @brief Draws frames into rgb (plan->width by plan->height RGB888) on black, each screen scaled by whole multiples. */
RL_RUNTIME_API void rl_screens_compose(const rl_screens_picture *plan, const rl_screens_frame frames[RL_SCREENS],
                                       uint8_t *rgb);
/**
 * @brief The screen and its pixel under pixel (x, y) of the picture; false when it lies on no screen. sizes as in
 * rl_screens_plan.
 */
RL_RUNTIME_API bool rl_screens_hit(const rl_screens_picture *plan, const uint32_t sizes[RL_SCREENS][2], uint32_t x,
                                   uint32_t y, uint32_t *screen, uint32_t *sx, uint32_t *sy);
/** @brief What the window of display index (0 first) shows: -1 both screens, else a screen's index. */
RL_RUNTIME_API int rl_screens_content(rl_screens_displays displays, uint32_t display, uint32_t display_count);

/** @brief Declares the guest's screens (their sizes) and the touch screen's index; call before presenting. */
RL_RUNTIME_API void rl_screens_declare(const uint32_t sizes[RL_SCREENS][2], uint32_t touch_screen);
/** @brief Forgets the windows the module opened and everything declared (tests, the end of a run). */
RL_RUNTIME_API void rl_screens_reset(void);
/**
 * @brief Shows the frames: composes the picture of each display's window (the first display's is main; the others
 * are opened through rl_window_open_display), tells the on-screen controller the main picture's size and presents.
 * While the on-screen controller pauses the game, presents again until it runs (rl_window_present_rgb).
 */
RL_RUNTIME_API void rl_screens_present(rl_window *main, const rl_screens_config *cfg,
                                       const rl_screens_frame frames[RL_SCREENS]);
/**
 * @brief The pen from the touches of the snapshot s (its pointer when the on-screen controller is not in use) and
 * of the secondary windows, mapped through what rl_screens_present drew last.
 */
RL_RUNTIME_API rl_screens_pen rl_screens_touch(rl_window *main, const rl_input_snapshot *s);
/** @brief The plan of display index as last drawn, or NULL when none. */
RL_RUNTIME_API const rl_screens_picture *rl_screens_last_plan(uint32_t display);

#endif
