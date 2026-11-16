/**
 * @file test_srv_ectx.c
 * @brief ectx:aw and notif:a driven through IPC the way the SDK uses them
 * at start-up: a context registrar that completes, and no alarms.
 */
#include "tc.h"

TEST(ectx_registrar_completes_with_descriptor_zero)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj o = tc_open_service(c, heap, "ectx:aw");
    CHECK(o.handle);
    tc_obj reg;
    CHECK_EQ(tc_sub(c, o, tc_new(0), &reg), 0);
    tc_req q = tc_new(0);
    tc_put32(&q, 0);
    tc_res r = tc_send(c, reg, q);
    CHECK_EQ(r.svc_rc, 0);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
}

TEST(notif_lists_no_alarms)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj o = tc_open_service(c, heap, "notif:a");
    CHECK(o.handle);
    tc_req q = tc_new(1000);
    q.send_pid = true;
    tc_put64(&q, 0);
    CHECK_EQ(tc_send(c, o, q).result, 0);
    tc_res r = tc_send(c, o, tc_new(520));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
}
