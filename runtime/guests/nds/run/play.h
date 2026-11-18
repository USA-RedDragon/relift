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

#include <stdbool.h>
#include <stdint.h>

/** @brief How the two screens share the picture. */
typedef enum rl_nds_layout {
    /** @brief The top screen above the bottom one: 256 x 384, as the console holds them. */
    RL_NDS_LAYOUT_STACKED = 0,
    /** @brief The top screen left of the bottom one: 512 x 192. */
    RL_NDS_LAYOUT_SIDE_BY_SIDE = 1,
} rl_nds_layout;

/** @brief The layout the video.nds_layout setting names. */
rl_nds_layout rl_nds_layout_setting(void);
/** @brief Width and height of a picture in layout l. */
void rl_nds_layout_size(rl_nds_layout l, uint32_t *w, uint32_t *h);
/** @brief Converts a frame (rl_nds_frame_fn) to 8-bit RGB in layout l, the picture rl_nds_layout_size gives. */
void rl_nds_layout_rgb(rl_nds_layout l, const uint32_t *pixels, uint8_t *rgb);
/** @brief The bottom screen's pixel at picture pixel (x, y) of layout l; false when that pixel is on the top screen or outside. */
bool rl_nds_layout_touch(rl_nds_layout l, uint32_t x, uint32_t y, uint8_t *tx, uint8_t *ty);

/** @brief The controls of the native ports that make the DS's input, found once. */
typedef struct rl_nds_controls {
    /** @brief p1's buttons in the order of KEYINPUT (A, B, Select, Start, Right, Left, Up, Down, R, L), then X and Y. */
    rl_control_ref key[12];
    /** @brief touch.press, touch.x and touch.y. */
    rl_control_ref press, x, y;
} rl_nds_controls;

/** @brief Finds the controls of the ports declared (rl_nds_ports and any a game adds). */
void rl_nds_controls_find(rl_nds_controls *c);

/** @brief Where a held mouse button points. */
typedef enum rl_nds_point {
    /** @brief No mouse button is held over a window: the touch port's x and y give the place. */
    RL_NDS_POINT_NONE = 0,
    /** @brief On the bottom screen, at the pixel given. */
    RL_NDS_POINT_BOTTOM = 1,
    /** @brief Elsewhere (the top screen, the margins): no touch. */
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
