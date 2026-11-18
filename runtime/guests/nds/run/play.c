/**
 * @file play.c
 * @brief The screens' layout in the window.
 */
#include "play.h"

#include <relift/settings.h>

#include <string.h>

#define SW RL_NDS_SCREEN_W
#define SH RL_NDS_SCREEN_H

rl_nds_layout rl_nds_layout_setting(void)
{
    return rl_setting_enum("video.nds_layout") == 1 ? RL_NDS_LAYOUT_SIDE_BY_SIDE : RL_NDS_LAYOUT_STACKED;
}

void rl_nds_layout_size(rl_nds_layout l, uint32_t *w, uint32_t *h)
{
    *w = l == RL_NDS_LAYOUT_SIDE_BY_SIDE ? 2 * SW : SW;
    *h = l == RL_NDS_LAYOUT_SIDE_BY_SIDE ? SH : 2 * SH;
}

void rl_nds_layout_rgb(rl_nds_layout l, const uint32_t *pixels, uint8_t *rgb)
{
    if (l == RL_NDS_LAYOUT_STACKED) {
        rl_nds_frame_rgb(pixels, rgb);
        return;
    }
    static uint8_t stacked[SW * 2 * SH * 3];
    rl_nds_frame_rgb(pixels, stacked);
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t y = 0; y < SH; y++)
            memcpy(rgb + (y * 2 * SW + s * SW) * 3, stacked + ((s * SH + y) * SW) * 3, SW * 3);
}

bool rl_nds_layout_touch(rl_nds_layout l, uint32_t x, uint32_t y, uint8_t *tx, uint8_t *ty)
{
    uint32_t w, h;
    rl_nds_layout_size(l, &w, &h);
    if (x >= w || y >= h)
        return false;
    if (l == RL_NDS_LAYOUT_SIDE_BY_SIDE) {
        if (x < SW)
            return false;
        x -= SW;
    } else {
        if (y < SH)
            return false;
        y -= SH;
    }
    *tx = (uint8_t)x;
    *ty = (uint8_t)y;
    return true;
}
