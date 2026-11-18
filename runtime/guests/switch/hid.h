/**
 * @file hid.h
 * @brief The hid shared memory (layout and update from host input) and the
 * hooks the runtime and the player use.
 *
 * The layout and the update rules follow Ryujinx (MIT, Copyright (c) Ryujinx
 * Team and Contributors) Hid/Types/SharedMemory and Hid/HidDevices, checked
 * against libnx (ISC) services/hid.h.
 */
#ifndef RELIFT_HID_H
#define RELIFT_HID_H

#include "hos.h"

#include <relift/input.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Size of the hid shared memory. */
#define HID_SHMEM_SIZE 0x40000u
/** @brief Entries of every ring. */
#define HID_LIFO_ENTRIES 17u
/** @brief Npad slots: players 1 to 8, handheld, other. */
#define HID_NPADS 10u
/** @brief Npad slot of the handheld id. */
#define HID_NPAD_HANDHELD 8u
/** @brief Most touch points in one entry. */
#define HID_TOUCHES 16u

/** @brief Header of a ring: timestamp, buffer count, index of the latest entry, number of entries. */
typedef struct hid_lifo_header {
    uint64_t timestamp;
    uint64_t buffer_count;
    uint64_t tail;
    uint64_t count;
} hid_lifo_header;

/** @brief Analog stick position, -32767 to 32767. */
typedef struct hid_stick {
    int32_t x, y;
} hid_stick;

/** @brief Debug pad state. */
typedef struct hid_debug_pad_state {
    uint64_t sampling_number;
    uint32_t attributes;
    uint32_t buttons;
    hid_stick r, l;
} hid_debug_pad_state;

/** @brief One touch point. */
typedef struct hid_touch_state {
    uint64_t delta_time;
    uint32_t attributes;
    uint32_t finger_id;
    uint32_t x, y;
    uint32_t diameter_x, diameter_y;
    uint32_t rotation_angle;
    uint32_t reserved;
} hid_touch_state;

/** @brief Touch screen state. */
typedef struct hid_touch_screen_state {
    uint64_t sampling_number;
    int32_t count;
    int32_t reserved;
    hid_touch_state touches[HID_TOUCHES];
} hid_touch_screen_state;

/** @brief Mouse (and debug mouse) state. */
typedef struct hid_mouse_state {
    uint64_t sampling_number;
    int32_t x, y;
    int32_t delta_x, delta_y;
    int32_t wheel_delta_x, wheel_delta_y;
    uint32_t buttons;
    uint32_t attributes;
} hid_mouse_state;

/** @brief Keyboard state. */
typedef struct hid_keyboard_state {
    uint64_t sampling_number;
    uint64_t modifiers;
    uint64_t keys[4];
} hid_keyboard_state;

/** @brief Npad state of the pad styles. */
typedef struct hid_npad_state {
    uint64_t sampling_number;
    uint64_t buttons;
    hid_stick l, r;
    uint32_t attributes;
    uint32_t reserved;
} hid_npad_state;

/** @brief Six axis sensor state. */
typedef struct hid_six_axis_state {
    uint64_t delta_time;
    uint64_t sampling_number;
    float acceleration[3];
    float angular_velocity[3];
    float angle[3];
    float direction[9];
    uint32_t attributes;
    uint32_t reserved;
} hid_six_axis_state;

/** @brief GameCube trigger state. */
typedef struct hid_gc_trigger_state {
    uint64_t sampling_number;
    uint32_t trigger_l, trigger_r;
} hid_gc_trigger_state;

/**
 * @brief Declares a ring of T. Each entry's seq is twice the state's sampling
 * number, and odd while the state is written: readers retry on an odd seq or
 * on a seq that changed during the copy.
 */
#define HID_LIFO_TYPE(name, T)                       \
    typedef struct name##_entry {                    \
        uint64_t seq;                                \
        T state;                                     \
    } name##_entry;                                  \
    typedef struct name {                            \
        hid_lifo_header header;                      \
        name##_entry entries[HID_LIFO_ENTRIES];      \
    } name

/** @brief Rings of each state type. */
HID_LIFO_TYPE(hid_debug_pad_lifo, hid_debug_pad_state);
HID_LIFO_TYPE(hid_touch_lifo, hid_touch_screen_state);
HID_LIFO_TYPE(hid_mouse_lifo, hid_mouse_state);
HID_LIFO_TYPE(hid_keyboard_lifo, hid_keyboard_state);
HID_LIFO_TYPE(hid_npad_lifo, hid_npad_state);
HID_LIFO_TYPE(hid_six_axis_lifo, hid_six_axis_state);
HID_LIFO_TYPE(hid_gc_lifo, hid_gc_trigger_state);

/** @brief Controller colors of a single controller. */
typedef struct hid_full_key_color {
    uint32_t attribute, body, buttons;
} hid_full_key_color;

/** @brief Controller colors of a split controller. */
typedef struct hid_joy_color {
    uint32_t attribute, left_body, left_buttons, right_body, right_buttons;
} hid_joy_color;

/** @brief The state of one npad. */
typedef struct hid_npad_internal {
    uint32_t style_set;
    uint32_t joy_assignment_mode;
    hid_full_key_color full_key_color;
    hid_joy_color joy_color;
    hid_npad_lifo full_key, handheld, joy_dual, joy_left, joy_right, palma, system_ext;
    hid_six_axis_lifo full_key_six, handheld_six, joy_dual_six, joy_dual_right_six, joy_left_six, joy_right_six;
    uint32_t device_type;
    uint32_t reserved1;
    uint64_t system_properties;
    uint32_t system_button_properties;
    uint32_t battery_level[3];
    uint32_t applet_footer_ui_attributes;
    uint8_t applet_footer_ui_type;
    uint8_t reserved2[123];
    hid_gc_lifo gc_trigger;
    uint32_t lark_type_left_and_main;
    uint32_t lark_type_right;
    uint32_t lucia_type;
    uint32_t unknown_43fc;
    uint64_t six_axis_sensor_properties;
} hid_npad_internal;

/** @brief Npad slot: the state padded to 0x5000. */
typedef union hid_npad_entry {
    hid_npad_internal state;
    uint8_t size[0x5000];
} hid_npad_entry;

/** @brief Pad condition block. */
typedef struct hid_npad_condition {
    uint32_t a, b;
    uint32_t hold_type;
    uint32_t c;
} hid_npad_condition;

/** @brief The shared memory. */
typedef struct hid_shared_memory {
    hid_debug_pad_lifo debug_pad;
    uint8_t pad0[0x400 - sizeof(hid_debug_pad_lifo)];
    hid_touch_lifo touch_screen;
    uint8_t pad1[0x3000 - sizeof(hid_touch_lifo)];
    hid_mouse_lifo mouse;
    uint8_t pad2[0x400 - sizeof(hid_mouse_lifo)];
    hid_keyboard_lifo keyboard;
    uint8_t pad3[0x6200 - sizeof(hid_keyboard_lifo)];
    hid_npad_entry npads[HID_NPADS];
    uint8_t pad4[0x3DC00 - 0x9A00 - sizeof(hid_npad_entry) * HID_NPADS];
    hid_mouse_lifo debug_mouse;
    uint8_t pad5[0x600 - sizeof(hid_mouse_lifo)];
    hid_npad_condition condition;
    uint8_t pad6[HID_SHMEM_SIZE - 0x3E200 - sizeof(hid_npad_condition)];
} hid_shared_memory;

_Static_assert(sizeof(hid_debug_pad_lifo) == 0x2C8, "debug pad lifo");
_Static_assert(sizeof(hid_touch_lifo) == 0x2C38, "touch lifo");
_Static_assert(sizeof(hid_mouse_lifo) == 0x350, "mouse lifo");
_Static_assert(sizeof(hid_keyboard_lifo) == 0x3D8, "keyboard lifo");
_Static_assert(sizeof(hid_npad_lifo) == 0x350, "npad lifo");
_Static_assert(sizeof(hid_six_axis_lifo) == 0x708, "six axis lifo");
_Static_assert(sizeof(hid_gc_lifo) == 0x1B8, "gc lifo");
_Static_assert(sizeof(hid_npad_internal) == 0x43F8, "npad internal");
_Static_assert(sizeof(hid_shared_memory) == HID_SHMEM_SIZE, "shared memory size");
_Static_assert(offsetof(hid_shared_memory, touch_screen) == 0x400, "touch offset");
_Static_assert(offsetof(hid_shared_memory, mouse) == 0x3400, "mouse offset");
_Static_assert(offsetof(hid_shared_memory, keyboard) == 0x3800, "keyboard offset");
_Static_assert(offsetof(hid_shared_memory, npads) == 0x9A00, "npad offset");
_Static_assert(offsetof(hid_shared_memory, debug_mouse) == 0x3DC00, "debug mouse offset");
_Static_assert(offsetof(hid_shared_memory, condition) == 0x3E200, "condition offset");
_Static_assert(offsetof(hid_npad_internal, handheld) == 0x378, "handheld offset");
_Static_assert(offsetof(hid_npad_internal, full_key_six) == 0x1758, "six axis offset");
_Static_assert(offsetof(hid_npad_internal, device_type) == 0x4188, "device type offset");
_Static_assert(offsetof(hid_npad_internal, system_properties) == 0x4190, "properties offset");
_Static_assert(offsetof(hid_npad_internal, applet_footer_ui_type) == 0x41AC, "footer offset");
_Static_assert(offsetof(hid_npad_internal, gc_trigger) == 0x4228, "gc offset");

/** @brief Npad style bits. */
enum {
    HID_STYLE_FULL_KEY = 1u << 0,
    HID_STYLE_HANDHELD = 1u << 1,
    HID_STYLE_JOY_DUAL = 1u << 2,
    HID_STYLE_JOY_LEFT = 1u << 3,
    HID_STYLE_JOY_RIGHT = 1u << 4,
    HID_STYLE_PALMA = 1u << 6,
    HID_STYLE_SYSTEM_EXT = 1u << 29,
    HID_STYLE_SYSTEM = 1u << 30
};

/** @brief Npad button bits. */
enum {
    HID_BTN_A = 1u << 0,
    HID_BTN_B = 1u << 1,
    HID_BTN_X = 1u << 2,
    HID_BTN_Y = 1u << 3,
    HID_BTN_STICK_L = 1u << 4,
    HID_BTN_STICK_R = 1u << 5,
    HID_BTN_L = 1u << 6,
    HID_BTN_R = 1u << 7,
    HID_BTN_ZL = 1u << 8,
    HID_BTN_ZR = 1u << 9,
    HID_BTN_PLUS = 1u << 10,
    HID_BTN_MINUS = 1u << 11,
    HID_BTN_LEFT = 1u << 12,
    HID_BTN_UP = 1u << 13,
    HID_BTN_RIGHT = 1u << 14,
    HID_BTN_DOWN = 1u << 15,
    HID_BTN_STICK_L_LEFT = 1u << 16,
    HID_BTN_STICK_L_UP = 1u << 17,
    HID_BTN_STICK_L_RIGHT = 1u << 18,
    HID_BTN_STICK_L_DOWN = 1u << 19,
    HID_BTN_STICK_R_LEFT = 1u << 20,
    HID_BTN_STICK_R_UP = 1u << 21,
    HID_BTN_STICK_R_RIGHT = 1u << 22,
    HID_BTN_STICK_R_DOWN = 1u << 23
};

/** @brief Npad attribute bits. */
enum {
    HID_ATTR_CONNECTED = 1u << 0,
    HID_ATTR_WIRED = 1u << 1,
    HID_ATTR_LEFT_CONNECTED = 1u << 2,
    HID_ATTR_LEFT_WIRED = 1u << 3,
    HID_ATTR_RIGHT_CONNECTED = 1u << 4,
    HID_ATTR_RIGHT_WIRED = 1u << 5
};

/** @brief Touch attribute bits. */
enum {
    HID_TOUCH_START = 1u << 0,
    HID_TOUCH_END = 1u << 1
};

/** @brief Mouse attribute and button bits. */
enum {
    HID_MOUSE_CONNECTED = 1u << 1,
    HID_MOUSE_LEFT = 1u << 0,
    HID_MOUSE_RIGHT = 1u << 1,
    HID_MOUSE_MIDDLE = 1u << 2,
    HID_MOUSE_FORWARD = 1u << 3,
    HID_MOUSE_BACK = 1u << 4
};

/** @brief Width and height touch coordinates are scaled to. */
#define HID_TOUCH_WIDTH 1280u
#define HID_TOUCH_HEIGHT 720u

/**
 * @brief Samples input once now and appends an entry to every ring. Does
 * nothing until a guest has created the applet resource. Tests use it with
 * rl_hid_set_manual(true) for deterministic sampling numbers.
 */
RL_RUNTIME_API void rl_hid_update_now(void);
/**
 * @brief Stops and joins the update thread and releases the shared memory
 * and events. The runtime calls it before rl_hos_shutdown.
 */
RL_RUNTIME_API void rl_hid_stop(void);
/** @brief True while the shared memory exists and the update thread is running. */
RL_RUNTIME_API bool rl_hid_is_active(void);
/** @brief When true, the update thread is not started (tests drive rl_hid_update_now). Reset by rl_hos_init. */
RL_RUNTIME_API void rl_hid_set_manual(bool manual);

#endif
