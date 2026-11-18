/**
 * @file play.h
 * @brief What a player sees: the two screens laid out in one picture, and
 * the setting that chooses the layout.
 */
#ifndef RELIFT_NDS_PLAY_H
#define RELIFT_NDS_PLAY_H

#include "nds.h"

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

#endif
