/**
 * @file test_play.c
 * @brief The screens' layouts, the pointer on the bottom screen, and the
 * native ports as the DS's keys and touch screen.
 */
#include "play.h"
#include "runner.h"

#include <relift/settings.h>
#include <relift/touchpad.h>

#include <string.h>
#include <unistd.h>

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

static bool find_screens_button(float *x, float *y)
{
    rl_touchpad_rect r[16];
    uint32_t n = rl_touchpad_placed(r, 16);
    for (uint32_t i = 0; i < n; i++)
        if (r[i].label && !strcmp(r[i].label, "Screens")) {
            *x = (r[i].x0 + r[i].x1) / 2;
            *y = (r[i].y0 + r[i].y1) / 2;
            return true;
        }
    return false;
}

static void touch_up_front(rl_nds_controls *c)
{
    rl_touchpad_reset();
    rl_settings_reset();
    rl_nds_declare_settings();
    CHECK(ports(c));
    rl_touchpad_declare(rl_nds_touchpad());
    CHECK(rl_ports_declare(rl_nds_ports_touchpad, rl_nds_ports_touchpad_count));
    rl_ports_bind_defaults();
    rl_nds_controls_find(c);
    CHECK_EQ(rl_settings_set("input.touch_pad.show", "always"), RL_SETTING_OK);
    rl_touchpad_view(1080, 2520, 2.625f, NULL);
}

static void poll(const rl_nds_controls *c, rl_nds_arrangement *a)
{
    rl_input_snapshot s;
    memset(&s, 0, sizeof s);
    rl_touchpad_fill(s.touch_pad, RL_INPUT_TOUCH_PAD);
    rl_nds_arrange(c, &s, a);
}

TEST(a_tap_on_the_screens_button_swaps_the_screens)
{
    rl_nds_controls c;
    touch_up_front(&c);
    rl_nds_arrangement a;
    memset(&a, 0, sizeof a);
    float x, y;
    CHECK(find_screens_button(&x, &y));
    rl_touchpad_finger(1, RL_TOUCHPAD_DOWN, x, y);
    poll(&c, &a);
    CHECK(!a.swap);
    rl_touchpad_finger(1, RL_TOUCHPAD_UP, x, y);
    poll(&c, &a);
    CHECK(a.swap);
    usleep(80000);
    poll(&c, &a);
    rl_touchpad_finger(2, RL_TOUCHPAD_DOWN, x, y);
    rl_touchpad_finger(2, RL_TOUCHPAD_UP, x, y);
    poll(&c, &a);
    CHECK(!a.swap);
    CHECK_EQ(rl_nds_screens_config(false).layout, RL_SCREENS_AUTO);
    rl_touchpad_reset();
}

TEST(holding_the_screens_button_cycles_the_layouts_and_saves_the_choice)
{
    static const rl_screens_layout order[] = {RL_SCREENS_SIDE_BY_SIDE, RL_SCREENS_LARGE_TOP,   RL_SCREENS_LARGE_BOTTOM,
                                              RL_SCREENS_TOP_ONLY,     RL_SCREENS_BOTTOM_ONLY, RL_SCREENS_STACKED};
    rl_nds_controls c;
    touch_up_front(&c);
    rl_nds_arrangement a;
    memset(&a, 0, sizeof a);
    float x, y;
    CHECK(find_screens_button(&x, &y));
    for (unsigned i = 0; i < sizeof order / sizeof order[0]; i++) {
        rl_touchpad_finger(1, RL_TOUCHPAD_DOWN, x, y);
        rl_touchpad_tick(rl_clock_ns());
        poll(&c, &a);
        CHECK_EQ(rl_nds_screens_config(false).layout, i == 0 ? RL_SCREENS_AUTO : order[i - 1]);
        usleep(520000);
        rl_touchpad_tick(rl_clock_ns());
        poll(&c, &a);
        CHECK_EQ(rl_nds_screens_config(false).layout, order[i]);
        CHECK(!a.swap);
        rl_touchpad_finger(1, RL_TOUCHPAD_UP, x, y);
        usleep(80000);
        poll(&c, &a);
        CHECK(!a.swap);
        CHECK_EQ(rl_nds_screens_config(false).layout, order[i]);
    }
    rl_touchpad_reset();
}
