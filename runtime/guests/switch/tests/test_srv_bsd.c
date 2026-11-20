/**
 * @file test_srv_bsd.c
 * @brief The socket services as nn::socket starts them: a client registers
 * (command 0 or the Switch 2 command 36) and its sockets fail with ENETDOWN.
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

TEST(bsd_register_client_switch2_answers_errno_zero)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "bsd:u");
    CHECK(s.handle != 0);
    tc_req q = tc_new(36);
    tc_put32(&q, 5);
    tc_put32(&q, 0);
    tc_put64(&q, 0);
    tc_res r = tc_send(c, s, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
}

TEST(bsd_poll_reports_no_descriptor_ready)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "bsd:u");
    uint32_t *in = (uint32_t *)(void *)(t_mem() + heap + 0x100000);
    uint32_t *out = (uint32_t *)(void *)(t_mem() + heap + 0x101000);
    in[0] = 7;
    in[1] = 1;
    in[2] = UINT32_MAX;
    in[3] = 1;
    tc_req q = tc_new(6);
    tc_put32(&q, 2);
    tc_put32(&q, 0);
    q.na = 1;
    q.a_addr[0] = heap + 0x100000;
    q.a_size[0] = 16;
    q.nb = 1;
    q.b_addr[0] = heap + 0x101000;
    q.b_size[0] = 16;
    tc_res r = tc_send(c, s, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ((int32_t)tc_u32(&r, 0), 0);
    CHECK_EQ(tc_u32(&r, 4), 0);
    CHECK_EQ(out[0], 7);
    CHECK_EQ(out[1], 1);
    CHECK_EQ(out[2], UINT32_MAX);
    CHECK_EQ(out[3], 1);
}

TEST(bsd_poll_without_descriptors_waits_out_the_timeout)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "bsd:u");
    tc_req q = tc_new(6);
    tc_put32(&q, 0);
    tc_put32(&q, 1);
    tc_res r = tc_send(c, s, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ((int32_t)tc_u32(&r, 0), 0);
    CHECK_EQ(tc_u32(&r, 4), 0);
}
