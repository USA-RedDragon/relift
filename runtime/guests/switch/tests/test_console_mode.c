/**
 * @file test_console_mode.c
 * @brief The console mode (system.console_mode): the setting picks docked or
 * handheld at start, every service answer that depends on the mode follows it
 * (am, apm, vi), and a switch while the game runs queues
 * OperationModeChanged and PerformanceModeChanged once, from the runtime call
 * or from the input script.
 */
#include "am.h"
#include "services.h"
#include "tc.h"
#include "vi.h"

#include <relift/ports.h>
#include <relift/settings.h>

#include <stdio.h>
#include <stdlib.h>

#define AM_NO_MSG 0x680u

typedef struct env {
    rl_ctx *c;
    uint64_t heap;
    tc_obj csg, apm, apm_sys, vi_sys;
} env;

static uint32_t rc_of(tc_res r) { return r.svc_rc ? r.svc_rc : r.result; }

/** @brief Declares the Switch settings with system.console_mode at mode ("" for the default), then boots. */
static void boot(env *out, const char *mode)
{
    rl_settings_reset();
    unsetenv("RELIFT_CONSOLE_MODE");
    rl_switch_declare_settings();
    if (*mode)
        CHECK_EQ(rl_settings_set("system.console_mode", mode), RL_SETTING_OK);
    env e = { 0 };
    e.c = t_boot(NULL);
    e.heap = t_heap(e.c, 0x200000);
    tc_obj oe = tc_open_service(e.c, e.heap, "appletOE");
    CHECK(oe.handle);
    CHECK_EQ(tc_to_domain(e.c, &oe), 0);
    tc_req q = tc_new(0);
    q.send_pid = true;
    q.ncopy = 1;
    q.copy[0] = 0xFFFF8001u;
    tc_put64(&q, 0);
    tc_obj proxy;
    CHECK_EQ(tc_sub(e.c, oe, q, &proxy), 0);
    CHECK_EQ(tc_sub(e.c, proxy, tc_new(0), &e.csg), 0);
    e.apm = tc_open_service(e.c, e.heap, "apm");
    e.apm_sys = tc_open_service(e.c, e.heap, "apm:sys");
    tc_obj m = tc_open_service(e.c, e.heap, "vi:m"), md;
    tc_req g = tc_new(2);
    tc_put32(&g, 1);
    CHECK_EQ(tc_sub(e.c, m, g, &md), 0);
    CHECK_EQ(tc_sub(e.c, md, tc_new(101), &e.vi_sys), 0);
    *out = e;
}

static uint32_t u32_of(env *e, tc_obj o, uint32_t cmd)
{
    tc_res r = tc_send(e->c, o, tc_new(cmd));
    return rc_of(r) ? 0xDEADBEEFu : tc_u32(&r, 0);
}

static void drain(env *e)
{
    for (;;) {
        tc_res r = tc_send(e->c, e->csg, tc_new(1));
        if (rc_of(r))
            return;
    }
}

/** @brief Checks every answer that depends on the mode. */
static void check_answers(env *e, bool docked)
{
    CHECK_EQ(rl_am_is_docked(), docked);
    tc_res r = tc_send(e->c, e->csg, tc_new(5));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.data[0], docked ? 1 : 0);
    CHECK_EQ(u32_of(e, e->csg, 6), docked ? 1u : 0u);
    r = tc_send(e->c, e->csg, tc_new(60));
    CHECK_EQ(tc_u32(&r, 0), docked ? 1920u : 1280u);
    CHECK_EQ(tc_u32(&r, 4), docked ? 1080u : 720u);
    CHECK_EQ(u32_of(e, e->csg, 91), docked ? 0x00020004u : 0x00020003u);
    CHECK_EQ(u32_of(e, e->apm, 1), docked ? 1u : 0u);
    CHECK_EQ(u32_of(e, e->apm_sys, 7), docked ? 0x00020004u : 0x00020003u);
    tc_req q = tc_new(3200);
    tc_put64(&q, 0);
    r = tc_send(e->c, e->vi_sys, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), docked ? 1920u : 1280u);
    CHECK_EQ(tc_u32(&r, 4), docked ? 1080u : 720u);
}

/** @brief The messages queued since the last drain: OperationModeChanged then PerformanceModeChanged, once. */
static void check_one_change(env *e)
{
    tc_res r = tc_send(e->c, e->csg, tc_new(1));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), RL_AM_MSG_OPERATION_MODE_CHANGED);
    r = tc_send(e->c, e->csg, tc_new(1));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), RL_AM_MSG_PERFORMANCE_MODE_CHANGED);
    CHECK_EQ(rc_of(tc_send(e->c, e->csg, tc_new(1))), AM_NO_MSG);
}

TEST(console_mode_defaults_to_handheld)
{
    env e = { 0 };
    boot(&e, "");
    CHECK(e.vi_sys.handle);
    CHECK_EQ(rl_setting_enum("system.console_mode"), 0u);
    check_answers(&e, false);
    rl_settings_reset();
}

TEST(console_mode_setting_starts_docked)
{
    env e = { 0 };
    boot(&e, "docked");
    CHECK(e.vi_sys.handle);
    check_answers(&e, true);
    rl_settings_reset();
}

TEST(console_mode_environment_variable_starts_docked)
{
    rl_settings_reset();
    setenv("RELIFT_CONSOLE_MODE", "docked", 1);
    rl_switch_declare_settings();
    rl_ctx *c = t_boot(NULL);
    (void)c;
    CHECK(rl_am_is_docked());
    unsetenv("RELIFT_CONSOLE_MODE");
    rl_settings_reset();
}

TEST(console_mode_switch_while_running_queues_both_messages_once)
{
    env e = { 0 };
    boot(&e, "handheld");
    CHECK(e.vi_sys.handle);
    drain(&e);
    rl_am_set_operation_mode(true);
    rl_am_set_operation_mode(true);
    check_answers(&e, true);
    check_one_change(&e);
    rl_am_set_operation_mode(false);
    check_answers(&e, false);
    check_one_change(&e);
    rl_settings_reset();
}

TEST(console_mode_input_script_switches_once)
{
    env e = { 0 };
    boot(&e, "");
    CHECK(e.vi_sys.handle);
    drain(&e);
    CHECK(rl_ports_set_script("2+3:console.docked,8+3:console.handheld"));
    for (uint64_t i = 0; i <= 6; i++)
        vi_script_frame(i);
    check_answers(&e, true);
    check_one_change(&e);
    for (uint64_t i = 7; i <= 12; i++)
        vi_script_frame(i);
    check_answers(&e, false);
    check_one_change(&e);
    rl_ports_script_reset();
    rl_settings_reset();
}

/** @brief Checks the display am and vi:s report: w by h. */
static void check_display(env *e, uint32_t w, uint32_t h)
{
    tc_res r = tc_send(e->c, e->csg, tc_new(60));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), w);
    CHECK_EQ(tc_u32(&r, 4), h);
    tc_req q = tc_new(3200);
    tc_put64(&q, 0);
    r = tc_send(e->c, e->vi_sys, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), w);
    CHECK_EQ(tc_u32(&r, 4), h);
}

TEST(render_resolution_caps_the_display_the_console_reports)
{
    env e = { 0 };
    boot(&e, "docked");
    CHECK(e.vi_sys.handle);
    hos.cfg.program_id = UINT64_C(0x04007ef01f8ae000);
    static const struct {
        uint32_t lines;
        bool docked;
        uint32_t w, h;
    } want[] = {
        { 0, true, 1920, 1080 },    { 1440, true, 1920, 1080 },  { 1080, true, 1920, 1080 },
        { 720, true, 1280, 720 },   { 0, false, 1280, 720 },     { 1080, false, 1280, 720 },
        { 720, false, 1280, 720 },
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        char v[16];
        snprintf(v, sizeof v, "%u", want[i].lines);
        CHECK_EQ(rl_settings_set("video.render_resolution", v), RL_SETTING_OK);
        rl_am_set_operation_mode(want[i].docked);
        check_display(&e, want[i].w, want[i].h);
    }
    /* A game with no list ignores the setting. */
    hos.cfg.program_id = 1;
    CHECK_EQ(rl_settings_set("video.render_resolution", "720"), RL_SETTING_OK);
    rl_am_set_operation_mode(true);
    check_display(&e, 1920, 1080);
    CHECK_EQ(rl_am_render_lines(), 0u);
    rl_settings_reset();
}
