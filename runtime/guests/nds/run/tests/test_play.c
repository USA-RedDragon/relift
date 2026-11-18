/**
 * @file test_play.c
 * @brief The screens' layouts, the pointer on the bottom screen, and the
 * native ports as the DS's keys and touch screen.
 */
#include "play.h"
#include "runner.h"

#include <relift/settings.h>

#include <string.h>

TEST(the_screens_settings_name_the_layout_displays_and_scaling)
{
    rl_nds_declare_settings();
    rl_screens_config c = rl_nds_screens_config(false);
    CHECK_EQ(c.layout, RL_SCREENS_AUTO);
    CHECK_EQ(c.displays, RL_SCREENS_DISPLAYS_AUTO);
    CHECK(c.integer_scale);
    CHECK(!c.swap);
    CHECK_EQ(rl_settings_set("video.nds_layout", "large_bottom"), RL_SETTING_OK);
    CHECK_EQ(rl_settings_set("video.nds_displays", "bottom_both"), RL_SETTING_OK);
    CHECK_EQ(rl_settings_set("video.nds_integer_scale", "false"), RL_SETTING_OK);
    c = rl_nds_screens_config(true);
    CHECK_EQ(c.layout, RL_SCREENS_LARGE_BOTTOM);
    CHECK_EQ(c.displays, RL_SCREENS_DISPLAYS_BOTTOM_BOTH);
    CHECK(!c.integer_scale);
    CHECK(c.swap);
    CHECK_EQ(rl_settings_set("video.nds_layout", "bottom_only"), RL_SETTING_OK);
    CHECK_EQ(rl_nds_screens_config(false).layout, RL_SCREENS_BOTTOM_ONLY);
    CHECK_EQ(rl_settings_set("video.nds_layout", "bogus"), RL_SETTING_INVALID);
}

TEST(a_frame_splits_into_the_two_screens)
{
    static uint32_t px[2 * RL_NDS_SCREEN_W * RL_NDS_SCREEN_H];
    static uint8_t rgb[2 * RL_NDS_SCREEN_W * RL_NDS_SCREEN_H * 3];
    for (uint32_t i = 0; i < sizeof px / sizeof px[0]; i++)
        px[i] = i >= RL_NDS_SCREEN_W * RL_NDS_SCREEN_H ? 0x3Fu : 0u;
    rl_screens_frame f[RL_SCREENS];
    rl_nds_screens_frames(px, rgb, f);
    CHECK_EQ(f[0].width, 256);
    CHECK_EQ(f[1].height, 192);
    CHECK_EQ(f[0].rgb[0], 0);
    CHECK(f[1].rgb[0] != 0);
    CHECK(f[1].rgb == f[0].rgb + 256 * 192 * 3);
}

static void key_down(rl_input_snapshot *s, unsigned usage) { s->keys[usage / 64] |= 1ull << (usage % 64); }

static bool ports(rl_nds_controls *c)
{
    rl_ports_reset();
    rl_ports_script_reset();
    bool ok = rl_ports_declare(rl_nds_ports, rl_nds_ports_count);
    rl_ports_bind_defaults();
    rl_nds_controls_find(c);
    return ok;
}

TEST(keyboard_and_pad_give_the_keys_as_the_gba_guest_binds_them)
{
    rl_nds_controls c;
    CHECK(ports(&c));
    rl_input_snapshot s;
    memset(&s, 0, sizeof s);
    s.pads[0].connected = true;
    rl_nds_input in;
    rl_nds_input_from(&c, &s, RL_NDS_POINT_NONE, 0, 0, &in);
    CHECK_EQ(in.keys, 0);
    CHECK(!in.x && !in.y && !in.touch && !in.lid_closed);
    key_down(&s, 0x1B);
    key_down(&s, 0x28);
    key_down(&s, 0x52);
    key_down(&s, 0x04);
    s.pads[0].buttons = 1u << RL_BTN_NORTH | 1u << RL_BTN_RIGHT_SHOULDER;
    rl_nds_input_from(&c, &s, RL_NDS_POINT_NONE, 0, 0, &in);
    CHECK_EQ(in.keys, RL_NDS_KEY_A | RL_NDS_KEY_START | RL_NDS_KEY_UP | RL_NDS_KEY_R);
    CHECK(in.x);
    CHECK(in.y);
    memset(&s, 0, sizeof s);
    s.pads[0].connected = true;
    s.pads[0].lx = -32767;
    s.pads[0].buttons = 1u << RL_BTN_SOUTH | 1u << RL_BTN_BACK | 1u << RL_BTN_LEFT_SHOULDER | 1u << RL_BTN_DPAD_DOWN;
    rl_nds_input_from(&c, &s, RL_NDS_POINT_NONE, 0, 0, &in);
    CHECK_EQ(in.keys, RL_NDS_KEY_B | RL_NDS_KEY_SELECT | RL_NDS_KEY_L | RL_NDS_KEY_DOWN | RL_NDS_KEY_LEFT);
}

TEST(the_pen_the_windows_report_is_the_touch)
{
    rl_nds_controls c;
    CHECK(ports(&c));
    rl_input_snapshot s;
    memset(&s, 0, sizeof s);
    rl_nds_input in;
    rl_nds_input_from(&c, &s, RL_NDS_POINT_BOTTOM, 120, 90, &in);
    CHECK(in.touch);
    CHECK_EQ(in.touch_x, 120);
    CHECK_EQ(in.touch_y, 90);
    rl_nds_input_from(&c, &s, RL_NDS_POINT_OFF, 120, 90, &in);
    CHECK(!in.touch);
    s.pointer.buttons = 1u << RL_PTR_LEFT;
    rl_nds_input_from(&c, &s, RL_NDS_POINT_OFF, 120, 90, &in);
    CHECK(!in.touch);
}

TEST(the_swap_control_is_bound_to_a_key_and_the_on_screen_button)
{
    rl_nds_controls c;
    CHECK(ports(&c));
    rl_input_snapshot s;
    memset(&s, 0, sizeof s);
    CHECK(rl_ports_eval(c.swap, &s) < 0.5f);
    key_down(&s, 0x2B);
    CHECK(rl_ports_eval(c.swap, &s) >= 0.5f);
    memset(&s, 0, sizeof s);
    s.touch_pad[26] = 1.0f;
    CHECK(rl_ports_eval(c.swap, &s) < 0.5f);
    CHECK(rl_ports_declare(rl_nds_ports_touchpad, rl_nds_ports_touchpad_count));
    rl_ports_bind_defaults();
    rl_nds_controls_find(&c);
    CHECK(rl_ports_eval(c.swap, &s) >= 0.5f);
}

TEST(an_input_script_touches_at_the_touch_ports_place)
{
    rl_nds_controls c;
    CHECK(ports(&c));
    CHECK(rl_ports_set_script("1:touch.press,1:touch.x=0.5,1:touch.y=0.25,1:p1.select"));
    rl_ports_script_frame(1);
    rl_input_snapshot s;
    memset(&s, 0, sizeof s);
    rl_nds_input in;
    rl_nds_input_from(&c, &s, RL_NDS_POINT_NONE, 0, 0, &in);
    CHECK(in.touch);
    CHECK_EQ(in.touch_x, 128);
    CHECK_EQ(in.touch_y, 48);
    CHECK_EQ(in.keys, RL_NDS_KEY_SELECT);
    rl_ports_script_frame(8);
    rl_nds_input_from(&c, &s, RL_NDS_POINT_NONE, 0, 0, &in);
    CHECK(!in.touch);
    CHECK_EQ(in.keys, 0);
    rl_ports_script_reset();
}
