/**
 * @file test_srv_csrng.c
 * @brief csrng driven through IPC the way libnx's csrngGetRandomBytes uses
 * it: command 0 with one receive (B) buffer.
 */
#include "tc.h"

#include <string.h>

static tc_res draw(rl_ctx *c, tc_obj o, uint64_t addr, uint64_t size)
{
    tc_req q = tc_new(0);
    q.b_addr[0] = addr;
    q.b_size[0] = size;
    q.nb = 1;
    return tc_send(c, o, q);
}

TEST(csrng_appears_through_sm)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK_EQ(tc_open_service_result(c, heap, "csrng"), 0);
}

TEST(csrng_fills_the_whole_buffer_with_fresh_bytes)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj o = tc_open_service(c, heap, "csrng");
    CHECK(o.handle);
    uint64_t a = heap + 0x100000, b = heap + 0x108000;
    uint32_t n = 0x5001;
    memset(t_mem() + a, 0, n + 16);
    memset(t_mem() + b, 0, n);
    tc_res r = draw(c, o, a, n);
    CHECK_EQ(r.svc_rc, 0);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(draw(c, o, b, n).result, 0);
    uint32_t zero_tail = 0, same = 0;
    for (uint32_t i = n - 64; i < n; i++)
        zero_tail += t_mem()[a + i] == 0;
    for (uint32_t i = 0; i < n; i++)
        same += t_mem()[a + i] == t_mem()[b + i];
    CHECK(zero_tail < 8);
    CHECK(same < n / 64);
    for (uint32_t i = 0; i < 16; i++)
        CHECK_EQ(t_mem()[a + n + i], 0);
}

TEST(csrng_without_a_buffer_succeeds)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj o = tc_open_service(c, heap, "csrng");
    CHECK_EQ(tc_cmd(c, o, 0), 0);
}
