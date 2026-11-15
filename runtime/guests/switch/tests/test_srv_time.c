/**
 * @file test_srv_time.c
 * @brief The time services driven through IPC the way libnx's timeInitialize
 * and its time readers use them: the five static services, the clocks, the
 * shared memory page read with libnx's lockless reader, and the time zone
 * service with the UTC rule, the host's zone and TZif data a program supplies.
 */
#include "tc.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define TIME_RC(desc) (116u | ((desc) << 9))
#define SHM_STEADY 0x00u
#define SHM_LOCAL 0x38u
#define SHM_NETWORK 0x80u
#define RULE_SIZE 0x4000u
#define RTC_EPOCH 946684800ll

typedef struct env {
    rl_ctx *c;
    uint64_t heap;
} env;

typedef struct clock_ctx {
    int64_t offset;
    int64_t time_point;
    uint8_t source[16];
} clock_ctx;

typedef struct cal {
    uint16_t year;
    uint8_t month, day, hour, minute, second, pad;
    uint32_t wday, yday;
    char name[8];
    uint32_t dst;
    int32_t gmt_offset;
} cal;

_Static_assert(sizeof(cal) == 0x20, "libnx TimeCalendarTime + TimeCalendarAdditionalInfo");

static void set_tz(const char *tz)
{
#ifdef _WIN32
    _putenv_s("TZ", tz);
#else
    setenv("TZ", tz, 1);
#endif
}

static env boot_in(const char *tz)
{
    set_tz(tz);
    env e;
    e.c = t_boot(NULL);
    e.heap = t_heap(e.c, 0x200000);
    return e;
}

static env boot(void) { return boot_in("UTC"); }

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

static tc_res call_buf(env *e, tc_obj o, uint32_t cmd, const void *in, uint32_t n, char kind, uint64_t buf,
                       uint64_t size)
{
    tc_req q = tc_new(cmd);
    if (n)
        tc_put(&q, in, n);
    if (kind == 'a') {
        q.na = 1;
        q.a_addr[0] = buf;
        q.a_size[0] = size;
    } else if (kind == 'b') {
        q.nb = 1;
        q.b_addr[0] = buf;
        q.b_size[0] = size;
    } else {
        q.nc = 1;
        q.c_addr[0] = buf;
        q.c_size[0] = size;
    }
    return tc_send(e->c, o, q);
}

static tc_obj sub(env *e, tc_obj o, uint32_t cmd)
{
    tc_obj out = { 0, 0, false };
    if (tc_sub(e->c, o, tc_new(cmd), &out) != 0)
        out.handle = 0;
    return out;
}

static int64_t now_wall(void) { return (int64_t)time(NULL); }

static bool near(int64_t a, int64_t b, int64_t slack) { return a - b <= slack && b - a <= slack; }

static void shm_read(const uint8_t *shm, uint32_t off, void *out, uint32_t size)
{
    uint32_t counter;
    memcpy(&counter, shm + off, 4);
    memcpy(out, shm + off + 8 + (counter & 1u) * size, size);
}

static int64_t armTicksToNs(uint64_t t) { return (int64_t)(t * 10000 / 192); }

typedef struct libnx_time {
    tc_obj srv, user, network, steady, zone, local;
    uint32_t shm_handle;
    const uint8_t *shm;
} libnx_time;

static bool time_initialize(env *e, const char *name, libnx_time *t)
{
    t->srv = tc_open_service(e->c, e->heap, name);
    if (!t->srv.handle)
        return false;
    t->user = sub(e, t->srv, 0);
    t->network = sub(e, t->srv, 1);
    t->steady = sub(e, t->srv, 2);
    t->zone = sub(e, t->srv, 3);
    t->local = sub(e, t->srv, 4);
    if (!t->user.handle || !t->network.handle || !t->steady.handle || !t->zone.handle || !t->local.handle)
        return false;
    tc_res r = call(e, t->srv, 20, NULL, 0);
    if (rc_of(r) != 0 || r.ncopy != 1)
        return false;
    t->shm_handle = r.copy[0];
    static uint64_t mapped;
    uint64_t at = t_free_map_addr() + (mapped % 8) * 0x10000 + 0x1000;
    mapped++;
    if (t_svc(e->c, SVC_MAP_SHARED_MEMORY, t->shm_handle, at, 0x1000, HOS_PERM_R, 0, 0) != 0)
        return false;
    t->shm = t_mem() + at;
    return true;
}

static int64_t shm_steady_seconds(env *e, const libnx_time *t, uint8_t source[16])
{
    struct {
        int64_t base_time;
        uint8_t source[16];
    } s;
    shm_read(t->shm, SHM_STEADY, &s, sizeof s);
    memcpy(source, s.source, 16);
    (void)t_svc(e->c, SVC_GET_SYSTEM_TICK, 0, 0, 0, 0, 0, 0);
    uint64_t ticks = e->c->x[0];
    return (s.base_time + armTicksToNs(ticks)) / 1000000000ll;
}

TEST(time_services_appear_through_sm)
{
    env e = boot();
    const char *names[] = { "time:u", "time:a", "time:s", "time:su", "time:r" };
    for (uint32_t i = 0; i < 5; i++)
        CHECK_EQ(tc_open_service_result(e.c, e.heap, names[i]), 0);
    CHECK_EQ(tc_open_service_result(e.c, e.heap, "time:x"), SM_RESULT_NOT_REGISTERED);
    CHECK_EQ(tc_open_service_result(e.c, e.heap, "time"), SM_RESULT_NOT_REGISTERED);
}

TEST(libnx_time_initialize_and_shared_memory)
{
    env e = boot();
    libnx_time t;
    CHECK(time_initialize(&e, "time:u", &t));

    uint8_t source[16];
    int64_t steady = shm_steady_seconds(&e, &t, source);

    tc_res r = call(&e, t.steady, 0, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    int64_t tp = (int64_t)tc_u64(&r, 0);
    CHECK(near(tp, steady, 2));
    CHECK(memcmp(r.data + 8, source, 16) == 0);
    CHECK(tp > 0);

    clock_ctx local, network;
    shm_read(t.shm, SHM_LOCAL, &local, sizeof local);
    shm_read(t.shm, SHM_NETWORK, &network, sizeof network);
    CHECK(memcmp(local.source, source, 16) == 0);
    CHECK(memcmp(network.source, source, 16) == 0);
    CHECK(near(local.offset + steady, now_wall(), 3));
    CHECK(near(network.offset + steady, now_wall(), 3));

    r = call(&e, t.user, 0, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK(near((int64_t)tc_u64(&r, 0), now_wall(), 3));
    r = call(&e, t.network, 0, NULL, 0);
    CHECK(near((int64_t)tc_u64(&r, 0), now_wall(), 3));
    r = call(&e, t.local, 0, NULL, 0);
    CHECK(near((int64_t)tc_u64(&r, 0), now_wall(), 3));
}

TEST(time_shared_memory_is_one_page_shared_by_sessions)
{
    env e = boot();
    libnx_time a, b;
    CHECK(time_initialize(&e, "time:u", &a));
    CHECK(time_initialize(&e, "time:s", &b));
    clock_ctx x, y;
    shm_read(a.shm, SHM_LOCAL, &x, sizeof x);
    shm_read(b.shm, SHM_LOCAL, &y, sizeof y);
    CHECK(memcmp(&x, &y, sizeof x) == 0);
    CHECK_EQ(a.shm[0xCC + (*(const uint32_t *)(const void *)(a.shm + 0xC8) & 1u)], 1);
}

TEST(time_clock_permissions)
{
    env e = boot();
    int64_t t1 = 1600000000;
    libnx_time u, a, s;
    CHECK(time_initialize(&e, "time:u", &u));
    CHECK(time_initialize(&e, "time:a", &a));
    CHECK(time_initialize(&e, "time:s", &s));

    CHECK_EQ(rc_of(call(&e, u.user, 1, &t1, 8)), TIME_RC(1));
    CHECK_EQ(rc_of(call(&e, u.network, 1, &t1, 8)), TIME_RC(1));
    CHECK_EQ(rc_of(call(&e, u.local, 1, &t1, 8)), TIME_RC(1));
    CHECK_EQ(rc_of(call(&e, a.network, 1, &t1, 8)), TIME_RC(1));
    CHECK_EQ(rc_of(call(&e, s.user, 1, &t1, 8)), TIME_RC(1));
    clock_ctx c = { 0 };
    CHECK_EQ(rc_of(call(&e, u.user, 3, &c, sizeof c)), TIME_RC(1));

    CHECK_EQ(rc_of(call(&e, a.user, 1, &t1, 8)), 0);
    tc_res r = call(&e, u.user, 0, NULL, 0);
    CHECK(near((int64_t)tc_u64(&r, 0), t1, 3));
    r = call(&e, u.local, 0, NULL, 0);
    CHECK(near((int64_t)tc_u64(&r, 0), t1, 3));
    r = call(&e, u.network, 0, NULL, 0);
    CHECK(near((int64_t)tc_u64(&r, 0), now_wall(), 3));

    clock_ctx local;
    uint8_t source[16];
    int64_t steady = shm_steady_seconds(&e, &u, source);
    shm_read(u.shm, SHM_LOCAL, &local, sizeof local);
    CHECK(near(local.offset + steady, t1, 3));

    int64_t t2 = 1700000000;
    CHECK_EQ(rc_of(call(&e, s.network, 1, &t2, 8)), 0);
    r = call(&e, u.network, 0, NULL, 0);
    CHECK(near((int64_t)tc_u64(&r, 0), t2, 3));
    shm_read(u.shm, SHM_NETWORK, &local, sizeof local);
    CHECK(near(local.offset + steady, t2, 3));
    r = call(&e, u.network, 2, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK(memcmp(res_raw(e.c), &local, sizeof local) == 0);
}

TEST(time_clock_context_roundtrip_and_mismatch)
{
    env e = boot();
    libnx_time a;
    CHECK(time_initialize(&e, "time:a", &a));
    clock_ctx c;
    for (uint32_t i = 0; i < sizeof c; i++)
        ((uint8_t *)&c)[i] = (uint8_t)(0x20 + i);
    CHECK_EQ(rc_of(call(&e, a.user, 3, &c, sizeof c)), 0);
    tc_res r = call(&e, a.user, 2, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK(memcmp(res_raw(e.c), &c, sizeof c) == 0);
    r = call(&e, a.user, 0, NULL, 0);
    CHECK_EQ(rc_of(r), TIME_RC(102));
    clock_ctx shm;
    shm_read(a.shm, SHM_LOCAL, &shm, sizeof shm);
    CHECK(memcmp(&shm, &c, sizeof c) == 0);
    r = call(&e, a.local, 0, NULL, 0);
    CHECK_EQ(rc_of(r), TIME_RC(102));
}

TEST(time_clock_operation_event)
{
    env e = boot();
    libnx_time a;
    CHECK(time_initialize(&e, "time:a", &a));
    tc_res r = call(&e, a.user, 4, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.ncopy, 1);
    *(uint32_t *)(void *)(t_mem() + e.heap + 0x800) = r.copy[0];
    CHECK_EQ(t_svc(e.c, SVC_WAIT_SYNCHRONIZATION, 0, e.heap + 0x800, 1, 0, 0, 0), KR_TIMED_OUT);
    int64_t t1 = 1650000000;
    CHECK_EQ(rc_of(call(&e, a.user, 1, &t1, 8)), 0);
    CHECK_EQ(t_svc(e.c, SVC_WAIT_SYNCHRONIZATION, 0, e.heap + 0x800, 1, 0, 0, 0), 0);
}

TEST(time_steady_clock)
{
    env e = boot();
    libnx_time u, r_;
    CHECK(time_initialize(&e, "time:u", &u));
    CHECK(time_initialize(&e, "time:r", &r_));

    tc_res r = call(&e, u.steady, 0, NULL, 0);
    int64_t t0 = (int64_t)tc_u64(&r, 0);
    CHECK_EQ(rc_of(call(&e, u.steady, 2, NULL, 0)), 0);
    r = call(&e, u.steady, 2, NULL, 0);
    CHECK_EQ(tc_u64(&r, 0), 0);
    r = call(&e, u.steady, 101, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.data[0], 0);
    r = call(&e, u.steady, 102, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
    CHECK_EQ(rc_of(call(&e, u.steady, 100, NULL, 0)), TIME_RC(990));
    r = call(&e, u.steady, 200, NULL, 0);
    CHECK_EQ(tc_u64(&r, 0), 0);

    int64_t off = 5 * 1000000000ll;
    CHECK_EQ(rc_of(call(&e, u.steady, 3, &off, 8)), TIME_RC(1));
    CHECK_EQ(rc_of(call(&e, u.steady, 201, &off, 8)), TIME_RC(1));
    CHECK_EQ(rc_of(call(&e, r_.steady, 201, &off, 8)), 0);
    r = call(&e, u.steady, 200, NULL, 0);
    CHECK_EQ((int64_t)tc_u64(&r, 0), off);
    r = call(&e, u.steady, 0, NULL, 0);
    int64_t t1 = (int64_t)tc_u64(&r, 0);
    CHECK(near(t1 - t0, 5, 2));

    uint8_t source[16];
    int64_t shm = shm_steady_seconds(&e, &u, source);
    CHECK(near(shm, t1, 2));

    int64_t test = 7 * 1000000000ll;
    CHECK_EQ(rc_of(call(&e, r_.steady, 3, &test, 8)), 0);
    r = call(&e, u.steady, 2, NULL, 0);
    CHECK_EQ((int64_t)tc_u64(&r, 0), test);
    shm = shm_steady_seconds(&e, &u, source);
    r = call(&e, u.steady, 0, NULL, 0);
    CHECK(near(shm, (int64_t)tc_u64(&r, 0), 2));
    CHECK(near((int64_t)tc_u64(&r, 0) - t1, 7, 2));
}

TEST(time_static_service_extras)
{
    env e = boot();
    tc_obj u = tc_open_service(e.c, e.heap, "time:u");
    tc_obj a = tc_open_service(e.c, e.heap, "time:a");
    tc_obj s = tc_open_service(e.c, e.heap, "time:s");
    tc_obj su = tc_open_service(e.c, e.heap, "time:su");
    tc_obj rr = tc_open_service(e.c, e.heap, "time:r");
    CHECK(u.handle && a.handle && s.handle && su.handle && rr.handle);

    tc_res r = call(&e, u, 100, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.data[0], 1);
    uint8_t off = 0;
    CHECK_EQ(rc_of(call(&e, u, 101, &off, 1)), TIME_RC(1));
    CHECK_EQ(rc_of(call(&e, a, 101, &off, 1)), 0);
    r = call(&e, u, 100, NULL, 0);
    CHECK_EQ(r.data[0], 0);
    r = call(&e, u, 201, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    int64_t tp = (int64_t)tc_u64(&r, 0);
    CHECK(tp >= 0);

    r = call(&e, u, 102, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 2019);
    CHECK_EQ(rc_of(call(&e, s, 102, NULL, 0)), TIME_RC(990));
    CHECK_EQ(rc_of(call(&e, su, 102, NULL, 0)), TIME_RC(990));
    CHECK_EQ(rc_of(call(&e, s, 51, NULL, 0)), TIME_RC(990));
    int64_t zero = 0;
    CHECK_EQ(rc_of(call(&e, s, 50, &zero, 8)), TIME_RC(990));
    CHECK_EQ(rc_of(call(&e, u, 50, &zero, 8)), TIME_RC(1));
    CHECK_EQ(rc_of(call(&e, rr, 50, &zero, 8)), 0);

    r = call(&e, u, 51, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK(near((int64_t)tc_u64(&r, 0), now_wall() - RTC_EPOCH, 3));

    r = call(&e, u, 200, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.data[0], 1);

    r = call(&e, u, 5, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.nmove, 1);
    CHECK_EQ(rc_of(call(&e, u, 99, NULL, 0)), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(time_monotonic_base_time_point)
{
    env e = boot();
    libnx_time u;
    CHECK(time_initialize(&e, "time:u", &u));
    tc_res r = call(&e, u.user, 2, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    clock_ctx c;
    memcpy(&c, res_raw(e.c), sizeof c);
    r = call(&e, u.srv, 300, &c, sizeof c);
    CHECK_EQ(rc_of(r), 0);
    int64_t base = (int64_t)tc_u64(&r, 0);
    t_svc(e.c, SVC_GET_SYSTEM_TICK, 0, 0, 0, 0, 0, 0);
    int64_t ticks_s = (int64_t)(e.c->x[0] / 19200000ull);
    CHECK(near(base + ticks_s, now_wall(), 3));
    c.source[0] ^= 0xFF;
    CHECK_EQ(rc_of(call(&e, u.srv, 300, &c, sizeof c)), TIME_RC(102));
}

TEST(time_clock_snapshot)
{
    env e = boot();
    libnx_time u;
    CHECK(time_initialize(&e, "time:u", &u));
    uint64_t buf = e.heap + 0x1000;
    uint8_t type = 1;
    tc_res r = call_buf(&e, u.srv, 400, &type, 1, 'c', buf, 0xD0);
    CHECK_EQ(rc_of(r), 0);
    const uint8_t *s = t_mem() + buf;
    int64_t ut, nt;
    memcpy(&ut, s + 0x40, 8);
    memcpy(&nt, s + 0x48, 8);
    CHECK(near(ut, now_wall(), 3));
    CHECK(near(nt, now_wall(), 3));
    tc_res ctx = call(&e, u.user, 2, NULL, 0);
    CHECK_EQ(rc_of(ctx), 0);
    CHECK(memcmp(s, res_raw(e.c), 32) == 0);
    struct tm tm;
    time_t t = (time_t)ut;
    gmtime_r(&t, &tm);
    uint16_t year;
    memcpy(&year, s + 0x50, 2);
    CHECK_EQ(year, tm.tm_year + 1900);
    CHECK_EQ(s[0x52], tm.tm_mon + 1);
    CHECK_EQ(s[0x53], tm.tm_mday);
    CHECK_EQ(s[0x54], tm.tm_hour);
    uint32_t wday;
    memcpy(&wday, s + 0x60, 4);
    CHECK_EQ(wday, tm.tm_wday);
    CHECK(strcmp((const char *)s + 0x68, "UTC") == 0);
    CHECK(strcmp((const char *)s + 0xA8, "UTC") == 0);
    CHECK_EQ(s[0xCC], 1);
    CHECK_EQ(s[0xCD], 1);
    CHECK_EQ(s[0xCE], 0);

    CHECK_EQ(rc_of(call(&e, u.srv, 400, &type, 1)), KR_INVALID_MEM_STATE);
    CHECK_EQ(rc_of(call_buf(&e, u.srv, 400, &type, 1, 'c', buf, 0x40)), KR_INVALID_MEM_STATE);
}

TEST(time_clock_snapshot_from_contexts_difference_and_span)
{
    env e = boot();
    libnx_time u, a;
    CHECK(time_initialize(&e, "time:u", &u));
    CHECK(time_initialize(&e, "time:a", &a));
    uint8_t no = 0;
    CHECK_EQ(rc_of(call(&e, a.srv, 101, &no, 1)), 0);
    tc_res r = call(&e, u.user, 2, NULL, 0);
    clock_ctx user;
    memcpy(&user, res_raw(e.c), sizeof user);
    r = call(&e, u.network, 2, NULL, 0);
    clock_ctx network;
    memcpy(&network, res_raw(e.c), sizeof network);

    uint8_t in[72] = { 0 };
    in[0] = 2;
    memcpy(in + 8, &user, 32);
    memcpy(in + 40, &network, 32);
    uint64_t sa = e.heap + 0x2000, sb = e.heap + 0x3000;
    r = call_buf(&e, u.srv, 401, in, sizeof in, 'c', sa, 0xD0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(t_mem()[sa + 0xCD], 2);

    user.offset += 100;
    memcpy(in + 8, &user, 32);
    r = call_buf(&e, u.srv, 401, in, sizeof in, 'c', sb, 0xD0);
    CHECK_EQ(rc_of(r), 0);

    tc_req q = tc_new(500);
    q.nx = 2;
    q.x_addr[0] = sa;
    q.x_size[0] = 0xD0;
    q.x_addr[1] = sb;
    q.x_size[1] = 0xD0;
    r = tc_send(e.c, u.srv, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ((int64_t)tc_u64(&r, 0), 100 * 1000000000ll);

    t_mem()[sb + 0xCC] = 1;
    t_mem()[sa + 0xCC] = 1;
    q.cmd = 500;
    r = tc_send(e.c, u.srv, q);
    CHECK_EQ(tc_u64(&r, 0), 0);

    q.cmd = 501;
    r = tc_send(e.c, u.srv, q);
    CHECK_EQ(rc_of(r), 0);
    int64_t span = (int64_t)tc_u64(&r, 0);
    CHECK(near(span, 0, 3 * 1000000000ll));

    t_mem()[sb + 0x98] ^= 0xFF;
    r = tc_send(e.c, u.srv, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK(near((int64_t)tc_u64(&r, 0), 0, 3 * 1000000000ll));

    int64_t zero = 0;
    memcpy(t_mem() + sa + 0x48, &zero, 8);
    r = tc_send(e.c, u.srv, q);
    CHECK_EQ(rc_of(r), TIME_RC(200));
}

TEST(time_zone_names_and_rule_version)
{
    env e = boot();
    libnx_time u, a;
    CHECK(time_initialize(&e, "time:u", &u));
    CHECK(time_initialize(&e, "time:a", &a));

    tc_res r = call(&e, u.zone, 0, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK(strcmp((const char *)res_raw(e.c), "UTC") == 0);
    r = call(&e, u.zone, 2, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 2);

    uint32_t index = 0;
    uint64_t buf = e.heap + 0x4000;
    memset(t_mem() + buf, 0xCC, 0x24 * 4);
    r = call_buf(&e, u.zone, 3, &index, 4, 'b', buf, 0x24 * 4);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 2);
    CHECK(strcmp((const char *)t_mem() + buf, "Etc/UTC") == 0);
    CHECK_EQ(t_mem()[buf + 0x23], 0);
    CHECK(strcmp((const char *)t_mem() + buf + 0x24, "UTC") == 0);
    index = 1;
    r = call_buf(&e, u.zone, 3, &index, 4, 'b', buf, 0x24 * 4);
    CHECK_EQ(tc_u32(&r, 0), 1);
    CHECK(strcmp((const char *)t_mem() + buf, "UTC") == 0);
    index = 0;
    r = call_buf(&e, u.zone, 3, &index, 4, 'b', buf, 0x24);
    CHECK_EQ(tc_u32(&r, 0), 1);
    index = 5;
    r = call_buf(&e, u.zone, 3, &index, 4, 'b', buf, 0x24 * 4);
    CHECK_EQ(tc_u32(&r, 0), 0);

    r = call(&e, u.zone, 5, NULL, 0);
    CHECK_EQ(rc_of(r), 0);

    r = call(&e, u.zone, 6, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    const uint8_t *raw = res_raw(e.c);
    CHECK(strcmp((const char *)raw, "UTC") == 0);
    uint8_t source[16];
    (void)shm_steady_seconds(&e, &u, source);
    CHECK(memcmp(raw + 0x30, source, 16) == 0);

    char name[0x24] = { 0 };
    memcpy(name, "Etc/UTC", 8);
    CHECK_EQ(rc_of(call(&e, u.zone, 1, name, sizeof name)), TIME_RC(1));
    CHECK_EQ(rc_of(call(&e, a.zone, 1, name, sizeof name)), 0);
    r = call(&e, u.zone, 0, NULL, 0);
    CHECK(strcmp((const char *)res_raw(e.c), "Etc/UTC") == 0);
    memcpy(name, "Europe/Paris", 13);
    CHECK_EQ(rc_of(call(&e, a.zone, 1, name, sizeof name)), TIME_RC(989));
    r = call(&e, u.zone, 0, NULL, 0);
    CHECK(strcmp((const char *)res_raw(e.c), "Etc/UTC") == 0);
}

TEST(time_zone_rule_blob_layout)
{
    env e = boot();
    libnx_time u;
    CHECK(time_initialize(&e, "time:u", &u));
    char name[0x24] = { 0 };
    memcpy(name, "UTC", 4);
    uint64_t rule = e.heap + 0x10000;
    memset(t_mem() + rule, 0xAB, RULE_SIZE);
    tc_res r = call_buf(&e, u.zone, 4, name, sizeof name, 'b', rule, RULE_SIZE);
    CHECK_EQ(rc_of(r), 0);
    const uint8_t *b = t_mem() + rule;
    int32_t v;
    memcpy(&v, b + 0, 4);
    CHECK_EQ(v, 0);
    memcpy(&v, b + 4, 4);
    CHECK_EQ(v, 1);
    memcpy(&v, b + 8, 4);
    CHECK_EQ(v, 4);
    CHECK_EQ(b[0xC], 0);
    CHECK_EQ(b[0xD], 0);
    memcpy(&v, b + 0x2338, 4);
    CHECK_EQ(v, 0);
    CHECK_EQ(b[0x2338 + 4], 0);
    memcpy(&v, b + 0x2338 + 8, 4);
    CHECK_EQ(v, 0);
    CHECK_EQ(b[0x2338 + 12], 0);
    CHECK_EQ(b[0x2338 + 13], 0);
    CHECK(strcmp((const char *)b + 0x2B38, "UTC") == 0);
    memcpy(&v, b + 0x2D38, 4);
    CHECK_EQ(v, 0);
    CHECK_EQ(b[0x1F50], 0);

    memcpy(name, "Mars/Olympus", 13);
    CHECK_EQ(rc_of(call_buf(&e, u.zone, 4, name, sizeof name, 'b', rule, RULE_SIZE)), TIME_RC(989));
    memcpy(name, "UTC", 4);
    CHECK_EQ(rc_of(call_buf(&e, u.zone, 4, name, sizeof name, 'b', rule, 0x100)), KR_INVALID_MEM_STATE);
}

static cal calendar_of(env *e, tc_obj zone, uint32_t cmd, int64_t t, uint64_t rule, bool *ok)
{
    cal c;
    memset(&c, 0, sizeof c);
    tc_res r = rule ? call_buf(e, zone, cmd, &t, 8, 'a', rule, RULE_SIZE) : call(e, zone, cmd, &t, 8);
    *ok = rc_of(r) == 0;
    if (*ok)
        memcpy(&c, res_raw(e->c), sizeof c);
    return c;
}

static bool cal_is(const cal *c, uint32_t y, uint32_t mo, uint32_t d, uint32_t h, uint32_t mi, uint32_t s, uint32_t wd,
                   uint32_t yd, const char *name, uint32_t dst, int32_t off)
{
    return c->year == y && c->month == mo && c->day == d && c->hour == h && c->minute == mi && c->second == s &&
           c->wday == wd && c->yday == yd && strcmp(c->name, name) == 0 && c->dst == dst && c->gmt_offset == off;
}

TEST(time_zone_follows_the_host_zone)
{
#ifndef _WIN32
    FILE *f = fopen("/usr/share/zoneinfo/America/Chicago", "rb");
    if (!f)
        return;
    fclose(f);
    env e = boot_in("America/Chicago");
    libnx_time u;
    CHECK(time_initialize(&e, "time:u", &u));
    tc_res r = call(&e, u.zone, 0, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK(strcmp((const char *)res_raw(e.c), "America/Chicago") == 0);
    r = call(&e, u.zone, 2, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 3);
    uint32_t index = 2;
    uint64_t buf = e.heap + 0x4000;
    r = call_buf(&e, u.zone, 3, &index, 4, 'b', buf, 0x24 * 4);
    CHECK_EQ(tc_u32(&r, 0), 1);
    CHECK(strcmp((const char *)t_mem() + buf, "America/Chicago") == 0);

    char name[0x24] = "America/Chicago";
    uint64_t rule = e.heap + 0x10000;
    CHECK_EQ(rc_of(call_buf(&e, u.zone, 4, name, sizeof name, 'b', rule, RULE_SIZE)), 0);
    bool ok;
    cal c = calendar_of(&e, u.zone, 100, 1768478400, rule, &ok);
    CHECK(ok);
    CHECK(cal_is(&c, 2026, 1, 15, 6, 0, 0, 4, 14, "CST", 0, -21600));
    c = calendar_of(&e, u.zone, 101, 1784116800, 0, &ok);
    CHECK(ok);
    CHECK(cal_is(&c, 2026, 7, 15, 7, 0, 0, 3, 195, "CDT", 1, -18000));

    memcpy(name, "UTC", 4);
    CHECK_EQ(rc_of(call_buf(&e, u.zone, 4, name, sizeof name, 'b', rule, RULE_SIZE)), 0);
    c = calendar_of(&e, u.zone, 100, 1768478400, rule, &ok);
    CHECK(cal_is(&c, 2026, 1, 15, 12, 0, 0, 4, 14, "UTC", 0, 0));
#endif
}

TEST(time_zone_ignores_a_bad_host_zone_name)
{
    env e = boot_in("../../etc/passwd");
    libnx_time u;
    CHECK(time_initialize(&e, "time:u", &u));
    tc_res r = call(&e, u.zone, 0, NULL, 0);
    CHECK(strcmp((const char *)res_raw(e.c), "UTC") == 0);
    r = call(&e, u.zone, 2, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 2);
}

TEST(time_zone_utc_calendar_conversions)
{
    env e = boot();
    libnx_time u;
    CHECK(time_initialize(&e, "time:u", &u));
    char name[0x24] = "UTC";
    uint64_t rule = e.heap + 0x10000;
    CHECK_EQ(rc_of(call_buf(&e, u.zone, 4, name, sizeof name, 'b', rule, RULE_SIZE)), 0);

    static const struct {
        int64_t t;
        uint32_t y, mo, d, h, mi, s, wd, yd;
    } t[] = {
        { 0, 1970, 1, 1, 0, 0, 0, 4, 0 },
        { -1, 1969, 12, 31, 23, 59, 59, 3, 364 },
        { 951782400, 2000, 2, 29, 0, 0, 0, 2, 59 },
        { 4107542400ll, 2100, 3, 1, 0, 0, 0, 1, 59 },
        { 1700000000, 2023, 11, 14, 22, 13, 20, 2, 317 },
        { -2208988800ll, 1900, 1, 1, 0, 0, 0, 1, 0 },
        { 253402300799ll, 9999, 12, 31, 23, 59, 59, 5, 364 },
    };
    for (uint32_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        bool ok;
        cal c = calendar_of(&e, u.zone, 100, t[i].t, rule, &ok);
        CHECK(ok);
        CHECK(cal_is(&c, t[i].y, t[i].mo, t[i].d, t[i].h, t[i].mi, t[i].s, t[i].wd, t[i].yd, "UTC", 0, 0));
        cal m = calendar_of(&e, u.zone, 101, t[i].t, 0, &ok);
        CHECK(ok);
        CHECK(memcmp(&c, &m, sizeof c) == 0);

        uint8_t in[8];
        memcpy(in, &c.year, 2);
        in[2] = c.month;
        in[3] = c.day;
        in[4] = c.hour;
        in[5] = c.minute;
        in[6] = c.second;
        in[7] = 0;
        uint64_t out = e.heap + 0x20000;
        tc_req q = tc_new(201);
        tc_put(&q, in, 8);
        q.na = 1;
        q.a_addr[0] = rule;
        q.a_size[0] = RULE_SIZE;
        q.nc = 1;
        q.c_addr[0] = out;
        q.c_size[0] = 8;
        tc_res r = tc_send(e.c, u.zone, q);
        CHECK_EQ(rc_of(r), 0);
        CHECK_EQ(tc_u32(&r, 0), 1);
        int64_t back;
        memcpy(&back, t_mem() + out, 8);
        CHECK_EQ(back, t[i].t);

        r = call_buf(&e, u.zone, 202, in, 8, 'c', out + 0x100, 8);
        CHECK_EQ(rc_of(r), 0);
        CHECK_EQ(tc_u32(&r, 0), 1);
        memcpy(&back, t_mem() + out + 0x100, 8);
        CHECK_EQ(back, t[i].t);
    }

    bool ok;
    (void)calendar_of(&e, u.zone, 100, 9223372036854775807ll, rule, &ok);
    CHECK(!ok);
    (void)calendar_of(&e, u.zone, 101, 4000000000000000ll, 0, &ok);
    CHECK(!ok);
}

TEST(time_zone_to_posix_normalizes_fields)
{
    env e = boot();
    libnx_time u;
    CHECK(time_initialize(&e, "time:u", &u));
    uint8_t in[8] = { 0 };
    uint16_t year = 2021;
    memcpy(in, &year, 2);
    in[2] = 14;
    in[3] = 31;
    in[4] = 25;
    in[5] = 61;
    in[6] = 62;
    uint64_t out = e.heap + 0x20000;
    tc_res r = call_buf(&e, u.zone, 202, in, 8, 'c', out, 8);
    CHECK_EQ(rc_of(r), 0);
    int64_t t;
    memcpy(&t, t_mem() + out, 8);
    CHECK_EQ(t, 1646359322ll);
}

TEST(time_zone_bad_rules_are_refused)
{
    env e = boot();
    libnx_time u;
    CHECK(time_initialize(&e, "time:u", &u));
    uint64_t rule = e.heap + 0x10000;
    memset(t_mem() + rule, 0x7F, RULE_SIZE);
    bool ok;
    (void)calendar_of(&e, u.zone, 100, 0, rule, &ok);
    CHECK(!ok);
    tc_res r = call_buf(&e, u.zone, 100, &(int64_t){ 0 }, 8, 'a', rule, 0x100);
    CHECK_EQ(rc_of(r), TIME_RC(903));
    memset(t_mem() + rule, 0, RULE_SIZE);
    (void)calendar_of(&e, u.zone, 100, 0, rule, &ok);
    CHECK(!ok);
}

static void put_be32(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)((uint32_t)v >> 24);
    p[1] = (uint8_t)((uint32_t)v >> 16);
    p[2] = (uint8_t)((uint32_t)v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t build_tzif(uint8_t *out)
{
    static const char chars[] = "EST\0EDT";
    uint8_t *p = out;
    memset(out, 0, 44);
    memcpy(out, "TZif", 4);
    put_be32(out + 32, 2);
    put_be32(out + 36, 2);
    put_be32(out + 40, sizeof chars + 1);
    p += 44;
    put_be32(p, 1000000000);
    put_be32(p + 4, 1100000000);
    p += 8;
    *p++ = 1;
    *p++ = 0;
    put_be32(p, -18000);
    p[4] = 0;
    p[5] = 0;
    p += 6;
    put_be32(p, -14400);
    p[4] = 1;
    p[5] = 4;
    p += 6;
    memcpy(p, chars, sizeof chars);
    p[sizeof chars] = 0;
    p += sizeof chars + 1;
    return (uint32_t)(p - out);
}

TEST(time_zone_supplied_tzif_rule)
{
    env e = boot();
    libnx_time a;
    CHECK(time_initialize(&e, "time:a", &a));
    uint8_t tz[128];
    uint32_t tzn = build_tzif(tz);
    uint64_t tzbuf = e.heap + 0x30000, rule = e.heap + 0x40000, out = e.heap + 0x60000;
    memcpy(t_mem() + tzbuf, tz, tzn);

    tc_res r = call_buf(&e, a.zone, 8, NULL, 0, 'b', rule, RULE_SIZE);
    CHECK_EQ(rc_of(r), TIME_RC(903));
    tc_req q = tc_new(8);
    q.na = 1;
    q.a_addr[0] = tzbuf;
    q.a_size[0] = tzn;
    q.nb = 1;
    q.b_addr[0] = rule;
    q.b_size[0] = RULE_SIZE;
    r = tc_send(e.c, a.zone, q);
    CHECK_EQ(rc_of(r), 0);
    const uint8_t *b = t_mem() + rule;
    int32_t v;
    memcpy(&v, b, 4);
    CHECK_EQ(v, 2);
    memcpy(&v, b + 4, 4);
    CHECK_EQ(v, 2);
    int64_t at;
    memcpy(&at, b + 0x10, 8);
    CHECK_EQ(at, 1000000000);
    memcpy(&at, b + 0x18, 8);
    CHECK_EQ(at, 1100000000);
    CHECK_EQ(b[0x1F50], 1);
    CHECK_EQ(b[0x1F51], 0);
    memcpy(&v, b + 0x2338, 4);
    CHECK_EQ(v, -18000);
    memcpy(&v, b + 0x2338 + 0x10, 4);
    CHECK_EQ(v, -14400);
    CHECK_EQ(b[0x2338 + 0x10 + 4], 1);
    CHECK_EQ(b[0x2338 + 0x10 + 8], 4);

    static const struct {
        int64_t t;
        uint32_t y, mo, d, h, mi, s, wd, yd;
        const char *name;
        uint32_t dst;
        int32_t off;
    } t[] = {
        { 999999999, 2001, 9, 8, 20, 46, 39, 6, 250, "EST", 0, -18000 },
        { 1000000000, 2001, 9, 8, 21, 46, 40, 6, 250, "EDT", 1, -14400 },
        { 1099999999, 2004, 11, 9, 7, 33, 19, 2, 313, "EDT", 1, -14400 },
        { 1100000000, 2004, 11, 9, 6, 33, 20, 2, 313, "EST", 0, -18000 },
        { 0, 1969, 12, 31, 19, 0, 0, 3, 364, "EST", 0, -18000 },
    };
    for (uint32_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        bool ok;
        cal c = calendar_of(&e, a.zone, 100, t[i].t, rule, &ok);
        CHECK(ok);
        CHECK(cal_is(&c, t[i].y, t[i].mo, t[i].d, t[i].h, t[i].mi, t[i].s, t[i].wd, t[i].yd, t[i].name, t[i].dst,
                     t[i].off));
        uint8_t in[8];
        memcpy(in, &c.year, 2);
        in[2] = c.month;
        in[3] = c.day;
        in[4] = c.hour;
        in[5] = c.minute;
        in[6] = c.second;
        in[7] = 0;
        tc_req p = tc_new(201);
        tc_put(&p, in, 8);
        p.na = 1;
        p.a_addr[0] = rule;
        p.a_size[0] = RULE_SIZE;
        p.nc = 1;
        p.c_addr[0] = out;
        p.c_size[0] = 8;
        r = tc_send(e.c, a.zone, p);
        CHECK_EQ(rc_of(r), 0);
        int64_t back;
        memcpy(&back, t_mem() + out, 8);
        cal again = calendar_of(&e, a.zone, 100, back, rule, &ok);
        CHECK(ok);
        CHECK(memcmp(&again, &c, 7) == 0);
        if (i != 3)
            CHECK_EQ(back, t[i].t);
    }

    q = tc_new(7);
    char name[0x24] = "Test/Eastern";
    tc_put(&q, name, sizeof name);
    q.na = 1;
    q.a_addr[0] = tzbuf;
    q.a_size[0] = tzn;
    r = tc_send(e.c, a.zone, q);
    CHECK_EQ(rc_of(r), 0);
    r = call(&e, a.zone, 0, NULL, 0);
    CHECK(strcmp((const char *)res_raw(e.c), "Test/Eastern") == 0);
    bool ok;
    cal c = calendar_of(&e, a.zone, 101, 1000000000, 0, &ok);
    CHECK(ok);
    CHECK(cal_is(&c, 2001, 9, 8, 21, 46, 40, 6, 250, "EDT", 1, -14400));
    uint8_t in[8] = { 0xD1, 0x07, 9, 8, 21, 46, 40, 0 };
    r = call_buf(&e, a.zone, 202, in, 8, 'c', out, 8);
    CHECK_EQ(rc_of(r), 0);
    int64_t back;
    memcpy(&back, t_mem() + out, 8);
    CHECK_EQ(back, 1000000000);

    q = tc_new(7);
    tc_put(&q, name, sizeof name);
    q.na = 1;
    q.a_addr[0] = tzbuf;
    q.a_size[0] = 20;
    CHECK_EQ(rc_of(tc_send(e.c, a.zone, q)), TIME_RC(903));
    t_mem()[tzbuf] = 'X';
    q.a_size[0] = tzn;
    CHECK_EQ(rc_of(tc_send(e.c, a.zone, q)), TIME_RC(903));
}

TEST(time_zone_service_of_system_services_is_the_psc_subset)
{
    env e = boot();
    libnx_time s;
    CHECK(time_initialize(&e, "time:s", &s));
    char name[0x24] = "UTC";
    uint32_t index = 0;
    uint64_t buf = e.heap + 0x4000;
    CHECK_EQ(rc_of(call_buf(&e, s.zone, 4, name, sizeof name, 'b', buf, RULE_SIZE)), TIME_RC(990));
    CHECK_EQ(rc_of(call_buf(&e, s.zone, 3, &index, 4, 'b', buf, 0x24)), TIME_RC(990));
    CHECK_EQ(rc_of(call(&e, s.zone, 1, name, sizeof name)), TIME_RC(1));
    tc_res r = call(&e, s.zone, 0, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    r = call(&e, s.zone, 2, NULL, 0);
    CHECK_EQ(tc_u32(&r, 0), 2);
    bool ok;
    cal c = calendar_of(&e, s.zone, 101, 0, 0, &ok);
    CHECK(ok);
    CHECK(cal_is(&c, 1970, 1, 1, 0, 0, 0, 4, 0, "UTC", 0, 0));

    libnx_time su;
    CHECK(time_initialize(&e, "time:su", &su));
    c = calendar_of(&e, su.zone, 101, 86400, 0, &ok);
    CHECK(ok);
    CHECK(cal_is(&c, 1970, 1, 2, 0, 0, 0, 5, 1, "UTC", 0, 0));
}

TEST(time_service_over_a_domain)
{
    env e = boot();
    tc_obj srv = tc_open_service(e.c, e.heap, "time:u");
    CHECK(srv.handle);
    CHECK_EQ(tc_to_domain(e.c, &srv), 0);
    tc_obj user;
    CHECK_EQ(tc_sub(e.c, srv, tc_new(0), &user), 0);
    CHECK(user.domain);
    tc_res r = call(&e, user, 0, NULL, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK(near((int64_t)tc_u64(&r, 0), now_wall(), 3));
}
