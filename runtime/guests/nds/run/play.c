/**
 * @file play.c
 * @brief The screens' layout in the window and the host's input as the
 * DS's keys and touch screen.
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

void rl_nds_controls_find(rl_nds_controls *c)
{
    static const char *const names[12] = {"a",  "b",    "select", "start", "right", "left",
                                          "up", "down", "r",      "l",     "x",     "y"};
    for (unsigned i = 0; i < 12; i++)
        c->key[i] = rl_ports_find("p1", names[i]);
    c->press = rl_ports_find("touch", "press");
    c->x = rl_ports_find("touch", "x");
    c->y = rl_ports_find("touch", "y");
}

static uint8_t scaled(float v, uint32_t size)
{
    if (!(v > 0.0f))
        return 0;
    if (v >= 1.0f)
        return (uint8_t)(size - 1);
    return (uint8_t)(v * (float)size);
}

void rl_nds_input_from(const rl_nds_controls *c, const rl_input_snapshot *s, rl_nds_point point, uint8_t px,
                       uint8_t py, rl_nds_input *out)
{
    memset(out, 0, sizeof *out);
    for (unsigned i = 0; i < 10; i++)
        if (rl_ports_eval(c->key[i], s) >= 0.5f)
            out->keys |= (uint16_t)(1u << i);
    out->x = rl_ports_eval(c->key[10], s) >= 0.5f;
    out->y = rl_ports_eval(c->key[11], s) >= 0.5f;
    if (point == RL_NDS_POINT_OFF || rl_ports_eval(c->press, s) < 0.5f)
        return;
    out->touch = true;
    if (point == RL_NDS_POINT_BOTTOM) {
        out->touch_x = px;
        out->touch_y = py;
    } else {
        out->touch_x = scaled(rl_ports_eval(c->x, s), SW);
        out->touch_y = scaled(rl_ports_eval(c->y, s), SH);
    }
}
