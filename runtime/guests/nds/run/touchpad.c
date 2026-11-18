/**
 * @file touchpad.c
 * @brief The DS on-screen controller: the d-pad, A B X Y, L and R, Start and
 * Select, the Hide button and the Menu tab. Below the picture in portrait;
 * in landscape the picture is a column in the middle and the controls sit
 * in the side bars. Fingers no shape takes are the bottom screen's pen.
 */
#include "play.h"

#include <relift/touchpad_layout.h>

#define DPAD(x, y, s) RL_TP_DPAD("up", "right", "down", "left", x, y, s)

static const rl_touchpad_item g_portrait[] = {
    RL_TP_MENU(540, 1790, 230, 78),
    DPAD(290, 2060, 380),
    RL_TP_FACE("A", "a", 940, 2060, 75),
    RL_TP_FACE("B", "b", 820, 2180, 75),
    RL_TP_FACE("X", "x", 820, 1940, 75),
    RL_TP_FACE("Y", "y", 700, 2060, 75),
    RL_TP_PILL("L", "l", 190, 1790, 340, 96),
    RL_TP_PILL("R", "r", 890, 1790, 340, 96),
    RL_TP_PILL("Select", "select", 255, 2330, 230, 84),
    RL_TP_PILL("Start", "start", 785, 2330, 230, 84),
    RL_TP_HIDE(975, 2330, 120, 56),
    RL_TP_PILL2("Screens", "swap", "layout", 520, 2330, 230, 84),
};

static const rl_touchpad_item g_landscape[] = {
    RL_TP_MENU(1200, 50, 230, 70),
    DPAD(220, 560, 360),
    RL_TP_FACE("A", "a", 2295, 560, 78),
    RL_TP_FACE("B", "b", 2170, 685, 78),
    RL_TP_FACE("X", "x", 2170, 435, 78),
    RL_TP_FACE("Y", "y", 2045, 560, 78),
    RL_TP_PILL("L", "l", 190, 120, 300, 100),
    RL_TP_PILL("R", "r", 2210, 120, 300, 100),
    RL_TP_PILL("Select", "select", 200, 930, 230, 88),
    RL_TP_PILL("Start", "start", 2200, 930, 230, 88),
    RL_TP_HIDE(120, 1026, 130, 56),
    RL_TP_PILL2("Screens", "swap", "layout", 200, 820, 230, 80),
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
    .portrait = {1080, 2400, g_portrait, RL_TP_COUNT(g_portrait), g_groups, RL_TP_COUNT(g_groups)},
    .landscape = {2400, 1080, g_landscape, RL_TP_COUNT(g_landscape), g_groups, RL_TP_COUNT(g_groups)},
};

const struct rl_touchpad_guest *rl_nds_touchpad(void) { return &g_nds; }
