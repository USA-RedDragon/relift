/**
 * @file test_srv_nsd.c
 * @brief nsd:u and nsd:a as nn::nsd's client calls them on a retail console
 * that is offline.
 */
#include "tc.h"

#include <string.h>

TEST(nsd_answers_the_retail_environment)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "nsd:a");
    CHECK(s.handle != 0);
    CHECK(tc_open_service(c, heap, "nsd:u").handle != 0);
    uint64_t buf = heap + 0x100000;

    tc_req q = tc_new(11);
    q.nb = 1;
    q.b_addr[0] = buf;
    q.b_size[0] = 8;
    tc_res r = tc_send(c, s, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(memcmp(t_mem() + buf, "lp1\0\0\0\0\0", 8), 0);

    q = tc_new(10);
    q.nb = 1;
    q.b_addr[0] = buf;
    q.b_size[0] = 0x100;
    CHECK_EQ(tc_send(c, s, q).result, 0);
    CHECK_EQ(memcmp(t_mem() + buf, "lp1", 4), 0);

    q = tc_new(100);
    r = tc_send(c, s, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.data[0], 1);

    q = tc_new(12);
    q.nb = 1;
    q.b_addr[0] = buf;
    q.b_size[0] = 16;
    CHECK_EQ(tc_send(c, s, q).result, 0);
}

TEST(nsd_resolve_leaves_a_name_as_it_is)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "nsd:u");
    uint64_t in = heap + 0x100000;
    uint64_t out = heap + 0x101000;
    memset(t_mem() + in, 0, 0x100);
    strcpy((char *)t_mem() + in, "api.example.invalid");
    for (uint32_t cmd = 20; cmd <= 21; cmd++) {
        tc_req q = tc_new(cmd);
        q.na = 1;
        q.a_addr[0] = in;
        q.a_size[0] = 0x100;
        q.nb = 1;
        q.b_addr[0] = out;
        q.b_size[0] = 0x100;
        tc_res r = tc_send(c, s, q);
        CHECK_EQ(r.result, 0);
        CHECK_EQ(strcmp((const char *)t_mem() + out, "api.example.invalid"), 0);
        memset(t_mem() + out, 0, 0x100);
    }
}
