/**
 * @file test_srv_prepo.c
 * @brief prepo:u as nn::prepo opens it on a console that is offline: reports
 * are accepted and discarded.
 */
#include "tc.h"

TEST(prepo_accepts_reports_and_reports_no_transmission)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "prepo:u");
    CHECK(s.handle != 0);
    CHECK(tc_open_service(c, heap, "prepo:a").handle != 0);
    tc_req q = tc_new(10106);
    q.send_pid = true;
    tc_put64(&q, 0);
    CHECK_EQ(tc_send(c, s, q).result, 0);
    CHECK_EQ(tc_cmd(c, s, 10200), 0);
    tc_res r = tc_send(c, s, tc_new(10300));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
    r = tc_send(c, s, tc_new(10400));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u64(&r, 0), 0);
}
