/**
 * @file ldn_lan.c
 * @brief The LAN backend of local wireless play: the ldn_mitm protocol, as
 * Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors)
 * Services/Ldn/UserServiceCreator/LdnMitm implements it. A station
 * broadcasts Scan on UDP port 11452 and every host answers it with its
 * NetworkInfo; a station joins by opening TCP to the host's node 0 address
 * and sending its NodeInfo, and the host sends the whole NetworkInfo
 * (SyncNetwork) to every member each time it changes. One host thread per
 * backend polls the sockets; the service's calls wait on it with a
 * condition variable. Node addresses are the host interface's own, so the
 * game's sockets reach the members directly (bsd_net.c routes them).
 */
#include "ldn_lan.h"

#include "ldn.h"

#include <relift/net.h>
#include <relift/settings.h>

#include "core.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

size_t rl_ldn_lan_compress(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t b = in[i++];
        if (o >= cap)
            return 0;
        out[o++] = b;
        if (b == 0) {
            uint8_t count = 0;
            while (i < n && in[i] == 0 && count < 0xff) {
                count++;
                i++;
            }
            if (o >= cap)
                return 0;
            out[o++] = count;
        }
    }
    return o;
}

size_t rl_ldn_lan_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t b = in[i++];
        if (o >= cap)
            return 0;
        out[o++] = b;
        if (b == 0) {
            if (i == n)
                return 0;
            size_t count = in[i++];
            if (o + count > cap)
                return 0;
            memset(out + o, 0, count);
            o += count;
        }
    }
    return o;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

size_t rl_ldn_lan_pack(uint8_t type, const void *body, size_t n, uint8_t *out, size_t cap)
{
    if (cap < LDN_LAN_HEADER)
        return 0;
    memset(out, 0, LDN_LAN_HEADER);
    uint32_t m = LDN_LAN_MAGIC;
    for (int i = 0; i < 4; i++)
        out[i] = (uint8_t)(m >> (8 * i));
    out[4] = type;
    if (n == 0)
        return LDN_LAN_HEADER;
    size_t c = rl_ldn_lan_compress(body, n, out + LDN_LAN_HEADER, cap - LDN_LAN_HEADER);
    if (c) {
        out[5] = 1;
        put16(out + 6, (uint16_t)c);
        put16(out + 8, (uint16_t)n);
        return LDN_LAN_HEADER + c;
    }
    if (LDN_LAN_HEADER + n > cap)
        return 0;
    memcpy(out + LDN_LAN_HEADER, body, n);
    put16(out + 6, (uint16_t)n);
    return LDN_LAN_HEADER + n;
}

bool rl_ldn_lan_feed(rl_ldn_lan_reader *r, const uint8_t *data, size_t n, rl_ldn_lan_packet_fn *fn, void *ctx)
{
    size_t at = 0;
    while (at < n) {
        if (r->len < LDN_LAN_HEADER) {
            size_t take = LDN_LAN_HEADER - r->len < n - at ? LDN_LAN_HEADER - r->len : n - at;
            memcpy(r->buf + r->len, data + at, take);
            r->len += take;
            at += take;
            if (r->len < LDN_LAN_HEADER)
                return true;
        }
        uint32_t magic = (uint32_t)r->buf[0] | (uint32_t)r->buf[1] << 8 | (uint32_t)r->buf[2] << 16 |
                         (uint32_t)r->buf[3] << 24;
        size_t total = LDN_LAN_HEADER + get16(r->buf + 6);
        if (magic != LDN_LAN_MAGIC || total > LDN_LAN_BUFFER) {
            r->len = 0;
            return false;
        }
        size_t take = total - r->len < n - at ? total - r->len : n - at;
        memcpy(r->buf + r->len, data + at, take);
        r->len += take;
        at += take;
        if (r->len < total)
            return true;
        r->len = 0;
        const uint8_t *body = r->buf + LDN_LAN_HEADER;
        size_t len = total - LDN_LAN_HEADER;
        if (r->buf[5] == 1) {
            uint8_t plain[LDN_LAN_BUFFER];
            size_t got = rl_ldn_lan_decompress(body, len, plain, sizeof plain);
            if (got != get16(r->buf + 8))
                return false;
            fn(ctx, r->buf[4], plain, got);
        } else {
            fn(ctx, r->buf[4], body, len);
        }
    }
    return true;
}

static pthread_mutex_t g_opts_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_has_opts;
static char g_opt_iface[64];
static uint16_t g_opt_port = LDN_LAN_PORT, g_opt_peer = LDN_LAN_PORT;
static uint32_t g_opt_scan_ip;

void rl_ldn_lan_set_opts(const rl_ldn_lan_opts *o)
{
    pthread_mutex_lock(&g_opts_lock);
    g_has_opts = o != NULL;
    g_opt_iface[0] = 0;
    if (o && o->iface)
        strncpy(g_opt_iface, o->iface, sizeof g_opt_iface - 1);
    g_opt_port = o ? o->port : LDN_LAN_PORT;
    g_opt_peer = o ? o->peer_port : LDN_LAN_PORT;
    g_opt_scan_ip = o ? o->scan_ip : 0;
    pthread_mutex_unlock(&g_opts_lock);
}

/** @brief How long a scan waits for the first answer, and a connect for the host. */
#define SCAN_WAIT_MS 1000
#define CONNECT_WAIT_MS 4000
/** @brief Least time between two scans, so a game that scans in a loop stays at one broadcast a second. */
#define SCAN_GAP_MS 1000
#define MAX_FOUND 16

/** @brief A station's TCP link as the host sees it. */
typedef struct member {
    rl_net_sock *sock;
    uint32_t ip;
    rl_ldn_lan_reader rd;
    int node;
    ldn_node_info info;
} member;

typedef struct lan {
    ldn_backend b;
    ldn_change_fn *change;
    void *ctx;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    bool stop;
    uint32_t ip, mask;
    uint16_t port, peer_port;
    uint32_t scan_ip;
    rl_net_sock *udp;
    rl_ldn_lan_reader udp_rd;
    uint32_t from_ip;
    uint16_t from_port;
    bool hosting;
    rl_net_sock *listen;
    member members[LDN_NODE_MAX - 1];
    rl_net_sock *client;
    rl_ldn_lan_reader client_rd;
    bool synced;
    bool lost;
    ldn_network_info net;
    bool scanning;
    ldn_network_info found[MAX_FOUND];
    int nfound;
    uint64_t last_scan_ms;
    uint64_t rng;
    bool notify;
    bool notify_connected;
    uint32_t notify_reason;
} lan;

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/** @brief Waits on the condition until deadline_ms (monotonic clock of now_ms). Lock held. */
static void wait_until(lan *l, uint64_t deadline_ms)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    uint64_t left = deadline_ms > now_ms() ? deadline_ms - now_ms() : 0;
    t.tv_sec += (time_t)(left / 1000);
    t.tv_nsec += (long)(left % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&l->cond, &l->lock, &t);
}

static uint64_t next_random(lan *l)
{
    uint64_t z = (l->rng += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static void fill_random(lan *l, uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)next_random(l);
}

/** @brief The made-up MAC of an address: 02:00 and the four address bytes. */
static void fake_mac(uint32_t ip, uint8_t mac[6])
{
    mac[0] = 2;
    mac[1] = 0;
    mac[2] = (uint8_t)(ip >> 24);
    mac[3] = (uint8_t)(ip >> 16);
    mac[4] = (uint8_t)(ip >> 8);
    mac[5] = (uint8_t)ip;
}

/** @brief Queues a change for the service, told after the lock is dropped. Lock held. */
static void queue_change(lan *l, bool connected, uint32_t reason)
{
    l->notify = true;
    l->notify_connected = connected;
    l->notify_reason = reason;
}

/** @brief Drops the lock and tells the service of a queued change. */
static void unlock_and_notify(lan *l)
{
    bool notify = l->notify;
    bool connected = l->notify_connected;
    uint32_t reason = l->notify_reason;
    ldn_network_info info = l->net;
    l->notify = false;
    pthread_mutex_unlock(&l->lock);
    if (notify)
        l->change(l->ctx, &info, connected, reason);
}

/** @brief Sends a packet on a TCP link; false when it failed. A full send buffer drops the link too, as members are few and slow. */
static bool send_tcp(rl_net_sock *s, uint8_t type, const void *body, size_t n)
{
    uint8_t pkt[LDN_LAN_BUFFER + LDN_LAN_HEADER];
    size_t len = rl_ldn_lan_pack(type, body, n, pkt, sizeof pkt);
    size_t at = 0;
    uint64_t deadline = now_ms() + 500;
    while (len && at < len) {
        long w = rl_net_send(s, pkt + at, len - at);
        if (w == RL_NET_FAILED)
            return false;
        if (w > 0) {
            at += (size_t)w;
            continue;
        }
        if (now_ms() > deadline)
            return false;
        rl_net_sock *one[1] = { s };
        uint8_t ev = RL_NET_OUT, rev = 0;
        rl_net_poll(one, &ev, &rev, 1, 10);
    }
    return len != 0;
}

/** @brief The host's node table after a member came or went: counts, SyncNetwork to every member, a change. Lock held. */
static void update_nodes(lan *l)
{
    int count = 1;
    for (int i = 0; i < LDN_NODE_MAX - 1; i++) {
        member *m = &l->members[i];
        if (m->sock && m->node > 0) {
            count++;
            m->info.node_id = (uint8_t)m->node;
            m->info.is_connected = 1;
            l->net.ldn.nodes[m->node] = m->info;
        }
    }
    l->net.ldn.node_count = (uint8_t)count;
    for (int i = 0; i < LDN_NODE_MAX - 1; i++) {
        member *m = &l->members[i];
        if (m->sock && m->node > 0 && !send_tcp(m->sock, LDN_LAN_SYNC_NETWORK, &l->net, sizeof l->net)) {
            rl_net_close(m->sock);
            m->sock = NULL;
        }
    }
    queue_change(l, true, 0);
}

/** @brief Drops member m and clears its node. Lock held. */
static void drop_member(lan *l, member *m)
{
    int node = m->node;
    rl_net_close(m->sock);
    memset(m, 0, sizeof *m);
    if (node > 0) {
        memset(&l->net.ldn.nodes[node], 0, sizeof l->net.ldn.nodes[node]);
        l->net.ldn.nodes[node].node_id = (uint8_t)node;
        update_nodes(l);
    }
}

/** @brief A free node slot of the hosted network, or -1. Lock held. */
static int free_node(lan *l)
{
    for (int n = 1; n < LDN_NODE_MAX && n < (l->net.ldn.node_count_max ? l->net.ldn.node_count_max : LDN_NODE_MAX);
         n++) {
        bool used = false;
        for (int i = 0; i < LDN_NODE_MAX - 1; i++)
            used |= l->members[i].sock && l->members[i].node == n;
        if (!used)
            return n;
    }
    return -1;
}

typedef struct from_member {
    lan *l;
    member *m;
} from_member;

/** @brief A packet from a station: Connect puts it in a node. Lock held. */
static void on_member_packet(void *ctx, uint8_t type, const uint8_t *body, size_t n)
{
    from_member *fm = ctx;
    if (type != LDN_LAN_CONNECT || n != sizeof(ldn_node_info) || fm->m->node > 0)
        return;
    int node = free_node(fm->l);
    if (node < 0) {
        drop_member(fm->l, fm->m);
        return;
    }
    memcpy(&fm->m->info, body, n);
    fm->m->node = node;
    update_nodes(fm->l);
}

/** @brief A packet from the host to this station: SyncNetwork. Lock held. */
static void on_client_packet(void *ctx, uint8_t type, const uint8_t *body, size_t n)
{
    lan *l = ctx;
    if (type != LDN_LAN_SYNC_NETWORK || n != sizeof(ldn_network_info))
        return;
    memcpy(&l->net, body, n);
    l->synced = true;
    queue_change(l, true, 0);
    pthread_cond_broadcast(&l->cond);
}

/** @brief A datagram on the discovery port: answer a Scan while hosting, keep a ScanResponse while scanning. Lock held. */
static void on_udp_packet(void *ctx, uint8_t type, const uint8_t *body, size_t n)
{
    lan *l = ctx;
    if (type == LDN_LAN_SCAN && l->hosting) {
        uint8_t pkt[LDN_LAN_BUFFER + LDN_LAN_HEADER];
        size_t len = rl_ldn_lan_pack(LDN_LAN_SCAN_RESPONSE, &l->net, sizeof l->net, pkt, sizeof pkt);
        if (len)
            rl_net_sendto(l->udp, pkt, len, l->from_ip, l->from_port);
    } else if (type == LDN_LAN_SCAN_RESPONSE && l->scanning && n == sizeof(ldn_network_info)) {
        const ldn_network_info *info = (const ldn_network_info *)(const void *)body;
        int at = 0;
        while (at < l->nfound && memcmp(l->found[at].common.bssid, info->common.bssid, 6) != 0)
            at++;
        if (at < MAX_FOUND) {
            l->found[at] = *info;
            if (at == l->nfound)
                l->nfound++;
            pthread_cond_broadcast(&l->cond);
        }
    }
}

/** @brief One turn of the service thread: poll every socket for up to 5 ms and handle what came. */
static void turn(lan *l)
{
    rl_net_sock *socks[LDN_NODE_MAX + 2];
    uint8_t ev[LDN_NODE_MAX + 2], rev[LDN_NODE_MAX + 2];
    int n = 0;
    pthread_mutex_lock(&l->lock);
    socks[n] = l->udp;
    ev[n++] = RL_NET_IN;
    if (l->listen) {
        socks[n] = l->listen;
        ev[n++] = RL_NET_IN;
    }
    for (int i = 0; i < LDN_NODE_MAX - 1; i++)
        if (l->members[i].sock) {
            socks[n] = l->members[i].sock;
            ev[n++] = RL_NET_IN;
        }
    if (l->client) {
        socks[n] = l->client;
        ev[n++] = RL_NET_IN;
    }
    pthread_mutex_unlock(&l->lock);
    if (rl_net_poll(socks, ev, rev, n, 5) <= 0)
        return;

    pthread_mutex_lock(&l->lock);
    uint8_t buf[LDN_LAN_BUFFER + LDN_LAN_HEADER];
    for (;;) {
        long got = rl_net_recvfrom(l->udp, buf, sizeof buf, &l->from_ip, &l->from_port);
        if (got < 0)
            break;
        bool self = l->from_ip == l->ip && l->from_port == l->port;
        bool near = (l->from_ip & l->mask) == (l->ip & l->mask);
        l->udp_rd.len = 0;
        if (!self && near)
            rl_ldn_lan_feed(&l->udp_rd, buf, (size_t)got, on_udp_packet, l);
    }
    if (l->listen) {
        uint32_t ip;
        rl_net_sock *s;
        while ((s = rl_net_tcp_accept(l->listen, &ip, NULL))) {
            int slot = 0;
            while (slot < LDN_NODE_MAX - 1 && l->members[slot].sock)
                slot++;
            if (slot == LDN_NODE_MAX - 1 || (ip & l->mask) != (l->ip & l->mask)) {
                rl_net_close(s);
                continue;
            }
            memset(&l->members[slot], 0, sizeof l->members[slot]);
            l->members[slot].sock = s;
            l->members[slot].ip = ip;
        }
    }
    for (int i = 0; i < LDN_NODE_MAX - 1; i++) {
        member *m = &l->members[i];
        while (m->sock) {
            long got = rl_net_recv(m->sock, buf, sizeof buf);
            if (got == RL_NET_AGAIN)
                break;
            from_member fm = { l, m };
            if (got <= 0 || !rl_ldn_lan_feed(&m->rd, buf, (size_t)got, on_member_packet, &fm)) {
                drop_member(l, m);
                break;
            }
        }
    }
    while (l->client) {
        long got = rl_net_recv(l->client, buf, sizeof buf);
        if (got == RL_NET_AGAIN)
            break;
        if (got <= 0 || !rl_ldn_lan_feed(&l->client_rd, buf, (size_t)got, on_client_packet, l)) {
            rl_net_close(l->client);
            l->client = NULL;
            bool was = l->synced;
            l->synced = false;
            l->lost = true;
            memset(&l->net, 0, sizeof l->net);
            if (was)
                queue_change(l, false, LDN_DESTROYED_BY_USER);
            pthread_cond_broadcast(&l->cond);
        }
    }
    unlock_and_notify(l);
}

static void *lan_thread(void *arg)
{
    lan *l = arg;
    for (;;) {
        pthread_mutex_lock(&l->lock);
        bool stop = l->stop;
        pthread_mutex_unlock(&l->lock);
        if (stop)
            return NULL;
        turn(l);
    }
}

static bool lan_address(ldn_backend *b, uint32_t *ip, uint32_t *mask)
{
    lan *l = (lan *)b;
    *ip = l->ip;
    *mask = l->mask;
    return true;
}

static int lan_scan(ldn_backend *b, uint16_t channel, const ldn_scan_filter *filter, ldn_network_info *out, int max)
{
    (void)channel;
    lan *l = (lan *)b;
    pthread_mutex_lock(&l->lock);
    uint64_t now = now_ms();
    if (l->last_scan_ms && now < l->last_scan_ms + SCAN_GAP_MS) {
        uint64_t until = l->last_scan_ms + SCAN_GAP_MS;
        while (now_ms() < until && !l->stop)
            wait_until(l, until);
    }
    l->last_scan_ms = now_ms();
    l->nfound = 0;
    l->scanning = true;
    uint8_t pkt[LDN_LAN_HEADER];
    size_t len = rl_ldn_lan_pack(LDN_LAN_SCAN, NULL, 0, pkt, sizeof pkt);
    rl_net_sendto(l->udp, pkt, len, l->scan_ip ? l->scan_ip : l->ip | ~l->mask, l->peer_port);
    uint64_t deadline = now_ms() + SCAN_WAIT_MS;
    while (l->nfound == 0 && now_ms() < deadline && !l->stop)
        wait_until(l, deadline);
    if (l->nfound) {
        uint64_t more = now_ms() + 33;
        while (now_ms() < more && !l->stop)
            wait_until(l, more);
    }
    l->scanning = false;
    int n = 0;
    for (int i = 0; i < l->nfound && n < max; i++)
        if (l->found[i].ldn.nodes[0].user_name[0] && rl_ldn_filter_match(filter, &l->found[i]))
            out[n++] = l->found[i];
    pthread_mutex_unlock(&l->lock);
    return n;
}

static uint32_t lan_create(ldn_backend *b, const ldn_security_config *sec, const ldn_user_config *user,
                           const ldn_network_config *net, const void *adv, size_t adv_size)
{
    lan *l = (lan *)b;
    rl_net_sock *ls = rl_net_tcp_listen(l->ip, l->port, LDN_NODE_MAX);
    if (!ls) {
        rl_log(RL_LOG_WARN, "ldn", "cannot listen on TCP port %u for local play", l->port);
        return LDN_RESULT_DEVICE_NOT_AVAILABLE;
    }
    pthread_mutex_lock(&l->lock);
    l->listen = ls;
    l->hosting = true;
    memset(&l->net, 0, sizeof l->net);
    fake_mac(l->ip, l->net.common.bssid);
    l->net.common.ssid.length = LDN_SSID_MAX;
    for (int i = 0; i < LDN_SSID_MAX; i++)
        l->net.common.ssid.name[i] = (char)('a' + next_random(l) % 26);
    l->net.common.channel = net->channel ? net->channel : 6;
    l->net.common.link_level = 3;
    l->net.common.network_type = 2;
    l->net.id.intent = net->intent;
    fill_random(l, l->net.id.session_id, sizeof l->net.id.session_id);
    l->net.ldn.node_count_max = net->node_count_max ? net->node_count_max : LDN_NODE_MAX;
    l->net.ldn.security_mode = sec->mode;
    for (int i = 0; i < LDN_NODE_MAX; i++)
        l->net.ldn.nodes[i].node_id = (uint8_t)i;
    ldn_node_info *me = &l->net.ldn.nodes[0];
    me->ipv4 = l->ip;
    fake_mac(l->ip, me->mac);
    me->is_connected = 1;
    memcpy(me->user_name, user->user_name, sizeof me->user_name - 1);
    me->local_communication_version = net->local_communication_version;
    memcpy(l->net.ldn.advertise, adv, adv_size);
    l->net.ldn.advertise_size = (uint16_t)adv_size;
    l->net.ldn.node_count = 1;
    queue_change(l, true, 0);
    unlock_and_notify(l);
    return 0;
}

static void lan_destroy(ldn_backend *b)
{
    lan *l = (lan *)b;
    pthread_mutex_lock(&l->lock);
    for (int i = 0; i < LDN_NODE_MAX - 1; i++)
        if (l->members[i].sock) {
            rl_net_close(l->members[i].sock);
            memset(&l->members[i], 0, sizeof l->members[i]);
        }
    rl_net_close(l->listen);
    l->listen = NULL;
    l->hosting = false;
    memset(&l->net, 0, sizeof l->net);
    pthread_mutex_unlock(&l->lock);
}

static void lan_set_advertise(ldn_backend *b, const void *data, size_t size)
{
    lan *l = (lan *)b;
    pthread_mutex_lock(&l->lock);
    if (l->hosting) {
        memset(l->net.ldn.advertise, 0, sizeof l->net.ldn.advertise);
        memcpy(l->net.ldn.advertise, data, size);
        l->net.ldn.advertise_size = (uint16_t)size;
        update_nodes(l);
    }
    unlock_and_notify(l);
}

static uint32_t lan_reject(ldn_backend *b, uint32_t ip)
{
    lan *l = (lan *)b;
    uint32_t rc = LDN_RESULT_NODE_NOT_FOUND;
    pthread_mutex_lock(&l->lock);
    for (int i = 0; i < LDN_NODE_MAX - 1; i++)
        if (l->members[i].sock && l->members[i].node > 0 && l->members[i].info.ipv4 == ip) {
            drop_member(l, &l->members[i]);
            rc = 0;
        }
    unlock_and_notify(l);
    return rc;
}

static uint32_t lan_connect(ldn_backend *b, const ldn_network_info *info, const ldn_security_config *sec,
                            const ldn_user_config *user, uint16_t version)
{
    (void)sec;
    lan *l = (lan *)b;
    if (info->ldn.node_count == 0)
        return LDN_RESULT_CONNECT_NOT_FOUND;
    uint32_t host = info->ldn.nodes[0].ipv4;
    if ((host & l->mask) != (l->ip & l->mask))
        return LDN_RESULT_CONNECT_NOT_FOUND;
    rl_net_sock *s = rl_net_tcp_connect(l->ip, host, l->peer_port);
    if (!s)
        return LDN_RESULT_CONNECT_FAILURE;
    rl_net_sock *one[1] = { s };
    uint8_t ev = RL_NET_OUT, rev = 0;
    if (rl_net_poll(one, &ev, &rev, 1, CONNECT_WAIT_MS) <= 0 || rl_net_error(s) != 0) {
        rl_net_close(s);
        return LDN_RESULT_CONNECT_FAILURE;
    }
    ldn_node_info me;
    memset(&me, 0, sizeof me);
    me.ipv4 = l->ip;
    fake_mac(l->ip, me.mac);
    me.is_connected = 1;
    memcpy(me.user_name, user->user_name, sizeof me.user_name - 1);
    me.local_communication_version = version;
    if (!send_tcp(s, LDN_LAN_CONNECT, &me, sizeof me)) {
        rl_net_close(s);
        return LDN_RESULT_CONNECT_FAILURE;
    }
    pthread_mutex_lock(&l->lock);
    l->client = s;
    l->client_rd.len = 0;
    l->synced = false;
    l->lost = false;
    uint64_t deadline = now_ms() + CONNECT_WAIT_MS;
    while (!l->synced && !l->lost && now_ms() < deadline && !l->stop)
        wait_until(l, deadline);
    uint32_t rc = l->synced ? 0 : LDN_RESULT_CONNECT_TIMEOUT;
    if (rc) {
        rl_net_close(l->client);
        l->client = NULL;
    }
    pthread_mutex_unlock(&l->lock);
    return rc;
}

static void lan_disconnect(ldn_backend *b)
{
    lan *l = (lan *)b;
    pthread_mutex_lock(&l->lock);
    rl_net_close(l->client);
    l->client = NULL;
    l->synced = false;
    memset(&l->net, 0, sizeof l->net);
    pthread_mutex_unlock(&l->lock);
}

static void lan_free(ldn_backend *b)
{
    lan *l = (lan *)b;
    pthread_mutex_lock(&l->lock);
    l->stop = true;
    pthread_cond_broadcast(&l->cond);
    pthread_mutex_unlock(&l->lock);
    pthread_join(l->thread, NULL);
    lan_destroy(b);
    lan_disconnect(b);
    rl_net_close(l->udp);
    pthread_cond_destroy(&l->cond);
    pthread_mutex_destroy(&l->lock);
    free(l);
}

ldn_backend *rl_ldn_lan_backend(ldn_change_fn *change, void *ctx)
{
    pthread_mutex_lock(&g_opts_lock);
    char want[64];
    strcpy(want, g_has_opts && g_opt_iface[0] ? g_opt_iface : "");
    bool from_settings = !g_has_opts || !g_opt_iface[0];
    uint16_t port = g_opt_port, peer = g_opt_peer;
    uint32_t scan_ip = g_opt_scan_ip;
    pthread_mutex_unlock(&g_opts_lock);
    if (from_settings) {
        strncpy(want, rl_setting_str("net.interface"), sizeof want - 1);
        want[sizeof want - 1] = 0;
    }
    rl_net_iface ifc;
    if (!rl_net_pick_iface(want, &ifc)) {
        rl_log(RL_LOG_WARN, "ldn", "no network interface %s%s%s for local play", want[0] ? "named " : "",
               want, want[0] ? "" : "is up");
        return NULL;
    }
    rl_net_sock *udp = rl_net_udp(0, port);
    if (!udp) {
        rl_log(RL_LOG_WARN, "ldn", "cannot open UDP port %u for local play", port);
        return NULL;
    }
    lan *l = calloc(1, sizeof *l);
    if (!l) {
        rl_net_close(udp);
        return NULL;
    }
    l->b = (ldn_backend){ lan_address, lan_scan,   lan_create,     lan_destroy, lan_set_advertise,
                          lan_reject,  lan_connect, lan_disconnect, lan_free };
    l->change = change;
    l->ctx = ctx;
    l->ip = ifc.ip;
    l->mask = ifc.mask;
    l->port = port;
    l->peer_port = peer;
    l->scan_ip = scan_ip;
    l->udp = udp;
    l->rng = now_ms() ^ ((uint64_t)ifc.ip << 32) ^ (uint64_t)(uintptr_t)l;
    pthread_mutex_init(&l->lock, NULL);
    pthread_cond_init(&l->cond, NULL);
    if (pthread_create(&l->thread, NULL, lan_thread, l) != 0) {
        rl_net_close(udp);
        pthread_cond_destroy(&l->cond);
        pthread_mutex_destroy(&l->lock);
        free(l);
        return NULL;
    }
    rl_log(RL_LOG_INFO, "ldn", "local play on %s %u.%u.%u.%u/%u.%u.%u.%u", ifc.name, ifc.ip >> 24, ifc.ip >> 16 & 255,
           ifc.ip >> 8 & 255, ifc.ip & 255, ifc.mask >> 24, ifc.mask >> 16 & 255, ifc.mask >> 8 & 255, ifc.mask & 255);
    return &l->b;
}
