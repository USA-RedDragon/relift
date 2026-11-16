/**
 * @file test_srv_gsv.c
 * @brief The Switch 2 graphics supervisor the way nn::gs::sdk::BootGraphicsSystem
 * reaches it: GetGraphicsSupervisor from am, then the boot, the contexts and
 * the INvDrv2User session over TIPC.
 */
#include "am.h"
#include "tc.h"

#define CUR_PROCESS_HANDLE 0xFFFF8001u

/** @brief A TIPC request on session h with the copy handles and the data words; returns the result. */
static uint32_t tipc(rl_ctx *c, uint32_t h, uint32_t cmd, const uint32_t *copy, uint32_t ncopy, uint32_t ndata)
{
    tc_req q = tc_new(cmd);
    q.tipc = true;
    q.ncopy = ncopy;
    for (uint32_t i = 0; i < ncopy; i++)
        q.copy[i] = copy[i];
    for (uint32_t i = 0; i < ndata; i++)
        tc_put32(&q, 0);
    tc_res r = tc_send(c, (tc_obj){ .handle = h }, q);
    return r.svc_rc ? r.svc_rc : r.result;
}

static uint32_t supervisor(rl_ctx *c, uint64_t heap)
{
    tc_obj svc = tc_open_service(c, heap, "appletOE");
    if (!svc.handle || tc_to_domain(c, &svc) != 0)
        return 0;
    tc_req q = tc_new(10);
    q.send_pid = true;
    q.ncopy = 1;
    q.copy[0] = CUR_PROCESS_HANDLE;
    tc_put64(&q, 0);
    tc_obj proxy, sc;
    if (tc_sub(c, svc, q, &proxy) != 0 || tc_sub(c, proxy, tc_new(1), &sc) != 0)
        return 0;
    tc_res r = tc_send(c, sc, tc_new(2000));
    if (r.svc_rc || r.result || r.ncopy != 1)
        return 0;
    return r.copy[0];
}

TEST(gsv_boots_and_opens_nvdrv2)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    rl_ctx *c = t_boot(&cfg);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = supervisor(c, heap);
    CHECK(h != 0);
    uint32_t proc = CUR_PROCESS_HANDLE;
    CHECK_EQ(tipc(c, h, 4, &proc, 1, 12), 0);
    CHECK_EQ(tipc(c, h, 11, &proc, 1, 0), 0);
    CHECK_EQ(tipc(c, h, 101, NULL, 0, 0), 0);
    CHECK_EQ(tipc(c, h, 201, NULL, 0, 0), 0);
    CHECK_EQ(tipc(c, h, 202, NULL, 0, 0), 0);
    CHECK_EQ(tipc(c, h, 1402, NULL, 0, 0), 0);
    CHECK_EQ(tipc(c, h, 4, &proc, 1, 2), SF_RESULT_INVALID_HEADER);
    CHECK_EQ(tipc(c, h, 999, NULL, 0, 0), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(gsv_is_not_on_the_switch)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK_EQ(supervisor(c, heap), 0);
}
