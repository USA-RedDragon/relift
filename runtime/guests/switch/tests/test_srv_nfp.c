/**
 * @file test_srv_nfp.c
 * @brief nfp:user as nn::nfp opens it: an interface that initializes,
 * finalizes and reports its state.
 */
#include "tc.h"

TEST(nfp_user_creates_an_interface_that_reports_its_state)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj s = tc_open_service(c, heap, "nfp:user");
    CHECK(s.handle != 0);
    CHECK_EQ(tc_to_domain(c, &s), 0);
    tc_obj u;
    CHECK_EQ(tc_sub(c, s, tc_new(0), &u), 0);
    CHECK_EQ(tc_cmd(c, u, 0), 0);
    tc_res r = tc_send(c, u, tc_new(2));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    CHECK_EQ(tc_cmd(c, u, 1), 0);
}
