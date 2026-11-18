/**
 * @file play.h
 * @brief What a player sees, hears and holds: the two screens laid out in
 * one picture, the host's input as the DS's keys and touch screen, and the
 * settings that choose the layout and the scaling.
 */
#ifndef RELIFT_NDS_PLAY_H
#define RELIFT_NDS_PLAY_H

#include "nds.h"

#include <relift/input.h>
#include <relift/ports.h>
#include <relift/screens.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief The screens' layout, displays and scaling the settings name, with the player's swap. */
rl_screens_config rl_nds_screens_config(bool swap);
/** @brief The two screens of a frame (rl_nds_frame_fn) as RGB888 frames: top first; rgb holds RL_NDS_SCREEN_W * 2 * RL_NDS_SCREEN_H * 3 bytes. */
void rl_nds_screens_frames(const uint32_t *pixels, uint8_t *rgb, rl_screens_frame frames[RL_SCREENS]);

struct rl_touchpad_guest;
/** @brief The on-screen controller's layouts (touchpad.h), declared by the program. */
const struct rl_touchpad_guest *rl_nds_touchpad(void);
/** @brief The on-screen controller's port (touchpad.h), declared when the run uses it. */
extern const rl_port_decl rl_nds_ports_touchpad[];
/** @brief Number of entries of rl_nds_ports_touchpad. */
extern const uint32_t rl_nds_ports_touchpad_count;

/** @brief The controls of the native ports that make the DS's input, found once. */
typedef struct rl_nds_controls {
    /** @brief p1's buttons in the order of KEYINPUT (A, B, Select, Start, Right, Left, Up, Down, R, L), then X and Y. */
    rl_control_ref key[12];
    /** @brief touch.press, touch.x and touch.y. */
    rl_control_ref press, x, y;
    /** @brief p1's swap control (Swap screens). */
    rl_control_ref swap;
} rl_nds_controls;

/** @brief Finds the controls of the ports declared (rl_nds_ports and any a game adds). */
void rl_nds_controls_find(rl_nds_controls *c);

/** @brief Where the pen is, as the windows show it. */
typedef enum rl_nds_point {
    /** @brief The windows say nothing: the touch port's x and y give the place. */
    RL_NDS_POINT_NONE = 0,
    /** @brief The pen is down on the bottom screen at the pixel given, whatever the press control reads. */
    RL_NDS_POINT_BOTTOM = 1,
    /** @brief The pen is up. */
    RL_NDS_POINT_OFF = 2,
} rl_nds_point;

/**
 * @brief The DS's input from snapshot s. The pen is down while touch.press
 * reads pressed, unless point is RL_NDS_POINT_OFF: at (px, py) with
 * RL_NDS_POINT_BOTTOM, else at touch.x and touch.y (0 to 1 across the
 * screen, as an input script or a binding sets them). The lid stays open.
 */
void rl_nds_input_from(const rl_nds_controls *c, const rl_input_snapshot *s, rl_nds_point point, uint8_t px,
                       uint8_t py, rl_nds_input *out);

/** @brief The native ports of the DS (generated, ports_gen.c): p1 and touch. */
extern const rl_port_decl rl_nds_ports[];
/** @brief Number of entries of rl_nds_ports. */
extern const uint32_t rl_nds_ports_count;

#endif
