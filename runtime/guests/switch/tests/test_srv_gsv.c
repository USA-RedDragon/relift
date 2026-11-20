/**
 * @file test_srv_gsv.c
 * @brief The Switch 2 graphics supervisor the way nn::gs::sdk::BootGraphicsSystem
 * reaches it: GetGraphicsSupervisor from am, then the boot, the contexts and
 * the INvDrv2User session over TIPC.
 */
#include "am.h"
#include "tc.h"
#include "services.h"

#include <relift/video.h>

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
    tc_req b = tc_new(126);
    b.tipc = true;
    tc_res br = tc_send(c, (tc_obj){ .handle = h }, b);
    CHECK_EQ(br.result, 0);
    CHECK_EQ(tc_u32(&br, 0), 0);
    CHECK_EQ(tipc(c, h, 120, NULL, 0, 0), SF_RESULT_UNKNOWN_COMMAND);
    b = tc_new(1202);
    b.tipc = true;
    br = tc_send(c, (tc_obj){ .handle = h }, b);
    CHECK_EQ(br.result, 0);
    CHECK_EQ(br.nmove, 1);
    uint32_t chat = br.move[0];
    b = tc_new(1);
    b.tipc = true;
    br = tc_send(c, (tc_obj){ .handle = chat }, b);
    CHECK_EQ(br.result, 0);
    CHECK_EQ(tc_u32(&br, 0), 0);
    b = tc_new(11);
    b.tipc = true;
    tc_put32(&b, 0);
    br = tc_send(c, (tc_obj){ .handle = chat }, b);
    CHECK_EQ(br.result, 0);
    CHECK_EQ(br.nmove, 1);
    CHECK_EQ(tipc(c, chat, 103, NULL, 0, 3), 0);
    CHECK_EQ(tipc(c, chat, 103, NULL, 0, 1), SF_RESULT_INVALID_HEADER);
    CHECK_EQ(tipc(c, chat, 2, NULL, 0, 0), SF_RESULT_UNKNOWN_COMMAND);
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
    q = tc_new(31);
    q.tipc = true;
    tc_put32(&q, 0);
    r = tc_send(c, (tc_obj){ .handle = d }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    CHECK_EQ(tipc(c, d, 101, NULL, 0, 3), 0);
    CHECK_EQ(tipc(c, d, 51, NULL, 0, 1), 0);
    CHECK_EQ(tipc(c, d, 999, NULL, 0, 0), SF_RESULT_UNKNOWN_COMMAND);
}

static uint32_t display_session(rl_ctx *c, uint64_t heap)
{
    uint32_t h = supervisor(c, heap);
    if (!h || tipc(c, h, 301, NULL, 0, 0) != 0)
        return 0;
    tc_req q = tc_new(305);
    q.tipc = true;
    tc_put64(&q, 0x300010000ull);
    tc_res r = tc_send(c, (tc_obj){ .handle = h }, q);
    return r.result || r.nmove != 1 ? 0 : r.move[0];
}

static tc_res display_query(rl_ctx *c, uint32_t d, uint32_t cmd, uint64_t name)
{
    tc_req q = tc_new(cmd);
    q.tipc = true;
    tc_put64(&q, name);
    return tc_send(c, (tc_obj){ .handle = d }, q);
}

TEST(gsv_display_change_events_fire_when_bound_and_the_queries_answer)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    rl_ctx *c = t_boot(&cfg);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t d = display_session(c, heap);
    CHECK(d != 0);
    tc_req q = tc_new(31);
    q.tipc = true;
    tc_put32(&q, 5);
    tc_res r = tc_send(c, (tc_obj){ .handle = d }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    *(uint32_t *)(void *)(t_mem() + heap + 0x800) = r.move[0];
    q = tc_new(262);
    q.tipc = true;
    tc_put64(&q, 0x00746c7561666564ull);
    tc_put32(&q, 5);
    CHECK_EQ(tc_send(c, (tc_obj){ .handle = d }, q).result, 0);
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 1, 0, 0, 0), KR_TIMED_OUT);
    q = tc_new(265);
    q.tipc = true;
    tc_put64(&q, 0x00746c7561666564ull);
    tc_put32(&q, 5);
    CHECK_EQ(tc_send(c, (tc_obj){ .handle = d }, q).result, 0);
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 1, 0, 0, 0), 0);
    q = tc_new(31);
    q.tipc = true;
    tc_put32(&q, 16);
    CHECK_EQ(tc_send(c, (tc_obj){ .handle = d }, q).result, SF_RESULT_INVALID_HEADER);

    r = display_query(c, d, 103, 0x00746c7561666564ull);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 60);
    CHECK_EQ(tc_u32(&r, 4), 0);
    r = display_query(c, d, 104, 0x00746c7561666564ull);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u64(&r, 0), 1);
    CHECK_EQ(tc_u32(&r, 8), 400);
    r = display_query(c, d, 105, 0x00746c7561666564ull);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 203);
    r = display_query(c, d, 354, 0x00746c7561666564ull);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 60);
    CHECK_EQ(tc_u32(&r, 4), 0);
    r = display_query(c, d, 107, 0x00746c7561666564ull);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u64(&r, 0), 0);
    CHECK_EQ(tc_u64(&r, 8), 0);
    CHECK_EQ(tc_u64(&r, 16), 0);
}

TEST(gsv_display_buffer_max_resolution_follows_the_operation_mode)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    rl_ctx *c = t_boot(&cfg);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t d = display_session(c, heap);
    CHECK(d != 0);
    tc_res r = display_query(c, d, 106, 0x00746c7561666564ull);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u64(&r, 0), 2);
    rl_am_set_operation_mode(true);
    r = display_query(c, d, 106, 0x00746c7561666564ull);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u64(&r, 0), 4);
    CHECK_EQ(display_query(c, d, 106, 0x6e69617274ull).result, SF_RESULT_NOT_SUPPORTED);
}

TEST(gsv_syncpt_slot_events_fire_when_a_counter_reaches_the_value)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    rl_ctx *c = t_boot(&cfg);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = supervisor(c, heap);
    CHECK(h != 0);
    tc_req q = tc_new(202);
    q.tipc = true;
    tc_res r = tc_send(c, (tc_obj){ .handle = h }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    tc_obj sp = { .handle = r.move[0] };
    uint32_t id = rl_gsv_counter_new();
    CHECK(id >= 192);
    q = tc_new(22);
    q.tipc = true;
    tc_put32(&q, 3);
    r = tc_send(c, sp, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.ncopy + r.nmove, 1);
    *(uint32_t *)(void *)(t_mem() + heap + 0x800) = r.ncopy ? r.copy[0] : r.move[0];
    q = tc_new(201);
    q.tipc = true;
    tc_put32(&q, 3);
    tc_put32(&q, ~id);
    tc_put64(&q, 2);
    tc_put32(&q, 1);
    CHECK_EQ(tc_send(c, sp, q).result, 0);
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 1, 0, 0, 0), KR_TIMED_OUT);
    rl_gsv_counter_signal(id, 1);
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 1, 0, 0, 0), KR_TIMED_OUT);
    rl_gsv_counter_signal(id, 2);
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 1, 0, 0, 0), 0);
    q = tc_new(103);
    q.tipc = true;
    tc_put32(&q, ~id);
    r = tc_send(c, sp, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u64(&r, 0), 2);
    q = tc_new(202);
    q.tipc = true;
    tc_put32(&q, 3);
    CHECK_EQ(tc_send(c, sp, q).result, 0);
}

TEST(gsv_decoder_image_pool_layout_is_nv12_in_128_row_planes)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    rl_ctx *c = t_boot(&cfg);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = supervisor(c, heap);
    CHECK(h != 0);
    uint32_t pref[16] = { 0, 1, 1919u | (1079u << 16) };
    tc_req q = tc_new(991);
    q.tipc = true;
    for (int i = 0; i < 16; i++)
        tc_put32(&q, pref[i]);
    tc_put32(&q, 2);
    tc_res r = tc_send(c, (tc_obj){ .handle = h }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u64(&r, 0), 2ull * (1920ull * 1152 + 1920ull * 640));
    CHECK_EQ(tc_u64(&r, 8), 8192);
    q = tc_new(911);
    q.tipc = true;
    for (int i = 0; i < 16; i++)
        tc_put32(&q, pref[i]);
    tc_put32(&q, 2);
    tc_put32(&q, 1);
    r = tc_send(c, (tc_obj){ .handle = h }, q);
    CHECK_EQ(r.result, 0);
    uint32_t first = tc_u32(&r, 0);
    CHECK(first != 0);
    q = tc_new(961);
    q.tipc = true;
    tc_put32(&q, first + 1);
    r = tc_send(c, (tc_obj){ .handle = h }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.data[0], 10);
    CHECK_EQ(tc_u32(&r, 8), 1920u * 1152 + 1920u * 640);
    CHECK_EQ(tc_u64(&r, 0x10), 1920);
    CHECK_EQ(tc_u64(&r, 0x18), 1920ull * 1152 + 1920ull * 1152 + 1920ull * 640);
    q = tc_new(961);
    q.tipc = true;
    tc_put32(&q, 9999);
    CHECK_EQ(tc_send(c, (tc_obj){ .handle = h }, q).result, SF_RESULT_INVALID_HEADER);
}

TEST(gsv_decoder_images_the_gpu_reads_are_described_by_428_and_426)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    rl_ctx *c = t_boot(&cfg);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = supervisor(c, heap);
    CHECK(h != 0);
    tc_req q = tc_new(111);
    q.tipc = true;
    tc_put32(&q, 0);
    tc_put64(&q, 0x100000);
    tc_res r = tc_send(c, (tc_obj){ .handle = h }, q);
    CHECK_EQ(r.result, 0);
    uint32_t mem = tc_u32(&r, 0);
    CHECK(mem != 0);
    q = tc_new(114);
    q.tipc = true;
    tc_put32(&q, mem);
    tc_put64(&q, heap + 0x10000);
    tc_put64(&q, 0x100000);
    tc_put64(&q, 0);
    tc_put64(&q, 0);
    CHECK_EQ(tc_send(c, (tc_obj){ .handle = h }, q).result, 0);
    uint32_t pref[16] = { 0, 0, 255u | (127u << 16) };
    q = tc_new(911);
    q.tipc = true;
    for (int i = 0; i < 16; i++)
        tc_put32(&q, pref[i]);
    tc_put32(&q, 2);
    tc_put32(&q, mem);
    r = tc_send(c, (tc_obj){ .handle = h }, q);
    CHECK_EQ(r.result, 0);
    uint32_t first = tc_u32(&r, 0);
    q = tc_new(426);
    q.tipc = true;
    tc_put32(&q, first + 1);
    r = tc_send(c, (tc_obj){ .handle = h }, q);
    CHECK_EQ(r.result, 0);
    uint64_t size = 256ull * 4 * 128;
    CHECK_EQ(tc_u64(&r, 0), heap + 0x10000 + size);
    CHECK_EQ(tc_u64(&r, 8), heap + 0x10000 + 2 * size - 1);
    CHECK_EQ(tc_u32(&r, 16), 1);
    CHECK_EQ(tc_u32(&r, 24), 255u | (127u << 16));
    CHECK_EQ(tc_u32(&r, 48), 0x01000001);
    CHECK_EQ(tc_u32(&r, 52), 1);
    CHECK_EQ(tc_u32(&r, 56), 0);
    CHECK_EQ(tc_u32(&r, 60), size);
    q = tc_new(428);
    q.tipc = true;
    tc_put32(&q, first + 1);
    r = tc_send(c, (tc_obj){ .handle = h }, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
    CHECK_EQ(tc_u32(&r, 56), size);
    uint64_t addr, len;
    CHECK(rl_gsv_native_image_region(first + 1, &addr, &len));
    CHECK_EQ(addr, heap + 0x10000 + size);
    CHECK_EQ(len, size);
    CHECK(!rl_gsv_native_image_region(9999, &addr, &len));
    q = tc_new(912);
    q.tipc = true;
    tc_put32(&q, first + 1);
    CHECK_EQ(tc_send(c, (tc_obj){ .handle = h }, q).result, 0);
    CHECK(!rl_gsv_native_image_region(first + 1, &addr, &len));
}

TEST(gsv_decoded_pixels_convert_with_bt709_limited_range_in_the_requested_order)
{
    uint8_t px[4];
    rl_gsv_yuv_to_rgba(63, 102, 240, false, px);
    CHECK(px[0] >= 253 && px[1] <= 2 && px[2] <= 2 && px[3] == 255);
    rl_gsv_yuv_to_rgba(63, 102, 240, true, px);
    CHECK(px[2] >= 253 && px[1] <= 2 && px[0] <= 2 && px[3] == 255);
    rl_gsv_yuv_to_rgba(16, 128, 128, false, px);
    CHECK(px[0] == 0 && px[1] == 0 && px[2] == 0);
    rl_gsv_yuv_to_rgba(235, 128, 128, false, px);
    CHECK(px[0] >= 254 && px[1] >= 254 && px[2] >= 254);
    rl_gsv_yuv_to_rgba(32, 240, 118, false, px);
    CHECK(px[2] > px[0] && px[2] > px[1]);
}

TEST(chat_u_gives_its_shim_and_event)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    rl_ctx *c = t_boot(&cfg);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj svc = tc_open_service(c, heap, "chat:u");
    CHECK(svc.handle != 0);
    CHECK_EQ(tc_to_domain(c, &svc), 0);
    tc_obj shim;
    CHECK_EQ(tc_sub(c, svc, tc_new(0), &shim), 0);
    tc_res r = tc_send(c, shim, tc_new(1));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.ncopy, 1);
    *(uint32_t *)(void *)(t_mem() + heap + 0x800) = r.copy[0];
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 1, 0, 0, 0), KR_TIMED_OUT);
    CHECK_EQ(tc_cmd(c, shim, 2), SF_RESULT_UNKNOWN_COMMAND);
    tc_req q = tc_new(23);
    tc_put32(&q, 1);
    CHECK_EQ(tc_send(c, svc, q).result, 0);
}

TEST(gsv_is_not_on_the_switch)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK_EQ(supervisor(c, heap), 0);
    CHECK_EQ(tc_open_service_result(c, heap, "chat:u"), SM_RESULT_NOT_REGISTERED);
}

static uint8_t g_pic[16 * 16 * 3 / 2];

static void *fake_open(void) { return g_pic; }

static void fake_close(void *d) { (void)d; }

static int fake_decode(void *d, const uint8_t *data, size_t size, rl_video_image *out)
{
    (void)d;
    (void)data;
    (void)size;
    *out = (rl_video_image){ .width = 16, .height = 16, .y = g_pic, .u = g_pic + 256, .v = g_pic + 320, .y_stride = 16,
                             .uv_stride = 8 };
    return 1;
}

static const rl_vp9_stream_backend g_fake = { "fake", fake_open, fake_close, fake_decode };

TEST(gsv_video_end_of_stream_reaches_the_client_when_the_frame_ring_is_full)
{
    rl_vp9_stream_backend_override(&g_fake);
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    rl_ctx *c = t_boot(&cfg);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = supervisor(c, heap);
    CHECK(h != 0);
    tc_res r = tc_send(c, (tc_obj){ .handle = h }, (tc_req){ .cmd = 906, .tipc = true });
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    tc_obj vs = { .handle = r.move[0] };
    memcpy(t_mem() + heap + 0x100, "video/vp9", 10);
    tc_req q = tc_new(101);
    q.tipc = true;
    q.na = 1;
    q.a_addr[0] = heap + 0x100;
    q.a_size[0] = 10;
    tc_put32(&q, 16);
    tc_put32(&q, 16);
    tc_put32(&q, 60);
    CHECK_EQ(tc_send(c, vs, q).result, 0);
    for (uint32_t i = 0; i < 20; i++) {
        q = tc_new(109);
        q.tipc = true;
        q.na = 1;
        q.a_addr[0] = heap + 0x200;
        q.a_size[0] = 4;
        tc_put64(&q, 1000 * i);
        uint8_t end = i == 19;
        tc_put(&q, &end, 1);
        CHECK_EQ(tc_send(c, vs, q).result, 0);
    }
    uint32_t frames = 0;
    bool end_seen = false;
    for (uint32_t i = 0; i < 40 && !end_seen; i++) {
        q = tc_new(311);
        q.tipc = true;
        tc_put32(&q, 1 + i % 2);
        CHECK_EQ(tc_send(c, vs, q).result, 0);
        q = tc_new(213);
        q.tipc = true;
        r = tc_send(c, vs, q);
        CHECK_EQ(r.result, 0);
        end_seen = tc_u32(&r, 32) == 1;
        frames++;
    }
    CHECK(end_seen);
    CHECK_EQ(frames, 17);
    rl_vp9_stream_backend_override(NULL);
}
