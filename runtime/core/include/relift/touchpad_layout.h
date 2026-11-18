/**
 * @file touchpad_layout.h
 * @brief Shorthands for writing a guest's on-screen controller layout
 * (touchpad.h) as a table: positions and sizes in the reference screen's
 * pixels (or in dp for a fitted layout), buttons by radius.
 */
#ifndef RELIFT_TOUCHPAD_LAYOUT_H
#define RELIFT_TOUCHPAD_LAYOUT_H

#include <relift/touchpad.h>

#define RL_TP_MENU(x, y, w, h) { RL_TOUCHPAD_MENU, false, "Menu", { NULL }, x, y, w, h }
#define RL_TP_HIDE(x, y, w, h) { RL_TOUCHPAD_HIDE, false, "Hide", { NULL }, x, y, w, h }
#define RL_TP_BUTTON(label, control, x, y, r) { RL_TOUCHPAD_BUTTON, false, label, { control }, x, y, 2 * (r), 2 * (r) }
#define RL_TP_FACE(label, control, x, y, r) { RL_TOUCHPAD_BUTTON, true, label, { control }, x, y, 2 * (r), 2 * (r) }
#define RL_TP_PILL(label, control, x, y, w, h) { RL_TOUCHPAD_PILL, false, label, { control }, x, y, w, h }
#define RL_TP_PILL2(label, control, long_control, x, y, w, h) \
    { RL_TOUCHPAD_PILL, false, label, { control, long_control }, x, y, w, h }
#define RL_TP_DPAD(up, right, down, left, x, y, s) { RL_TOUCHPAD_DPAD, false, NULL, { up, right, down, left }, x, y, s, s }
#define RL_TP_STICK(cx, cy, x, y, r) { RL_TOUCHPAD_STICK, false, NULL, { cx, cy }, x, y, 2 * (r), 2 * (r) }
/** @brief The fixed sizes in dp of a fitted layout: a side column, a small pill, and a shoulder pill (a column wide). */
#define RL_TP_COLUMN 144
#define RL_TP_SMALL_W 68
#define RL_TP_SMALL_H 32
#define RL_TP_SHOULDER_H 40
#define RL_TP_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))
/** A layout in reference pixels, of w by h pixels, from the arrays of its items and groups. */
#define RL_TP_LAYOUT(w, h, items, groups) { w, h, items, RL_TP_COUNT(items), groups, RL_TP_COUNT(groups), false, false }
/** A fitted layout in dp (see rl_touchpad_layout), of w by h dp; panel: it is a panel along the bottom of a tall view. */
#define RL_TP_FITTED(w, h, items, groups, panel) { w, h, items, RL_TP_COUNT(items), groups, RL_TP_COUNT(groups), true, panel }

#endif
