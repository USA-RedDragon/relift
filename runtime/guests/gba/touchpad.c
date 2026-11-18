/**
 * @file touchpad.c
 * @brief The GBA on-screen controller: the d-pad, A and B, L and R, Select
 * and Start, and the small Hide and Menu buttons, written in dp and fitted to
 * the view: a panel below the picture in portrait, the side bars in landscape.
 */
#include "gba.h"

#include <relift/touchpad_layout.h>

#define DPAD(x, y, s) RL_TP_DPAD("up", "right", "down", "left", x, y, s)

static const rl_touchpad_item g_portrait[] = {
    RL_TP_MENU(205.5f, 26, 76, 36),
    DPAD(86, 140, 140),
    RL_TP_FACE("A", "a", 363, 118, 32),
    RL_TP_FACE("B", "b", 291, 162, 32),
    RL_TP_PILL("L", "l", 68, 26, 104, 44),
    RL_TP_PILL("R", "r", 343, 26, 104, 44),
    RL_TP_PILL("Select", "select", 155.5f, 252, 88, 40),
    RL_TP_PILL("Start", "start", 255.5f, 252, 88, 40),
    RL_TP_HIDE(365, 252, 60, 36),
};

static const rl_touchpad_item g_landscape[] = {
    RL_TP_MENU(54, 88, 76, 36),
    DPAD(86, 192, 140),
    RL_TP_FACE("A", "a", 752, 170, 32),
    RL_TP_FACE("B", "b", 680, 214, 32),
    RL_TP_PILL("L", "l", 68, 34, 104, 44),
    RL_TP_PILL("R", "r", 732, 34, 104, 44),
    RL_TP_PILL("Select", "select", 60, 304, 88, 40),
    RL_TP_PILL("Start", "start", 740, 304, 88, 40),
    RL_TP_HIDE(754, 88, 60, 36),
};

static const rl_touchpad_group g_groups[] = {
    { "dpad", 1, 1 }, { "ab", 2, 2 },     { "l", 4, 1 },    { "r", 5, 1 },
    { "select", 6, 1 }, { "start", 7, 1 }, { "hide", 8, 1 },
};

static const rl_touchpad_guest g_gba = {
    .name = "gba",
    .picture_width = 240,
    .picture_height = 160,
    .integer_scale = true,
    .portrait = RL_TP_FITTED(411, 288, g_portrait, g_groups, true),
    .landscape = RL_TP_FITTED(800, 336, g_landscape, g_groups, false),
};

const struct rl_touchpad_guest *rl_gba_touchpad(void) { return &g_gba; }
