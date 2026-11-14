/**
 * @file test_srv_acc.c
 * @brief acc:u0 driven through IPC the way nn::account initializes and reads
 * its user: user count, user lists, the profile and the null uid check.
 */
#include "tc.h"

#include <string.h>

TEST(acc_has_one_open_user_with_a_profile)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj acc = tc_open_service(c, heap, "acc:u0");
    CHECK(acc.handle);

    tc_req init = tc_new(140);
    init.send_pid = true;
    tc_put64(&init, 0);
    CHECK_EQ(tc_send(c, acc, init).result, 0);

    tc_res n = tc_send(c, acc, tc_new(0));
    CHECK_EQ(n.result, 0);
    CHECK_EQ(tc_u32(&n, 0), 1);

    uint64_t list = heap + 0x100000;
    memset(t_mem() + list, 0xAA, 0xA0);
    tc_req l = tc_new(3);
    l.c_addr[0] = list;
    l.c_size[0] = 0xA0;
    l.nc = 1;
    CHECK_EQ(tc_send(c, acc, l).result, 0);
    uint64_t uid[2], zero[2] = { 0, 0 };
    memcpy(uid, t_mem() + list, 16);
    CHECK(uid[0] || uid[1]);
    CHECK_EQ(memcmp(t_mem() + list + 16, zero, 16), 0);

    tc_res last = tc_send(c, acc, tc_new(4));
    CHECK_EQ(last.result, 0);
    CHECK_EQ(tc_u64(&last, 0), uid[0]);
    CHECK_EQ(tc_u64(&last, 8), uid[1]);

    tc_req e = tc_new(1);
    tc_put(&e, uid, 16);
    tc_res ex = tc_send(c, acc, e);
    CHECK_EQ(ex.result, 0);
    CHECK_EQ(ex.data[0], 1);

    tc_req p = tc_new(5);
    tc_put(&p, uid, 16);
    tc_obj prof;
    CHECK_EQ(tc_sub(c, acc, p, &prof), 0);
    CHECK(prof.handle);
    tc_res base = tc_send(c, prof, tc_new(1));
    CHECK_EQ(base.result, 0);
    CHECK_EQ(tc_u64(&base, 0), uid[0]);
    CHECK(base.data[0x18] != 0);

    tc_req z = tc_new(5);
    tc_put(&z, zero, 16);
    CHECK_EQ(tc_send(c, acc, z).result, RL_MAKE_RESULT(124, 20));
}
