/**
 * @file touchpad.c
 * @brief The GB and GBC on-screen controller: d-pad, A and B, Select and
 * Start, the Hide button and the Menu tab, beside the picture in landscape
 * and below it in portrait.
 */
#include "gb.h"

#include <relift/touchpad_layout.h>

#define DPAD(x, y, s) RL_TP_DPAD("up", "right", "down", "left", x, y, s)

static const rl_touchpad_item g_portrait[] = {
    RL_TP_MENU(540, 1262, 230, 78),
    DPAD(280, 1800, 440),
    RL_TP_FACE("A", "a", 900, 1790, 100),
    RL_TP_FACE("B", "b", 690, 1930, 100),
    RL_TP_PILL("Select", "select", 400, 2190, 230, 84),
    RL_TP_PILL("Start", "start", 680, 2190, 230, 84),
    RL_TP_HIDE(960, 2200, 130, 56),
};

static const rl_touchpad_item g_landscape[] = {
    RL_TP_MENU(1200, 50, 230, 70),
    DPAD(300, 520, 420),
    RL_TP_FACE("A", "a", 2230, 520, 105),
    RL_TP_FACE("B", "b", 2010, 650, 105),
    RL_TP_PILL("Select", "select", 300, 890, 230, 88),
    RL_TP_PILL("Start", "start", 2100, 890, 230, 88),
    RL_TP_HIDE(120, 1026, 130, 56),
};

static const rl_touchpad_group g_groups[] = {
    { "dpad", 1, 1 }, { "ab", 2, 2 }, { "select", 4, 1 }, { "start", 5, 1 }, { "hide", 6, 1 },
};

static const rl_touchpad_guest g_gb = {
    .name = "gb",
    .picture_width = 160,
    .picture_height = 144,
    .integer_scale = true,
    .portrait = { 1080, 2400, g_portrait, RL_TP_COUNT(g_portrait), g_groups, RL_TP_COUNT(g_groups) },
    .landscape = { 2400, 1080, g_landscape, RL_TP_COUNT(g_landscape), g_groups, RL_TP_COUNT(g_groups) },
};

const struct rl_touchpad_guest *rl_gb_touchpad(void) { return &g_gb; }
