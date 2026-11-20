/**
 * @file test_srv_friend.c
 * @brief friend:u and nifm:u of an offline console as nn::friends and
 * nn::nifm open them, and the friend commands no document answers stop.
 */
#include "tc.h"

TEST(friend_service_answers_an_offline_console)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "friend:u");
    CHECK(s.handle != 0);
    CHECK_EQ(tc_to_domain(c, &s), 0);
    tc_obj fs, suspend;
    CHECK_EQ(tc_sub(c, s, tc_new(0), &fs), 0);
    CHECK_EQ(tc_sub(c, s, tc_new(2), &suspend), 0);
    uint8_t uid[16];
    for (int i = 0; i < 16; i++)
        uid[i] = (uint8_t)(0x40 + i);
    tc_req q = tc_new(10120);
    tc_put(&q, uid, 16);
    tc_res rep = tc_send(c, fs, q);
    CHECK_EQ(rep.result, 0);
    CHECK_EQ(rep.data[0], 1);
    uint64_t buf = heap + 0x100000;
    q = tc_new(10700);
    tc_put64(&q, 1);
    tc_put(&q, uid, 16);
    q.nb = 1;
    q.b_addr[0] = buf;
    q.b_size[0] = 0x40;
    rep = tc_send(c, fs, q);
    CHECK_EQ(rep.result, 0);
    const uint8_t *key = t_mem() + buf;
    CHECK_EQ(key[0], 1);
    CHECK_EQ(key[1], 1);
    CHECK_EQ(key[4], 1);
    CHECK_EQ(key[0x10], 0x40);
    CHECK_EQ(key[0x1f], 0x4f);
    CHECK_EQ(key[0x20], 0);
}

TEST(nifm_opens_a_general_service)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "nifm:u");
    CHECK(s.handle != 0);
    CHECK_EQ(tc_to_domain(c, &s), 0);
    tc_obj g;
    tc_req q = tc_new(5);
    q.send_pid = true;
    tc_put64(&q, 0);
    CHECK_EQ(tc_sub(c, s, q, &g), 0);
    tc_obj old;
    CHECK_EQ(tc_sub(c, s, tc_new(4), &old), 0);
}

TEST(nifm_requests_report_no_network)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "nifm:u");
    CHECK_EQ(tc_to_domain(c, &s), 0);
    tc_obj g;
    tc_req q = tc_new(5);
    q.send_pid = true;
    tc_put64(&q, 0);
    CHECK_EQ(tc_sub(c, s, q, &g), 0);
    tc_res r = tc_send(c, g, tc_new(1));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    r = tc_send(c, g, tc_new(21));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.data[0], 0);
    r = tc_send(c, g, tc_new(18));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.data[2], 0);
    tc_obj req;
    q = tc_new(4);
    tc_put32(&q, 3);
    CHECK_EQ(tc_sub(c, g, q, &req), 0);
    r = tc_send(c, req, tc_new(0));
    CHECK_EQ(tc_u32(&r, 0), 1);
    r = tc_send(c, req, tc_new(2));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.ncopy, 2);
    CHECK_EQ(tc_send(c, req, tc_new(4)).result, 0);
    r = tc_send(c, req, tc_new(0));
    CHECK_EQ(tc_u32(&r, 0), 2);
    CHECK_EQ(tc_send(c, req, tc_new(1)).result, RL_MAKE_RESULT(110, 111));
    r = tc_send(c, req, tc_new(0));
    CHECK_EQ(tc_u32(&r, 0), 1);
    CHECK_EQ(tc_send(c, req, tc_new(1)).result, RL_MAKE_RESULT(110, 1111));
    CHECK_EQ(tc_send(c, g, tc_new(12)).result, RL_MAKE_RESULT(110, 1111));
}
