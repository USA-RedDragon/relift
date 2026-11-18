/**
 * @file play.c
 * @brief The screens' layout in the window and the host's input as the
 * DS's keys and touch screen.
 */
#include "play.h"

#include <relift/settings.h>
#include <relift/touchpad.h>

#include <string.h>

#define SW RL_NDS_SCREEN_W
#define SH RL_NDS_SCREEN_H

rl_screens_config rl_nds_screens_config(bool swap)
{
    rl_screens_config c = {
        .layout = (rl_screens_layout)rl_setting_enum("video.nds_layout"),
        .displays = (rl_screens_displays)rl_setting_enum("video.nds_displays"),
        .integer_scale = rl_setting_bool("video.nds_integer_scale"),
        .swap = swap,
    };
    return c;
}

void rl_nds_screens_frames(const uint32_t *pixels, uint8_t *rgb, rl_screens_frame frames[RL_SCREENS])
{
    rl_nds_frame_rgb(pixels, rgb);
    for (uint32_t s = 0; s < RL_SCREENS; s++)
        frames[s] = (rl_screens_frame){rgb + (size_t)s * SW * SH * 3u, SW, SH};
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
    c->swap = rl_ports_find("p1", "swap");
    c->layout = rl_ports_find("p1", "layout");
}

static const char *const g_cycle[] = {"stacked", "side_by_side", "large_top", "large_bottom", "top_only", "bottom_only"};
static const char *const g_names[] = {"Stacked", "Side by side", "Large top", "Large bottom", "Top only", "Bottom only"};

static void cycle_layout(void)
{
    uint32_t now = rl_setting_enum("video.nds_layout");
    unsigned count = sizeof g_cycle / sizeof g_cycle[0], next = now == 0 || now > count ? 1 : now % count;
    rl_settings_save("video.nds_layout", g_cycle[next]);
    rl_touchpad_notice(g_names[next]);
}

void rl_nds_arrange(const rl_nds_controls *c, const rl_input_snapshot *s, rl_nds_arrangement *a)
{
    bool swap_now = rl_ports_eval(c->swap, s) >= 0.5f, layout_now = rl_ports_eval(c->layout, s) >= 0.5f;
    if (swap_now && !a->swap_held) {
        a->swap = !a->swap;
        rl_touchpad_notice("Screens swapped");
    }
    if (layout_now && !a->layout_held)
        cycle_layout();
    a->swap_held = swap_now;
    a->layout_held = layout_now;
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
    if (point == RL_NDS_POINT_OFF || (point == RL_NDS_POINT_NONE && rl_ports_eval(c->press, s) < 0.5f))
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
