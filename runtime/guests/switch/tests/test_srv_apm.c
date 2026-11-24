/**
 * @file test_srv_apm.c
 * @brief apm, apm:am, apm:sys, apm:p and psm driven through IPC the way
 * libnx's apmInitialize, apmGetPerformanceMode and the psm wrappers use them.
 * The performance mode is the console's: boost docked, normal handheld.
 */
#include "am.h"
#include "tc.h"

#define APM_INVALID_PARAMETERS (148u | (1u << 9))

typedef struct env {
    rl_ctx *c;
    uint64_t heap;
} env;

static env boot(void)
{
    env e;
    e.c = t_boot(NULL);
    e.heap = t_heap(e.c, 0x200000);
    return e;
}

static uint32_t rc_of(tc_res r) { return r.svc_rc ? r.svc_rc : r.result; }

static const uint8_t *res_raw(rl_ctx *c)
{
    const uint32_t *w = (const uint32_t *)(const void *)(t_mem() + c->tpidrro_el0);
    uint32_t pos = 2;
    if (w[1] >> 31) {
        uint32_t sp = w[pos++];
        if (sp & 1)
            pos += 2;
        pos += ((sp >> 1) & 0xF) + ((sp >> 5) & 0xF);
    }
    return (const uint8_t *)w + ((pos * 4 + 15) & ~15u) + 16;
}

static tc_res call(env *e, tc_obj o, uint32_t cmd, const void *in, uint32_t n)
{
    tc_req q = tc_new(cmd);
    if (n)
        tc_put(&q, in, n);
    return tc_send(e->c, o, q);
}

static uint32_t session_of(env *e, tc_obj mgr, tc_obj *out) { return tc_sub(e->c, mgr, tc_new(0), out); }

TEST(apm_and_psm_appear_through_sm)
{
    env e = boot();
    const char *names[] = { "apm", "apm:am", "apm:sys", "apm:p", "psm" };
    for (uint32_t i = 0; i < 5; i++)
        CHECK_EQ(tc_open_service_result(e.c, e.heap, names[i]), 0);
    CHECK_EQ(tc_open_service_result(e.c, e.heap, "apm:x"), SM_RESULT_NOT_REGISTERED);
    CHECK_EQ(tc_open_service_result(e.c, e.heap, "psm:x"), SM_RESULT_NOT_REGISTERED);
}

TEST(libnx_apm_init_and_performance_mode)
{
    env e = boot();
    tc_obj apm = tc_open_service(e.c, e.heap, "apm");
    CHECK(apm.handle);
    tc_obj session;
    CHECK_EQ(session_of(&e, apm, &session), 0);
    CHECK(session.handle);
    CHECK(session.handle != apm.handle);

    tc_res r = call(&e, apm, 1, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 0);

    uint32_t in[2] = { 0, 0x00020003 };
    uint32_t mode = 0;
    r = call(&e, session, 1, &mode, 4);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 0x00020003);
    mode = 1;
    r = call(&e, session, 1, &mode, 4);
    CHECK_EQ(tc_u32(&r, 0), 0x00020004);

    in[0] = 1;
    in[1] = 0x92220009;
    CHECK_EQ(rc_of(call(&e, session, 0, in, 8)), 0);
    r = call(&e, session, 1, &mode, 4);
    CHECK_EQ(tc_u32(&r, 0), 0x92220009);
    mode = 0;
    r = call(&e, session, 1, &mode, 4);
    CHECK_EQ(tc_u32(&r, 0), 0x00020003);

    in[0] = 2;
    CHECK_EQ(rc_of(call(&e, session, 0, in, 8)), APM_INVALID_PARAMETERS);
    mode = 2;
    CHECK_EQ(rc_of(call(&e, session, 1, &mode, 4)), APM_INVALID_PARAMETERS);
    CHECK_EQ(rc_of(call(&e, session, 77, NULL, 0)), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(apm_cpu_overclock)
{
    env e = boot();
    tc_obj apm = tc_open_service(e.c, e.heap, "apm:am");
    CHECK(apm.handle);
    tc_obj session;
    CHECK_EQ(session_of(&e, apm, &session), 0);
    tc_res r = call(&e, apm, 6, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.data[0], 0);
    uint8_t on = 1;
    CHECK_EQ(rc_of(call(&e, session, 2, &on, 1)), 0);
    r = call(&e, apm, 6, NULL, 0);
    CHECK_EQ(r.data[0], 1);
    on = 0;
    CHECK_EQ(rc_of(call(&e, session, 2, &on, 1)), 0);
    r = call(&e, apm, 6, NULL, 0);
    CHECK_EQ(r.data[0], 0);
    CHECK_EQ(rc_of(call(&e, apm, 2, NULL, 0)), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(apm_sys_requests_and_current_configuration)
{
    env e = boot();
    tc_obj sys = tc_open_service(e.c, e.heap, "apm:sys");
    tc_obj apm = tc_open_service(e.c, e.heap, "apm");
    CHECK(sys.handle && apm.handle);
    tc_res r = call(&e, sys, 7, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 0x00020003);

    uint32_t mode = 1;
    CHECK_EQ(rc_of(call(&e, sys, 0, &mode, 4)), 0);
    r = call(&e, sys, 7, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 0x00020003);
    r = call(&e, apm, 1, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);

    rl_am_set_operation_mode(true);
    r = call(&e, sys, 7, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 0x00020004);
    r = call(&e, apm, 1, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 1);

    tc_obj session;
    CHECK_EQ(session_of(&e, apm, &session), 0);
    uint32_t in[2] = { 1, 0x9222000B };
    CHECK_EQ(rc_of(call(&e, session, 0, in, 8)), 0);
    r = call(&e, sys, 7, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 0x9222000B);

    uint32_t boost = 1;
    CHECK_EQ(rc_of(call(&e, sys, 6, &boost, 4)), 0);
    CHECK_EQ(rc_of(call(&e, sys, 1, NULL, 0)), SF_RESULT_UNKNOWN_COMMAND);
    rl_am_set_operation_mode(false);
}

TEST(apm_p_opens_sessions)
{
    env e = boot();
    tc_obj p = tc_open_service(e.c, e.heap, "apm:p");
    CHECK(p.handle);
    tc_obj session;
    CHECK_EQ(session_of(&e, p, &session), 0);
    uint32_t mode = 0;
    tc_res r = call(&e, session, 1, &mode, 4);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 0x00020003);
    CHECK_EQ(rc_of(call(&e, p, 1, NULL, 0)), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(apm_over_a_domain)
{
    env e = boot();
    tc_obj apm = tc_open_service(e.c, e.heap, "apm");
    CHECK(apm.handle);
    CHECK_EQ(tc_to_domain(e.c, &apm), 0);
    tc_obj session;
    CHECK_EQ(session_of(&e, apm, &session), 0);
    CHECK(session.domain);
    uint32_t mode = 0;
    tc_res r = call(&e, session, 1, &mode, 4);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 0x00020003);
}

TEST(psm_battery_and_charger)
{
    env e = boot();
    tc_obj psm = tc_open_service(e.c, e.heap, "psm");
    CHECK(psm.handle);
    tc_res r = call(&e, psm, 0, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 100);
    r = call(&e, psm, 1, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    r = call(&e, psm, 4, NULL, 0);
    CHECK_EQ(r.data[0], 1);
    CHECK_EQ(rc_of(call(&e, psm, 3, NULL, 0)), 0);
    r = call(&e, psm, 4, NULL, 0);
    CHECK_EQ(r.data[0], 0);
    CHECK_EQ(rc_of(call(&e, psm, 2, NULL, 0)), 0);
    r = call(&e, psm, 4, NULL, 0);
    CHECK_EQ(r.data[0], 1);

    uint32_t stubs[] = { 5, 6, 8, 9, 10, 11 };
    for (uint32_t i = 0; i < sizeof stubs / sizeof stubs[0]; i++)
        CHECK_EQ(rc_of(call(&e, psm, stubs[i], NULL, 0)), 0);

    r = call(&e, psm, 12, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 3);
    double d;
    r = call(&e, psm, 13, NULL, 0);
    d = 0;
    memcpy(&d, r.data, 8);
    CHECK(d == 100.0);
    r = call(&e, psm, 15, NULL, 0);
    memcpy(&d, r.data, 8);
    CHECK(d == 100.0);
    r = call(&e, psm, 14, NULL, 0);
    CHECK_EQ(r.data[0], 1);
    CHECK_EQ(rc_of(call(&e, psm, 99, NULL, 0)), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(psm_charge_info_fields_and_events)
{
    env e = boot();
    tc_obj psm = tc_open_service(e.c, e.heap, "psm");
    CHECK(psm.handle);
    tc_res r = call(&e, psm, 17, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    const uint8_t *raw = res_raw(e.c);
    uint32_t f[0x54 / 4];
    memcpy(f, raw, sizeof f);
    CHECK_EQ(f[4], 1);
    CHECK_EQ(((const uint8_t *)&f[5])[1], 1);
    CHECK_EQ(f[8], 100000);
    CHECK_EQ(f[10], 100000);
    CHECK_EQ(f[16], 0);

    uint32_t cmds[2] = { 16, 18 };
    for (uint32_t i = 0; i < 2; i++) {
        r = call(&e, psm, cmds[i], NULL, 0);
        CHECK_EQ(rc_of(r), 0);
        CHECK_EQ(r.ncopy, 1);
        *(uint32_t *)(void *)(t_mem() + e.heap + 0x800) = r.copy[0];
        CHECK_EQ(t_svc(e.c, SVC_WAIT_SYNCHRONIZATION, 0, e.heap + 0x800, 1, 0, 0, 0), KR_TIMED_OUT);
    }
}

TEST(libnx_psm_bind_state_change_event)
{
    env e = boot();
    tc_obj psm = tc_open_service(e.c, e.heap, "psm");
    CHECK(psm.handle);
    tc_obj session;
    CHECK_EQ(tc_sub(e.c, psm, tc_new(7), &session), 0);
    CHECK(session.handle);

    uint8_t on = 1;
    for (uint32_t cmd = 2; cmd <= 4; cmd++)
        CHECK_EQ(rc_of(call(&e, session, cmd, &on, 1)), 0);

    tc_res r = call(&e, session, 0, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.ncopy, 1);
    uint32_t ev = r.copy[0];
    *(uint32_t *)(void *)(t_mem() + e.heap + 0x800) = ev;
    CHECK_EQ(t_svc(e.c, SVC_WAIT_SYNCHRONIZATION, 0, e.heap + 0x800, 1, 0, 0, 0), KR_TIMED_OUT);

    r = call(&e, session, 0, NULL, 0);
    CHECK_EQ(r.ncopy, 1);
    CHECK_EQ(rc_of(call(&e, session, 1, NULL, 0)), 0);
    CHECK_EQ(rc_of(call(&e, session, 1, NULL, 0)), 0);
    CHECK_EQ(rc_of(call(&e, session, 9, NULL, 0)), SF_RESULT_UNKNOWN_COMMAND);
    tc_close(e.c, session.handle);
}
