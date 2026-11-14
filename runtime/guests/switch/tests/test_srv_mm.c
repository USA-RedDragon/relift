/**
 * @file test_srv_mm.c
 * @brief mm:u clock requests through IPC: the request id interface and the
 * old one by module.
 */
#include "tc.h"

TEST(mm_records_clock_requests)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj mm = tc_open_service(c, heap, "mm:u");
    CHECK(mm.handle);

    tc_req i = tc_new(4);
    tc_put32(&i, 5);
    tc_put32(&i, 0);
    tc_put32(&i, 0);
    tc_res ir = tc_send(c, mm, i);
    CHECK_EQ(ir.result, 0);
    uint32_t id = tc_u32(&ir, 0);
    CHECK(id != 0);

    tc_req s = tc_new(6);
    tc_put32(&s, id);
    tc_put32(&s, 768000000);
    tc_put32(&s, 0);
    CHECK_EQ(tc_send(c, mm, s).result, 0);

    tc_req g = tc_new(7);
    tc_put32(&g, id);
    tc_res gr = tc_send(c, mm, g);
    CHECK_EQ(gr.result, 0);
    CHECK_EQ(tc_u32(&gr, 0), 768000000);

    tc_req go = tc_new(3);
    tc_put32(&go, 5);
    tc_res gor = tc_send(c, mm, go);
    CHECK_EQ(gor.result, 0);
    CHECK_EQ(tc_u32(&gor, 0), 768000000);

    tc_req f = tc_new(5);
    tc_put32(&f, id);
    CHECK_EQ(tc_send(c, mm, f).result, 0);
    tc_res after = tc_send(c, mm, go);
    CHECK_EQ(tc_u32(&after, 0), 0);
}
