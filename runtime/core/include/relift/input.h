/**
 * @file input.h
 * @brief Host input in the core's own terms: a snapshot of gamepads, touch
 * points, keys and a pointer that a player (or a test) publishes and a
 * guest layer samples. It knows no guest controller layout; a guest layer
 * maps these generic buttons to its own.
 */
#ifndef RELIFT_INPUT_H
#define RELIFT_INPUT_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief Most gamepads in a snapshot. */
#define RL_INPUT_PADS 8
/** @brief Most simultaneous touch points. */
#define RL_INPUT_TOUCHES 16
/** @brief Entries of rl_input_snapshot.touch_pad (the TOUCH names of input_names.h). */
#define RL_INPUT_TOUCH_PAD 32

/** @brief Generic gamepad buttons, bit numbers in rl_input_pad.buttons. */
enum rl_input_button {
    RL_BTN_SOUTH = 0,
    RL_BTN_EAST = 1,
    RL_BTN_WEST = 2,
    RL_BTN_NORTH = 3,
    RL_BTN_LEFT_STICK = 4,
    RL_BTN_RIGHT_STICK = 5,
    RL_BTN_LEFT_SHOULDER = 6,
    RL_BTN_RIGHT_SHOULDER = 7,
    RL_BTN_LEFT_TRIGGER = 8,
    RL_BTN_RIGHT_TRIGGER = 9,
    RL_BTN_START = 10,
    RL_BTN_BACK = 11,
    RL_BTN_DPAD_UP = 12,
    RL_BTN_DPAD_RIGHT = 13,
    RL_BTN_DPAD_DOWN = 14,
    RL_BTN_DPAD_LEFT = 15,
    RL_BTN_GUIDE = 16,
    RL_BTN_MISC = 17
};

/** @brief Pointer buttons, bit numbers in rl_input_pointer.buttons. */
enum rl_input_pointer_button {
    RL_PTR_LEFT = 0,
    RL_PTR_RIGHT = 1,
    RL_PTR_MIDDLE = 2,
    RL_PTR_BACK = 3,
    RL_PTR_FORWARD = 4
};

/** @brief State of one gamepad. Sticks run from -32767 to 32767, up and right positive. */
typedef struct rl_input_pad {
    /** @brief True while the pad is connected. */
    bool connected;
    /** @brief Bit mask of enum rl_input_button. */
    uint32_t buttons;
    /** @brief Left stick. */
    int16_t lx, ly;
    /** @brief Right stick. */
    int16_t rx, ry;
} rl_input_pad;

/** @brief One touch point: x and y run from 0 to 65535 across the view. */
typedef struct rl_input_touch {
    /** @brief Finger id, stable while the finger is down. */
    uint32_t id;
    /** @brief Position. */
    uint16_t x, y;
} rl_input_touch;

/** @brief The pointer. */
typedef struct rl_input_pointer {
    /** @brief Position in view pixels. */
    int32_t x, y;
    /** @brief Movement since the previous snapshot. */
    int32_t dx, dy;
    /** @brief Wheel movement since the previous snapshot. */
    int32_t wheel;
    /** @brief Bit mask of enum rl_input_pointer_button. */
    uint32_t buttons;
} rl_input_pointer;

/** @brief Everything the host reports at one moment. */
typedef struct rl_input_snapshot {
    /** @brief Gamepads. */
    rl_input_pad pads[RL_INPUT_PADS];
    /** @brief Touch points down. */
    rl_input_touch touches[RL_INPUT_TOUCHES];
    /** @brief Number of valid entries in touches. */
    uint32_t touch_count;
    /** @brief Pointer. */
    rl_input_pointer pointer;
    /** @brief Keys down, one bit per USB HID usage code of the keyboard page (0 to 255). */
    uint64_t keys[4];
    /** @brief Modifier bits: 1 control, 2 shift, 4 alt, 8 meta, 16 caps lock, 32 num lock, 64 scroll lock. */
    uint32_t modifiers;
    /** @brief The on-screen controller's controls: buttons 0 or 1, stick axes -1 to 1 (up and right positive). */
    float touch_pad[RL_INPUT_TOUCH_PAD];
} rl_input_snapshot;

/** @brief Fills a snapshot; called from whichever thread samples input. */
typedef void rl_input_source_fn(void *user, rl_input_snapshot *out);

/** @brief Sets the function that provides snapshots (NULL to use published snapshots). */
RL_RUNTIME_API void rl_input_set_source(rl_input_source_fn *fn, void *user);
/** @brief Publishes the latest snapshot, used when no source function is set. Thread safe. */
RL_RUNTIME_API void rl_input_publish(const rl_input_snapshot *s);
/**
 * @brief Samples input: from the source function, else the last published
 * snapshot, else a snapshot with pad 0 connected and nothing pressed. The
 * on-screen controller's controls come from the touch pad (touchpad.h).
 */
RL_RUNTIME_API void rl_input_sample(rl_input_snapshot *out);

#endif
