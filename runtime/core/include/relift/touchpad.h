/**
 * @file touchpad.h
 * @brief The on-screen controller: a host overlay drawn over the game picture
 * on touch screens. A guest layer declares its layouts (shapes in a
 * reference screen, grouped for editing); the host feeds fingers, the view
 * size and the number of connected gamepads, composites the overlay the
 * module draws and asks it where to put the guest picture. The module owns
 * the touch rules (which finger belongs to which shape), the values of the
 * `touchpad` port's controls (rl_input_snapshot.touch_pad), the pause sheet,
 * the settings sheet, the layout editor and the toasts, and the settings
 * `input.touch_pad.*`. The guest never sees any of it except through the port.
 *
 * Threads: the host calls the finger, view, gamepad and draw functions from
 * its loop thread or its present thread; the guest calls rl_touchpad_frame
 * and samples input from its own. One lock covers the state.
 */
#ifndef RELIFT_TOUCHPAD_H
#define RELIFT_TOUCHPAD_H

#include <relift/abi_core.h>
#include <relift/input.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief The id of the on-screen controller's input port. */
#define RL_TOUCHPAD_PORT "touchpad"

/** @brief Pixels per dp of the reference screen layouts are written in (2400 by 1080, 411 by 914 dp), unless fitted. */
#define RL_TOUCHPAD_REF_PX_PER_DP 2.625f

/** @brief Kinds of shape. */
enum rl_touchpad_shape {
    /** @brief A round button: w is the diameter. */
    RL_TOUCHPAD_BUTTON = 0,
    /** @brief A pill button: w by h. */
    RL_TOUCHPAD_PILL = 1,
    /** @brief A d-pad of side w; controls are up, right, down, left. */
    RL_TOUCHPAD_DPAD = 2,
    /** @brief A fixed stick of diameter w; controls are its x and y axes. */
    RL_TOUCHPAD_STICK = 3,
    /** @brief The Hide and Show pill: a host control, no guest control. */
    RL_TOUCHPAD_HIDE = 4,
    /** @brief The Menu tab that opens the pause sheet; not in a group, not editable. */
    RL_TOUCHPAD_MENU = 5
};

/** @brief One shape of a layout, in the reference screen's pixels. */
typedef struct rl_touchpad_item {
    /** @brief enum rl_touchpad_shape. */
    uint8_t shape;
    /** @brief True for buttons a finger may slide onto from another of its group (face buttons). */
    bool slide;
    /** @brief The text drawn on it, or NULL. */
    const char *label;
    /**
     * @brief The touchpad port controls it drives (see the shape kinds); a button's first is also its id. A pill's
     * second control, when set, is the one a long press drives: the first then fires on a short tap, when the finger
     * lifts, and the second fires once the finger has rested for half a second.
     */
    const char *controls[4];
    /** @brief Center. */
    float x, y;
    /** @brief Size. */
    float w, h;
} rl_touchpad_item;

/** @brief A group of consecutive items that move and resize together. */
typedef struct rl_touchpad_group {
    /** @brief The group id in the layout setting keys. */
    const char *id;
    /** @brief The first item. */
    uint32_t first;
    /** @brief The number of items. */
    uint32_t count;
} rl_touchpad_group;

/** @brief The built-in layout of one orientation. */
typedef struct rl_touchpad_layout {
    /** @brief The reference screen it is drawn for, in pixels. */
    float width, height;
    /** @brief Shapes. */
    const rl_touchpad_item *items;
    /** @brief Number of shapes. */
    uint32_t nitems;
    /** @brief Groups. */
    const rl_touchpad_group *groups;
    /** @brief Number of groups. */
    uint32_t ngroups;
    /**
     * @brief True for a layout written in dp (x, y, w and h) that the module fits to the view: each group hangs from
     * the nearest edge or the middle of the view and the whole layout scales down together until no two controls meet.
     * The controls are an overlay: the picture keeps the size it has without them. False: reference pixels
     * (RL_TOUCHPAD_REF_PX_PER_DP), and groups beside the picture shrink to keep off it.
     */
    bool fitted;
    /** @brief With fitted: the layout is a panel along the bottom of a tall view, and every group hangs from the bottom edge. */
    bool panel;
} rl_touchpad_layout;

/** @brief What a guest layer declares. */
typedef struct rl_touchpad_guest {
    /** @brief The guest id in the layout setting keys (`input.touch_pad.layout.<name>...`). */
    const char *name;
    /** @brief The size of the guest picture in its own pixels. */
    uint32_t picture_width, picture_height;
    /** @brief True when the picture is shown at whole multiples of its size. */
    bool integer_scale;
    /** @brief True when fingers no shape took are the guest's touch screen (rl_touchpad_game_touches). */
    bool game_touch;
    /** @brief True when the built-in groups keep off the picture where there is room, even with game_touch. */
    bool keep_clear;
    /** @brief The layouts. */
    rl_touchpad_layout portrait, landscape;
} rl_touchpad_guest;

/** @brief What the host does for the module. Any member may be NULL. */
typedef struct rl_touchpad_host {
    /** @brief A short tick of the device's vibration (about 10 ms). */
    void (*haptic)(void);
    /** @brief Leave game: end the program as a window close would. */
    void (*leave)(void);
    /** @brief The overlay changed: make the loop draw again (any thread). */
    void (*wake)(void);
    /**
     * @brief Settings on the pause sheet: the host shows its own settings page over the paused game and calls
     * rl_touchpad_host_settings_done when the player leaves it. NULL: the module's own settings sheet opens.
     */
    void (*settings)(void);
} rl_touchpad_host;

/** @brief Finger phases. */
enum rl_touchpad_phase { RL_TOUCHPAD_DOWN = 0, RL_TOUCHPAD_MOVE = 1, RL_TOUCHPAD_UP = 2, RL_TOUCHPAD_CANCEL = 3 };

/**
 * @brief Declares the guest's layouts and the settings `input.touch_pad.show`, `.opacity`, `.size`, `.haptics` and
 * the guest's layout keys. Call before the settings are read. The guest must outlive the run.
 */
RL_RUNTIME_API void rl_touchpad_declare(const rl_touchpad_guest *g);
/**
 * @brief The guest picture's size now, for a guest whose picture changes with its layout (it overrides
 * picture_width and picture_height), and whether it is shown at whole multiples (integer_scale). Any thread.
 */
RL_RUNTIME_API void rl_touchpad_set_picture(uint32_t width, uint32_t height, bool integer_scale);
/** @brief The declared guest, or NULL. */
RL_RUNTIME_API const rl_touchpad_guest *rl_touchpad_current(void);
/**
 * @brief True when this run uses the on-screen controller: a guest declared it and either the screen is a touch
 * screen (Android) or input.touch_pad.show is always. A guest declares the touchpad port only then.
 */
RL_RUNTIME_API bool rl_touchpad_wanted(void);
/** @brief Says whether the screen is a touch screen (true by default on Android only). */
RL_RUNTIME_API void rl_touchpad_set_touch_screen(bool on);
/** @brief Sets the host's services. */
RL_RUNTIME_API void rl_touchpad_set_host(const rl_touchpad_host *h);
/** @brief Forgets everything (tests). */
RL_RUNTIME_API void rl_touchpad_reset(void);

/**
 * @brief The view: size in pixels, pixels per dp (0: the short side is 411 dp, a phone's), and the safe insets
 * (left, top, right, bottom pixels kept clear of cutouts and gesture edges; NULL for none).
 */
RL_RUNTIME_API void rl_touchpad_view(uint32_t width, uint32_t height, float px_per_dp, const int32_t safe[4]);
/** @brief The view size rl_touchpad_view set; 0 by 0 before it. */
RL_RUNTIME_API void rl_touchpad_view_size(uint32_t *width, uint32_t *height);
/** @brief A gamepad connected (true) or disconnected; count is the number connected after it; name is its own. */
RL_RUNTIME_API void rl_touchpad_gamepad(bool connected, const char *name, uint32_t count);
/** @brief A finger event in view pixels. */
RL_RUNTIME_API void rl_touchpad_finger(uint64_t id, int phase, float x, float y);
/** @brief The system back gesture: opens the pause sheet, or steps back out of a sheet. */
RL_RUNTIME_API void rl_touchpad_back(void);
/** @brief Where the player goes from the host's settings page. */
enum rl_touchpad_host_next {
    /** @brief Back to the pause sheet. */
    RL_TOUCHPAD_HOST_PAUSE = 0,
    /** @brief The module's on-screen controller settings sheet; its back returns to the host page. */
    RL_TOUCHPAD_HOST_CONTROLLER = 1,
    /** @brief Back to the game. */
    RL_TOUCHPAD_HOST_RESUME = 2
};

/** @brief Where one control of the layout is drawn and where it takes touches, in view pixels. */
typedef struct rl_touchpad_rect {
    /** @brief enum rl_touchpad_shape. */
    uint8_t shape;
    /** @brief The item's label and its first control, or NULL. */
    const char *label, *control;
    /** @brief The index of the item's group in the layout, or -1. */
    int32_t group;
    /** @brief The text size in pixels of a pill's label, or 0. */
    float label_size;
    /** @brief The bounds of the drawn shape. */
    float x0, y0, x1, y1;
    /**
     * @brief The bounds of the area that takes its touches: the shape and a margin that grows into free space (a
     * circle's bounds for a round shape, the shared disc's for a slide group's buttons, which split it by nearest center).
     */
    float hit_x0, hit_y0, hit_x1, hit_y1;
} rl_touchpad_rect;
/** @brief Fills out with the controls now in the view (the current orientation's layout); returns how many. */
RL_RUNTIME_API uint32_t rl_touchpad_placed(rl_touchpad_rect *out, uint32_t max);
/**
 * @brief Shows a short label over the picture for about a second (a layout's name after a change); any thread.
 * Nothing when the guest has no layout.
 */
RL_RUNTIME_API void rl_touchpad_notice(const char *text);
/** @brief The host's settings page closed; next is an rl_touchpad_host_next. */
RL_RUNTIME_API void rl_touchpad_host_settings_done(int next);
/** @brief Moves time on (fades, toasts, long presses); returns true while something animates or waits. */
RL_RUNTIME_API bool rl_touchpad_tick(uint64_t now_ns);

/**
 * @brief Where the host draws the guest picture in the view: x, y, width, height pixels. Centered and as large as
 * fits (at whole multiples when the guest asks); in portrait with the pad shown it sits at the top.
 */
RL_RUNTIME_API void rl_touchpad_picture(int32_t rect[4]);
/**
 * @brief Draws the overlay when it changed and returns its pixels: premultiplied RGBA8, the view's size, rows
 * packed. dirty gets the changed rectangle (x, y, width, height); empty when nothing changed. NULL when the overlay
 * has nothing to show.
 */
RL_RUNTIME_API const uint8_t *rl_touchpad_pixels(uint32_t *width, uint32_t *height, int32_t dirty[4]);

/** @brief Writes the touchpad port's control values (index = TOUCH value of input_names.h). */
RL_RUNTIME_API void rl_touchpad_fill(float *values, uint32_t count);
/** @brief The fingers that belong to the game, in picture coordinates (0 to 65535); returns the count. */
RL_RUNTIME_API uint32_t rl_touchpad_game_touches(rl_input_touch *out, uint32_t max);

/** @brief True while a sheet or the editor is open: the game is paused. */
RL_RUNTIME_API bool rl_touchpad_paused(void);
/**
 * @brief The guest showed a frame. While the game is paused, waits up to ms milliseconds for the overlay to change
 * and returns true: the caller shows the same frame again (with the new overlay) and calls it again. Returns false
 * once the game runs.
 */
RL_RUNTIME_API bool rl_touchpad_hold(uint32_t ms);

#endif
