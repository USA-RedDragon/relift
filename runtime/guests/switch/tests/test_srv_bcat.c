/**
 * @file test_srv_bcat.c
 * @brief bcat:u as nn::bcat starts it (the services open and the delivery
 * cache holds no directory) and stpl:u as nn::streamplay sizes its memory.
 */
#include "tc.h"

TEST(bcat_delivery_cache_is_empty)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "bcat:u");
    CHECK(s.handle != 0);
    CHECK_EQ(tc_to_domain(c, &s), 0);
    tc_obj bcat, storage;
    tc_req q = tc_new(0);
    q.send_pid = true;
    tc_put64(&q, 0);
    CHECK_EQ(tc_sub(c, s, q, &bcat), 0);
    q = tc_new(1);
    q.send_pid = true;
    tc_put64(&q, 0);
    CHECK_EQ(tc_sub(c, s, q, &storage), 0);
    tc_res r = tc_send(c, storage, tc_new(10));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
}

TEST(stpl_needs_no_memory_on_the_switch2)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    rl_ctx *c = t_boot(&cfg);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "stpl:u");
    CHECK(s.handle != 0);
    CHECK_EQ(tc_to_domain(c, &s), 0);
    tc_obj shim;
    CHECK_EQ(tc_sub(c, s, tc_new(0), &shim), 0);
    tc_req q = tc_new(1);
    tc_put32(&q, 0);
    tc_put32(&q, 0);
    tc_put32(&q, 1);
    tc_res r = tc_send(c, shim, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
}
