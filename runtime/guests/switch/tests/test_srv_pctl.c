/**
 * @file test_srv_pctl.c
 * @brief pctl, pctl:a, pctl:r and pctl:s driven through IPC the way
 * nn::pctl::Initialize and the stereo vision checks use them.
 */
#include "tc.h"

#define PCTL_STEREO_VISION_DENIED (142u | (104u << 9))
#define PCTL_PERMISSION_DENIED (142u | (133u << 9))

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

static tc_res call(env *e, tc_obj o, uint32_t cmd, const void *in, uint32_t n)
{
    tc_req q = tc_new(cmd);
    if (n)
        tc_put(&q, in, n);
    return tc_send(e->c, o, q);
}

TEST(pctl_services_appear_through_sm)
{
    env e = boot();
    const char *names[] = { "pctl", "pctl:a", "pctl:r", "pctl:s" };
    for (uint32_t i = 0; i < 4; i++)
        CHECK_EQ(tc_open_service_result(e.c, e.heap, names[i]), 0);
    CHECK_EQ(tc_open_service_result(e.c, e.heap, "pctl:x"), SM_RESULT_NOT_REGISTERED);
}

TEST(pctl_create_service_allows_everything)
{
    env e = boot();
    tc_obj f = tc_open_service(e.c, e.heap, "pctl");
    CHECK(f.handle);
    tc_obj s;
    CHECK_EQ(tc_sub(e.c, f, tc_new(0), &s), 0);
    CHECK(s.handle);
    CHECK_EQ(rc_of(call(&e, s, 1, NULL, 0)), 0);
    CHECK_EQ(rc_of(call(&e, s, 1001, NULL, 0)), 0);
    CHECK_EQ(rc_of(call(&e, s, 1013, NULL, 0)), 0);
    tc_res r = call(&e, s, 1065, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0) & 0xff, 1);
    r = call(&e, s, 1031, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0) & 0xff, 0);
    CHECK_EQ(rc_of(call(&e, s, 1061, NULL, 0)), 0);
}

TEST(pctl_stereo_vision_restriction_needs_pctl_s)
{
    env e = boot();
    tc_obj f = tc_open_service(e.c, e.heap, "pctl:s");
    tc_obj s;
    CHECK_EQ(tc_sub(e.c, f, tc_new(1), &s), 0);
    tc_res r = call(&e, s, 1031, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0) & 0xff, 0);
    uint8_t on = 1;
    CHECK_EQ(rc_of(call(&e, s, 1063, &on, 1)), 0);
    r = call(&e, s, 1062, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0) & 0xff, 1);
    CHECK_EQ(rc_of(call(&e, s, 1013, NULL, 0)), PCTL_STEREO_VISION_DENIED);
    r = call(&e, s, 1065, NULL, 0);
    CHECK_EQ(rc_of(r), PCTL_STEREO_VISION_DENIED);
}

TEST(pctl_r_create_service_without_initialize)
{
    env e = boot();
    tc_obj f = tc_open_service(e.c, e.heap, "pctl:r");
    tc_obj s;
    CHECK_EQ(tc_sub(e.c, f, tc_new(1), &s), 0);
    CHECK_EQ(rc_of(call(&e, s, 1, NULL, 0)), 0);
    CHECK_EQ(rc_of(call(&e, s, 1061, NULL, 0)), PCTL_PERMISSION_DENIED);
    CHECK_EQ(rc_of(call(&e, s, 1062, NULL, 0)), PCTL_PERMISSION_DENIED);
    CHECK_EQ(rc_of(call(&e, s, 77, NULL, 0)), SF_RESULT_UNKNOWN_COMMAND);
}
