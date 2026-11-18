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

/** @brief Bytes of a frame of both screens as RGB888. */
#define RL_NDS_RGB_BYTES (2u * RL_NDS_SCREEN_W * RL_NDS_SCREEN_H * 3u)

/** @brief What the panel remembers: the last two frames as the game drew them (the older one's slot is last[older]), and how many it has seen (up to 2). */
typedef struct rl_nds_lcd {
    uint8_t last[2][RL_NDS_RGB_BYTES];
    uint32_t older;
    uint32_t seen;
} rl_nds_lcd;

/**
 * @brief The console's LCD answers slowly: a pixel that goes to another color
 * and back on every frame shows as the mean of the two, steady, which is how
 * games make flicker transparency. rgb (a frame as rl_nds_screens_frames made
 * it) is changed in place: a pixel whose color is within a quarter of its last
 * change of the color it had two frames ago (it came back, perhaps on a
 * background that is fading) becomes the mean of itself and the last frame's;
 * every other pixel stays as drawn.
 */
void rl_nds_lcd_response(rl_nds_lcd *l, uint8_t *rgb);

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
    /** @brief p1's swap control (Swap screens) and layout control (Cycle screen layout). */
    rl_control_ref swap, layout;
} rl_nds_controls;

/** @brief What the player did to the screens' arrangement: the swap, and which controls were held at the last poll. */
typedef struct rl_nds_arrangement {
    bool swap, swap_held, layout_held;
} rl_nds_arrangement;

/**
 * @brief Reads the swap and layout controls from snapshot s. A press of swap toggles a->swap; a press of layout moves
 * `video.nds_layout` on to the next of stacked, side by side, large top, large bottom, top only and bottom only (auto
 * counts as stacked) and saves it. Either names the result on the on-screen controller for about a second.
 */
void rl_nds_arrange(const rl_nds_controls *c, const rl_input_snapshot *s, rl_nds_arrangement *a);

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
