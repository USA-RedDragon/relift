/**
 * @file test_touchpad.c
 * @brief The on-screen controller: placement in dp from the view's edges, the
 * touch rules (first shape a finger lands on, d-pad zones and slides, face
 * button slides, sticks, game touch through the gaps), the merge into p1,
 * hiding on a gamepad, the Hide button, the pause sheet, the editor and the
 * saved layout keys. RL_TOUCHPAD_PNG names a folder to write the overlay of
 * each scene to.
 */
#include "runner.h"

#include <relift/ports.h>
#include <relift/settings.h>
#include <relift/touchpad.h>
#include <relift/touchpad_layout.h>

#include "touchpad_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const rl_touchpad_item g_items[] = {
    RL_TP_MENU(1200, 50, 230, 70),
    RL_TP_DPAD("up", "right", "down", "left", 300, 520, 420),
    RL_TP_FACE("A", "a", 2230, 520, 105),
    RL_TP_FACE("B", "b", 2010, 650, 105),
    RL_TP_PILL("Start", "start", 2100, 890, 230, 88),
    RL_TP_STICK("lstick_x", "lstick_y", 1200, 800, 140),
    RL_TP_HIDE(120, 1026, 130, 56),
};

static const rl_touchpad_group g_groups[] = {
    { "dpad", 1, 1 }, { "ab", 2, 2 }, { "start", 4, 1 }, { "stick", 5, 1 }, { "hide", 6, 1 },
};

static const rl_touchpad_guest g_guest = {
    .name = "test",
    .picture_width = 160,
    .picture_height = 144,
    .game_touch = true,
    .portrait = RL_TP_LAYOUT(2400, 1080, g_items, g_groups),
    .landscape = RL_TP_LAYOUT(2400, 1080, g_items, g_groups),
};

static const rl_control_decl g_p1_controls[] = {
    { "a", 0, "key:x" },  { "b", 0, "key:z" },  { "start", 0, NULL }, { "up", 0, NULL },
    { "down", 0, NULL },  { "left", 0, NULL },  { "right", 0, NULL }, { "lstick_x", 2, NULL },
    { "lstick_y", 2, NULL },
};
static const rl_control_decl g_tp_controls[] = {
    { "a", 0, "touch:a" },          { "b", 0, "touch:b" },          { "start", 0, "touch:start" },
    { "up", 0, "touch:up" },        { "down", 0, "touch:down" },    { "left", 0, "touch:left" },
    { "right", 0, "touch:right" },  { "lstick_x", 2, "touch:lstick_x" }, { "lstick_y", 2, "touch:lstick_y" },
};
static const rl_port_decl g_ports[] = {
    { "p1", "pad", 9, g_p1_controls },
    { RL_TOUCHPAD_PORT, "On-screen controller", 9, g_tp_controls },
};

static char g_file[512];

static void up(void)
{
    rl_touchpad_reset();
    rl_settings_reset();
    rl_ports_reset();
    const char *tmp = getenv("TMPDIR");
    snprintf(g_file, sizeof g_file, "%s/rl_touchpad_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    int fd = mkstemp(g_file);
    if (fd >= 0) {
        if (write(fd, "{}", 2) != 2)
            g_file[0] = 0;
        close(fd);
    }
    rl_touchpad_declare(&g_guest);
    char *argv[] = { "x", "--relift-settings", g_file, "--relift-set", "input.touch_pad.show=always", NULL };
    rl_settings_args(5, argv);
    rl_ports_declare(g_ports, 2);
    rl_ports_bind_defaults();
    rl_touchpad_view(1920, 1080, 2.0f, NULL);
}

static void down(void)
{
    if (g_file[0])
        unlink(g_file);
}

static float rd(const char *c)
{
    rl_input_snapshot s;
    memset(&s, 0, sizeof s);
    rl_touchpad_fill(s.touch_pad, RL_INPUT_TOUCH_PAD);
    return rl_ports_eval(rl_ports_find("p1", c), &s);
}

static void png(const char *name)
{
    const char *dir = getenv("RL_TOUCHPAD_PNG");
    if (!dir)
        return;
    uint32_t w, h;
    int32_t d[4];
    const uint8_t *px = rl_touchpad_pixels(&w, &h, d);
    if (!px)
        return;
    char path[600];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t *p = px + i * 4;
        uint8_t bg = 0x7d;
        uint8_t o[3] = { (uint8_t)(p[0] + bg * (255 - p[3]) / 255), (uint8_t)(p[1] + bg * (255 - p[3]) / 255),
                         (uint8_t)(p[2] + bg * (255 - p[3]) / 255) };
        fwrite(o, 1, 3, f);
    }
    fclose(f);
}

static const char *slurp(const char *path)
{
    static char buf[8192];
    FILE *f = fopen(path, "rb");
    if (!f)
        return "";
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

TEST(touchpad_places_groups_in_dp_from_the_edges_and_holds_quick_taps)
{
    up();
    int32_t pic[4];
    rl_touchpad_picture(pic);
    CHECK_EQ(pic[3], 1080);
    CHECK_EQ(pic[2], 1200);
    CHECK_EQ(pic[0], 360);
    rl_touchpad_finger(1, RL_TOUCHPAD_DOWN, 1920 - 2 * 65 / 2.625f * 1.0f - 2 * 105 / 2.625f, 540);
    CHECK(rd("a") == 1.0f);
    rl_touchpad_finger(1, RL_TOUCHPAD_UP, 0, 0);
    CHECK(rd("a") == 1.0f);
    usleep(60000);
    CHECK(rd("a") == 0.0f);
    png("landscape");
    down();
}

TEST(touchpad_merges_into_p1_and_keeps_bindings)
{
    up();
    rl_input_snapshot s;
    memset(&s, 0, sizeof s);
    s.keys[27 / 64] |= 1ull << (27 % 64);
    CHECK(rl_ports_eval(rl_ports_find("p1", "a"), &s) == 1.0f);
    CHECK(rl_ports_bind(RL_TOUCHPAD_PORT, "a", ""));
    CHECK(rl_ports_bind("p1", "a", ""));
    rl_touchpad_finger(1, RL_TOUCHPAD_DOWN, 1820, 540);
    CHECK(rd("a") == 0.0f);
    CHECK(rl_ports_bind(RL_TOUCHPAD_PORT, "a", "touch:a"));
    CHECK(rd("a") == 1.0f);
    down();
}

TEST(touchpad_dpad_zones_slide_and_corners)
{
    up();
    float cx = 2 * 90 / 2.625f + 2 * 210 / 2.625f, cy = 520;
    rl_touchpad_finger(3, RL_TOUCHPAD_DOWN, cx, cy - 100);
    CHECK(rd("up") == 1.0f && rd("left") == 0.0f);
    rl_touchpad_finger(3, RL_TOUCHPAD_MOVE, cx - 100, cy - 100);
    CHECK(rd("up") == 1.0f && rd("left") == 1.0f);
    png("dpad-up-left");
    rl_touchpad_finger(3, RL_TOUCHPAD_MOVE, cx + 100, cy);
    CHECK(rd("right") == 1.0f && rd("up") == 0.0f);
    rl_touchpad_finger(3, RL_TOUCHPAD_MOVE, cx + 5, cy);
    CHECK(rd("right") == 0.0f);
    down();
}

TEST(touchpad_face_buttons_slide_and_a_finger_keeps_its_shape)
{
    up();
    rl_touchpad_finger(1, RL_TOUCHPAD_DOWN, 1820, 540);
    CHECK(rd("a") == 1.0f);
    rl_touchpad_finger(1, RL_TOUCHPAD_MOVE, 1652, 640);
    CHECK(rd("a") == 0.0f && rd("b") == 1.0f);
    rl_touchpad_finger(2, RL_TOUCHPAD_DOWN, 200, 540);
    rl_touchpad_finger(2, RL_TOUCHPAD_MOVE, 1820, 540);
    CHECK(rd("a") == 0.0f);
    down();
}

TEST(touchpad_game_touch_takes_the_gaps)
{
    up();
    rl_touchpad_finger(5, RL_TOUCHPAD_DOWN, 960, 300);
    rl_touchpad_finger(6, RL_TOUCHPAD_DOWN, 1820, 540);
    rl_input_touch t[4];
    CHECK_EQ(rl_touchpad_game_touches(t, 4), 1u);
    CHECK_EQ(t[0].id, 5u);
    CHECK_EQ(t[0].x, 32767);
    rl_touchpad_finger(5, RL_TOUCHPAD_MOVE, 1820, 540);
    CHECK_EQ(rl_touchpad_game_touches(t, 4), 1u);
    CHECK(rd("a") == 1.0f);
    down();
}

TEST(touchpad_stick_dead_zone_and_clamp)
{
    up();
    float cx = 960, cy = 1080 - 2 * 280 / 2.625f;
    rl_touchpad_finger(1, RL_TOUCHPAD_DOWN, cx + 5, cy);
    CHECK(rd("lstick_x") == 0.0f);
    rl_touchpad_finger(1, RL_TOUCHPAD_MOVE, cx + 400, cy);
    CHECK(rd("lstick_x") > 0.99f);
    png("stick");
    down();
}

TEST(touchpad_hides_on_a_gamepad_and_releases_held_controls)
{
    up();
    rl_touchpad_set_touch_screen(true);
    rl_settings_set("input.touch_pad.show", "auto");
    rl_touchpad_finger(1, RL_TOUCHPAD_DOWN, 1820, 540);
    CHECK(rd("a") == 1.0f);
    usleep(1600000);
    rl_touchpad_gamepad(true, "Pad", 1);
    CHECK(rd("a") == 0.0f);
    png("toast-connected");
    CHECK(rl_touchpad_pixels(&(uint32_t){ 0 }, &(uint32_t){ 0 }, (int32_t[4]){ 0 }) != NULL);
    rl_touchpad_finger(1, RL_TOUCHPAD_MOVE, 1821, 540);
    CHECK(rd("a") == 0.0f);
    rl_touchpad_finger(1, RL_TOUCHPAD_UP, 0, 0);
    rl_touchpad_finger(2, RL_TOUCHPAD_DOWN, 1820, 540);
    CHECK(rd("a") == 0.0f);
    rl_touchpad_finger(2, RL_TOUCHPAD_UP, 0, 0);
    rl_touchpad_gamepad(false, "Pad", 0);
    rl_touchpad_finger(3, RL_TOUCHPAD_DOWN, 1820, 540);
    CHECK(rd("a") == 1.0f);
    down();
}

TEST(touchpad_hide_button_and_pause_sheet)
{
    up();
    float hx = 2 * 55 / 2.625f + 2 * 65 / 2.625f, hy = 1080 - 2 * 26 / 2.625f - 2 * 28 / 2.625f;
    rl_touchpad_finger(1, RL_TOUCHPAD_DOWN, hx, hy);
    rl_touchpad_finger(1, RL_TOUCHPAD_UP, hx, hy);
    rl_touchpad_finger(2, RL_TOUCHPAD_DOWN, 1500, 540);
    rl_touchpad_finger(7, RL_TOUCHPAD_DOWN, 1820, 540);
    CHECK(rd("a") == 0.0f);
    rl_touchpad_finger(7, RL_TOUCHPAD_UP, 0, 0);
    rl_input_touch t[4];
    CHECK_EQ(rl_touchpad_game_touches(t, 4), 1u);
    rl_touchpad_finger(2, RL_TOUCHPAD_UP, 0, 0);
    png("hidden");
    rl_touchpad_finger(1, RL_TOUCHPAD_DOWN, hx, hy);
    rl_touchpad_finger(1, RL_TOUCHPAD_UP, hx, hy);
    rl_touchpad_back();
    CHECK(rl_touchpad_paused());
    CHECK(rl_touchpad_hold(1));
    png("pause");
    rl_touchpad_finger(1, RL_TOUCHPAD_DOWN, 960, 540 - 350 * 0.7619f + 190 * 0.7619f);
    rl_touchpad_finger(1, RL_TOUCHPAD_UP, 960, 540 - 350 * 0.7619f + 190 * 0.7619f);
    CHECK(!rl_touchpad_paused());
    CHECK(!rl_touchpad_hold(1));
    down();
}

static int g_host_opens;

static void host_settings(void) { g_host_opens++; }

TEST(touchpad_settings_open_the_host_page_and_its_controller_row_returns_to_it)
{
    up();
    static const rl_touchpad_host host = { .settings = host_settings };
    rl_touchpad_set_host(&host);
    g_host_opens = 0;
    float k = 0.7619f, row0 = 540 - 350 * k + 190 * k;
    rl_touchpad_back();
    rl_touchpad_finger(1, RL_TOUCHPAD_DOWN, 960, row0 + 2 * 130 * k);
    rl_touchpad_finger(1, RL_TOUCHPAD_UP, 960, row0 + 2 * 130 * k);
    CHECK_EQ(g_host_opens, 1);
    CHECK(rl_touchpad_paused());
    rl_touchpad_finger(2, RL_TOUCHPAD_DOWN, 960, row0);
    rl_touchpad_finger(2, RL_TOUCHPAD_UP, 960, row0);
    rl_touchpad_back();
    CHECK(rl_touchpad_paused());
    CHECK_EQ(g_host_opens, 1);
    rl_touchpad_host_settings_done(RL_TOUCHPAD_HOST_CONTROLLER);
    CHECK(rl_touchpad_paused());
    rl_touchpad_back();
    CHECK_EQ(g_host_opens, 2);
    rl_touchpad_host_settings_done(RL_TOUCHPAD_HOST_PAUSE);
    CHECK(rl_touchpad_paused());
    rl_touchpad_finger(3, RL_TOUCHPAD_DOWN, 960, row0 + 2 * 130 * k);
    rl_touchpad_finger(3, RL_TOUCHPAD_UP, 960, row0 + 2 * 130 * k);
    CHECK_EQ(g_host_opens, 3);
    rl_touchpad_host_settings_done(RL_TOUCHPAD_HOST_RESUME);
    CHECK(!rl_touchpad_paused());
    rl_touchpad_set_host(NULL);
    down();
}

TEST(touchpad_editor_saves_groups_and_detached_buttons)
{
    up();
    rl_touchpad_back();
    float k = 0.7619f, y = 540 - 350 * k + 190 * k + 130 * k;
    rl_touchpad_finger(1, RL_TOUCHPAD_DOWN, 960, y);
    rl_touchpad_finger(1, RL_TOUCHPAD_UP, 960, y);
    CHECK(rl_touchpad_paused());
    float cx = 2 * 90 / 2.625f + 2 * 210 / 2.625f, cy = 520;
    rl_touchpad_finger(2, RL_TOUCHPAD_DOWN, cx, cy);
    rl_touchpad_finger(2, RL_TOUCHPAD_MOVE, cx + 100, cy + 50);
    rl_touchpad_finger(2, RL_TOUCHPAD_UP, cx + 100, cy + 50);
    rl_touchpad_finger(3, RL_TOUCHPAD_DOWN, 1820, 540);
    rl_touchpad_tick(rl_clock_ns() + 600000000ull);
    rl_touchpad_finger(3, RL_TOUCHPAD_MOVE, 1700, 300);
    rl_touchpad_finger(3, RL_TOUCHPAD_UP, 1700, 300);
    png("editor");
    float bw = rl_tp_text_width("Drag to move. Drag the dot to resize. Hold a button to detach it.", 30 * k, false);
    float x1 = 960 + (bw + 40 * k + 300 * k + 60 * k) / 2;
    rl_touchpad_finger(4, RL_TOUCHPAD_DOWN, x1 - 95 * k, 70 * k);
    rl_touchpad_finger(4, RL_TOUCHPAD_UP, x1 - 95 * k, 70 * k);
    CHECK(!rl_touchpad_paused());
    const char *t = slurp(g_file);
    CHECK(strstr(t, "\"input.touch_pad.layout.test.landscape.dpad\": \"") != NULL);
    CHECK(strstr(t, "\"input.touch_pad.layout.test.landscape.control.a\": \"0.8") != NULL);
    CHECK(strstr(t, "layout.test.landscape.ab") == NULL);
    rl_touchpad_finger(5, RL_TOUCHPAD_DOWN, 1700, 300);
    CHECK(rd("a") == 1.0f);
    rl_touchpad_finger(5, RL_TOUCHPAD_UP, 1700, 300);
    usleep(60000);
    rl_touchpad_finger(5, RL_TOUCHPAD_DOWN, 1820, 540);
    CHECK(rd("a") == 0.0f);
    down();
}
