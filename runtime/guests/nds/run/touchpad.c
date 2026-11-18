/**
 * @file touchpad.c
 * @brief The DS on-screen controller, written in dp and fitted to the view.
 * Every orientation has two columns: L, the d-pad and Select on the left,
 * R, X Y A B and Start on the right, each column on one center line with the
 * same width. Menu, Screens and Hide are one row at the top center. In
 * portrait the panel sits below the picture; in landscape the columns are the
 * side bars. Fingers no shape takes are the bottom screen's pen.
 */
#include "play.h"

#include <relift/touchpad_layout.h>

#define DPAD(x, y, s) RL_TP_DPAD("up", "right", "down", "left", x, y, s)

static const rl_touchpad_item g_portrait[] = {
    RL_TP_MENU(205.5, 96, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_PILL2("Screens", "swap", "layout", 205.5, 138, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_HIDE(205.5, 180, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_PILL("L", "l", 88, 36, RL_TP_COLUMN, RL_TP_SHOULDER_H),
    RL_TP_PILL("R", "r", 323, 36, RL_TP_COLUMN, RL_TP_SHOULDER_H),
    DPAD(88, 138, RL_TP_COLUMN),
    RL_TP_FACE("A", "a", 369, 138, 26),
    RL_TP_FACE("B", "b", 323, 184, 26),
    RL_TP_FACE("X", "x", 323, 92, 26),
    RL_TP_FACE("Y", "y", 277, 138, 26),
    RL_TP_PILL("Select", "select", 88, 238, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_PILL("Start", "start", 323, 238, RL_TP_SMALL_W, RL_TP_SMALL_H),
};

static const rl_touchpad_group g_portrait_groups[] = {
    { "swap", 1, 1 },
    { "hide", 2, 1 },
    { "l", 3, 1 },
    { "r", 4, 1 },
    { "dpad", 5, 1 },
    { "face", 6, 4 },
    { "select", 10, 1 },
    { "start", 11, 1 },
};

static const rl_touchpad_item g_landscape[] = {
    RL_TP_MENU(322, 36, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_PILL2("Screens", "swap", "layout", 400, 36, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_HIDE(478, 36, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_PILL("L", "l", 88, 36, RL_TP_COLUMN, RL_TP_SHOULDER_H),
    RL_TP_PILL("R", "r", 712, 36, RL_TP_COLUMN, RL_TP_SHOULDER_H),
    DPAD(88, 168, RL_TP_COLUMN),
    RL_TP_FACE("A", "a", 758, 168, 26),
    RL_TP_FACE("B", "b", 712, 214, 26),
    RL_TP_FACE("X", "x", 712, 122, 26),
    RL_TP_FACE("Y", "y", 666, 168, 26),
    RL_TP_PILL("Select", "select", 88, 304, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_PILL("Start", "start", 712, 304, RL_TP_SMALL_W, RL_TP_SMALL_H),
};

static const rl_touchpad_group g_landscape_groups[] = {
    { "swap", 1, 1 },
    { "hide", 2, 1 },
    { "l", 3, 1 },
    { "r", 4, 1 },
    { "dpad", 5, 1 },
    { "face", 6, 4 },
    { "select", 10, 1 },
    { "start", 11, 1 },
};

static const rl_touchpad_guest g_nds = {
    .name = "nds",
    .picture_width = 256,
    .picture_height = 384,
    .integer_scale = false,
    .game_touch = true,
    .keep_clear = true,
    .portrait = RL_TP_FITTED(411, 270, g_portrait, g_portrait_groups, true),
    .landscape = RL_TP_FITTED(800, 336, g_landscape, g_landscape_groups, false),
};

const struct rl_touchpad_guest *rl_nds_touchpad(void) { return &g_nds; }
