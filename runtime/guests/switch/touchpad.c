/**
 * @file touchpad.c
 * @brief The Switch on-screen controller: both sticks with their L3 and R3
 * buttons, the d-pad, A B X Y, the shoulder pairs, Minus and Plus, the Hide
 * button and the Menu tab. Below the picture in portrait; over the corners
 * of the full-size picture in landscape, where fingers no shape takes are
 * the touch screen's.
 */
#include "hid_priv.h"

#include <relift/touchpad_layout.h>

#define DPAD(x, y, s) RL_TP_DPAD("dpad_up", "dpad_right", "dpad_down", "dpad_left", x, y, s)

static const rl_touchpad_item g_portrait[] = {
    RL_TP_MENU(540, 855, 230, 78),
    RL_TP_STICK("lstick_x", "lstick_y", 230, 1440, 150),
    RL_TP_BUTTON("L3", "lstick", 455, 1350, 44),
    RL_TP_STICK("rstick_x", "rstick_y", 720, 1940, 150),
    RL_TP_BUTTON("R3", "rstick", 950, 2000, 44),
    DPAD(360, 1940, 340),
    RL_TP_FACE("A", "a", 970, 1420, 82),
    RL_TP_FACE("B", "b", 850, 1540, 82),
    RL_TP_FACE("X", "x", 850, 1300, 82),
    RL_TP_FACE("Y", "y", 730, 1420, 82),
    RL_TP_PILL("ZL", "zl", 200, 960, 320, 96),
    RL_TP_PILL("L", "l", 200, 1075, 320, 96),
    RL_TP_PILL("ZR", "zr", 880, 960, 320, 96),
    RL_TP_PILL("R", "r", 880, 1075, 320, 96),
    RL_TP_PILL("Minus", "minus", 455, 1025, 170, 70),
    RL_TP_PILL("Plus", "plus", 625, 1025, 170, 70),
    RL_TP_HIDE(960, 2200, 130, 56),
};

static const rl_touchpad_item g_landscape[] = {
    RL_TP_MENU(1200, 50, 230, 70),
    RL_TP_STICK("lstick_x", "lstick_y", 230, 540, 140),
    RL_TP_BUTTON("L3", "lstick", 440, 400, 44),
    RL_TP_STICK("rstick_x", "rstick_y", 1920, 900, 140),
    RL_TP_BUTTON("R3", "rstick", 2140, 990, 44),
    DPAD(480, 900, 300),
    RL_TP_FACE("A", "a", 2275, 540, 72),
    RL_TP_FACE("B", "b", 2170, 645, 72),
    RL_TP_FACE("X", "x", 2170, 435, 72),
    RL_TP_FACE("Y", "y", 2065, 540, 72),
    RL_TP_PILL("ZL", "zl", 200, 100, 300, 92),
    RL_TP_PILL("L", "l", 200, 215, 300, 92),
    RL_TP_PILL("ZR", "zr", 2200, 100, 300, 92),
    RL_TP_PILL("R", "r", 2200, 215, 300, 92),
    RL_TP_PILL("Minus", "minus", 500, 80, 150, 64),
    RL_TP_PILL("Plus", "plus", 1900, 80, 150, 64),
    RL_TP_HIDE(120, 1026, 130, 56),
};

static const rl_touchpad_group g_groups[] = {
    { "left_stick", 1, 2 },     { "right_stick", 3, 2 },     { "dpad", 5, 1 },  { "face", 6, 4 },
    { "left_shoulder", 10, 2 }, { "right_shoulder", 12, 2 }, { "minus", 14, 1 }, { "plus", 15, 1 },
    { "hide", 16, 1 },
};

static const rl_touchpad_guest g_switch = {
    .name = "switch",
    .picture_width = 1280,
    .picture_height = 720,
    .game_touch = true,
    .portrait = RL_TP_LAYOUT(1080, 2400, g_portrait, g_groups),
    .landscape = RL_TP_LAYOUT(2400, 1080, g_landscape, g_groups),
};

const struct rl_touchpad_guest *rl_switch_touchpad(void) { return &g_switch; }
