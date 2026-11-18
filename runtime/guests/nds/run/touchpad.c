/**
 * @file touchpad.c
 * @brief The DS on-screen controller: the d-pad, A B X Y, L and R, Start and
 * Select, and the small Screens, Hide and Menu buttons, written in dp and
 * fitted to the view. A panel below the picture in portrait (L, Menu and R
 * on top, the d-pad and the face buttons under them, Screens, Select, Start
 * and Hide along the bottom); in landscape the picture is a column in the
 * middle and the controls sit in the side bars. Fingers no shape takes are
 * the bottom screen's pen.
 */
#include "play.h"

#include <relift/touchpad_layout.h>

#define DPAD(x, y, s) RL_TP_DPAD("up", "right", "down", "left", x, y, s)

static const rl_touchpad_item g_portrait[] = {
    RL_TP_MENU(205.5f, 26, 76, 36),
    DPAD(86, 140, 140),
    RL_TP_FACE("A", "a", 367, 140, 28),
    RL_TP_FACE("B", "b", 319, 188, 28),
    RL_TP_FACE("X", "x", 319, 92, 28),
    RL_TP_FACE("Y", "y", 271, 140, 28),
    RL_TP_PILL("L", "l", 68, 26, 104, 44),
    RL_TP_PILL("R", "r", 343, 26, 104, 44),
    RL_TP_PILL("Select", "select", 155.5f, 252, 88, 40),
    RL_TP_PILL("Start", "start", 255.5f, 252, 88, 40),
    RL_TP_HIDE(365, 252, 60, 36),
    RL_TP_PILL2("Screens", "swap", "layout", 58, 252, 84, 36),
};

static const rl_touchpad_item g_landscape[] = {
    RL_TP_MENU(54, 88, 76, 36),
    DPAD(86, 192, 140),
    RL_TP_FACE("A", "a", 756, 192, 28),
    RL_TP_FACE("B", "b", 708, 240, 28),
    RL_TP_FACE("X", "x", 708, 144, 28),
    RL_TP_FACE("Y", "y", 660, 192, 28),
    RL_TP_PILL("L", "l", 68, 34, 104, 44),
    RL_TP_PILL("R", "r", 732, 34, 104, 44),
    RL_TP_PILL("Select", "select", 60, 304, 88, 40),
    RL_TP_PILL("Start", "start", 740, 304, 88, 40),
    RL_TP_HIDE(756, 88, 56, 36),
    RL_TP_PILL2("Screens", "swap", "layout", 668, 88, 84, 36),
};

static const rl_touchpad_group g_groups[] = {
    {"dpad", 1, 1}, {"face", 2, 4}, {"l", 6, 1}, {"r", 7, 1}, {"select", 8, 1}, {"start", 9, 1}, {"hide", 10, 1}, {"swap", 11, 1},
};

static const rl_touchpad_guest g_nds = {
    .name = "nds",
    .picture_width = 256,
    .picture_height = 384,
    .integer_scale = false,
    .game_touch = true,
    .keep_clear = true,
    .portrait = RL_TP_FITTED(411, 288, g_portrait, g_groups, true),
    .landscape = RL_TP_FITTED(800, 336, g_landscape, g_groups, false),
};

const struct rl_touchpad_guest *rl_nds_touchpad(void) { return &g_nds; }
