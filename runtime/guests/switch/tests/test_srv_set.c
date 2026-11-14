/**
 * @file test_srv_set.c
 * @brief The settings services driven through IPC the way libnx's
 * setInitialize, setsysInitialize and setsysGetFirmwareVersion use them,
 * with the structure layouts of libnx set.h checked byte by byte.
 */
#include "tc.h"

#define SET_RC(desc) (105u | ((desc) << 9))
#define LANG_EN_US 0x53552D6E65ull
#define LANG_JA 0x616Aull
#define LANG_ZH_HANS 0x736E61482D687Aull

typedef struct env {
    rl_ctx *c;
    uint64_t heap;
} env;

static env boot(void)
{
    env e;
    e.c = t_boot(NULL);
    e.heap = t_heap(e.c, 0x200000);
    return e;
}

static uint32_t rc_of(tc_res r) { return r.svc_rc ? r.svc_rc : r.result; }

static const uint8_t *res_raw(rl_ctx *c)
{
    const uint32_t *w = (const uint32_t *)(const void *)(t_mem() + c->tpidrro_el0);
    uint32_t pos = 2;
    if (w[1] >> 31) {
        uint32_t sp = w[pos++];
        if (sp & 1)
            pos += 2;
        pos += ((sp >> 1) & 0xF) + ((sp >> 5) & 0xF);
    }
    return (const uint8_t *)w + ((pos * 4 + 15) & ~15u) + 16;
}

static tc_res call(env *e, tc_obj o, uint32_t cmd, const void *in, uint32_t n)
{
    tc_req q = tc_new(cmd);
    if (n)
        tc_put(&q, in, n);
    return tc_send(e->c, o, q);
}

static tc_res call_c(env *e, tc_obj o, uint32_t cmd, const void *in, uint32_t n, uint64_t buf, uint64_t size)
{
    tc_req q = tc_new(cmd);
    if (n)
        tc_put(&q, in, n);
    q.nc = 1;
    q.c_addr[0] = buf;
    q.c_size[0] = size;
    return tc_send(e->c, o, q);
}

static tc_res call_b(env *e, tc_obj o, uint32_t cmd, const void *in, uint32_t n, uint64_t buf, uint64_t size)
{
    tc_req q = tc_new(cmd);
    if (n)
        tc_put(&q, in, n);
    q.nb = 1;
    q.b_addr[0] = buf;
    q.b_size[0] = size;
    return tc_send(e->c, o, q);
}

static tc_res call_a(env *e, tc_obj o, uint32_t cmd, const void *in, uint32_t n, uint64_t buf, uint64_t size)
{
    tc_req q = tc_new(cmd);
    if (n)
        tc_put(&q, in, n);
    q.na = 1;
    q.a_addr[0] = buf;
    q.a_size[0] = size;
    return tc_send(e->c, o, q);
}

static uint64_t u64_at(const uint8_t *p, uint32_t off)
{
    uint64_t v;
    memcpy(&v, p + off, 8);
    return v;
}

TEST(set_and_sys_appear_through_sm)
{
    env e = boot();
    const char *names[] = { "set", "set:sys", "set:cal", "set:fd" };
    for (uint32_t i = 0; i < 4; i++)
        CHECK_EQ(tc_open_service_result(e.c, e.heap, names[i]), 0);
    CHECK_EQ(tc_open_service_result(e.c, e.heap, "set:xyz"), SM_RESULT_NOT_REGISTERED);
    CHECK_EQ(tc_open_service_result(e.c, e.heap, "nope"), SM_RESULT_NOT_REGISTERED);
    tc_obj cal = tc_open_service(e.c, e.heap, "set:cal");
    CHECK(cal.handle);
    CHECK_EQ(tc_cmd(e.c, cal, 0), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(set_language_codes)
{
    env e = boot();
    tc_obj s = tc_open_service(e.c, e.heap, "set");
    CHECK(s.handle);

    tc_res r = call(&e, s, 0, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), LANG_EN_US);

    r = call(&e, s, 3, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 15);
    r = call(&e, s, 6, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 18);

    uint64_t buf = e.heap + 0x1000;
    r = call_b(&e, s, 5, NULL, 0, buf, 0x40 * 8);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 18);
    uint64_t codes[18];
    memcpy(codes, t_mem() + buf, sizeof codes);
    CHECK_EQ(codes[0], LANG_JA);
    CHECK_EQ(codes[1], LANG_EN_US);
    CHECK_EQ(codes[15], LANG_ZH_HANS);

    r = call_b(&e, s, 5, NULL, 0, buf + 0x800, 3 * 8);
    CHECK_EQ(tc_u32(&r, 0), 3);

    memset(t_mem() + buf, 0, 0x400);
    r = call_c(&e, s, 1, NULL, 0, buf, 0x40 * 8);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 15);
    memcpy(codes, t_mem() + buf, sizeof codes);
    CHECK_EQ(codes[1], LANG_EN_US);
    CHECK_EQ(u64_at(t_mem() + buf, 15 * 8), 0);

    uint32_t idx = 15;
    r = call(&e, s, 2, &idx, 4);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), LANG_ZH_HANS);
    idx = 18;
    CHECK_EQ(rc_of(call(&e, s, 2, &idx, 4)), SET_RC(625));
    idx = 0xFFFFFFFFu;
    CHECK_EQ(rc_of(call(&e, s, 2, &idx, 4)), SET_RC(625));
}

TEST(set_region_quest_and_nickname)
{
    env e = boot();
    tc_obj s = tc_open_service(e.c, e.heap, "set");
    CHECK(s.handle);
    tc_res r = call(&e, s, 4, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    r = call(&e, s, 8, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.data[0], 0);

    uint64_t buf = e.heap + 0x2000;
    memset(t_mem() + buf, 0xAA, 0x80);
    r = call_b(&e, s, 11, NULL, 0, buf, 0x80);
    CHECK_EQ(rc_of(r), 0);
    CHECK(memcmp(t_mem() + buf, "relift", 7) == 0);
    CHECK_EQ(t_mem()[buf + 0x7F], 0);
    CHECK_EQ(rc_of(call(&e, s, 11, NULL, 0)), SET_RC(808));
    CHECK_EQ(rc_of(call(&e, s, 7, NULL, 0)), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(libnx_set_init_sequence)
{
    env e = boot();
    tc_obj s = tc_open_service(e.c, e.heap, "set");
    CHECK(s.handle);
    uint64_t buf = e.heap + 0x3000;
    tc_res r = call_b(&e, s, 5, NULL, 0, buf, 0x40 * sizeof(uint64_t));
    CHECK_EQ(rc_of(r), 0);
    int32_t total = (int32_t)tc_u32(&r, 0);
    CHECK_EQ(total, 18);
    uint64_t codes[0x40];
    memcpy(codes, t_mem() + buf, sizeof codes);
    int32_t found = -1;
    for (int32_t i = 0; i < total; i++)
        if (codes[i] == LANG_EN_US)
            found = i;
    CHECK_EQ(found, 1);
    r = call(&e, s, 0, NULL, 0);
    CHECK_EQ(tc_u64(&r, 0), codes[found]);
}

TEST(sys_firmware_version)
{
    env e = boot();
    tc_obj s = tc_open_service(e.c, e.heap, "set:sys");
    CHECK(s.handle);
    uint64_t buf = e.heap + 0x4000;
    for (uint32_t cmd = 3; cmd <= 4; cmd++) {
        memset(t_mem() + buf, 0xEE, 0x100);
        tc_res r = call_c(&e, s, cmd, NULL, 0, buf, 0x100);
        CHECK_EQ(rc_of(r), 0);
        const uint8_t *v = t_mem() + buf;
        CHECK_EQ(v[0], 19);
        CHECK_EQ(v[1], 0);
        CHECK_EQ(v[2], 0);
        CHECK_EQ(v[3], 0);
        CHECK_EQ(v[4], 1);
        CHECK_EQ(v[5], 0);
        CHECK_EQ(v[6], 0);
        CHECK_EQ(v[7], 0);
        CHECK(strcmp((const char *)v + 8, "NX") == 0);
        CHECK_EQ(v[8 + 3], 0);
        CHECK_EQ(strlen((const char *)v + 0x28), 40);
        CHECK(strcmp((const char *)v + 0x68, "19.0.0") == 0);
        CHECK(strcmp((const char *)v + 0x80, "NintendoSDK Firmware for NX 19.0.0-1.0") == 0);
        CHECK_EQ(v[0xFF], 0);
    }
    CHECK_EQ(rc_of(call(&e, s, 4, NULL, 0)), KR_INVALID_MEM_STATE);
}

TEST(sys_serial_digest_and_nickname)
{
    env e = boot();
    tc_obj s = tc_open_service(e.c, e.heap, "set:sys");
    CHECK(s.handle);
    tc_res r = call(&e, s, 68, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK(memcmp(r.data, "RLF00000000000", 14) == 0);
    CHECK_EQ(r.data[14], 0);
    CHECK_EQ(r.data[23], 0);

    r = call(&e, s, 5, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    const uint8_t *raw = res_raw(e.c);
    CHECK(memcmp(raw, "relift", 6) == 0);

    uint64_t buf = e.heap + 0x5000;
    r = call_b(&e, s, 77, NULL, 0, buf, 0x80);
    CHECK_EQ(rc_of(r), 0);
    CHECK(strcmp((const char *)t_mem() + buf, "relift") == 0);
    memset(t_mem() + buf, 0, 0x80);
    memcpy(t_mem() + buf, "Handheld", 9);
    CHECK_EQ(rc_of(call_a(&e, s, 78, NULL, 0, buf, 0x80)), 0);
    memset(t_mem() + buf, 0, 0x80);
    r = call_b(&e, s, 77, NULL, 0, buf, 0x80);
    CHECK(strcmp((const char *)t_mem() + buf, "Handheld") == 0);
    r = call_b(&e, tc_open_service(e.c, e.heap, "set"), 11, NULL, 0, buf, 0x80);
    CHECK(strcmp((const char *)t_mem() + buf, "Handheld") == 0);
}

TEST(sys_shared_language_region_and_quest)
{
    env e = boot();
    tc_obj set = tc_open_service(e.c, e.heap, "set");
    tc_obj sys = tc_open_service(e.c, e.heap, "set:sys");
    CHECK(set.handle && sys.handle);

    uint64_t ja = LANG_JA;
    CHECK_EQ(rc_of(call(&e, sys, 0, &ja, 8)), 0);
    tc_res r = call(&e, set, 0, NULL, 0);
    CHECK_EQ(tc_u64(&r, 0), LANG_JA);

    uint32_t region = 2;
    CHECK_EQ(rc_of(call(&e, sys, 57, &region, 4)), 0);
    r = call(&e, set, 4, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 2);

    uint8_t flag = 1;
    CHECK_EQ(rc_of(call(&e, sys, 48, &flag, 1)), 0);
    r = call(&e, set, 8, NULL, 0);
    CHECK_EQ(r.data[0], 1);
    r = call(&e, sys, 47, NULL, 0);
    CHECK_EQ(r.data[0], 1);
}

TEST(sys_flags_and_scalars)
{
    env e = boot();
    tc_obj s = tc_open_service(e.c, e.heap, "set:sys");
    CHECK(s.handle);

    tc_res r = call(&e, s, 7, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.data[0], 0);
    uint8_t one = 1;
    CHECK_EQ(rc_of(call(&e, s, 8, &one, 1)), 0);
    r = call(&e, s, 7, NULL, 0);
    CHECK_EQ(r.data[0], 1);

    r = call(&e, s, 60, NULL, 0);
    CHECK_EQ(r.data[0], 1);
    r = call(&e, s, 62, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.data[0], 0);
    CHECK_EQ(rc_of(call(&e, s, 63, NULL, 0)), 0);
    CHECK_EQ(rc_of(call(&e, s, 61, &one, 1)), 0);

    uint32_t color = 1;
    CHECK_EQ(rc_of(call(&e, s, 24, &color, 4)), 0);
    r = call(&e, s, 23, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    r = call(&e, s, 183, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    r = call(&e, s, 79, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 1);

    uint64_t rtc = 0x1122334455667788ull;
    CHECK_EQ(rc_of(call(&e, s, 91, &rtc, 8)), 0);
    r = call(&e, s, 92, NULL, 0);
    CHECK_EQ(tc_u64(&r, 0), rtc);
    r = call(&e, s, 51, NULL, 0);
    CHECK_EQ(tc_u64(&r, 0), 0x0100000000001000ull);
    r = call(&e, s, 52, NULL, 0);
    CHECK_EQ(tc_u64(&r, 0), 0x010000000000100Cull);

    float vol;
    r = call(&e, s, 35, NULL, 0);
    memcpy(&vol, r.data, 4);
    CHECK(vol == 1.0f);
    vol = 0.5f;
    CHECK_EQ(rc_of(call(&e, s, 36, &vol, 4)), 0);
    r = call(&e, s, 35, NULL, 0);
    memcpy(&vol, r.data, 4);
    CHECK(vol == 0.5f);

    uint8_t ptm = 7;
    CHECK_EQ(rc_of(call(&e, s, 163, &ptm, 1)), 0);
    r = call(&e, s, 162, NULL, 0);
    CHECK_EQ(r.data[0], 7);
}

TEST(sys_struct_settings_roundtrip)
{
    env e = boot();
    tc_obj s = tc_open_service(e.c, e.heap, "set:sys");
    CHECK(s.handle);
    static const struct {
        uint32_t get, set, size;
    } t[] = {
        { 9, 10, 40 },   { 107, 108, 44 }, { 17, 18, 4 },   { 29, 30, 24 },  { 39, 40, 32 },  { 49, 50, 8 },
        { 71, 72, 12 },  { 75, 76, 32 },   { 84, 85, 24 },  { 86, 87, 24 },  { 128, 129, 12 }, { 130, 131, 12 },
        { 132, 133, 36 }, { 134, 135, 36 }, { 145, 146, 12 }, { 147, 148, 36 }, { 158, 159, 16 }, { 160, 161, 16 },
        { 175, 176, 8 }, { 177, 178, 16 }, { 13, 14, 16 },  { 15, 16, 32 },  { 58, 59, 32 },  { 53, 54, 0x24 },
        { 150, 151, 24 }, { 152, 153, 24 },
    };
    for (uint32_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        uint8_t in[64];
        for (uint32_t k = 0; k < t[i].size; k++)
            in[k] = (uint8_t)(0x40 + i + k);
        CHECK_EQ(rc_of(call(&e, s, t[i].set, in, t[i].size)), 0);
        tc_res r = call(&e, s, t[i].get, NULL, 0);
        CHECK_EQ(rc_of(r), 0);
        CHECK(memcmp(res_raw(e.c), in, t[i].size) == 0);
    }
    CHECK_EQ(rc_of(call(&e, s, 90, NULL, 0)), 0);
    CHECK(memcmp(res_raw(e.c), "relift", 6) == 0);
    CHECK_EQ(rc_of(call(&e, s, 67, NULL, 0)), 0);
    CHECK_EQ(rc_of(call(&e, s, 149, NULL, 0)), 0);
    CHECK_EQ(rc_of(call(&e, s, 174, NULL, 0)), 0);
}

TEST(sys_default_time_zone_name)
{
    env e = boot();
    tc_obj s = tc_open_service(e.c, e.heap, "set:sys");
    CHECK(s.handle);
    tc_res r = call(&e, s, 53, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK(strcmp((const char *)res_raw(e.c), "UTC") == 0);
}

TEST(sys_audio_volume_and_mode)
{
    env e = boot();
    tc_obj s = tc_open_service(e.c, e.heap, "set:sys");
    CHECK(s.handle);
    uint8_t set[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 1, 0, 0, 0 };
    CHECK_EQ(rc_of(call(&e, s, 20, set, 12)), 0);
    uint32_t device = 1;
    tc_res r = call(&e, s, 19, &device, 4);
    CHECK_EQ(rc_of(r), 0);
    CHECK(memcmp(r.data, set, 8) == 0);
    device = 0;
    r = call(&e, s, 19, &device, 4);
    CHECK_EQ(tc_u64(&r, 0), 0);
    device = 3;
    CHECK_EQ(rc_of(call(&e, s, 19, &device, 4)), SET_RC(1));

    uint32_t target = 2;
    r = call(&e, s, 43, &target, 4);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    uint32_t mode[2] = { 2, 5 };
    CHECK_EQ(rc_of(call(&e, s, 44, mode, 8)), 0);
    r = call(&e, s, 43, &target, 4);
    CHECK_EQ(tc_u32(&r, 0), 5);
    target = 4;
    CHECK_EQ(rc_of(call(&e, s, 43, &target, 4)), SET_RC(687));
}

TEST(sys_edid_and_host_fs)
{
    env e = boot();
    tc_obj s = tc_open_service(e.c, e.heap, "set:sys");
    CHECK(s.handle);
    uint64_t buf = e.heap + 0x8000;
    for (uint32_t i = 0; i < 512; i++)
        t_mem()[buf + i] = (uint8_t)(i * 7);
    CHECK_EQ(rc_of(call_a(&e, s, 42, NULL, 0, buf, 512)), 0);
    memset(t_mem() + buf, 0, 512);
    CHECK_EQ(rc_of(call_c(&e, s, 41, NULL, 0, buf, 512)), 0);
    for (uint32_t i = 0; i < 512; i++)
        CHECK_EQ(t_mem()[buf + i], (uint8_t)(i * 7));
    CHECK_EQ(rc_of(call_a(&e, s, 42, NULL, 0, buf, 100)), KR_INVALID_MEM_STATE);

    memset(t_mem() + buf, 0x55, 256);
    CHECK_EQ(rc_of(call_b(&e, s, 140, NULL, 0, buf, 256)), 0);
    CHECK_EQ(t_mem()[buf], 0);
    CHECK_EQ(t_mem()[buf + 255], 0);
}

static tc_req item_req(uint32_t cmd, uint64_t name, uint64_t key, const char *n, const char *k)
{
    tc_req q = tc_new(cmd);
    memset(t_mem() + name, 0, 0x48);
    memset(t_mem() + key, 0, 0x48);
    memcpy(t_mem() + name, n, strlen(n) + 1);
    memcpy(t_mem() + key, k, strlen(k) + 1);
    q.nx = 2;
    q.x_addr[0] = name;
    q.x_size[0] = 0x40;
    q.x_addr[1] = key;
    q.x_size[1] = 0x40;
    return q;
}

TEST(sys_settings_items)
{
    env e = boot();
    tc_obj s = tc_open_service(e.c, e.heap, "set:sys");
    CHECK(s.handle);
    uint64_t name = e.heap + 0x9000, key = e.heap + 0x9100, out = e.heap + 0x9200;

    tc_res r = tc_send(e.c, s, item_req(37, name, key, "time", "standard_user_clock_initial_year"));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 4);

    tc_req q = item_req(38, name, key, "time", "standard_user_clock_initial_year");
    q.nb = 1;
    q.b_addr[0] = out;
    q.b_size[0] = 4;
    r = tc_send(e.c, s, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 4);
    uint32_t year;
    memcpy(&year, t_mem() + out, 4);
    CHECK_EQ(year, 2019);

    q = item_req(38, name, key, "settings_debug", "is_debug_mode_enabled");
    q.nb = 1;
    q.b_addr[0] = out;
    q.b_size[0] = 1;
    t_mem()[out] = 0xFF;
    r = tc_send(e.c, s, q);
    CHECK_EQ(tc_u64(&r, 0), 1);
    CHECK_EQ(t_mem()[out], 0);

    r = tc_send(e.c, s, item_req(37, name, key, "no", "such_item"));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 0);
}

TEST(sys_dirty_flag_events)
{
    env e = boot();
    tc_obj s = tc_open_service(e.c, e.heap, "set:sys");
    CHECK(s.handle);
    uint32_t cmds[2] = { 82, 93 };
    for (uint32_t i = 0; i < 2; i++) {
        tc_res r = call(&e, s, cmds[i], NULL, 0);
        CHECK_EQ(rc_of(r), 0);
        CHECK_EQ(r.ncopy, 1);
        *(uint32_t *)(void *)(t_mem() + e.heap + 0x800) = r.copy[0];
        CHECK_EQ(t_svc(e.c, SVC_WAIT_SYNCHRONIZATION, 0, e.heap + 0x800, 1, 0, 0, 0), KR_TIMED_OUT);
        tc_res f = call(&e, s, cmds[i] + 1, NULL, 0);
        CHECK_EQ(rc_of(f), 0);
        const uint8_t *raw = res_raw(e.c);
        CHECK_EQ(u64_at(raw, 0), 0);
        CHECK_EQ(u64_at(raw, 8), 0);
    }
}

TEST(sys_absent_commands_are_unknown)
{
    env e = boot();
    tc_obj s = tc_open_service(e.c, e.heap, "set:sys");
    CHECK(s.handle);
    CHECK_EQ(rc_of(call(&e, s, 1, NULL, 0)), SF_RESULT_UNKNOWN_COMMAND);
    CHECK_EQ(rc_of(call(&e, s, 2, NULL, 0)), SF_RESULT_UNKNOWN_COMMAND);
    CHECK_EQ(rc_of(call(&e, s, 206, NULL, 0)), SF_RESULT_UNKNOWN_COMMAND);
    CHECK_EQ(rc_of(call(&e, s, 9999, NULL, 0)), SF_RESULT_UNKNOWN_COMMAND);
}
