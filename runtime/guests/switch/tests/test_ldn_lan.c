/**
 * @file test_ldn_lan.c
 * @brief The ldn_mitm LAN protocol: packet bytes and compression against
 * fixed values, stream reassembly, and a host and a station in one process
 * over the loopback interface (scan, connect, the host seeing the station
 * join and leave, the station seeing the host go).
 */
#include "t.h"

#include "ldn.h"
#include "ldn_lan.h"

#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

TEST(ldn_lan_compression_writes_zero_runs_and_reads_them_back)
{
    const uint8_t in[] = { 1, 0, 0, 0, 2, 0, 3 };
    uint8_t c[16], d[16];
    size_t n = rl_ldn_lan_compress(in, sizeof in, c, sizeof c);
    const uint8_t want[] = { 1, 0, 2, 2, 0, 0, 3 };
    CHECK_EQ(n, sizeof want);
    CHECK(memcmp(c, want, sizeof want) == 0);
    CHECK_EQ(rl_ldn_lan_decompress(c, n, d, sizeof d), sizeof in);
    CHECK(memcmp(d, in, sizeof in) == 0);
    uint8_t zeros[600] = { 0 };
    n = rl_ldn_lan_compress(zeros, sizeof zeros, c, sizeof c);
    CHECK_EQ(n, 6);
    CHECK_EQ(c[1], 255);
    CHECK_EQ(c[3], 255);
    CHECK_EQ(c[5], 600 - 3 - 255 - 255);
    uint8_t back[600];
    CHECK_EQ(rl_ldn_lan_decompress(c, n, back, sizeof back), 600);
    const uint8_t cut[] = { 5, 0 };
    CHECK_EQ(rl_ldn_lan_decompress(cut, sizeof cut, d, sizeof d), 0);
}

typedef struct seen {
    int n;
    uint8_t type[4];
    size_t len[4];
    uint8_t body[4][LDN_LAN_BUFFER];
} seen;

static void keep(void *ctx, uint8_t type, const uint8_t *body, size_t n)
{
    seen *s = ctx;
    if (s->n < 4) {
        s->type[s->n] = type;
        s->len[s->n] = n;
        memcpy(s->body[s->n], body, n);
    }
    s->n++;
}

TEST(ldn_lan_packets_have_the_ldn_mitm_header_and_reassemble_from_a_stream)
{
    uint8_t pkt[64];
    size_t n = rl_ldn_lan_pack(LDN_LAN_SCAN, NULL, 0, pkt, sizeof pkt);
    const uint8_t scan[12] = { 0x00, 0x14, 0x45, 0x11, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK_EQ(n, 12);
    CHECK(memcmp(pkt, scan, 12) == 0);
    const uint8_t body[] = { 7, 0, 0, 9 };
    n = rl_ldn_lan_pack(LDN_LAN_CONNECT, body, sizeof body, pkt, sizeof pkt);
    const uint8_t want[] = { 0x00, 0x14, 0x45, 0x11, 2, 1, 4, 0, 4, 0, 0, 0, 7, 0, 1, 9 };
    CHECK_EQ(n, sizeof want);
    CHECK(memcmp(pkt, want, sizeof want) == 0);

    uint8_t two[128];
    size_t a = rl_ldn_lan_pack(LDN_LAN_SCAN, NULL, 0, two, sizeof two);
    size_t b = rl_ldn_lan_pack(LDN_LAN_CONNECT, body, sizeof body, two + a, sizeof two - a);
    rl_ldn_lan_reader r;
    memset(&r, 0, sizeof r);
    seen s;
    memset(&s, 0, sizeof s);
    for (size_t i = 0; i < a + b; i++)
        CHECK(rl_ldn_lan_feed(&r, two + i, 1, keep, &s));
    CHECK_EQ(s.n, 2);
    CHECK_EQ(s.type[0], LDN_LAN_SCAN);
    CHECK_EQ(s.len[0], 0);
    CHECK_EQ(s.type[1], LDN_LAN_CONNECT);
    CHECK_EQ(s.len[1], 4);
    CHECK(memcmp(s.body[1], body, 4) == 0);
    uint8_t bad[12] = { 1, 2, 3, 4 };
    CHECK(!rl_ldn_lan_feed(&r, bad, sizeof bad, keep, &s));
    CHECK_EQ(r.len, 0);
}

/** @brief What one backend's change callback saw. */
typedef struct watch {
    pthread_mutex_t lock;
    int changes;
    bool connected;
    uint32_t reason;
    ldn_network_info info;
} watch;

static void on_change(void *ctx, const ldn_network_info *info, bool connected, uint32_t reason)
{
    watch *w = ctx;
    pthread_mutex_lock(&w->lock);
    w->changes++;
    w->connected = connected;
    w->reason = reason;
    w->info = *info;
    pthread_mutex_unlock(&w->lock);
}

/** @brief Waits up to 3 s until the watched network has nodes members (or, with nodes 0, is gone). */
static bool wait_nodes(watch *w, int nodes)
{
    for (int i = 0; i < 300; i++) {
        pthread_mutex_lock(&w->lock);
        bool ok = nodes ? w->connected && w->info.ldn.node_count == nodes : !w->connected && w->changes > 0;
        pthread_mutex_unlock(&w->lock);
        if (ok)
            return true;
        struct timespec ts = { 0, 10000000 };
        nanosleep(&ts, NULL);
    }
    return false;
}

TEST(ldn_lan_host_and_station_find_each_other_and_join_over_loopback)
{
    uint16_t base = (uint16_t)(30000 + (getpid() % 10000) * 2);
    watch hw, sw;
    memset(&hw, 0, sizeof hw);
    memset(&sw, 0, sizeof sw);
    pthread_mutex_init(&hw.lock, NULL);
    pthread_mutex_init(&sw.lock, NULL);
    rl_ldn_lan_opts o = { "127.0.0.1", base, base, 0x7f000001 };
    rl_ldn_lan_set_opts(&o);
    ldn_backend *host = rl_ldn_lan_backend(on_change, &hw);
    o.port = (uint16_t)(base + 1);
    rl_ldn_lan_set_opts(&o);
    ldn_backend *sta = rl_ldn_lan_backend(on_change, &sw);
    rl_ldn_lan_set_opts(NULL);
    CHECK(host != NULL);
    CHECK(sta != NULL);
    if (!host || !sta)
        return;

    ldn_security_config sec;
    memset(&sec, 0, sizeof sec);
    sec.mode = 1;
    ldn_user_config hu, su;
    memset(&hu, 0, sizeof hu);
    memset(&su, 0, sizeof su);
    strcpy(hu.user_name, "host");
    strcpy(su.user_name, "station");
    ldn_network_config net;
    memset(&net, 0, sizeof net);
    net.intent.local_communication_id = 0x0100abcd00000000ull;
    net.node_count_max = 4;
    CHECK_EQ(host->create(host, &sec, &hu, &net, "adv", 3), 0);
    CHECK(wait_nodes(&hw, 1));
    uint32_t ip, mask;
    CHECK(host->address(host, &ip, &mask));
    CHECK_EQ(ip, 0x7f000001u);
    CHECK_EQ(hw.info.ldn.nodes[0].ipv4, 0x7f000001u);

    ldn_scan_filter f;
    memset(&f, 0, sizeof f);
    f.flags = LDN_FILTER_LOCAL_COMMUNICATION_ID;
    f.id.intent.local_communication_id = net.intent.local_communication_id;
    ldn_network_info found[4];
    int n = sta->scan(sta, 0, &f, found, 4);
    CHECK_EQ(n, 1);
    CHECK(strcmp(found[0].ldn.nodes[0].user_name, "host") == 0);
    CHECK_EQ(found[0].ldn.advertise_size, 3);
    f.id.intent.local_communication_id = 1;
    CHECK_EQ(sta->scan(sta, 0, &f, found + 1, 3), 0);

    CHECK_EQ(sta->connect(sta, &found[0], &sec, &su, 2), 0);
    CHECK(wait_nodes(&sw, 2));
    CHECK(wait_nodes(&hw, 2));
    CHECK(strcmp(hw.info.ldn.nodes[1].user_name, "station") == 0);
    CHECK_EQ(hw.info.ldn.nodes[1].node_id, 1);
    CHECK(strcmp(sw.info.ldn.nodes[1].user_name, "station") == 0);

    sta->disconnect(sta);
    CHECK(wait_nodes(&hw, 1));

    CHECK_EQ(sta->connect(sta, &found[0], &sec, &su, 2), 0);
    CHECK(wait_nodes(&hw, 2));
    host->destroy(host);
    CHECK(wait_nodes(&sw, 0));
    CHECK_EQ(sw.reason, LDN_DESTROYED_BY_USER);
    CHECK(sta->connect(sta, &found[0], &sec, &su, 2) != 0);

    host->free(host);
    sta->free(sta);
}
