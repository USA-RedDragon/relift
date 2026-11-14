/**
 * @file test_srv_pointer_buffer.c
 * @brief Every registered service answers QueryPointerBufferSize with a
 * nonzero size, since the SDK sends X buffers through the pointer buffer.
 */
#include "tc.h"

#include <string.h>

TEST(every_service_has_a_pointer_buffer)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t sm = tc_connect_sm(c, heap);
    CHECK(sm);
    CHECK_EQ(tc_sm_register(c, sm, false), 0);
    uint32_t n = hos_ipc_service_count();
    CHECK(n > 10);
    for (uint32_t i = 0; i < n; i++) {
        char name[9] = { 0 };
        uint64_t v = hos_ipc_service_name(i);
        memcpy(name, &v, 8);
        tc_res s = tc_sm_get(c, sm, name, false);
        CHECK_EQ(s.result, 0);
        CHECK_EQ(s.nmove, 1);
        tc_req q = tc_new(3);
        q.type = HIPC_CONTROL;
        tc_res r = tc_call(c, s.move[0], &q, false);
        CHECK_EQ(r.result, 0);
        uint16_t pbs;
        memcpy(&pbs, r.data, 2);
        if (pbs == 0)
            fprintf(stderr, "service %s answers pointer buffer size 0\n", name);
        CHECK(pbs != 0);
        tc_close(c, s.move[0]);
    }
}
