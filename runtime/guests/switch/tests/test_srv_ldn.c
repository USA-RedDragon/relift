/**
 * @file test_srv_ldn.c
 * @brief ldn:u as nn::ldn drives it: the state machine of a host and of a
 * station over a fake backend (create, scan, connect, a station joining and
 * leaving, the host going away, disconnect), the state change event, the
 * node update flags, and the offline answers.
 */
#include "tc.h"

#include "hos.h"
#include "ldn.h"
#include "services.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { GET_STATE = 0, GET_INFO = 1, GET_IP = 2, GET_REASON = 3, GET_CONFIG = 5, ATTACH = 100, LATEST = 101,
       SCAN = 102, OPEN_AP = 200, CLOSE_AP = 201, CREATE = 202, DESTROY = 204, ADVERTISE = 206, OPEN_STA = 300,
       CLOSE_STA = 301, CONNECT = 302, DISCONNECT = 304, INIT_OLD = 400, FINALIZE = 401 };

/** @brief The fake backend: a network in memory, and what the service asked of it. */
typedef struct fake {
    ldn_backend b;
    ldn_change_fn *change;
    void *ctx;
    ldn_network_info net;
    int creates, destroys, connects, disconnects, advertises;
} fake;

static fake *g_fake;

static bool f_address(ldn_backend *b, uint32_t *ip, uint32_t *mask)
{
    (void)b;
    *ip = 0x0a0d0001;
    *mask = 0xffffff00;
    return true;
}

/** @brief A network another host offers, for scans and connects. */
static ldn_network_info offered(void)
{
    ldn_network_info n;
    memset(&n, 0, sizeof n);
    n.id.intent.local_communication_id = 0x0100abcd00000000ull;
    n.id.intent.scene_id = 7;
    n.ldn.node_count_max = 4;
    n.ldn.node_count = 1;
    n.ldn.nodes[0].ipv4 = 0x0a0d0002;
    n.ldn.nodes[0].is_connected = 1;
    strcpy(n.ldn.nodes[0].user_name, "host");
    return n;
}

static int f_scan(ldn_backend *b, uint16_t channel, const ldn_scan_filter *f, ldn_network_info *out, int max)
{
    (void)b;
    (void)channel;
    ldn_network_info n = offered();
    if (max < 1 || !rl_ldn_filter_match(f, &n))
        return 0;
    out[0] = n;
    return 1;
}

static uint32_t f_create(ldn_backend *b, const ldn_security_config *sec, const ldn_user_config *user,
                         const ldn_network_config *net, const void *adv, size_t adv_size)
{
    (void)sec;
    fake *f = (fake *)b;
    f->creates++;
    memset(&f->net, 0, sizeof f->net);
    f->net.id.intent = net->intent;
    f->net.ldn.node_count_max = net->node_count_max;
    f->net.ldn.node_count = 1;
    f->net.ldn.nodes[0].ipv4 = 0x0a0d0001;
    f->net.ldn.nodes[0].is_connected = 1;
    f->net.ldn.nodes[0].local_communication_version = net->local_communication_version;
    memcpy(f->net.ldn.nodes[0].user_name, user->user_name, 32);
    memcpy(f->net.ldn.advertise, adv, adv_size);
    f->net.ldn.advertise_size = (uint16_t)adv_size;
    f->change(f->ctx, &f->net, true, 0);
    return 0;
}

static void f_destroy(ldn_backend *b) { ((fake *)b)->destroys++; }

static void f_set_advertise(ldn_backend *b, const void *data, size_t size)
{
    (void)data;
    (void)size;
    ((fake *)b)->advertises++;
}

static uint32_t f_reject(ldn_backend *b, uint32_t ip)
{
    (void)b;
    (void)ip;
    return 0;
}

static uint32_t f_connect(ldn_backend *b, const ldn_network_info *info, const ldn_security_config *sec,
                          const ldn_user_config *user, uint16_t version)
{
    (void)sec;
    (void)version;
    fake *f = (fake *)b;
    f->connects++;
    f->net = *info;
    f->net.ldn.node_count = 2;
    f->net.ldn.nodes[1].ipv4 = 0x0a0d0001;
    f->net.ldn.nodes[1].node_id = 1;
    f->net.ldn.nodes[1].is_connected = 1;
    memcpy(f->net.ldn.nodes[1].user_name, user->user_name, 32);
    f->change(f->ctx, &f->net, true, 0);
    return 0;
}

static void f_disconnect(ldn_backend *b) { ((fake *)b)->disconnects++; }

static void f_free(ldn_backend *b)
{
    if (g_fake == (fake *)b)
        g_fake = NULL;
    free(b);
}

static ldn_backend *fake_factory(ldn_change_fn *change, void *ctx)
{
    fake *f = calloc(1, sizeof *f);
    f->b = (ldn_backend){ f_address, f_scan, f_create, f_destroy, f_set_advertise, f_reject, f_connect, f_disconnect,
                          f_free };
    f->change = change;
    f->ctx = ctx;
    g_fake = f;
    return &f->b;
}

/** @brief CHECK for helpers that return a value: records the failure and goes on. */
#define NEED(cond)                                                                                                     \
    do {                                                                                                               \
        if (!(cond))                                                                                                   \
            t_fail(__FILE__, __LINE__, #cond, 0, 0, 0);                                                                \
    } while (0)
/** @brief CHECK_EQ for helpers that return a value. */
#define NEED_EQ(a, b) NEED((unsigned long long)(a) == (unsigned long long)(b))

typedef struct fx {
    rl_ctx *c;
    uint64_t heap;
    tc_obj u;
    uint32_t event;
} fx;

/** @brief Opens ldn:u, creates the local communication service, attaches the event and initializes. */
static fx open_ldn(ldn_backend_factory *factory)
{
    fx f;
    memset(&f, 0, sizeof f);
    rl_ldn_set_backend_factory(factory);
    f.c = t_boot(NULL);
    f.heap = t_heap(f.c, 0x200000);
    tc_obj s = tc_open_service(f.c, f.heap, "ldn:u");
    NEED(s.handle != 0);
    NEED_EQ(tc_to_domain(f.c, &s), 0);
    NEED_EQ(tc_sub(f.c, s, tc_new(0), &f.u), 0);
    tc_res r = tc_send(f.c, f.u, tc_new(ATTACH));
    NEED_EQ(r.result, 0);
    NEED_EQ(r.ncopy, 1);
    f.event = r.copy[0];
    return f;
}

/** @brief Reads and clears the state change event. */
static bool take_event(fx f)
{
    hos_lock();
    hos_revent *e = (hos_revent *)hos_handle_get(f.event, KOBJ_READABLE_EVENT);
    hos_unlock();
    NEED(e != NULL);
    bool was = e->signaled;
    rl_hos_event_clear(e);
    hos_kobj_unref(&e->obj);
    return was;
}

static uint32_t state(fx f)
{
    tc_res r = tc_send(f.c, f.u, tc_new(GET_STATE));
    NEED_EQ(r.result, 0);
    return tc_u32(&r, 0);
}

#define INFO_BUF(f) ((f).heap + 0x100000)
#define UPD_BUF(f) ((f).heap + 0x101000)

static uint32_t create(fx f, uint8_t nodes)
{
    struct {
        ldn_security_config sec;
        ldn_user_config user;
        uint32_t pad;
        ldn_network_config net;
    } in;
    memset(&in, 0, sizeof in);
    in.sec.mode = 1;
    strcpy(in.user.user_name, "me");
    in.net.intent.local_communication_id = 0x0100abcd00000000ull;
    in.net.node_count_max = nodes;
    in.net.local_communication_version = 3;
    tc_req q = tc_new(CREATE);
    tc_put(&q, &in, sizeof in);
    return tc_send(f.c, f.u, q).result;
}

static tc_res get_info(fx f)
{
    tc_req q = tc_new(GET_INFO);
    q.nc = 1;
    q.c_addr[0] = INFO_BUF(f);
    q.c_size[0] = sizeof(ldn_network_info);
    return tc_send(f.c, f.u, q);
}

static ldn_network_info *info_at(fx f) { return (ldn_network_info *)(void *)(t_mem() + INFO_BUF(f)); }

/** @brief GetNetworkInfoLatestUpdate with room for 8 updates; the update bytes land at UPD_BUF. */
static uint32_t latest(fx f)
{
    tc_req q = tc_new(LATEST);
    q.nc = 2;
    q.c_addr[0] = INFO_BUF(f);
    q.c_size[0] = sizeof(ldn_network_info);
    q.c_addr[1] = UPD_BUF(f);
    q.c_size[1] = 64;
    return tc_send(f.c, f.u, q).result;
}

static uint8_t update_of(fx f, int node) { return t_mem()[UPD_BUF(f) + 8 * (uint64_t)node]; }

static tc_res scan(fx f, uint32_t flags, uint64_t lcid)
{
    struct {
        uint16_t channel;
        uint8_t pad[6];
        ldn_scan_filter filter;
    } in;
    memset(&in, 0, sizeof in);
    in.filter.flags = flags;
    in.filter.id.intent.local_communication_id = lcid;
    tc_req q = tc_new(SCAN);
    tc_put(&q, &in, sizeof in);
    q.nb = 1;
    q.b_addr[0] = INFO_BUF(f);
    q.b_size[0] = 4 * sizeof(ldn_network_info);
    return tc_send(f.c, f.u, q);
}

static uint32_t connect_to(fx f, const ldn_network_info *n)
{
    memcpy(t_mem() + INFO_BUF(f) + 0x2000, n, sizeof *n);
    struct {
        ldn_security_config sec;
        ldn_user_config user;
        uint32_t version;
        uint32_t option;
    } in;
    memset(&in, 0, sizeof in);
    in.sec.mode = 1;
    strcpy(in.user.user_name, "guest");
    in.version = 3;
    tc_req q = tc_new(CONNECT);
    tc_put(&q, &in, sizeof in);
    q.nx = 1;
    q.x_addr[0] = INFO_BUF(f) + 0x2000;
    q.x_size[0] = sizeof *n;
    return tc_send(f.c, f.u, q).result;
}

TEST(ldn_host_creates_a_network_that_stations_join_and_leave)
{
    fx f = open_ldn(fake_factory);
    CHECK_EQ(state(f), LDN_STATE_NONE);
    CHECK_EQ(tc_cmd(f.c, f.u, OPEN_AP), LDN_RESULT_INVALID_STATE);
    CHECK_EQ(tc_cmd(f.c, f.u, INIT_OLD), 0);
    CHECK_EQ(state(f), LDN_STATE_INITIALIZED);
    CHECK(take_event(f));
    CHECK_EQ(tc_cmd(f.c, f.u, INIT_OLD), LDN_RESULT_INVALID_STATE);
    CHECK_EQ(create(f, 4), LDN_RESULT_INVALID_STATE);
    CHECK_EQ(get_info(f).result, LDN_RESULT_INVALID_STATE);

    CHECK_EQ(tc_cmd(f.c, f.u, OPEN_AP), 0);
    CHECK_EQ(state(f), LDN_STATE_ACCESS_POINT);
    CHECK(take_event(f));
    memcpy(t_mem() + UPD_BUF(f), "advert", 6);
    tc_req q = tc_new(ADVERTISE);
    q.na = 1;
    q.a_addr[0] = UPD_BUF(f);
    q.a_size[0] = 6;
    CHECK_EQ(tc_send(f.c, f.u, q).result, 0);
    CHECK_EQ(create(f, 9), LDN_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(create(f, 4), 0);
    CHECK_EQ(g_fake->creates, 1);
    CHECK_EQ(state(f), LDN_STATE_ACCESS_POINT_CREATED);
    CHECK(take_event(f));
    CHECK_EQ(get_info(f).result, 0);
    CHECK_EQ(info_at(f)->ldn.node_count, 1);
    CHECK_EQ(info_at(f)->ldn.advertise_size, 6);
    CHECK(memcmp(info_at(f)->ldn.advertise, "advert", 6) == 0);
    CHECK(strcmp(info_at(f)->ldn.nodes[0].user_name, "me") == 0);
    tc_res r = tc_send(f.c, f.u, tc_new(GET_IP));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 0x0a0d0001u);
    CHECK_EQ(tc_u32(&r, 4), 0xffffff00u);
    r = tc_send(f.c, f.u, tc_new(GET_CONFIG));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u64(&r, 0), 0x0100abcd00000000ull);
    CHECK_EQ(tc_u32(&r, 0x10) >> 16 & 0xff, 4);
    uint32_t ip, mask;
    CHECK(rl_ldn_route(NULL, &ip, &mask));
    CHECK_EQ(ip, 0x0a0d0001u);
    CHECK_EQ(latest(f), 0);
    CHECK_EQ(update_of(f, 0), LDN_NODE_CONNECT);

    g_fake->net.ldn.nodes[1].is_connected = 1;
    g_fake->net.ldn.node_count = 2;
    g_fake->change(g_fake->ctx, &g_fake->net, true, 0);
    CHECK(take_event(f));
    CHECK_EQ(latest(f), 0);
    CHECK_EQ(update_of(f, 0), 0);
    CHECK_EQ(update_of(f, 1), LDN_NODE_CONNECT);
    CHECK_EQ(info_at(f)->ldn.node_count, 2);
    g_fake->net.ldn.nodes[1].is_connected = 0;
    g_fake->net.ldn.node_count = 1;
    g_fake->change(g_fake->ctx, &g_fake->net, true, 0);
    CHECK_EQ(latest(f), 0);
    CHECK_EQ(update_of(f, 1), LDN_NODE_DISCONNECT);
    CHECK_EQ(state(f), LDN_STATE_ACCESS_POINT_CREATED);

    CHECK_EQ(tc_cmd(f.c, f.u, DESTROY), 0);
    CHECK_EQ(g_fake->destroys, 1);
    CHECK_EQ(state(f), LDN_STATE_ACCESS_POINT);
    CHECK(!rl_ldn_route(NULL, &ip, &mask));
    r = tc_send(f.c, f.u, tc_new(GET_REASON));
    CHECK_EQ(tc_u32(&r, 0) & 0xffff, LDN_DESTROYED_BY_USER);
    CHECK_EQ(tc_cmd(f.c, f.u, DESTROY), LDN_RESULT_INVALID_STATE);
    CHECK_EQ(tc_cmd(f.c, f.u, CLOSE_AP), 0);
    CHECK_EQ(state(f), LDN_STATE_INITIALIZED);
    CHECK_EQ(tc_cmd(f.c, f.u, FINALIZE), 0);
    CHECK_EQ(state(f), LDN_STATE_NONE);
    CHECK(g_fake == NULL);
    rl_ldn_set_backend_factory(NULL);
}

TEST(ldn_station_scans_connects_and_goes_back_to_station_when_the_host_leaves)
{
    fx f = open_ldn(fake_factory);
    CHECK_EQ(tc_cmd(f.c, f.u, INIT_OLD), 0);
    CHECK_EQ(tc_cmd(f.c, f.u, OPEN_STA), 0);
    CHECK_EQ(state(f), LDN_STATE_STATION);
    tc_res r = scan(f, LDN_FILTER_LOCAL_COMMUNICATION_ID, 0x0100abcd00000000ull);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    CHECK(strcmp(info_at(f)->ldn.nodes[0].user_name, "host") == 0);
    r = scan(f, LDN_FILTER_LOCAL_COMMUNICATION_ID, 0x0100000000000001ull);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
    CHECK_EQ(scan(f, 0x40, 0).result, LDN_RESULT_INVALID_ARGUMENT);

    ldn_network_info n = offered();
    take_event(f);
    CHECK_EQ(connect_to(f, &n), 0);
    CHECK_EQ(state(f), LDN_STATE_STATION_CONNECTED);
    CHECK(take_event(f));
    CHECK_EQ(get_info(f).result, 0);
    CHECK_EQ(info_at(f)->ldn.node_count, 2);
    CHECK(strcmp(info_at(f)->ldn.nodes[1].user_name, "guest") == 0);
    CHECK_EQ(connect_to(f, &n), LDN_RESULT_INVALID_STATE);
    CHECK_EQ(tc_cmd(f.c, f.u, DISCONNECT), 0);
    CHECK_EQ(g_fake->disconnects, 1);
    CHECK_EQ(state(f), LDN_STATE_STATION);

    CHECK_EQ(connect_to(f, &n), 0);
    CHECK_EQ(state(f), LDN_STATE_STATION_CONNECTED);
    ldn_network_info gone;
    memset(&gone, 0, sizeof gone);
    take_event(f);
    g_fake->change(g_fake->ctx, &gone, false, LDN_DISCONNECT_SIGNAL_LOST);
    CHECK(take_event(f));
    CHECK_EQ(state(f), LDN_STATE_STATION);
    r = tc_send(f.c, f.u, tc_new(GET_REASON));
    CHECK_EQ(tc_u32(&r, 0) & 0xffff, LDN_DISCONNECT_SIGNAL_LOST);
    uint32_t ip, mask;
    CHECK(!rl_ldn_route(NULL, &ip, &mask));
    CHECK_EQ(tc_cmd(f.c, f.u, CLOSE_STA), 0);
    CHECK_EQ(state(f), LDN_STATE_INITIALIZED);
    CHECK_EQ(tc_cmd(f.c, f.u, CLOSE_STA), LDN_RESULT_INVALID_STATE);
    rl_ldn_set_backend_factory(NULL);
}

TEST(ldn_offline_finds_no_one_and_cannot_host_or_join)
{
    fx f = open_ldn(rl_ldn_offline_backend);
    CHECK_EQ(tc_cmd(f.c, f.u, INIT_OLD), 0);
    CHECK_EQ(tc_cmd(f.c, f.u, OPEN_STA), 0);
    tc_res r = scan(f, 0, 0);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
    ldn_network_info n = offered();
    CHECK_EQ(connect_to(f, &n), LDN_RESULT_CONNECT_NOT_FOUND);
    CHECK_EQ(state(f), LDN_STATE_STATION);
    CHECK_EQ(tc_cmd(f.c, f.u, CLOSE_STA), 0);
    CHECK_EQ(tc_cmd(f.c, f.u, OPEN_AP), 0);
    CHECK_EQ(create(f, 4), LDN_RESULT_DEVICE_NOT_AVAILABLE);
    CHECK_EQ(state(f), LDN_STATE_ACCESS_POINT);
    CHECK_EQ(tc_cmd(f.c, f.u, FINALIZE), 0);
    CHECK_EQ(state(f), LDN_STATE_NONE);
    rl_ldn_set_backend_factory(NULL);
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/** @brief Nodes of the network the process is in, 0 when in none. */
static int node_count(fx f)
{
    tc_res r = get_info(f);
    return r.result ? 0 : info_at(f)->ldn.node_count;
}

/**
 * @brief Two processes on a network, through ldn:u with net.mode lan: RELIFT_LDN_PAIR=host hosts a network
 * and waits for a station to join and leave; RELIFT_LDN_PAIR=station finds it, joins and leaves. Without the
 * variable it does nothing (tools/ldn_pair.sh runs it in two network namespaces).
 */
TEST(ldn_pair_hosts_or_joins_a_network_of_another_process)
{
    const char *role = getenv("RELIFT_LDN_PAIR");
    if (!role)
        return;
    setenv("RELIFT_NET", "lan", 1);
    rl_switch_declare_settings();
    fx f = open_ldn(NULL);
    CHECK_EQ(tc_cmd(f.c, f.u, INIT_OLD), 0);
    if (strcmp(role, "host") == 0) {
        CHECK_EQ(tc_cmd(f.c, f.u, OPEN_AP), 0);
        CHECK_EQ(create(f, 4), 0);
        CHECK_EQ(state(f), LDN_STATE_ACCESS_POINT_CREATED);
        int i = 0;
        while (node_count(f) < 2 && i++ < 300)
            sleep_ms(50);
        CHECK_EQ(node_count(f), 2);
        CHECK(strcmp(info_at(f)->ldn.nodes[1].user_name, "guest") == 0);
        printf("ldn pair: host saw %s join\n", info_at(f)->ldn.nodes[1].user_name);
        i = 0;
        while (node_count(f) == 2 && i++ < 300)
            sleep_ms(50);
        CHECK_EQ(node_count(f), 1);
        printf("ldn pair: host saw the station leave\n");
        CHECK_EQ(tc_cmd(f.c, f.u, DESTROY), 0);
    } else {
        CHECK_EQ(tc_cmd(f.c, f.u, OPEN_STA), 0);
        tc_res r;
        int i = 0;
        do
            r = scan(f, LDN_FILTER_LOCAL_COMMUNICATION_ID, 0x0100abcd00000000ull);
        while (r.result == 0 && tc_u32(&r, 0) == 0 && i++ < 10);
        CHECK_EQ(r.result, 0);
        CHECK_EQ(tc_u32(&r, 0), 1);
        ldn_network_info n = *info_at(f);
        printf("ldn pair: station found %s at %08x\n", n.ldn.nodes[0].user_name, n.ldn.nodes[0].ipv4);
        CHECK_EQ(connect_to(f, &n), 0);
        CHECK_EQ(state(f), LDN_STATE_STATION_CONNECTED);
        CHECK_EQ(node_count(f), 2);
        tc_res ip = tc_send(f.c, f.u, tc_new(GET_IP));
        CHECK_EQ(ip.result, 0);
        printf("ldn pair: station joined as %08x\n", tc_u32(&ip, 0));
        sleep_ms(500);
        CHECK_EQ(tc_cmd(f.c, f.u, DISCONNECT), 0);
    }
    CHECK_EQ(tc_cmd(f.c, f.u, FINALIZE), 0);
}
