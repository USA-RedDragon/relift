/**
 * @file test_srv_bsd.c
 * @brief The socket services as nn::socket starts them: a client registers
 * and its sockets fail with ENETDOWN.
 */
#include "tc.h"

TEST(bsd_registers_and_has_no_network)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "bsd:s");
    CHECK(s.handle != 0);
    tc_req q = tc_new(0);
    for (unsigned i = 0; i < 8; i++)
        tc_put32(&q, 0);
    tc_put64(&q, 0);
    tc_put64(&q, 0x10000);
    tc_res r = tc_send(c, s, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
    q = tc_new(2);
    tc_put32(&q, 2);
    tc_put32(&q, 1);
    tc_put32(&q, 0);
    r = tc_send(c, s, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ((int32_t)tc_u32(&r, 0), -1);
    CHECK_EQ(tc_u32(&r, 4), 50);
    CHECK(tc_open_service(c, heap, "bsd:u").handle != 0);
}
