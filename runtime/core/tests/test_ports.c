/**
 * @file test_ports.c
 * @brief Ports, bindings and control values, including an extra port that a
 * game or mod declares and an override reads through relift_input.
 */
#include "runner.h"

#include <relift/override.h>
#include <relift/settings.h>

static const rl_control_decl pad_controls[] = {
    { "a", RL_CONTROL_BUTTON, "gamepad:east,key:x" },
    { "b", RL_CONTROL_BUTTON, NULL },
    { "stick_x", RL_CONTROL_AXIS, "gamepad:left_x" },
    { "zr", RL_CONTROL_TRIGGER, "gamepad:right_trigger" },
};

static const rl_port_decl native[] = {
    { "p1", "Test pad", 4, pad_controls },
    { "p2", "Test pad", 4, pad_controls },
};

static const rl_control_decl dial_controls[] = { { "turn", RL_CONTROL_AXIS, "mouse:left" } };
static const rl_port_decl extra[] = { { "dial", "Rotary dial", 1, dial_controls } };

static rl_input_snapshot blank(void)
{
    rl_input_snapshot s;
    memset(&s, 0, sizeof s);
    s.pads[0].connected = true;
    s.pads[1].connected = true;
    return s;
}

TEST(ports_declare_and_find)
{
    rl_ports_reset();
    CHECK(rl_ports_declare(native, 2));
    CHECK_EQ(rl_ports_count(), 2);
    CHECK(!strcmp(rl_ports_id(1), "p2"));
    CHECK(rl_ports_id(2) == NULL);
    CHECK_EQ(rl_ports_control_count("p1"), 4);
    CHECK(rl_ports_find("p1", "a") != RL_PORT_NONE);
    CHECK_EQ(rl_ports_find("p1", "nope"), RL_PORT_NONE);
    CHECK_EQ(rl_ports_find("p9", "a"), RL_PORT_NONE);
}

TEST(ports_refuse_bad_declarations)
{
    rl_ports_reset();
    CHECK(rl_ports_declare(native, 1));
    CHECK(!rl_ports_declare(native, 1));
    rl_control_decl bad_default[] = { { "x", RL_CONTROL_BUTTON, "key:nope" } };
    rl_port_decl bd = { "q", "k", 1, bad_default };
    CHECK(!rl_ports_declare(&bd, 1));
    rl_port_decl wrong_kind = { "p1", "Other", 1, dial_controls };
    CHECK(!rl_ports_declare(&wrong_kind, 1));
    CHECK_EQ(rl_ports_count(), 1);
    CHECK_EQ(rl_ports_control_count("p1"), 4);
}

TEST(ports_a_port_gains_extra_controls)
{
    rl_ports_reset();
    CHECK(rl_ports_declare(native, 1));
    rl_control_decl more[] = { { "turbo", RL_CONTROL_BUTTON, "key:t" } };
    rl_port_decl p = { "p1", "Test pad", 1, more };
    CHECK(rl_ports_declare(&p, 1));
    CHECK_EQ(rl_ports_control_count("p1"), 5);
    rl_input_snapshot s = blank();
    s.keys[0] |= 1ull << 23;
    CHECK(rl_ports_eval(rl_ports_find("p1", "turbo"), &s) == 1.0f);
}

TEST(ports_default_bindings_resolve_per_port_pad)
{
    rl_ports_reset();
    CHECK(rl_ports_declare(native, 2));
    rl_input_snapshot s = blank();
    s.pads[1].buttons = 1u << RL_BTN_EAST;
    CHECK(rl_ports_eval(rl_ports_find("p1", "a"), &s) == 0.0f);
    CHECK(rl_ports_eval(rl_ports_find("p2", "a"), &s) == 1.0f);
    s.pads[0].lx = 16384;
    CHECK(rl_ports_eval(rl_ports_find("p1", "stick_x"), &s) > 0.49f);
    s.pads[0].lx = -32768;
    CHECK(rl_ports_eval(rl_ports_find("p1", "stick_x"), &s) == -1.0f);
}

TEST(ports_rebinding_and_settings)
{
    rl_ports_reset();
    CHECK(rl_ports_declare(native, 2));
    rl_control_ref b = rl_ports_find("p1", "b");
    rl_input_snapshot s = blank();
    CHECK(rl_ports_bind("p1", "b", "key:z, gamepad2:south"));
    s.keys[0] |= 1ull << 29;
    CHECK(rl_ports_eval(b, &s) == 1.0f);
    s.keys[0] = 0;
    s.pads[1].buttons = 1u << RL_BTN_SOUTH;
    CHECK(rl_ports_eval(b, &s) == 1.0f);
    CHECK(rl_ports_apply_setting("bind.p1.b", "mouse:right"));
    s.pads[1].buttons = 0;
    s.pointer.buttons = 1u << RL_PTR_RIGHT;
    CHECK(rl_ports_eval(b, &s) == 1.0f);
    CHECK(!rl_ports_apply_setting("fullscreen", "true"));
    CHECK(!rl_ports_apply_setting("bind.p1.zz", "key:z"));
    CHECK(!rl_ports_apply_setting("bind.p1.b", "key:bogus"));
    CHECK(rl_ports_apply_setting("bind.p1.b", ""));
    CHECK(rl_ports_eval(b, &s) == 0.0f);
    rl_ports_bind_defaults();
    s.pads[0].buttons = 1u << RL_BTN_EAST;
    CHECK(rl_ports_eval(rl_ports_find("p1", "a"), &s) == 1.0f);
}

TEST(ports_axis_halves_and_triggers)
{
    rl_ports_reset();
    CHECK(rl_ports_declare(native, 1));
    rl_input_snapshot s = blank();
    s.pads[0].lx = -32767;
    CHECK(rl_ports_bind("p1", "a", "gamepad:left_x-"));
    CHECK(rl_ports_eval(rl_ports_find("p1", "a"), &s) == 1.0f);
    CHECK(rl_ports_bind("p1", "stick_x", "gamepad:left_x+"));
    CHECK(rl_ports_eval(rl_ports_find("p1", "stick_x"), &s) == 0.0f);
    CHECK(rl_ports_bind("p1", "stick_x", "key:left-,key:right+"));
    s.keys[1] |= 1ull << (80 % 64);
    CHECK(rl_ports_eval(rl_ports_find("p1", "stick_x"), &s) == -1.0f);
}

static void source(void *user, rl_input_snapshot *out)
{
    (void)user;
    out->pads[0].connected = true;
    out->pointer.buttons = 1u << RL_PTR_LEFT;
}

TEST(ports_an_extra_port_is_readable_through_the_override_api)
{
    rl_ports_reset();
    CHECK(rl_ports_declare(native, 1));
    CHECK(rl_ports_declare(extra, 1));
    CHECK_EQ(rl_ports_count(), 2);
    rl_input_set_source(source, NULL);
    CHECK(relift_input("dial", "turn") == 1.0f);
    CHECK(relift_input("dial", "missing") == 0.0f);
    CHECK(relift_input("p1", "a") == 0.0f);
    CHECK(rl_ports_bind("dial", "turn", "mouse:right"));
    CHECK(relift_input("dial", "turn") == 0.0f);
    rl_input_set_source(NULL, NULL);
}

TEST(ports_controls_are_key_binding_settings)
{
    rl_settings_reset();
    rl_ports_reset();
    CHECK(rl_ports_declare(native, 2));
    CHECK(!strcmp(rl_setting_binding("bind.p1.a"), "gamepad:east,key:x"));
    CHECK_EQ(rl_settings_set("bind.p2.b", "key:q"), RL_SETTING_OK);
    CHECK(!strcmp(rl_setting_binding("bind.p2.b"), "key:q"));
    rl_input_snapshot s = blank();
    s.keys[0] |= 1ull << 20;
    CHECK(rl_ports_eval(rl_ports_find("p2", "b"), &s) == 1.0f);
    CHECK(rl_ports_eval(rl_ports_find("p1", "b"), &s) == 0.0f);
    CHECK_EQ(rl_settings_set("bind.p2.b", "key:nope"), RL_SETTING_INVALID);
    CHECK(rl_ports_eval(rl_ports_find("p2", "b"), &s) == 1.0f);
    char buf[8192];
    rl_settings_schema_json(buf, sizeof buf);
    CHECK(strstr(buf, "\"key\":\"bind.p1.stick_x\",\"type\":\"key_binding\""));
    CHECK(strstr(buf, "\"default\":\"gamepad:east,key:x\""));
}

TEST(ports_a_value_set_before_the_declaration_applies)
{
    rl_settings_reset();
    rl_ports_reset();
    CHECK(rl_ports_declare(extra, 1));
    CHECK_EQ(rl_settings_set("bind.dial.turn", "key:z"), RL_SETTING_OK);
    rl_ports_reset();
    CHECK(rl_ports_declare(extra, 1));
    rl_input_snapshot s = blank();
    s.keys[0] |= 1ull << 29;
    CHECK(rl_ports_eval(rl_ports_find("dial", "turn"), &s) == 1.0f);
}

TEST(ports_extras_may_be_declared_before_the_native_ports)
{
    rl_ports_reset();
    rl_control_decl more[] = { { "turbo", RL_CONTROL_BUTTON, "key:t" } };
    rl_port_decl p = { "p1", "Test pad", 1, more };
    CHECK(rl_ports_declare(&p, 1));
    CHECK(rl_ports_declare(native, 2));
    CHECK_EQ(rl_ports_count(), 2);
    CHECK_EQ(rl_ports_control_count("p1"), 5);
    CHECK(rl_ports_find("p1", "turbo") != RL_PORT_NONE);
    CHECK(rl_ports_find("p1", "a") != RL_PORT_NONE);
}
