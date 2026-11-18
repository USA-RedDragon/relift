/**
 * @file test_srv_auddmg.c
 * @brief auddmg:u the way a Switch 2 SDK's nn::audio::devmgr reaches it:
 * a user session, its one input device, and a named stop for what relift
 * does not answer.
 */
#include "tc.h"

#include <string.h>

static rl_ctx *boot2(void)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    return t_boot(&cfg);
}

TEST(auddmg_reports_one_input_device)
{
    rl_ctx *c = boot2();
    uint64_t heap = t_heap(c, 0x200000), names = heap + 0x10000;
    tc_obj m = tc_open_service(c, heap, "auddmg:u");
    CHECK(m.handle != 0);
    tc_req q = tc_new(0);
    q.send_pid = true;
    tc_put32(&q, 2);
    tc_put32(&q, 0);
    tc_put64(&q, 0);
    tc_obj s;
    CHECK_EQ(tc_sub(c, m, q, &s), 0);

    q = tc_new(0);
    tc_put32(&q, 0);
    tc_put32(&q, 2);
    q.nb = 1;
    q.b_addr[0] = names;
    q.b_size[0] = 8 * 0x100;
    tc_res r = tc_send(c, s, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    CHECK(strcmp((const char *)t_mem() + names, "MainAudioIn") == 0);

    q = tc_new(100);
    tc_put32(&q, 2);
    q.nb = 1;
    q.b_addr[0] = names + 0x100;
    q.b_size[0] = 0x100;
    CHECK_EQ(tc_send(c, s, q).result, 0);
    CHECK(strcmp((const char *)t_mem() + names + 0x100, "MainAudioIn") == 0);

    q = tc_new(300);
    uint32_t fmt[3] = { 2, 1, 48000 };
    tc_put(&q, fmt, sizeof fmt);
    tc_put32(&q, 2);
    q.na = 1;
    q.a_addr[0] = names;
    q.a_size[0] = 0x100;
    r = tc_send(c, s, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.data[0], 1);

    q = tc_new(700);
    tc_send(c, s, q);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
}

TEST(auddmg_is_switch2_only)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK(tc_open_service_result(c, heap, "auddmg:u") != 0);
}
