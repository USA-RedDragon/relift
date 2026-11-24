/**
 * @file test_srv_am.c
 * @brief The applet manager services driven through IPC the way libnx's
 * appletInitialize and a typical game use them: appletOE, appletAE, the
 * proxies and their objects, the message queue and the events.
 */
#include "am.h"
#include "services.h"
#include "srv_link.h"
#include "tc.h"

#include <relift/settings.h>

#define CUR_PROCESS_HANDLE 0xFFFF8001u
#define AM_NO_MSG 0x680u
#define AM_NOT_AVAIL 0x480u
#define AM_INVALID_PARAMS (128u | (506u << 9))
#define AM_UNBALANCED (128u | (512u << 9))
#define AM_NOT_IMPL (128u | (998u << 9))
#define AM_STUB (128u | (999u << 9))
#define AM_NOT_ACQUIRED (128u | (504u << 9))
#define AM_OBJ_INVALID (128u | (500u << 9))
#define AM_OOB (128u | (503u << 9))
#define AM_TERMINATED (128u | (22u << 9))

typedef struct env {
    rl_ctx *c;
    uint64_t heap;
    tc_obj svc, proxy, csg, sc, wc, ac, dc, lc, af, dbg;
    uint64_t aruid;
    uint32_t msg_event;
    bool ok;
} env;

static tc_req proxy_req(uint32_t cmd)
{
    tc_req q = tc_new(cmd);
    q.send_pid = true;
    q.ncopy = 1;
    q.copy[0] = CUR_PROCESS_HANDLE;
    tc_put64(&q, 0);
    return q;
}

static uint32_t get(env *e, tc_obj parent, uint32_t cmd, tc_obj *out) { return tc_sub(e->c, parent, tc_new(cmd), out); }

static uint32_t rc_of(tc_res r) { return r.svc_rc ? r.svc_rc : r.result; }

static tc_res call_data(env *e, tc_obj o, uint32_t cmd, const void *data, uint32_t n)
{
    tc_req q = tc_new(cmd);
    if (n)
        tc_put(&q, data, n);
    return tc_send(e->c, o, q);
}

static uint32_t get_event(env *e, tc_obj o, uint32_t cmd)
{
    tc_res r = tc_send(e->c, o, tc_new(cmd));
    if (rc_of(r) || r.ncopy != 1)
        return 0;
    return r.copy[0];
}

static uint32_t wait_h(env *e, uint32_t h, uint64_t ns)
{
    uint32_t *p = (uint32_t *)(void *)(t_mem() + e->heap + 0x800);
    *p = h;
    return t_svc(e->c, SVC_WAIT_SYNCHRONIZATION, 0, e->heap + 0x800, 1, ns, 0, 0);
}

static bool signaled(env *e, uint32_t h) { return wait_h(e, h, 0) == 0; }

static uint32_t recv_msg(env *e, tc_obj csg, uint32_t *msg)
{
    tc_res r = tc_send(e->c, csg, tc_new(1));
    uint32_t rc = rc_of(r);
    if (rc == 0)
        *msg = tc_u32(&r, 0);
    return rc;
}

static uint8_t u8_out(env *e, tc_obj o, uint32_t cmd)
{
    tc_res r = tc_send(e->c, o, tc_new(cmd));
    return rc_of(r) ? 0xEE : r.data[0];
}

static uint32_t u32_out(env *e, tc_obj o, uint32_t cmd)
{
    tc_res r = tc_send(e->c, o, tc_new(cmd));
    return rc_of(r) ? 0xDEADBEEFu : tc_u32(&r, 0);
}

static uint32_t set8(env *e, tc_obj o, uint32_t cmd, uint8_t v) { return rc_of(call_data(e, o, cmd, &v, 1)); }
static uint32_t set32(env *e, tc_obj o, uint32_t cmd, uint32_t v) { return rc_of(call_data(e, o, cmd, &v, 4)); }

static void env_open(env *e, const char *name, uint32_t open_cmd)
{
    memset(e, 0, sizeof *e);
    e->c = t_boot(NULL);
    e->heap = t_heap(e->c, 0x200000);
    e->svc = tc_open_service(e->c, e->heap, name);
    if (!e->svc.handle || tc_to_domain(e->c, &e->svc) != 0)
        return;
    if (tc_sub(e->c, e->svc, proxy_req(open_cmd), &e->proxy) != 0)
        return;
    e->ok = true;
}

static void libnx_init_application(env *e)
{
    e->ok = false;
    env_open(e, "appletOE", 0);
    if (!e->ok)
        return;
    e->ok = false;
    tc_obj *objs[] = { &e->af, &e->lc, &e->csg, &e->sc, &e->wc, &e->ac, &e->dc, &e->dbg };
    uint32_t cmds[] = { 20, 11, 0, 1, 2, 3, 4, 1000 };
    for (uint32_t i = 0; i < 8; i++) {
        if (get(e, e->proxy, cmds[i], objs[i]) != 0)
            return;
        if (cmds[i] == 2) {
            tc_res r = tc_send(e->c, e->wc, tc_new(1));
            if (rc_of(r))
                return;
            e->aruid = tc_u64(&r, 0);
        }
    }
    e->ok = true;
}

TEST(am_sm_get_service)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK_EQ(tc_open_service_result(c, heap, "appletOE"), 0);
    CHECK_EQ(tc_open_service_result(c, heap, "appletAE"), 0);
    CHECK(tc_open_service_result(c, heap, "appletXX") != 0);
}

TEST(am_libnx_initialize_application)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    CHECK(e.aruid != 0);

    tc_res r = tc_send(e.c, e.sc, tc_new(91));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.ncopy, 1);
    uint32_t tick_event = r.copy[0];
    CHECK(signaled(&e, tick_event));
    r = tc_send(e.c, e.sc, tc_new(90));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 0);

    e.msg_event = get_event(&e, e.csg, 0);
    CHECK(e.msg_event != 0);
    CHECK(signaled(&e, e.msg_event));

    CHECK_EQ(u8_out(&e, e.csg, 9), RL_AM_FOCUS_IN);

    CHECK_EQ(rc_of(tc_send(e.c, e.wc, tc_new(10))), 0);
    uint8_t flags[3] = { 1, 0, 0 };
    CHECK_EQ(rc_of(call_data(&e, e.sc, 13, flags, 3)), 0);

    r = tc_send(e.c, e.af, tc_new(40));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.data[0], 1);

    CHECK_EQ(u8_out(&e, e.csg, 5), 0);
    CHECK_EQ(u32_out(&e, e.csg, 6), 0);
    CHECK_EQ(set8(&e, e.sc, 11, 1), 0);
    CHECK_EQ(set8(&e, e.sc, 12, 1), 0);

    uint32_t msg = 0;
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, RL_AM_MSG_FOCUS_STATE_CHANGED);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, RL_AM_MSG_CHANGE_INTO_FOREGROUND);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), AM_NO_MSG);
    CHECK(!signaled(&e, e.msg_event));
}

TEST(am_libnx_initialize_application_not_in_focus)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    uint32_t ev = get_event(&e, e.csg, 0);
    CHECK(ev != 0);
    uint32_t msg;
    while (recv_msg(&e, e.csg, &msg) == 0)
        ;
    rl_am_set_focus(false);
    CHECK_EQ(u8_out(&e, e.csg, 9), RL_AM_FOCUS_OUT);

    uint32_t iterations = 0;
    uint8_t focus = u8_out(&e, e.csg, 9);
    while (focus != RL_AM_FOCUS_IN && iterations < 8) {
        iterations++;
        if (iterations == 2)
            rl_am_set_focus(true);
        uint32_t rc = wait_h(&e, ev, iterations == 1 ? 0 : (uint64_t)-1);
        if (iterations == 1) {
            CHECK_EQ(rc, 0);
            CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
            CHECK_EQ(msg, RL_AM_MSG_FOCUS_STATE_CHANGED);
            CHECK_EQ(recv_msg(&e, e.csg, &msg), AM_NO_MSG);
            focus = u8_out(&e, e.csg, 9);
            continue;
        }
        CHECK_EQ(rc, 0);
        while (recv_msg(&e, e.csg, &msg) == 0)
            if (msg == RL_AM_MSG_FOCUS_STATE_CHANGED)
                focus = u8_out(&e, e.csg, 9);
    }
    CHECK_EQ(focus, RL_AM_FOCUS_IN);
    CHECK_EQ(iterations, 2);
}

TEST(am_libnx_initialize_system_applet)
{
    env e;
    env_open(&e, "appletAE", 100);
    CHECK(e.ok);
    tc_obj common, home, global, creator, lc, csg, sc, wc, ac, dc, dbg;
    CHECK_EQ(get(&e, e.proxy, 23, &common), 0);
    CHECK_EQ(get(&e, e.proxy, 20, &home), 0);
    CHECK_EQ(get(&e, e.proxy, 21, &global), 0);
    CHECK_EQ(get(&e, e.proxy, 22, &creator), 0);
    CHECK_EQ(get(&e, e.proxy, 11, &lc), 0);
    CHECK_EQ(get(&e, e.proxy, 0, &csg), 0);
    CHECK_EQ(get(&e, e.proxy, 1, &sc), 0);
    CHECK_EQ(get(&e, e.proxy, 2, &wc), 0);
    tc_res r = tc_send(e.c, wc, tc_new(1));
    CHECK_EQ(rc_of(r), 0);
    CHECK(tc_u64(&r, 0) != 0);
    CHECK_EQ(get(&e, e.proxy, 3, &ac), 0);
    CHECK_EQ(get(&e, e.proxy, 4, &dc), 0);
    CHECK_EQ(get(&e, e.proxy, 1000, &dbg), 0);
    CHECK(get_event(&e, home, 21) != 0);
    CHECK_EQ(rc_of(tc_send(e.c, home, tc_new(10))), 0);
    CHECK(get_event(&e, csg, 0) != 0);
    CHECK_EQ(u8_out(&e, csg, 5), 0);
    CHECK_EQ(u32_out(&e, csg, 6), 0);
    CHECK_EQ(u8_out(&e, csg, 9), RL_AM_FOCUS_IN);
    CHECK_EQ(set8(&e, sc, 11, 1), 0);
    CHECK_EQ(set8(&e, sc, 12, 1), 0);
    CHECK_EQ(tc_cmd(e.c, global, 0), SF_RESULT_UNKNOWN_COMMAND);
    CHECK_EQ(tc_cmd(e.c, creator, 0), SF_RESULT_UNKNOWN_COMMAND);
    CHECK_EQ(tc_cmd(e.c, common, 0), SF_RESULT_UNKNOWN_COMMAND);
    CHECK_EQ(tc_cmd(e.c, dbg, 0), SF_RESULT_UNKNOWN_COMMAND);
    CHECK_EQ(tc_cmd(e.c, e.proxy, 77), SF_RESULT_UNKNOWN_COMMAND);
    (void)lc;
    (void)ac;
    (void)dc;
}

TEST(am_applet_ae_open_commands)
{
    env e;
    env_open(&e, "appletAE", 100);
    CHECK(e.ok);
    tc_obj p;
    CHECK_EQ(tc_sub(e.c, e.svc, proxy_req(200), &p), 0);
    tc_obj winding, accessor, common;
    CHECK_EQ(get(&e, p, 10, &winding), 0);
    tc_res r = tc_send(e.c, winding, tc_new(0));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
    CHECK_EQ(get(&e, p, 20, &accessor), 0);
    CHECK_EQ(get(&e, p, 21, &common), 0);
    CHECK_EQ(tc_cmd(e.c, accessor, 0), AM_NOT_AVAIL);
    r = tc_send(e.c, accessor, tc_new(14));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 3);
    CHECK_EQ(tc_u64(&r, 8), 0x0100000000001000ull);

    tc_req q = proxy_req(201);
    for (int i = 0; i < 4; i++)
        tc_put64(&q, 0);
    CHECK_EQ(tc_sub(e.c, e.svc, q, &p), 0);
    CHECK_EQ(get(&e, p, 0, &common), 0);
    CHECK_EQ(tc_sub(e.c, e.svc, proxy_req(350), &p), 0);
    CHECK_EQ(get(&e, p, 20, &common), 0);
    CHECK_EQ(tc_cmd(e.c, e.svc, 0), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(am_application_proxy_wrong_commands)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    tc_obj o;
    CHECK_EQ(get(&e, e.proxy, 10, &o), SF_RESULT_UNKNOWN_COMMAND);
    CHECK_EQ(get(&e, e.proxy, 21, &o), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(am_aruid_stable_per_pid)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    tc_obj wc2;
    CHECK_EQ(get(&e, e.proxy, 2, &wc2), 0);
    tc_res r = tc_send(e.c, wc2, tc_new(1));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), e.aruid);
    CHECK_EQ(rc_of(tc_send(e.c, wc2, tc_new(11))), 0);
    CHECK_EQ(rc_of(tc_send(e.c, wc2, tc_new(12))), 0);
}

TEST(am_message_queue_and_event)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    uint32_t ev = get_event(&e, e.csg, 0);
    uint32_t ev2 = get_event(&e, e.csg, 0);
    CHECK(ev != 0 && ev2 != 0 && ev != ev2);
    uint32_t msg;
    CHECK(signaled(&e, ev));
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, 15);
    CHECK(signaled(&e, ev));
    CHECK(signaled(&e, ev2));
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, 1);
    CHECK(!signaled(&e, ev));
    CHECK(!signaled(&e, ev2));
    CHECK_EQ(recv_msg(&e, e.csg, &msg), AM_NO_MSG);

    rl_am_push_message(RL_AM_MSG_RESUME);
    rl_am_push_message(RL_AM_MSG_APPLICATION_EXITED);
    rl_am_push_message(77);
    CHECK(signaled(&e, ev));
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, 16);
    CHECK(signaled(&e, ev));
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, 6);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, 77);
    CHECK(!signaled(&e, ev));
    CHECK_EQ(recv_msg(&e, e.csg, &msg), AM_NO_MSG);

    CHECK_EQ(rc_of(tc_send(e.c, e.sc, tc_new(1))), 0);
    CHECK(rl_am_request_exit());
    CHECK(signaled(&e, ev));
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, RL_AM_MSG_EXIT);

    for (uint32_t i = 0; i < 100; i++)
        rl_am_push_message(1000 + i);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, 1036);
}

TEST(am_focus_messages)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    uint32_t msg;
    while (recv_msg(&e, e.csg, &msg) == 0)
        ;
    rl_am_set_focus(false);
    CHECK_EQ(u8_out(&e, e.csg, 9), RL_AM_FOCUS_OUT);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, RL_AM_MSG_FOCUS_STATE_CHANGED);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), AM_NO_MSG);
    rl_am_set_focus(true);
    CHECK_EQ(u8_out(&e, e.csg, 9), RL_AM_FOCUS_IN);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, RL_AM_MSG_FOCUS_STATE_CHANGED);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, RL_AM_MSG_CHANGE_INTO_FOREGROUND);
}

TEST(am_operation_mode)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    uint32_t ev = get_event(&e, e.csg, 61);
    CHECK(ev != 0);
    CHECK(!signaled(&e, ev));
    tc_res r = tc_send(e.c, e.csg, tc_new(60));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 1280);
    CHECK_EQ(tc_u32(&r, 4), 720);
    CHECK_EQ(u32_out(&e, e.csg, 91), 0x00020003);
    uint32_t msg;
    while (recv_msg(&e, e.csg, &msg) == 0)
        ;

    rl_am_set_operation_mode(false);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), AM_NO_MSG);
    CHECK(!signaled(&e, ev));

    rl_am_set_operation_mode(true);
    CHECK(rl_am_is_docked());
    CHECK_EQ(u8_out(&e, e.csg, 5), 1);
    CHECK_EQ(u32_out(&e, e.csg, 6), 1);
    CHECK_EQ(u32_out(&e, e.csg, 91), 0x00020004);
    CHECK(signaled(&e, ev));
    r = tc_send(e.c, e.csg, tc_new(60));
    CHECK_EQ(tc_u32(&r, 0), 1920);
    CHECK_EQ(tc_u32(&r, 4), 1080);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, RL_AM_MSG_OPERATION_MODE_CHANGED);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, RL_AM_MSG_PERFORMANCE_MODE_CHANGED);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), AM_NO_MSG);

    rl_am_set_operation_mode(false);
    CHECK_EQ(u8_out(&e, e.csg, 5), 0);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, RL_AM_MSG_OPERATION_MODE_CHANGED);
}

TEST(am_common_state_getter_commands)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    CHECK_EQ(u8_out(&e, e.csg, 8), 0);
    CHECK_EQ(rc_of(tc_send(e.c, e.csg, tc_new(10))), 0);
    uint32_t sleep_ev = get_event(&e, e.csg, 13);
    CHECK(sleep_ev != 0);
    CHECK(!signaled(&e, sleep_ev));
    CHECK_EQ(u8_out(&e, e.csg, 50), 0);
    CHECK_EQ(set8(&e, e.csg, 51, 1), 0);
    CHECK_EQ(u8_out(&e, e.csg, 50), 1);
    CHECK_EQ(rc_of(tc_send(e.c, e.csg, tc_new(54))), 0);
    CHECK_EQ(u8_out(&e, e.csg, 50), 0);
    CHECK_EQ(rc_of(tc_send(e.c, e.csg, tc_new(53))), 0);
    CHECK_EQ(u8_out(&e, e.csg, 50), 1);
    CHECK_EQ(set8(&e, e.csg, 52, 1), 0);
    CHECK_EQ(u32_out(&e, e.csg, 62), 0);
    CHECK_EQ(set32(&e, e.csg, 66, 0), 0);
    CHECK_EQ(set32(&e, e.csg, 66, 1), 0);
    CHECK_EQ(set32(&e, e.csg, 66, 2), AM_INVALID_PARAMS);
    CHECK_EQ(u8_out(&e, e.csg, 300), 1);
    CHECK_EQ(rc_of(tc_send(e.c, e.csg, tc_new(900))), 0);
    CHECK_EQ(tc_cmd(e.c, e.csg, 7), SF_RESULT_UNKNOWN_COMMAND);
}

static uint32_t g_exit_codes[4];
static uint32_t g_exits;

static void record_exit(uint32_t code)
{
    if (g_exits < 4)
        g_exit_codes[g_exits] = code;
    g_exits++;
}

TEST(am_self_controller_exit_ends_process)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    g_exits = 0;
    rl_set_exit_hook(record_exit);
    CHECK_EQ(rc_of(tc_send(e.c, e.sc, tc_new(0))), 0);
    rl_set_exit_hook(NULL);
    CHECK_EQ(g_exits, 1u);
    CHECK_EQ(g_exit_codes[0], 0u);
}

TEST(am_a_close_ends_a_program_that_never_opted_in_at_once)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    uint32_t msg;
    while (recv_msg(&e, e.csg, &msg) == 0)
        ;
    g_exits = 0;
    rl_set_exit_hook(record_exit);
    rl_am_request_exit();
    rl_set_exit_hook(NULL);
    CHECK_EQ(g_exits, 1u);
    CHECK_EQ(g_exit_codes[0], 0u);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), AM_NO_MSG);
}

TEST(am_a_close_asks_a_program_in_an_exit_handling_section_and_waits)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    uint32_t msg;
    while (recv_msg(&e, e.csg, &msg) == 0)
        ;
    g_exits = 0;
    rl_set_exit_hook(record_exit);
    CHECK_EQ(rc_of(tc_send(e.c, e.sc, tc_new(1))), 0);
    CHECK(rl_am_request_exit());
    CHECK_EQ(g_exits, 0u);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, RL_AM_MSG_EXIT);
    CHECK_EQ(g_exits, 0u);
    CHECK_EQ(rc_of(tc_send(e.c, e.sc, tc_new(2))), 0);
    rl_set_exit_hook(NULL);
    CHECK_EQ(g_exits, 1u);
    CHECK_EQ(g_exit_codes[0], 0u);
}

TEST(am_locking_exit_after_a_close_ends_the_program)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    g_exits = 0;
    rl_set_exit_hook(record_exit);
    CHECK_EQ(rc_of(tc_send(e.c, e.sc, tc_new(1))), 0);
    CHECK(rl_am_request_exit());
    CHECK_EQ(rc_of(tc_send(e.c, e.sc, tc_new(1))), 0);
    rl_set_exit_hook(NULL);
    CHECK_EQ(g_exits, 1u);
}

TEST(am_self_controller_commands)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    tc_obj sc = e.sc;
    CHECK_EQ(rc_of(tc_send(e.c, sc, tc_new(1))), 0);
    CHECK_EQ(rc_of(tc_send(e.c, sc, tc_new(2))), 0);

    CHECK_EQ(tc_cmd(e.c, sc, 4), AM_UNBALANCED);
    CHECK_EQ(rc_of(tc_send(e.c, sc, tc_new(3))), 0);
    CHECK_EQ(rc_of(tc_send(e.c, sc, tc_new(3))), 0);
    CHECK_EQ(rc_of(tc_send(e.c, sc, tc_new(4))), 0);
    CHECK_EQ(rc_of(tc_send(e.c, sc, tc_new(4))), 0);
    CHECK_EQ(tc_cmd(e.c, sc, 4), AM_UNBALANCED);

    uint32_t launchable = get_event(&e, sc, 9);
    CHECK(launchable != 0);
    CHECK(signaled(&e, launchable));

    CHECK_EQ(set8(&e, sc, 10, 1), 0);
    CHECK_EQ(set8(&e, sc, 14, 1), 0);
    CHECK_EQ(set8(&e, sc, 16, 1), 0);
    CHECK_EQ(set32(&e, sc, 19, 1), 0);
    CHECK_EQ(set8(&e, sc, 50, 1), 0);
    CHECK_EQ(set8(&e, sc, 100, 1), 0);
    CHECK_EQ(set8(&e, sc, 130, 1), 0);
    CHECK_EQ(call_data(&e, sc, 10, NULL, 0).svc_rc, 0);
    CHECK_EQ(rc_of(tc_send(e.c, sc, tc_new(65))), 0);

    CHECK_EQ(u8_out(&e, sc, 69), 0);
    CHECK_EQ(set8(&e, sc, 68, 1), 0);
    CHECK_EQ(u8_out(&e, sc, 69), 1);
    CHECK_EQ(u8_out(&e, sc, 67), 1);

    CHECK_EQ(u32_out(&e, sc, 63), 0);
    CHECK_EQ(set32(&e, sc, 62, 5), 0);
    CHECK_EQ(u32_out(&e, sc, 63), 5);

    tc_res r = tc_send(e.c, sc, tc_new(71));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.data[0], 1);
    float lux;
    memcpy(&lux, r.data + 4, 4);
    CHECK(lux == 10000.0f);

    CHECK_EQ(set32(&e, sc, 80, 0), 0);
    CHECK_EQ(set32(&e, sc, 80, 2), 0);
    CHECK_EQ(set32(&e, sc, 80, 3), AM_INVALID_PARAMS);
    CHECK_EQ(set32(&e, sc, 120, 3), 0);
    CHECK_EQ(set32(&e, sc, 120, 4), AM_INVALID_PARAMS);
    CHECK_EQ(tc_cmd(e.c, sc, 41), AM_NOT_IMPL);

    uint32_t tick1 = get_event(&e, sc, 91);
    CHECK(tick1 != 0);
    CHECK(signaled(&e, tick1));
}

static rl_result layer_hook(uint64_t *id)
{
    static uint64_t next = 100;
    *id = next++;
    return KR_SUCCESS;
}

TEST(am_managed_display_layer)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    rl_srv_vi_layer_hook = NULL;
    tc_res r = tc_send(e.c, e.sc, tc_new(40));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 1);
    r = tc_send(e.c, e.sc, tc_new(40));
    CHECK_EQ(tc_u64(&r, 0), 1);
    r = tc_send(e.c, e.sc, tc_new(44));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 1);
    CHECK_EQ(tc_u64(&r, 8), 2);

    rl_srv_vi_layer_hook = layer_hook;
    r = tc_send(e.c, e.sc, tc_new(40));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 100);
    r = tc_send(e.c, e.sc, tc_new(44));
    CHECK_EQ(tc_u64(&r, 0), 101);
    CHECK_EQ(tc_u64(&r, 8), 102);
    rl_srv_vi_layer_hook = NULL;
}

TEST(am_audio_controller)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    tc_res r = tc_send(e.c, e.ac, tc_new(1));
    CHECK_EQ(rc_of(r), 0);
    float v;
    memcpy(&v, r.data, 4);
    CHECK(v == 1.0f);
    float two[2] = { 0.5f, 0.25f };
    CHECK_EQ(rc_of(call_data(&e, e.ac, 0, two, 8)), 0);
    r = tc_send(e.c, e.ac, tc_new(1));
    memcpy(&v, r.data, 4);
    CHECK(v == 0.5f);
    r = tc_send(e.c, e.ac, tc_new(2));
    memcpy(&v, r.data, 4);
    CHECK(v == 0.25f);
    struct {
        float volume;
        uint32_t pad;
        uint64_t ns;
    } ch = { 0.75f, 0, 1000 };
    CHECK_EQ(rc_of(call_data(&e, e.ac, 3, &ch, sizeof ch)), 0);
    r = tc_send(e.c, e.ac, tc_new(1));
    memcpy(&v, r.data, 4);
    CHECK(v == 0.75f);
    float rate = 0.5f;
    CHECK_EQ(rc_of(call_data(&e, e.ac, 4, &rate, 4)), 0);
}

TEST(am_display_controller)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    struct {
        uint8_t flag;
        uint8_t pad[3];
        int32_t stack;
    } shot = { 1, { 0 }, 0 };
    CHECK_EQ(rc_of(call_data(&e, e.dc, 8, &shot, sizeof shot)), 0);
    CHECK_EQ(tc_cmd(e.c, e.dc, 11), AM_NOT_ACQUIRED);
    CHECK_EQ(tc_cmd(e.c, e.dc, 15), AM_NOT_ACQUIRED);
    CHECK_EQ(tc_cmd(e.c, e.dc, 16), (128u | (2u << 9)));
}

TEST(am_application_functions_basic)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    tc_obj f = e.af;

    tc_res r = tc_send(e.c, f, tc_new(21));
    CHECK_EQ(rc_of(r), 0);
    char code[9] = { 0 };
    memcpy(code, r.data, 8);
    CHECK(strcmp(code, "en-US") == 0);
    CHECK_EQ(tc_u64(&r, 0), RL_AM_LANGUAGE_CODE_EN_US);
    rl_am_set_desired_language(0x4A4A);
    r = tc_send(e.c, f, tc_new(21));
    CHECK_EQ(tc_u64(&r, 0), 0x4A4A);

    uint64_t uid[2] = { 1, 0 };
    r = call_data(&e, f, 20, uid, 16);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 0);

    CHECK_EQ(set32(&e, f, 22, 0), 0);
    r = tc_send(e.c, f, tc_new(23));
    CHECK_EQ(rc_of(r), 0);
    for (int i = 0; i < 16; i++)
        CHECK_EQ(r.data[i], 0);
    /* A display version from the program's control data comes back as its 16-byte field: the text, then zeros. */
    rl_am_set_display_version("2.7.31");
    r = tc_send(e.c, f, tc_new(23));
    CHECK_EQ(rc_of(r), 0);
    CHECK(memcmp(r.data, "2.7.31\0\0\0\0\0\0\0\0\0\0", 16) == 0);
    /* A field longer than the 15 characters the console's field holds keeps its first 15 and a terminator. */
    rl_am_set_display_version("0123456789abcdefgh");
    r = tc_send(e.c, f, tc_new(23));
    CHECK(memcmp(r.data, "0123456789abcde\0", 16) == 0);
    rl_am_set_display_version(NULL);
    r = tc_send(e.c, f, tc_new(23));
    for (int i = 0; i < 16; i++)
        CHECK_EQ(r.data[i], 0);

    struct {
        uint64_t type;
        uint64_t uid[2];
    } sz = { 1, { 1, 0 } };
    r = call_data(&e, f, 26, &sz, sizeof sz);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 200000000);
    CHECK_EQ(tc_u64(&r, 8), 200000000);
    struct {
        uint64_t type;
        uint64_t uid[2];
        int64_t size;
        int64_t journal;
    } ext = { 1, { 1, 0 }, 300000000, 400000000 };
    r = call_data(&e, f, 25, &ext, sizeof ext);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 0);
    r = call_data(&e, f, 26, &sz, sizeof sz);
    CHECK_EQ(tc_u64(&r, 0), 300000000);
    CHECK_EQ(tc_u64(&r, 8), 400000000);
    r = tc_send(e.c, f, tc_new(28));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 300000000);
    CHECK_EQ(tc_u64(&r, 8), 400000000);

    struct {
        uint16_t index;
        uint16_t pad[3];
        int64_t size;
        int64_t journal;
    } cache = { 0, { 0 }, 1000, 1000 };
    r = call_data(&e, f, 27, &cache, sizeof cache);
    CHECK_EQ(rc_of(r), 0);

    CHECK_EQ(rc_of(tc_send(e.c, f, tc_new(30))), 0);
    CHECK_EQ(rc_of(tc_send(e.c, f, tc_new(31))), 0);
    uint64_t ns = 1000;
    CHECK_EQ(rc_of(call_data(&e, f, 32, &ns, 8)), 0);
    CHECK_EQ(rc_of(tc_send(e.c, f, tc_new(33))), 0);

    r = tc_send(e.c, f, tc_new(40));
    CHECK_EQ(r.data[0], 1);
    r = tc_send(e.c, f, tc_new(50));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 0);
    CHECK_EQ(tc_u64(&r, 8), 0);
    CHECK_EQ(set8(&e, f, 60, 1), 0);
    CHECK_EQ(u8_out(&e, f, 65), 0);
    CHECK_EQ(set32(&e, f, 67, 1), 0);
    CHECK_EQ(u8_out(&e, f, 65), 1);
    CHECK_EQ(set32(&e, f, 67, 0), 0);
    CHECK_EQ(u8_out(&e, f, 65), 0);
    tc_req q = tc_new(66);
    q.ncopy = 1;
    q.copy[0] = CUR_PROCESS_HANDLE;
    tc_put64(&q, 0);
    CHECK_EQ(rc_of(tc_send(e.c, f, q)), 0);
    CHECK_EQ(set8(&e, f, 90, 1), 0);

    struct {
        int32_t w, h;
        uint64_t size;
    } fb = { 1280, 720, 0x400000 };
    CHECK_EQ(rc_of(call_data(&e, f, 100, &fb, sizeof fb)), 0);
    fb.size = 0x1000;
    CHECK_EQ(rc_of(call_data(&e, f, 100, &fb, sizeof fb)), AM_INVALID_PARAMS);
    fb.size = 0x400000;
    fb.w = 1281;
    CHECK_EQ(rc_of(call_data(&e, f, 100, &fb, sizeof fb)), AM_INVALID_PARAMS);

    struct {
        int32_t x, y, w, h;
        uint32_t origin;
    } img = { 0, 0, 10, 10, 0 };
    CHECK_EQ(rc_of(call_data(&e, f, 101, &img, sizeof img)), 0);
    img.w = 0;
    CHECK_EQ(rc_of(call_data(&e, f, 101, &img, sizeof img)), AM_INVALID_PARAMS);
    img.w = 1;
    img.x = -1;
    CHECK_EQ(rc_of(call_data(&e, f, 101, &img, sizeof img)), AM_INVALID_PARAMS);
    CHECK_EQ(set8(&e, f, 102, 1), 0);

    CHECK_EQ(u32_out(&e, f, 123), 0xFFFFFFFFu);
    r = tc_send(e.c, f, tc_new(141));
    CHECK_EQ(rc_of(r), AM_NOT_AVAIL);
    CHECK_EQ(rc_of(tc_send(e.c, f, tc_new(1001))), 0);
    uint64_t title = 0;
    CHECK_EQ(rc_of(call_data(&e, f, 12, &title, 8)), 0);
    title = 0x0100000000001234ull;
    CHECK_EQ(rc_of(call_data(&e, f, 12, &title, 8)), AM_NOT_IMPL);
    CHECK_EQ(tc_cmd(e.c, f, 9999), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(am_application_functions_events)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    uint32_t cmds[] = { 130, 140, 150, 160, 210 };
    uint32_t handles[5];
    for (int i = 0; i < 5; i++) {
        handles[i] = get_event(&e, e.af, cmds[i]);
        CHECK(handles[i] != 0);
        CHECK(!signaled(&e, handles[i]));
    }
    uint32_t again = get_event(&e, e.af, 130);
    CHECK(again != 0);
    CHECK(!signaled(&e, again));
}

TEST(am_application_functions_play_statistics)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    uint8_t *out = t_mem() + e.heap + 0x2000;
    memset(out, 0xAB, 64);
    uint64_t *ids = (uint64_t *)(void *)(t_mem() + e.heap + 0x1000);
    ids[0] = 0x0100000000001234ull;
    tc_req q = tc_new(110);
    q.na = 1;
    q.a_addr[0] = e.heap + 0x1000;
    q.a_size[0] = 8;
    q.nb = 1;
    q.b_addr[0] = e.heap + 0x2000;
    q.b_size[0] = 64;
    tc_res r = tc_send(e.c, e.af, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
    for (int i = 0; i < 64; i++)
        CHECK_EQ(out[i], 0);

    memset(out, 0xAB, 64);
    q = tc_new(111);
    uint64_t uid[2] = { 1, 0 };
    tc_put(&q, uid, 16);
    q.na = 1;
    q.a_addr[0] = e.heap + 0x1000;
    q.a_size[0] = 8;
    q.nb = 1;
    q.b_addr[0] = e.heap + 0x2000;
    q.b_size[0] = 64;
    r = tc_send(e.c, e.af, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
    CHECK_EQ(out[0], 0);
}

static tc_res read_storage(env *e, tc_obj acc, uint64_t offset, uint64_t addr, uint64_t size)
{
    tc_req q = tc_new(11);
    tc_put64(&q, offset);
    q.nb = 1;
    q.b_addr[0] = addr;
    q.b_size[0] = size;
    return tc_send(e->c, acc, q);
}

static tc_res write_storage(env *e, tc_obj acc, uint64_t offset, uint64_t addr, uint64_t size)
{
    tc_req q = tc_new(10);
    tc_put64(&q, offset);
    q.na = 1;
    q.a_addr[0] = addr;
    q.a_size[0] = size;
    return tc_send(e->c, acc, q);
}

TEST(am_launch_parameters)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    tc_obj st, acc;
    tc_req q = tc_new(1);
    tc_put32(&q, 2);
    CHECK_EQ(tc_sub(e.c, e.af, q, &st), 0);
    CHECK_EQ(get(&e, st, 0, &acc), 0);
    tc_res r = tc_send(e.c, acc, tc_new(0));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 0x88);
    uint8_t *buf = t_mem() + e.heap + 0x3000;
    memset(buf, 0, 0x100);
    r = read_storage(&e, acc, 0, e.heap + 0x3000, 0x88);
    CHECK_EQ(rc_of(r), 0);
    uint32_t magic, selected;
    uint64_t uid[2];
    memcpy(&magic, buf, 4);
    memcpy(&selected, buf + 4, 4);
    memcpy(uid, buf + 8, 16);
    CHECK_EQ(magic, 0xC79497CAu);
    CHECK_EQ(selected, 1);
    CHECK_EQ(uid[0], 1);
    CHECK_EQ(uid[1], 0);

    rl_am_set_preselected_user(0x1111, 0x2222);
    q = tc_new(1);
    tc_put32(&q, 2);
    CHECK_EQ(tc_sub(e.c, e.af, q, &st), 0);
    CHECK_EQ(get(&e, st, 0, &acc), 0);
    r = read_storage(&e, acc, 8, e.heap + 0x3000, 16);
    CHECK_EQ(rc_of(r), 0);
    memcpy(uid, buf, 16);
    CHECK_EQ(uid[0], 0x1111);
    CHECK_EQ(uid[1], 0x2222);

    r = read_storage(&e, acc, 0x88, e.heap + 0x3000, 16);
    CHECK_EQ(rc_of(r), 0);
    r = read_storage(&e, acc, 0x89, e.heap + 0x3000, 16);
    CHECK_EQ(rc_of(r), AM_OOB);

    q = tc_new(1);
    tc_put32(&q, 1);
    CHECK_EQ(tc_sub(e.c, e.af, q, &st), AM_NOT_AVAIL);
    q = tc_new(1);
    tc_put32(&q, 3);
    CHECK_EQ(tc_sub(e.c, e.af, q, &st), AM_OBJ_INVALID);
    q = tc_new(1);
    tc_put32(&q, 0);
    CHECK_EQ(tc_sub(e.c, e.af, q, &st), AM_OBJ_INVALID);
}

TEST(am_user_channel_unpop)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    tc_obj creator_st, acc;
    tc_req q = tc_new(10);
    tc_put64(&q, 16);
    CHECK_EQ(tc_sub(e.c, e.lc, q, &creator_st), 0);
    CHECK_EQ(get(&e, creator_st, 0, &acc), 0);
    uint8_t *buf = t_mem() + e.heap + 0x3000;
    for (int i = 0; i < 16; i++)
        buf[i] = (uint8_t)(0x40 + i);
    CHECK_EQ(rc_of(write_storage(&e, acc, 0, e.heap + 0x3000, 16)), 0);
    CHECK_EQ(rc_of(write_storage(&e, acc, 17, e.heap + 0x3000, 16)), AM_OOB);

    q = tc_new(122);
    q.object_id = e.af.id;
    q.n_in_objects = 1;
    q.in_objects[0] = creator_st.id;
    tc_res r = tc_call(e.c, e.af.handle, &q, true);
    CHECK_EQ(rc_of(r), 0);

    tc_obj st2;
    q = tc_new(1);
    tc_put32(&q, 1);
    CHECK_EQ(tc_sub(e.c, e.af, q, &st2), 0);
    tc_obj acc2;
    CHECK_EQ(get(&e, st2, 0, &acc2), 0);
    memset(buf, 0, 16);
    CHECK_EQ(rc_of(read_storage(&e, acc2, 0, e.heap + 0x3000, 16)), 0);
    for (int i = 0; i < 16; i++)
        CHECK_EQ(buf[i], 0x40 + i);

    q = tc_new(1);
    tc_put32(&q, 1);
    CHECK_EQ(tc_sub(e.c, e.af, q, &st2), AM_NOT_AVAIL);

    q = tc_new(122);
    q.object_id = e.af.id;
    q.n_in_objects = 1;
    q.in_objects[0] = creator_st.id;
    CHECK_EQ(rc_of(tc_call(e.c, e.af.handle, &q, true)), 0);
    CHECK_EQ(rc_of(tc_send(e.c, e.af, tc_new(121))), 0);
    q = tc_new(1);
    tc_put32(&q, 1);
    CHECK_EQ(tc_sub(e.c, e.af, q, &st2), AM_NOT_AVAIL);

    q = tc_new(122);
    q.object_id = e.af.id;
    q.n_in_objects = 1;
    q.in_objects[0] = e.af.id;
    CHECK(rc_of(tc_call(e.c, e.af.handle, &q, true)) != 0);
}

TEST(am_storage_creation)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    tc_obj st;
    tc_req q = tc_new(10);
    tc_put64(&q, 0);
    CHECK_EQ(tc_sub(e.c, e.lc, q, &st), AM_OBJ_INVALID);
}

TEST(am_execute_program)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    struct {
        uint32_t kind, pad;
        uint64_t value;
    } ex = { 0, 0, 3 };
    CHECK_EQ(rc_of(call_data(&e, e.af, 120, &ex, sizeof ex)), 0);
    CHECK_EQ(u32_out(&e, e.af, 123), 0);
    ex.kind = 2;
    CHECK_EQ(rc_of(call_data(&e, e.af, 120, &ex, sizeof ex)), 0);
    CHECK_EQ(u32_out(&e, e.af, 123), 3);
    ex.kind = 1;
    CHECK_EQ(rc_of(call_data(&e, e.af, 120, &ex, sizeof ex)), AM_NOT_IMPL);
}

TEST(am_library_applet_creator)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    tc_obj la;
    tc_req q = tc_new(0);
    tc_put32(&q, 0x11);
    tc_put32(&q, 0);
    CHECK_EQ(tc_sub(e.c, e.lc, q, &la), 0);
    uint32_t changed = get_event(&e, la, 0);
    CHECK(changed != 0);
    CHECK(!signaled(&e, changed));
    CHECK_EQ(rc_of(tc_send(e.c, la, tc_new(20))), 0);
    CHECK(signaled(&e, changed));
    CHECK_EQ(u8_out(&e, la, 1), 1);
    CHECK_EQ(rc_of(tc_send(e.c, la, tc_new(10))), 0);
    CHECK_EQ(rc_of(tc_send(e.c, la, tc_new(30))), AM_TERMINATED);
    CHECK_EQ(rc_of(tc_send(e.c, la, tc_new(60))), 0);
    CHECK_EQ(tc_cmd(e.c, la, 101), AM_NOT_AVAIL);
    CHECK_EQ(tc_cmd(e.c, la, 104), AM_NOT_AVAIL);
    CHECK_EQ(tc_cmd(e.c, la, 110), AM_STUB);
    CHECK_EQ(tc_cmd(e.c, la, 150), AM_STUB);
    uint32_t out_ev = get_event(&e, la, 105);
    CHECK(out_ev != 0);
    CHECK(!signaled(&e, out_ev));
    CHECK(get_event(&e, la, 106) != 0);
    tc_res r = tc_send(e.c, la, tc_new(160));
    CHECK_EQ(rc_of(r), 0);
    CHECK(tc_u64(&r, 0) != 0);

    tc_obj st;
    q = tc_new(10);
    tc_put64(&q, 8);
    CHECK_EQ(tc_sub(e.c, e.lc, q, &st), 0);
    q = tc_new(100);
    q.object_id = la.id;
    q.n_in_objects = 1;
    q.in_objects[0] = st.id;
    CHECK_EQ(rc_of(tc_call(e.c, la.handle, &q, true)), 0);
}

TEST(am_state_resets_on_init)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    rl_am_set_operation_mode(true);
    rl_am_set_focus(false);
    rl_am_set_desired_language(7);
    rl_hos_shutdown();
    libnx_init_application(&e);
    CHECK(e.ok);
    CHECK(!rl_am_is_docked());
    CHECK_EQ(u8_out(&e, e.csg, 9), RL_AM_FOCUS_IN);
    CHECK_EQ(u8_out(&e, e.csg, 5), 0);
    tc_res r = tc_send(e.c, e.af, tc_new(21));
    CHECK_EQ(tc_u64(&r, 0), RL_AM_LANGUAGE_CODE_EN_US);
    uint32_t msg;
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, 15);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), 0);
    CHECK_EQ(msg, 1);
    CHECK_EQ(recv_msg(&e, e.csg, &msg), AM_NO_MSG);
}

TEST(am_proxy_without_pid_uses_default)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj oe = tc_open_service(c, heap, "appletOE");
    CHECK(oe.handle != 0);
    CHECK_EQ(tc_to_domain(c, &oe), 0);
    tc_req q = tc_new(0);
    tc_put64(&q, 0);
    tc_obj p, wc;
    CHECK_EQ(tc_sub(c, oe, q, &p), 0);
    CHECK_EQ(tc_sub(c, p, tc_new(2), &wc), 0);
    tc_res r = tc_send(c, wc, tc_new(1));
    CHECK_EQ(rc_of(r), 0);
    CHECK(tc_u64(&r, 0) != 0);
}

TEST(am_switch2_proxy_and_its_functions)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    env e;
    memset(&e, 0, sizeof e);
    e.c = t_boot(&cfg);
    e.heap = t_heap(e.c, 0x200000);
    e.svc = tc_open_service(e.c, e.heap, "appletOE");
    CHECK(e.svc.handle && tc_to_domain(e.c, &e.svc) == 0);
    CHECK_EQ(tc_sub(e.c, e.svc, proxy_req(10), &e.proxy), 0);
    tc_obj perf, scaling;
    CHECK_EQ(get(&e, e.proxy, 40, &perf), 0);
    CHECK_EQ(u8_out(&e, perf, 0), 0);
    CHECK_EQ(set8(&e, perf, 1, 1), 0);
    CHECK_EQ(u8_out(&e, perf, 0), 1);
    CHECK_EQ(get(&e, e.proxy, 60, &scaling), 0);
    tc_res r = tc_send(e.c, scaling, tc_new(10));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 10);
    CHECK_EQ(tc_u32(&r, 4) | tc_u32(&r, 8), 0);
    CHECK_EQ(get(&e, e.proxy, 20, &e.af), 0);
    r = tc_send(e.c, e.af, tc_new(320));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.data[0] | r.data[1], 0);
    CHECK_EQ(u8_out(&e, e.af, 321), 0);
    CHECK_EQ(u8_out(&e, e.af, 330), 0);
    CHECK_EQ(get(&e, e.proxy, 1, &e.sc), 0);
    CHECK_EQ(set8(&e, e.sc, 2400, 1), 0);
    CHECK_EQ(get(&e, e.proxy, 0, &e.csg), 0);
    CHECK_EQ(set32(&e, e.csg, 66, 2), 0);
    CHECK_EQ(set32(&e, e.csg, 66, 0), 0);
    CHECK_EQ(set32(&e, e.csg, 66, 1), AM_INVALID_PARAMS);
}

TEST(am_graphics_scale_follows_the_games_render_size)
{
    rl_settings_reset();
    rl_switch_declare_settings();
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    cfg.program_id = UINT64_C(0x0400486024dec000);
    env e;
    memset(&e, 0, sizeof e);
    e.c = t_boot(&cfg);
    e.heap = t_heap(e.c, 0x200000);
    e.svc = tc_open_service(e.c, e.heap, "appletOE");
    CHECK(e.svc.handle && tc_to_domain(e.c, &e.svc) == 0);
    CHECK_EQ(tc_sub(e.c, e.svc, proxy_req(10), &e.proxy), 0);
    tc_obj scaling;
    CHECK_EQ(get(&e, e.proxy, 60, &scaling), 0);
    rl_am_set_operation_mode(true);
    /* Scale code 10 is x1 (1440 lines), 20 is x0.8 (1152), 30 is x0.667 (960); a value between sizes takes the one below. */
    static const struct {
        const char *lines;
        uint32_t code, rendered;
    } want[] = {
        { "0", 10, 1440 }, { "1440", 10, 1440 }, { "1152", 20, 1152 }, { "1080", 30, 960 }, { "960", 30, 960 }, { "720", 30, 960 },
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        CHECK_EQ(rl_settings_set("video.render_resolution", want[i].lines), RL_SETTING_OK);
        tc_res r = tc_send(e.c, scaling, tc_new(10));
        CHECK_EQ(rc_of(r), 0);
        CHECK_EQ(tc_u32(&r, 0), want[i].code);
        CHECK_EQ(rl_am_render_lines(), want[i].rendered);
    }
    rl_render_size sizes[4];
    CHECK_EQ(rl_render_sizes(sizes, 4), 3u);
    CHECK_EQ(sizes[1].lines, 1152u);
    /* Handheld was not measured: no list, the setting is ignored. */
    rl_am_set_operation_mode(false);
    CHECK_EQ(rl_render_sizes(sizes, 4), 0u);
    CHECK_EQ(rl_am_render_lines(), 0u);
    tc_res r = tc_send(e.c, scaling, tc_new(10));
    CHECK_EQ(tc_u32(&r, 0), 10u);
    rl_settings_reset();
}

TEST(am_switch_has_no_switch2_proxy)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    tc_obj o;
    CHECK_EQ(get(&e, e.proxy, 40, &o), SF_RESULT_UNKNOWN_COMMAND);
    CHECK_EQ(get(&e, e.proxy, 60, &o), SF_RESULT_UNKNOWN_COMMAND);
    CHECK(tc_sub(e.c, e.svc, proxy_req(10), &o) != 0);
    CHECK_EQ(set8(&e, e.sc, 2400, 1), SF_RESULT_UNKNOWN_COMMAND);
    CHECK_EQ(rc_of(tc_send(e.c, e.af, tc_new(321))), SF_RESULT_UNKNOWN_COMMAND);
}

/** @brief Library applet id the SDK 22 keyboard asks for, its library version and config size. */
#define SWKBD_APPLET 0x1020011u
#define SWKBD_VERSION 0x160012u
#define SWKBD_CONFIG 0x530u

/** @brief Creates a storage of size bytes in the creator and writes data at guest scratch into it. */
static tc_obj make_storage(env *e, const void *data, uint32_t size)
{
    tc_obj st = { 0 }, acc;
    tc_req q = tc_new(10);
    tc_put64(&q, size);
    if (tc_sub(e->c, e->lc, q, &st) != 0 || get(e, st, 0, &acc) != 0)
        return (tc_obj){ 0 };
    memcpy(t_mem() + e->heap + 0x10000, data, size);
    if (rc_of(write_storage(e, acc, 0, e->heap + 0x10000, size)) != 0)
        return (tc_obj){ 0 };
    return st;
}

static uint32_t push_storage(env *e, tc_obj la, uint32_t cmd, tc_obj st)
{
    tc_req q = tc_new(cmd);
    q.object_id = la.id;
    q.n_in_objects = 1;
    q.in_objects[0] = st.id;
    return rc_of(tc_call(e->c, la.handle, &q, true));
}

static void put16(uint8_t *cfg, uint32_t off, const char *ascii)
{
    for (uint32_t i = 0; ascii[i]; i++)
        cfg[off + 2 * i] = (uint8_t)ascii[i];
}

/** @brief Opens the keyboard as nn::swkbd::ShowKeyboard of SDK 22 does, up to Start; la gets the accessor. */
static void start_keyboard(env *e, bool utf8, bool text_check, const char *initial, tc_obj *la_out, uint32_t *changed,
                           uint32_t *interactive)
{
    tc_obj la = { 0 };
    tc_req q = tc_new(0);
    tc_put32(&q, SWKBD_APPLET);
    tc_put32(&q, 0);
    CHECK_EQ(tc_sub(e->c, e->lc, q, &la), 0);
    *la_out = la;
    *changed = get_event(e, la, 0);
    CHECK(get_event(e, la, 170) != 0);
    *interactive = get_event(e, la, 106);
    uint32_t common[8] = { 1, 0x20, SWKBD_VERSION, 0, 0, 0, 0, 0 };
    CHECK_EQ(push_storage(e, la, 100, make_storage(e, common, sizeof common)), 0);
    uint8_t cfg[SWKBD_CONFIG];
    memset(cfg, 0, sizeof cfg);
    put16(cfg, 0x24, "Name");
    put16(cfg, 0x1a8, "Enter a name");
    cfg[0x3aa] = utf8;
    uint32_t max = 10, min = 1, off = 0x14, len = (uint32_t)strlen(initial);
    memcpy(cfg + 0x3ac, &max, 4);
    memcpy(cfg + 0x3b0, &min, 4);
    memcpy(cfg + 0x3bc, &off, 4);
    memcpy(cfg + 0x3c0, &len, 4);
    cfg[0x48d] = text_check;
    CHECK_EQ(push_storage(e, la, 100, make_storage(e, cfg, sizeof cfg)), 0);
    uint64_t work = e->heap + 0x20000;
    memset(t_mem() + work, 0, 0x1000);
    put16(t_mem() + work, off, initial);
    CHECK_EQ(t_svc(e->c, SVC_CREATE_TRANSFER_MEMORY, 0, work, 0x1000, 0, 0, 0), 0);
    tc_req tq = tc_new(11);
    tq.ncopy = 1;
    tq.copy[0] = (uint32_t)e->c->x[1];
    tc_put64(&tq, 1);
    tc_put64(&tq, 0x1000);
    tc_obj ws;
    CHECK_EQ(tc_sub(e->c, e->lc, tq, &ws), 0);
    CHECK_EQ(push_storage(e, la, 100, ws), 0);
    CHECK(!signaled(e, *changed));
    CHECK_EQ(rc_of(call_data(e, la, 90, (uint8_t[2]){ 0, 0 }, 2)), 0);
    CHECK_EQ(rc_of(tc_send(e->c, la, tc_new(10))), 0);
}

/** @brief Pops a storage of channel cmd and reads its first n bytes to out; returns its size, 0 when empty. */
static uint64_t pop_storage(env *e, tc_obj la, uint32_t cmd, void *out, uint32_t n)
{
    tc_obj st, acc;
    if (tc_sub(e->c, la, tc_new(cmd), &st) != 0 || get(e, st, 0, &acc) != 0)
        return 0;
    tc_res r = tc_send(e->c, acc, tc_new(0));
    uint64_t size = tc_u64(&r, 0);
    if (rc_of(read_storage(e, acc, 0, e->heap + 0x30000, n)) != 0)
        return 0;
    memcpy(out, t_mem() + e->heap + 0x30000, n);
    return size;
}

TEST(am_software_keyboard_answers_from_the_settings)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    rl_settings_declare_runtime();
    CHECK_EQ(rl_settings_set("text_entry.text", "Ada Lovelace"), RL_SETTING_OK);
    uint32_t changed, inter;
    tc_obj la;
    start_keyboard(&e, false, false, "", &la, &changed, &inter);
    CHECK(signaled(&e, changed));
    CHECK_EQ(u8_out(&e, la, 1), 1);
    CHECK_EQ(rc_of(tc_send(e.c, la, tc_new(30))), 0);
    uint8_t out[0x7d8];
    CHECK_EQ(pop_storage(&e, la, 101, out, sizeof out), 0x7d8);
    uint32_t close;
    memcpy(&close, out, 4);
    CHECK_EQ(close, 0);
    const char *want = "Ada Lovela";
    for (uint32_t i = 0; i < 10; i++)
        CHECK_EQ(out[4 + 2 * i] | (out[5 + 2 * i] << 8), (uint8_t)want[i]);
    CHECK_EQ(out[24] | out[25], 0);
    CHECK_EQ(tc_cmd(e.c, la, 101), AM_NOT_AVAIL);
    CHECK(!signaled(&e, inter));
    CHECK_EQ(rl_settings_set("text_entry.text", ""), RL_SETTING_OK);
}

TEST(am_software_keyboard_returns_the_initial_text_in_utf8)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    rl_settings_declare_runtime();
    CHECK_EQ(rl_settings_set("text_entry.text", ""), RL_SETTING_OK);
    uint32_t changed, inter;
    tc_obj la;
    start_keyboard(&e, true, false, "Mira", &la, &changed, &inter);
    CHECK(signaled(&e, changed));
    CHECK_EQ(rc_of(tc_send(e.c, la, tc_new(30))), 0);
    uint8_t out[16];
    CHECK_EQ(pop_storage(&e, la, 101, out, sizeof out), 0x7d8);
    CHECK_EQ(out[0], 0);
    CHECK(!memcmp(out + 4, "Mira", 5));
}

TEST(am_software_keyboard_default_text_without_initial_text)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    rl_settings_declare_runtime();
    CHECK_EQ(rl_settings_set("text_entry.text", ""), RL_SETTING_OK);
    CHECK_EQ(rl_settings_set("text_entry.default", "Tess"), RL_SETTING_OK);
    uint32_t changed, inter;
    tc_obj la;
    start_keyboard(&e, true, false, "", &la, &changed, &inter);
    CHECK(signaled(&e, changed));
    uint8_t out[16];
    CHECK_EQ(pop_storage(&e, la, 101, out, sizeof out), 0x7d8);
    CHECK(!memcmp(out + 4, "Tess", 5));
    CHECK_EQ(rl_settings_set("text_entry.default", "Player"), RL_SETTING_OK);
}

TEST(am_software_keyboard_text_check)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    rl_settings_declare_runtime();
    CHECK_EQ(rl_settings_set("text_entry.text", "Bad"), RL_SETTING_OK);
    uint32_t changed, inter;
    tc_obj la;
    start_keyboard(&e, true, true, "", &la, &changed, &inter);
    CHECK(!signaled(&e, changed));
    CHECK(signaled(&e, inter));
    uint8_t chk[16];
    CHECK_EQ(pop_storage(&e, la, 104, chk, sizeof chk), 8 + 0x7d4);
    uint64_t n;
    memcpy(&n, chk, 8);
    CHECK_EQ(n, 4);
    CHECK(!memcmp(chk + 8, "Bad", 4));
    CHECK(!signaled(&e, inter));
    uint8_t verdict[0x7d8];
    memset(verdict, 0, sizeof verdict);
    verdict[0] = 1;
    memcpy(verdict + 4, "No", 3);
    CHECK_EQ(push_storage(&e, la, 103, make_storage(&e, verdict, sizeof verdict)), 0);
    CHECK(signaled(&e, inter));
    CHECK_EQ(pop_storage(&e, la, 104, chk, sizeof chk), 8 + 0x7d4);
    CHECK(!signaled(&e, changed));
    verdict[0] = 0;
    CHECK_EQ(push_storage(&e, la, 103, make_storage(&e, verdict, sizeof verdict)), 0);
    CHECK(signaled(&e, changed));
    CHECK_EQ(rc_of(tc_send(e.c, la, tc_new(30))), 0);
    uint8_t out[16];
    CHECK_EQ(pop_storage(&e, la, 101, out, sizeof out), 0x7d8);
    CHECK_EQ(out[0], 0);
    CHECK(!memcmp(out + 4, "Bad", 4));
    CHECK_EQ(rl_settings_set("text_entry.text", ""), RL_SETTING_OK);
}

TEST(am_unknown_library_applet_ends_as_terminated)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    tc_obj la;
    tc_req q = tc_new(0);
    tc_put32(&q, 0x13);
    tc_put32(&q, 0);
    CHECK_EQ(tc_sub(e.c, e.lc, q, &la), 0);
    uint32_t changed = get_event(&e, la, 0);
    CHECK_EQ(rc_of(tc_send(e.c, la, tc_new(10))), 0);
    CHECK(signaled(&e, changed));
    CHECK_EQ(rc_of(tc_send(e.c, la, tc_new(30))), AM_TERMINATED);
    tc_res r = tc_send(e.c, la, tc_new(120));
    CHECK_EQ(tc_u32(&r, 0), 0x13);
}
