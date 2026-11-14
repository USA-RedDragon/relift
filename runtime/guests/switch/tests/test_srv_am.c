/**
 * @file test_srv_am.c
 * @brief The applet manager services driven through IPC the way libnx's
 * appletInitialize and a typical game use them: appletOE, appletAE, the
 * proxies and their objects, the message queue and the events.
 */
#include "am.h"
#include "srv_link.h"
#include "tc.h"

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
        uint32_t rc = wait_h(&e, ev, iterations == 1 ? 0 : 1000000000ull);
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

    rl_am_request_exit();
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

TEST(am_self_controller_commands)
{
    env e;
    libnx_init_application(&e);
    CHECK(e.ok);
    tc_obj sc = e.sc;
    CHECK_EQ(rc_of(tc_send(e.c, sc, tc_new(1))), 0);
    CHECK_EQ(rc_of(tc_send(e.c, sc, tc_new(2))), 0);
    CHECK_EQ(rc_of(tc_send(e.c, sc, tc_new(0))), 0);

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
    CHECK_EQ(rc_of(tc_send(e.c, la, tc_new(10))), 0);
    CHECK_EQ(rc_of(tc_send(e.c, la, tc_new(30))), 0);
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
