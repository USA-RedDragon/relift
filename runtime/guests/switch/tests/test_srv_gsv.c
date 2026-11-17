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
    CHECK_EQ(tipc(c, h, 115, NULL, 0, 2), 0);
    CHECK_EQ(tipc(c, h, 1101, NULL, 0, 0), 0);
    CHECK_EQ(tipc(c, h, 1102, NULL, 0, 0), 0);
    tc_req q = tc_new(497);
    q.tipc = true;
    tc_put64(&q, 4);
    tc_res r = tc_send(c, (tc_obj){ .handle = h }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    uint32_t gpu = r.move[0];
    q = tc_new(11);
    q.tipc = true;
    r = tc_send(c, (tc_obj){ .handle = gpu }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 4);
    CHECK_EQ(tc_u32(&r, 4), 4);
    q = tc_new(12);
    q.tipc = true;
    tc_put32(&q, 4);
    r = tc_send(c, (tc_obj){ .handle = gpu }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    *(uint32_t *)(void *)(t_mem() + heap + 0x800) = r.move[0];
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 1, 0, 0, 0), KR_TIMED_OUT);
    q = tc_new(12);
    q.tipc = true;
    tc_put32(&q, 3);
    CHECK_EQ(tc_send(c, (tc_obj){ .handle = gpu }, q).result, SF_RESULT_NOT_SUPPORTED);
    q = tc_new(21);
    q.tipc = true;
    tc_put32(&q, 4);
    CHECK_EQ(tc_send(c, (tc_obj){ .handle = gpu }, q).result, 0);
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 1, 0, 0, 0), 0);
    q = tc_new(22);
    q.tipc = true;
    r = tc_send(c, (tc_obj){ .handle = gpu }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    CHECK_EQ(tipc(c, h, 401, NULL, 0, 0), 0);
    CHECK_EQ(tipc(c, h, 402, NULL, 0, 0), 0);
    CHECK_EQ(tipc(c, h, 4, &proc, 1, 2), SF_RESULT_INVALID_HEADER);
    CHECK_EQ(tipc(c, h, 999, NULL, 0, 0), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(gsv_syncpt_area_has_the_header_the_sdk_checks)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    rl_ctx *c = t_boot(&cfg);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = supervisor(c, heap);
    CHECK(h != 0);
    uint64_t b = heap + 0x100000;
    uint32_t tm[2];
    CHECK_EQ(t_svc(c, SVC_CREATE_TRANSFER_MEMORY, 0, b, 0x1000, 3, 0, 0), 0);
    tm[0] = (uint32_t)c->x[1];
    CHECK_EQ(t_svc(c, SVC_CREATE_TRANSFER_MEMORY, 0, b + 0x1000, 0x2000, 1, 0, 0), 0);
    tm[1] = (uint32_t)c->x[1];
    tc_req q = tc_new(204);
    q.tipc = true;
    q.ncopy = 2;
    q.copy[0] = tm[0];
    q.copy[1] = tm[1];
    tc_put32(&q, 1);
    tc_put32(&q, 0x1ff);
    tc_put64(&q, 0x1000);
    tc_put64(&q, 0x2000);
    tc_res r = tc_send(c, (tc_obj){ .handle = h }, q);
    CHECK_EQ(r.svc_rc, 0);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.ncopy, 2);
    const uint32_t *hdr = (const uint32_t *)(const void *)(t_mem() + b + 0x1000);
    CHECK_EQ(hdr[0], 1);
    CHECK((hdr[2] + 64) / 64 - 1 < 64);
    CHECK(hdr[3] < 0x2000 && hdr[5] < 0x2000 && hdr[7] < 0x2000);
    CHECK(hdr[4] < 0x1000 && hdr[6] < 0x1000);
}

TEST(gsv_display_session_answers_nn_vi)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    rl_ctx *c = t_boot(&cfg);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = supervisor(c, heap);
    CHECK(h != 0);
    CHECK_EQ(tipc(c, h, 301, NULL, 0, 0), 0);
    tc_req q = tc_new(305);
    q.tipc = true;
    tc_put64(&q, 0x300010000ull);
    tc_res r = tc_send(c, (tc_obj){ .handle = h }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    uint32_t d = r.move[0];
    q = tc_new(3);
    q.tipc = true;
    r = tc_send(c, (tc_obj){ .handle = d }, q);
    CHECK_EQ(r.result, 0);
    CHECK(tc_u32(&r, 0) < 8);
    q = tc_new(1);
    q.tipc = true;
    r = tc_send(c, (tc_obj){ .handle = d }, q);
    CHECK_EQ(r.result, 0);
    CHECK(tc_u32(&r, 0) < 0x400);
    q = tc_new(5);
    q.tipc = true;
    r = tc_send(c, (tc_obj){ .handle = d }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 7);
    q = tc_new(11);
    q.tipc = true;
    tc_put32(&q, 0);
    r = tc_send(c, (tc_obj){ .handle = d }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    q = tc_new(212);
    q.tipc = true;
    tc_put64(&q, 1);
    r = tc_send(c, (tc_obj){ .handle = d }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u64(&r, 0), 0);
    CHECK_EQ(tipc(c, d, 101, NULL, 0, 3), 0);
    CHECK_EQ(tipc(c, d, 51, NULL, 0, 1), 0);
    CHECK_EQ(tipc(c, d, 999, NULL, 0, 0), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(gsv_is_not_on_the_switch)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK_EQ(supervisor(c, heap), 0);
}
