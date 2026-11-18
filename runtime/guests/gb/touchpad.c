/**
 * @file touchpad.c
 * @brief The GB and GBC on-screen controller, written in dp and fitted to
 * the view: the d-pad and Select on the left, A B and Start on the right, and
 * Menu and Hide at the top center.
 */
#include "gb.h"

#include <relift/touchpad_layout.h>

#define DPAD(x, y, s) RL_TP_DPAD("up", "right", "down", "left", x, y, s)

static const rl_touchpad_item g_portrait[] = {
    RL_TP_MENU(205.5, 67, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_HIDE(205.5, 109, RL_TP_SMALL_W, RL_TP_SMALL_H),
    DPAD(88, 88, RL_TP_COLUMN),
    RL_TP_FACE("A", "a", 363, 64, 32),
    RL_TP_FACE("B", "b", 283, 112, 32),
    RL_TP_PILL("Select", "select", 88, 188, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_PILL("Start", "start", 323, 188, RL_TP_SMALL_W, RL_TP_SMALL_H),
};

static const rl_touchpad_group g_portrait_groups[] = {
    { "hide", 1, 1 },
    { "dpad", 2, 1 },
    { "ab", 3, 2 },
    { "select", 5, 1 },
    { "start", 6, 1 },
};

static const rl_touchpad_item g_landscape[] = {
    RL_TP_MENU(361, 36, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_HIDE(439, 36, RL_TP_SMALL_W, RL_TP_SMALL_H),
    DPAD(88, 168, RL_TP_COLUMN),
    RL_TP_FACE("A", "a", 752, 144, 32),
    RL_TP_FACE("B", "b", 672, 192, 32),
    RL_TP_PILL("Select", "select", 88, 304, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_PILL("Start", "start", 712, 304, RL_TP_SMALL_W, RL_TP_SMALL_H),
};

static const rl_touchpad_group g_landscape_groups[] = {
    { "hide", 1, 1 },
    { "dpad", 2, 1 },
    { "ab", 3, 2 },
    { "select", 5, 1 },
    { "start", 6, 1 },
};

static const rl_touchpad_guest g_gb = {
    .name = "gb",
    .picture_width = 160,
    .picture_height = 144,
    .integer_scale = true,
    .portrait = RL_TP_FITTED(411, 220, g_portrait, g_portrait_groups, true),
    .landscape = RL_TP_FITTED(800, 336, g_landscape, g_landscape_groups, false),
};

const struct rl_touchpad_guest *rl_gb_touchpad(void) { return &g_gb; }
