/**
 * @file test_srv_audsmx.c
 * @brief audsmx:u the way a Switch 2 SDK's nn::audio::sysmix reaches it: an
 * input stream over a client ring, filled with silence once engaged, its
 * data event, purge, and a named stop for an output stream.
 */
#include "t.h"
#include "tc.h"

#include <string.h>

#define CUR_PROCESS_HANDLE 0xFFFF8001u

/** @brief tc_sub keeping the reply, for its raw output. */
static uint32_t sub_with(rl_ctx *c, tc_obj o, tc_req q, tc_obj *out, tc_res *res)
{
    *res = tc_send(c, o, q);
    uint32_t rc = res->svc_rc ? res->svc_rc : res->result;
    memset(out, 0, sizeof *out);
    if (rc)
        return rc;
    if (o.domain) {
        out->handle = o.handle;
        out->id = res->objects[0];
        out->domain = true;
    } else {
        out->handle = res->move[0];
    }
    return 0;
}

static rl_ctx *boot2(void)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    return t_boot(&cfg);
}

static uint32_t ring32(uint64_t a) { return rl_ld32(t_mem(), a); }

TEST(audsmx_fills_an_engaged_input_stream_with_silence)
{
    rl_ctx *c = boot2();
    uint64_t heap = t_heap(c, 0x200000), ring = heap + 0x40000, scratch = heap + 0x1000;
    const uint64_t ring_size = 0x10000;
    tc_obj m = tc_open_service(c, heap, "audsmx:u");
    CHECK(m.handle != 0);
    tc_req q = tc_new(0);
    q.send_pid = true;
    q.copy[q.ncopy++] = CUR_PROCESS_HANDLE;
    tc_put32(&q, 1);
    tc_put32(&q, 0);
    tc_put64(&q, 0);
    tc_obj ses;
    CHECK_EQ(tc_sub(c, m, q, &ses), 0);

    memcpy(t_mem() + scratch, "mic", 3);
    q = tc_new(1);
    tc_put32(&q, 2);
    tc_put32(&q, 2);
    tc_put64(&q, 0);
    tc_put64(&q, ring);
    tc_put64(&q, ring_size);
    q.na = 1;
    q.a_addr[0] = scratch;
    q.a_size[0] = 3;
    tc_obj st;
    tc_res cr;
    CHECK_EQ(sub_with(c, ses, q, &st, &cr), 0);
    uint32_t id = tc_u32(&cr, 0);

    memset(t_mem() + ring, 0, 0x1040);
    memset(t_mem() + ring + 0x1040, 0x5a, 961 * 2);
    q = tc_new(0);
    uint32_t open[6] = { 2, 1, 48000, 960, 0, 0 };
    tc_put(&q, open, sizeof open);
    CHECK_EQ(tc_send(c, st, q).result, 0);
    q = tc_new(7);
    tc_put32(&q, 2);
    tc_put32(&q, 480);
    tc_res ev = tc_send(c, st, q);
    CHECK_EQ(ev.result, 0);
    CHECK_EQ(ev.ncopy, 1);

    t_sleep(30000000);
    CHECK_EQ(ring32(ring + 0x1004), 0);

    rl_st32(t_mem(), scratch, id);
    q = tc_new(2);
    q.na = 1;
    q.a_addr[0] = scratch;
    q.a_size[0] = 4;
    CHECK_EQ(tc_send(c, ses, q).result, 0);
    T_WAIT_UNTIL(ring32(ring + 0x1004) >= 480);
    uint32_t w = ring32(ring + 0x1004);
    CHECK(w <= 960);
    CHECK_EQ(rl_ld32(t_mem(), ring + 0x1040), 0);

    q = tc_new(3);
    q.na = 1;
    q.a_addr[0] = scratch;
    q.a_size[0] = 4;
    CHECK_EQ(tc_send(c, ses, q).result, 0);
    t_sleep(20000000);
    w = ring32(ring + 0x1004);
    q = tc_new(11);
    tc_put32(&q, 0);
    tc_res pr = tc_send(c, st, q);
    CHECK_EQ(pr.result, 0);
    CHECK_EQ(tc_u32(&pr, 0), w);
    CHECK_EQ(ring32(ring + 0x44), w);

    q = tc_new(1);
    tc_put32(&q, 1);
    tc_put32(&q, 0x24);
    tc_put64(&q, 0);
    tc_put64(&q, ring);
    tc_put64(&q, ring_size);
    q.na = 1;
    q.a_addr[0] = scratch;
    q.a_size[0] = 3;
    tc_send(c, ses, q);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
}

TEST(audsmx_is_switch2_only)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK(tc_open_service_result(c, heap, "audsmx:u") != 0);
}
