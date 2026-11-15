/**
 * @file touchpad.c
 * @brief The GBA on-screen controller: the GB layout with L and R above the
 * d-pad and the buttons in portrait and in the top corners in landscape.
 */
#include "gba.h"

#include <relift/touchpad_layout.h>

#define DPAD(x, y, s) RL_TP_DPAD("up", "right", "down", "left", x, y, s)

static const rl_touchpad_item g_portrait[] = {
    RL_TP_MENU(540, 1010, 230, 78),
    DPAD(280, 1790, 440),
    RL_TP_FACE("A", "a", 900, 1770, 100),
    RL_TP_FACE("B", "b", 690, 1910, 100),
    RL_TP_PILL("L", "l", 245, 1215, 390, 110),
    RL_TP_PILL("R", "r", 835, 1215, 390, 110),
    RL_TP_PILL("Select", "select", 400, 2200, 230, 84),
    RL_TP_PILL("Start", "start", 680, 2200, 230, 84),
    RL_TP_HIDE(960, 2200, 130, 56),
};

static const rl_touchpad_item g_landscape[] = {
    RL_TP_MENU(1200, 50, 230, 70),
    DPAD(200, 560, 340),
    RL_TP_FACE("A", "a", 2290, 540, 90),
    RL_TP_FACE("B", "b", 2120, 670, 90),
    RL_TP_PILL("L", "l", 190, 110, 300, 100),
    RL_TP_PILL("R", "r", 2210, 110, 300, 100),
    RL_TP_PILL("Select", "select", 200, 890, 230, 88),
    RL_TP_PILL("Start", "start", 2200, 890, 230, 88),
    RL_TP_HIDE(120, 1026, 130, 56),
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
    .portrait = { 1080, 2400, g_portrait, RL_TP_COUNT(g_portrait), g_groups, RL_TP_COUNT(g_groups) },
    .landscape = { 2400, 1080, g_landscape, RL_TP_COUNT(g_landscape), g_groups, RL_TP_COUNT(g_groups) },
};

const struct rl_touchpad_guest *rl_gba_touchpad(void) { return &g_gba; }
