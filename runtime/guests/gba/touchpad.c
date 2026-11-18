/**
 * @file touchpad.c
 * @brief The GBA on-screen controller, written in dp and fitted to the view:
 * the DS's columns (L, the d-pad and Select; R, A B and Start) and its row
 * of Menu and Hide at the top center.
 */
#include "gba.h"

#include <relift/touchpad_layout.h>

#define DPAD(x, y, s) RL_TP_DPAD("up", "right", "down", "left", x, y, s)

static const rl_touchpad_item g_portrait[] = {
    RL_TP_MENU(166.5, 32, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_HIDE(244.5, 32, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_PILL("L", "l", 90, 78, RL_TP_COLUMN, RL_TP_SHOULDER_H),
    RL_TP_PILL("R", "r", 321, 78, RL_TP_COLUMN, RL_TP_SHOULDER_H),
    DPAD(90, 182, RL_TP_COLUMN),
    RL_TP_FACE("A", "a", 363, 158, 32),
    RL_TP_FACE("B", "b", 279, 206, 32),
    RL_TP_PILL("Select", "select", 90, 284, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_PILL("Start", "start", 321, 284, RL_TP_SMALL_W, RL_TP_SMALL_H),
};

static const rl_touchpad_group g_portrait_groups[] = {
    { "hide", 1, 1 },
    { "l", 2, 1 },
    { "r", 3, 1 },
    { "dpad", 4, 1 },
    { "ab", 5, 2 },
    { "select", 7, 1 },
    { "start", 8, 1 },
};

static const rl_touchpad_item g_landscape[] = {
    RL_TP_MENU(361, 32, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_HIDE(439, 32, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_PILL("L", "l", 90, 36, RL_TP_COLUMN, RL_TP_SHOULDER_H),
    RL_TP_PILL("R", "r", 710, 36, RL_TP_COLUMN, RL_TP_SHOULDER_H),
    DPAD(90, 168, RL_TP_COLUMN),
    RL_TP_FACE("A", "a", 752, 144, 32),
    RL_TP_FACE("B", "b", 668, 192, 32),
    RL_TP_PILL("Select", "select", 90, 304, RL_TP_SMALL_W, RL_TP_SMALL_H),
    RL_TP_PILL("Start", "start", 710, 304, RL_TP_SMALL_W, RL_TP_SMALL_H),
};

static const rl_touchpad_group g_landscape_groups[] = {
    { "hide", 1, 1 },
    { "l", 2, 1 },
    { "r", 3, 1 },
    { "dpad", 4, 1 },
    { "ab", 5, 2 },
    { "select", 7, 1 },
    { "start", 8, 1 },
};

static const rl_touchpad_guest g_gba = {
    .name = "gba",
    .picture_width = 240,
    .picture_height = 160,
    .integer_scale = true,
    .portrait = RL_TP_FITTED(411, 316, g_portrait, g_portrait_groups, true),
    .landscape = RL_TP_FITTED(800, 336, g_landscape, g_landscape_groups, false),
};

const struct rl_touchpad_guest *rl_gba_touchpad(void) { return &g_gba; }
