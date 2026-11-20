/**
 * @file test_srv_ssl.c
 * @brief ssl and ssl:s as nn::ssl's client opens them on a console that is
 * offline: contexts and connections exist, a connection has no socket.
 */
#include "tc.h"

#define SSL_NO_SOCKET RL_MAKE_RESULT(123, 103)
#define SSL_INVALID_SOCKET RL_MAKE_RESULT(123, 106)

static void make_context(rl_ctx *c, tc_obj s, tc_obj *ctx)
{
    tc_req q = tc_new(0);
    q.send_pid = true;
    tc_put32(&q, 0x6000000);
    tc_put32(&q, 0);
    tc_put64(&q, 0);
    CHECK_EQ(tc_sub(c, s, q, ctx), 0);
}

TEST(ssl_creates_contexts_and_connections)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "ssl");
    CHECK(s.handle != 0);
    CHECK_EQ(tc_to_domain(c, &s), 0);
    tc_req q = tc_new(5);
    tc_put32(&q, 0x6000000);
    CHECK_EQ(tc_send(c, s, q).result, 0);
    tc_obj ctx;
    make_context(c, s, &ctx);
    tc_res r = tc_send(c, s, tc_new(1));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    tc_obj conn;
    CHECK_EQ(tc_sub(c, ctx, tc_new(2), &conn), 0);
    r = tc_send(c, ctx, tc_new(3));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    q = tc_new(0);
    tc_put32(&q, 3);
    r = tc_send(c, conn, q);
    CHECK_EQ(r.result, SSL_INVALID_SOCKET);
    CHECK_EQ(tc_send(c, conn, tc_new(8)).result, SSL_NO_SOCKET);
    q = tc_new(2);
    tc_put32(&q, 1);
    CHECK_EQ(tc_send(c, conn, q).result, 0);
    r = tc_send(c, conn, tc_new(6));
    CHECK_EQ(tc_u32(&r, 0), 1);
}

TEST(ssl_system_service_adds_create_context_for_system)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "ssl:s");
    CHECK(s.handle != 0);
    CHECK_EQ(tc_to_domain(c, &s), 0);
    tc_obj ctx;
    tc_req q = tc_new(100);
    q.send_pid = true;
    tc_put32(&q, 0x6000000);
    tc_put32(&q, 0);
    tc_put64(&q, 0);
    CHECK_EQ(tc_sub(c, s, q, &ctx), 0);
}
