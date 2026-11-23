/**
 * @file srv_ldn.c
 * @brief Local wireless play: ldn:u (nn::ldn::detail::IUserServiceCreator)
 * and its IUserLocalCommunicationService, the state machine of a process
 * that hosts a network (access point) or joins one (station). What carries
 * the network is a backend (ldn.h): offline by default, or the ldn_mitm LAN
 * protocol when net.mode is lan. Command ids, layouts, checks and results
 * follow Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors)
 * Services/Ldn/UserServiceCreator/IUserLocalCommunicationService.cs; a
 * station that loses its host goes back to Station, as Eglantine (GPL-3,
 * src/core/hle/service/ldn) does, as facts only.
 */
#include "ldn.h"

#include "hos.h"
#include "services.h"

#include <relift/settings.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/** @brief The network the process is in, for the socket layer's route. */
static pthread_mutex_t g_route_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_route_up;
static ldn_network_info g_route_info;
static uint32_t g_route_ip, g_route_mask;

static ldn_backend_factory *g_factory;

void rl_ldn_set_backend_factory(ldn_backend_factory *f) { g_factory = f; }

/** @brief The service's state, shared by its handlers and its backend's change callback. */
typedef struct ldn_svc {
    pthread_mutex_t lock;
    uint32_t state;
    uint32_t reason;
    hos_revent *event;
    ldn_backend *backend;
    ldn_network_info info;
    uint8_t latest[LDN_NODE_MAX];
    uint8_t advertise[LDN_ADVERTISE_MAX];
    size_t advertise_size;
} ldn_svc;

static ldn_svc *svc_of(rl_ipc_req *r) { return r->self->state; }

static uint32_t state_of(ldn_svc *s)
{
    pthread_mutex_lock(&s->lock);
    uint32_t st = s->state;
    pthread_mutex_unlock(&s->lock);
    return st;
}

/** @brief Publishes the network the process is in (or none) for the socket layer. */
static void route_publish(ldn_svc *s, bool up)
{
    uint32_t ip = 0, mask = 0;
    if (up && s->backend && !s->backend->address(s->backend, &ip, &mask))
        up = false;
    pthread_mutex_lock(&g_route_lock);
    g_route_up = up;
    if (up)
        g_route_info = s->info;
    g_route_ip = ip;
    g_route_mask = mask;
    pthread_mutex_unlock(&g_route_lock);
}

bool rl_ldn_route(ldn_network_info *info, uint32_t *ip, uint32_t *mask)
{
    pthread_mutex_lock(&g_route_lock);
    bool up = g_route_up;
    if (up) {
        if (info)
            *info = g_route_info;
        *ip = g_route_ip;
        *mask = g_route_mask;
    }
    pthread_mutex_unlock(&g_route_lock);
    return up;
}

/** @brief Sets the state and signals the state change event. */
static void set_state(ldn_svc *s, uint32_t st)
{
    pthread_mutex_lock(&s->lock);
    s->state = st;
    pthread_mutex_unlock(&s->lock);
    rl_hos_event_signal(s->event);
}

/** @brief Marks the nodes that joined or left between two network infos. Lock held. */
static void diff_nodes(ldn_svc *s, const ldn_network_info *before, const ldn_network_info *after)
{
    for (int i = 0; i < LDN_NODE_MAX; i++) {
        bool was = before->ldn.nodes[i].is_connected != 0, is = after->ldn.nodes[i].is_connected != 0;
        if (!was && is)
            s->latest[i] |= LDN_NODE_CONNECT;
        else if (was && !is)
            s->latest[i] |= LDN_NODE_DISCONNECT;
    }
}

/** @brief The backend's change callback: a network came up, changed or went away. */
static void on_change(void *ctx, const ldn_network_info *info, bool connected, uint32_t reason)
{
    ldn_svc *s = ctx;
    pthread_mutex_lock(&s->lock);
    diff_nodes(s, &s->info, info);
    s->info = *info;
    if (connected) {
        if (s->state == LDN_STATE_ACCESS_POINT)
            s->state = LDN_STATE_ACCESS_POINT_CREATED;
        else if (s->state == LDN_STATE_STATION)
            s->state = LDN_STATE_STATION_CONNECTED;
    } else if (s->state == LDN_STATE_ACCESS_POINT_CREATED) {
        s->state = LDN_STATE_ACCESS_POINT;
        s->reason = reason ? reason : LDN_DESTROYED_BY_SYSTEM;
    } else if (s->state == LDN_STATE_STATION_CONNECTED) {
        s->state = LDN_STATE_STATION;
        s->reason = reason ? reason : LDN_DISCONNECT_BY_SYSTEM;
    }
    bool up = s->state == LDN_STATE_ACCESS_POINT_CREATED || s->state == LDN_STATE_STATION_CONNECTED;
    pthread_mutex_unlock(&s->lock);
    route_publish(s, up);
    rl_hos_event_signal(s->event);
}

/** @brief Forgets the network the process was in (after the user ended it). */
static void leave_network(ldn_svc *s, uint32_t st, uint32_t reason)
{
    pthread_mutex_lock(&s->lock);
    ldn_network_info empty;
    memset(&empty, 0, sizeof empty);
    diff_nodes(s, &s->info, &empty);
    s->info = empty;
    s->state = st;
    if (reason)
        s->reason = reason;
    pthread_mutex_unlock(&s->lock);
    route_publish(s, false);
    rl_hos_event_signal(s->event);
}

/** @brief The network info of a process that is in a network; false otherwise. */
static bool current_info(ldn_svc *s, ldn_network_info *out)
{
    pthread_mutex_lock(&s->lock);
    bool in = s->state == LDN_STATE_ACCESS_POINT_CREATED || s->state == LDN_STATE_STATION_CONNECTED;
    if (in)
        *out = s->info;
    pthread_mutex_unlock(&s->lock);
    return in;
}

/** @brief Writes size bytes of zeros to a client buffer, in pieces. */
static void zero_buf(const rl_ipc_buf *b)
{
    static const uint8_t zero[256];
    for (uint64_t at = 0; at < b->size; at += sizeof zero) {
        uint64_t n = b->size - at < sizeof zero ? b->size - at : sizeof zero;
        rl_ipc_buf part = { b->addr + at, n, b->attr };
        rl_ipc_buf_write(&part, zero, n);
    }
}

/** @brief GetState (0): an s32 state. */
static rl_result get_state(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = state_of(svc_of(r));
    return KR_SUCCESS;
}

/** @brief GetNetworkInfo (1): the network the process is in, to a 0x480 buffer; InvalidState when it is in none. */
static rl_result get_network_info(rl_ipc_req *r)
{
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    zero_buf(&b);
    ldn_network_info info;
    if (!current_info(svc_of(r), &info))
        return LDN_RESULT_INVALID_STATE;
    return rl_ipc_buf_write(&b, &info, sizeof info) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

/** @brief GetIpv4Address (2): u32 address and u32 subnet mask of a process in a network; InvalidArgument otherwise. */
static rl_result get_ipv4_address(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    ldn_network_info info;
    uint32_t ip, mask;
    if (!current_info(s, &info) || !s->backend || !s->backend->address(s->backend, &ip, &mask))
        return LDN_RESULT_INVALID_ARGUMENT;
    uint32_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    out[0] = ip;
    out[1] = mask;
    return KR_SUCCESS;
}

/** @brief GetDisconnectReason (3): u16 reason the process last left a network. */
static rl_result get_disconnect_reason(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    uint16_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&s->lock);
    *out = (uint16_t)s->reason;
    pthread_mutex_unlock(&s->lock);
    return KR_SUCCESS;
}

/** @brief GetSecurityParameter (4): 16 zero bytes and the network's session id. */
static rl_result get_security_parameter(rl_ipc_req *r)
{
    ldn_network_info info;
    if (!current_info(svc_of(r), &info))
        return LDN_RESULT_INVALID_STATE;
    uint8_t *out = rl_ipc_out(r, 0x20);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    memcpy(out + 16, info.id.session_id, 16);
    return KR_SUCCESS;
}

/** @brief GetNetworkConfig (5): the network's NetworkConfig. */
static rl_result get_network_config(rl_ipc_req *r)
{
    ldn_network_info info;
    if (!current_info(svc_of(r), &info))
        return LDN_RESULT_INVALID_STATE;
    ldn_network_config *out = rl_ipc_out(r, sizeof *out);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    out->intent = info.id.intent;
    out->channel = info.common.channel;
    out->node_count_max = info.ldn.node_count_max;
    out->local_communication_version = info.ldn.nodes[0].local_communication_version;
    return KR_SUCCESS;
}

/** @brief AttachStateChangeEvent (100): a copy handle of the event signaled at every state or network change. */
static rl_result attach_state_change_event(rl_ipc_req *r) { return rl_ipc_push_copy(r, &svc_of(r)->event->obj); }

/** @brief GetNetworkInfoLatestUpdate (101): the network info, and one NodeLatestUpdate per node, consumed. */
static rl_result get_network_info_latest_update(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    rl_ipc_buf b = rl_ipc_auto_out(r, 0), u = rl_ipc_auto_out(r, 1);
    zero_buf(&b);
    ldn_network_info info;
    if (!current_info(s, &info))
        return LDN_RESULT_INVALID_STATE;
    zero_buf(&u);
    uint8_t updates[LDN_NODE_MAX * 8] = { 0 };
    uint64_t n = u.size / 8 < LDN_NODE_MAX ? u.size / 8 : LDN_NODE_MAX;
    pthread_mutex_lock(&s->lock);
    for (uint64_t i = 0; i < n; i++) {
        updates[i * 8] = s->latest[i];
        s->latest[i] = 0;
    }
    pthread_mutex_unlock(&s->lock);
    if (n)
        rl_ipc_buf_write(&u, updates, n * 8);
    return rl_ipc_buf_write(&b, &info, sizeof info) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

bool rl_ldn_filter_match(const ldn_scan_filter *f, const ldn_network_info *info)
{
    if ((f->flags & LDN_FILTER_LOCAL_COMMUNICATION_ID) &&
        f->id.intent.local_communication_id != info->id.intent.local_communication_id)
        return false;
    if ((f->flags & LDN_FILTER_SESSION_ID) && memcmp(f->id.session_id, info->id.session_id, 16) != 0)
        return false;
    if ((f->flags & LDN_FILTER_NETWORK_TYPE) && f->network_type != info->common.network_type)
        return false;
    if ((f->flags & LDN_FILTER_MAC) && memcmp(f->bssid, info->common.bssid, 6) != 0)
        return false;
    if ((f->flags & LDN_FILTER_SSID) &&
        (f->ssid.length != info->common.ssid.length || memcmp(f->ssid.name, info->common.ssid.name, f->ssid.length) != 0))
        return false;
    if ((f->flags & LDN_FILTER_SCENE_ID) && f->id.intent.scene_id != info->id.intent.scene_id)
        return false;
    return true;
}

/** @brief Scan (102) and ScanPrivate (103): u16 channel (padded to 8), ScanFilter; u16 count and the networks found. */
static rl_result scan(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    struct {
        uint16_t channel;
        uint8_t pad[6];
        ldn_scan_filter filter;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    if (!b.size || !b.addr)
        return LDN_RESULT_INVALID_ARGUMENT;
    if ((in.filter.flags & LDN_FILTER_NETWORK_TYPE) && in.filter.network_type > 3)
        return LDN_RESULT_INVALID_ARGUMENT;
    if ((in.filter.flags & LDN_FILTER_SSID) && in.filter.ssid.length <= 31)
        return LDN_RESULT_INVALID_ARGUMENT;
    if (in.filter.flags & ~(uint32_t)LDN_FILTER_ALL)
        return LDN_RESULT_INVALID_ARGUMENT;
    uint32_t st = state_of(s);
    if (st == LDN_STATE_NONE || st == LDN_STATE_ACCESS_POINT_CREATED || st == LDN_STATE_ERROR)
        return LDN_RESULT_INVALID_STATE;
    int max = (int)(b.size / sizeof(ldn_network_info));
    if (max > 24)
        max = 24;
    ldn_network_info found[24];
    int n = max > 0 ? s->backend->scan(s->backend, in.channel, &in.filter, found, max) : 0;
    zero_buf(&b);
    for (int i = 0; i < n; i++) {
        rl_ipc_buf one = { b.addr + (uint64_t)i * sizeof found[0], sizeof found[0], b.attr };
        rl_ipc_buf_write(&one, &found[i], sizeof found[i]);
    }
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = (uint32_t)n;
    return KR_SUCCESS;
}

/** @brief SetWirelessControllerRestriction (104): u32 0 or 1, only while Initialized; nothing to restrict here. */
static rl_result set_wireless_controller_restriction(rl_ipc_req *r)
{
    uint32_t v;
    if (!rl_srv_in(r, &v, 4))
        return SF_RESULT_INVALID_HEADER;
    if (v > 1)
        return LDN_RESULT_INVALID_ARGUMENT;
    return state_of(svc_of(r)) == LDN_STATE_INITIALIZED ? KR_SUCCESS : LDN_RESULT_INVALID_STATE;
}

/** @brief SetProtocol (106): u32 protocol, 1 or 3. */
static rl_result set_protocol(rl_ipc_req *r)
{
    uint32_t v;
    if (!rl_srv_in(r, &v, 4))
        return SF_RESULT_INVALID_HEADER;
    return v == 1 || v == 3 ? KR_SUCCESS : LDN_RESULT_INVALID_ARGUMENT;
}

/** @brief OpenAccessPoint (200): Initialized becomes AccessPoint. */
static rl_result open_access_point(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    if (state_of(s) != LDN_STATE_INITIALIZED)
        return LDN_RESULT_INVALID_STATE;
    pthread_mutex_lock(&s->lock);
    s->advertise_size = 0;
    pthread_mutex_unlock(&s->lock);
    set_state(s, LDN_STATE_ACCESS_POINT);
    return KR_SUCCESS;
}

/** @brief CloseAccessPoint (201): ends a hosted network; AccessPoint or AccessPointCreated becomes Initialized. */
static rl_result close_access_point(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    uint32_t st = state_of(s);
    if (st != LDN_STATE_ACCESS_POINT && st != LDN_STATE_ACCESS_POINT_CREATED)
        return LDN_RESULT_INVALID_STATE;
    if (st == LDN_STATE_ACCESS_POINT_CREATED)
        s->backend->destroy(s->backend);
    leave_network(s, LDN_STATE_INITIALIZED, st == LDN_STATE_ACCESS_POINT_CREATED ? LDN_DESTROYED_BY_USER : 0);
    return KR_SUCCESS;
}

/** @brief Hosts a network: the checks of CreateNetwork, then the backend. */
static rl_result create_network_with(rl_ipc_req *r, const ldn_security_config *sec, const ldn_user_config *user,
                                     ldn_network_config *net)
{
    ldn_svc *s = svc_of(r);
    if (net->node_count_max > LDN_NODE_MAX || (net->local_communication_version & 0x8000) || sec->mode > 1 ||
        sec->passphrase_size > LDN_PASSPHRASE_MAX)
        return LDN_RESULT_INVALID_ARGUMENT;
    if (state_of(s) != LDN_STATE_ACCESS_POINT)
        return LDN_RESULT_INVALID_STATE;
    uint8_t adv[LDN_ADVERTISE_MAX];
    pthread_mutex_lock(&s->lock);
    size_t adv_size = s->advertise_size;
    memcpy(adv, s->advertise, adv_size);
    pthread_mutex_unlock(&s->lock);
    return s->backend->create(s->backend, sec, user, net, adv, adv_size);
}

/** @brief CreateNetwork (202): SecurityConfig, UserConfig, 4 bytes of padding, NetworkConfig. */
static rl_result create_network(rl_ipc_req *r)
{
    struct {
        ldn_security_config sec;
        ldn_user_config user;
        uint32_t pad;
        ldn_network_config net;
    } in;
    _Static_assert(sizeof in == 0x98, "CreateNetwork input");
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    return create_network_with(r, &in.sec, &in.user, &in.net);
}

/** @brief CreateNetworkPrivate (203): as CreateNetwork with a SecurityParameter and an address list, which the LAN protocol does not carry. */
static rl_result create_network_private(rl_ipc_req *r)
{
    struct {
        ldn_security_config sec;
        uint8_t parameter[0x20];
        ldn_user_config user;
        uint32_t pad;
        ldn_network_config net;
    } in;
    _Static_assert(sizeof in == 0xb8, "CreateNetworkPrivate input");
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    return create_network_with(r, &in.sec, &in.user, &in.net);
}

/** @brief DestroyNetwork (204): AccessPointCreated becomes AccessPoint. */
static rl_result destroy_network(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    if (state_of(s) != LDN_STATE_ACCESS_POINT_CREATED)
        return LDN_RESULT_INVALID_STATE;
    s->backend->destroy(s->backend);
    leave_network(s, LDN_STATE_ACCESS_POINT, LDN_DESTROYED_BY_USER);
    return KR_SUCCESS;
}

/** @brief Reject (205): u32 address of a station to drop from the hosted network. */
static rl_result reject(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    uint32_t ip;
    if (!rl_srv_in(r, &ip, 4))
        return SF_RESULT_INVALID_HEADER;
    if (state_of(s) != LDN_STATE_ACCESS_POINT_CREATED)
        return LDN_RESULT_INVALID_STATE;
    return s->backend->reject(s->backend, ip);
}

/** @brief SetAdvertiseData (206): up to 0x180 bytes the network shows to scans, kept for CreateNetwork and sent to a hosted network. */
static rl_result set_advertise_data(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    rl_ipc_buf b = rl_ipc_auto_in(r, 0);
    if (b.size == 0 || b.size > LDN_ADVERTISE_MAX)
        return LDN_RESULT_INVALID_ARGUMENT;
    uint8_t data[LDN_ADVERTISE_MAX];
    if (!rl_ipc_buf_read(&b, data, b.size))
        return SF_RESULT_INVALID_HEADER;
    uint32_t st = state_of(s);
    if (st != LDN_STATE_ACCESS_POINT && st != LDN_STATE_ACCESS_POINT_CREATED)
        return LDN_RESULT_INVALID_STATE;
    pthread_mutex_lock(&s->lock);
    memcpy(s->advertise, data, b.size);
    s->advertise_size = b.size;
    pthread_mutex_unlock(&s->lock);
    if (st == LDN_STATE_ACCESS_POINT_CREATED)
        s->backend->set_advertise(s->backend, data, b.size);
    return KR_SUCCESS;
}

/** @brief SetStationAcceptPolicy (207): u8 policy, 0 to 3, while hosting; the LAN protocol accepts every station. */
static rl_result set_station_accept_policy(rl_ipc_req *r)
{
    uint8_t v;
    if (!rl_srv_in(r, &v, 1))
        return SF_RESULT_INVALID_HEADER;
    if (v > 3)
        return LDN_RESULT_INVALID_ARGUMENT;
    uint32_t st = state_of(svc_of(r));
    return st == LDN_STATE_ACCESS_POINT || st == LDN_STATE_ACCESS_POINT_CREATED ? KR_SUCCESS : LDN_RESULT_INVALID_STATE;
}

/** @brief OpenStation (300): Initialized becomes Station. */
static rl_result open_station(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    if (state_of(s) != LDN_STATE_INITIALIZED)
        return LDN_RESULT_INVALID_STATE;
    set_state(s, LDN_STATE_STATION);
    return KR_SUCCESS;
}

/** @brief CloseStation (301): leaves a joined network; Station or StationConnected becomes Initialized. */
static rl_result close_station(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    uint32_t st = state_of(s);
    if (st != LDN_STATE_STATION && st != LDN_STATE_STATION_CONNECTED)
        return LDN_RESULT_INVALID_STATE;
    if (st == LDN_STATE_STATION_CONNECTED)
        s->backend->disconnect(s->backend);
    leave_network(s, LDN_STATE_INITIALIZED, st == LDN_STATE_STATION_CONNECTED ? LDN_DISCONNECT_BY_USER : 0);
    return KR_SUCCESS;
}

/**
 * @brief Connect (302): SecurityConfig, UserConfig, u32 local communication version, u32 option, and the network
 * to join (a 0x480 send buffer); StationConnected once the host answers.
 */
static rl_result connect_network(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    struct {
        ldn_security_config sec;
        ldn_user_config user;
        uint32_t version;
        uint32_t option;
    } in;
    _Static_assert(sizeof in == 0x7c, "Connect input");
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    ldn_network_info info;
    rl_ipc_buf b = rl_ipc_auto_in(r, 0);
    if (b.size < sizeof info || !rl_ipc_buf_read(&b, &info, sizeof info))
        return LDN_RESULT_INVALID_ARGUMENT;
    if (in.sec.mode < 1 || in.sec.mode > 2 || in.option > 1 || in.sec.passphrase_size > LDN_PASSPHRASE_MAX)
        return LDN_RESULT_INVALID_ARGUMENT;
    if (in.version > 0x7fff)
        return LDN_RESULT_VERSION_TOO_HIGH;
    if (state_of(s) != LDN_STATE_STATION)
        return LDN_RESULT_INVALID_STATE;
    return s->backend->connect(s->backend, &info, &in.sec, &in.user, (uint16_t)in.version);
}

/** @brief ConnectPrivate (303): the LAN protocol has no private networks, so none is found. */
static rl_result connect_private(rl_ipc_req *r)
{
    return state_of(svc_of(r)) == LDN_STATE_STATION ? LDN_RESULT_CONNECT_NOT_FOUND : LDN_RESULT_INVALID_STATE;
}

/** @brief Disconnect (304): StationConnected becomes Station. */
static rl_result disconnect_network(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    if (state_of(s) != LDN_STATE_STATION_CONNECTED)
        return LDN_RESULT_INVALID_STATE;
    s->backend->disconnect(s->backend);
    leave_network(s, LDN_STATE_STATION, LDN_DISCONNECT_BY_USER);
    return KR_SUCCESS;
}

/** @brief The factory net.mode selects. */
static ldn_backend_factory *factory(void)
{
    if (g_factory)
        return g_factory;
    return rl_setting_enum("net.mode") == 1 ? rl_ldn_lan_backend : rl_ldn_offline_backend;
}

/** @brief Initialize (400 with a pid, 402 with u32 address, u32 mask and a pid): None becomes Initialized. */
static rl_result initialize(rl_ipc_req *r)
{
    ldn_svc *s = svc_of(r);
    if (state_of(s) != LDN_STATE_NONE)
        return LDN_RESULT_INVALID_STATE;
    ldn_backend *b = factory()(on_change, s);
    if (!b) {
        rl_log(RL_LOG_WARN, "ldn", "the local network did not start; local play is off");
        b = rl_ldn_offline_backend(on_change, s);
    }
    if (!b)
        return LDN_RESULT_DEVICE_DISABLED;
    s->backend = b;
    set_state(s, LDN_STATE_INITIALIZED);
    return KR_SUCCESS;
}

/** @brief Ends whatever network the process is in and stops its backend. */
static void finalize_impl(ldn_svc *s)
{
    uint32_t st = state_of(s);
    if (st == LDN_STATE_ACCESS_POINT_CREATED)
        s->backend->destroy(s->backend);
    else if (st == LDN_STATE_STATION_CONNECTED)
        s->backend->disconnect(s->backend);
    if (st != LDN_STATE_NONE)
        leave_network(s, LDN_STATE_NONE, 0);
    if (s->backend) {
        s->backend->free(s->backend);
        s->backend = NULL;
    }
}

/** @brief Finalize (401): back to None. */
static rl_result finalize(rl_ipc_req *r)
{
    finalize_impl(svc_of(r));
    return KR_SUCCESS;
}

static void svc_destroy(void *state)
{
    ldn_svc *s = state;
    finalize_impl(s);
    hos_kobj_unref(&s->event->obj);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

static const rl_ipc_cmd local_cmds[] = {
    { 0, "GetState", get_state },
    { 1, "GetNetworkInfo", get_network_info },
    { 2, "GetIpv4Address", get_ipv4_address },
    { 3, "GetDisconnectReason", get_disconnect_reason },
    { 4, "GetSecurityParameter", get_security_parameter },
    { 5, "GetNetworkConfig", get_network_config },
    { 100, "AttachStateChangeEvent", attach_state_change_event },
    { 101, "GetNetworkInfoLatestUpdate", get_network_info_latest_update },
    { 102, "Scan", scan },
    { 103, "ScanPrivate", scan },
    { 104, "SetWirelessControllerRestriction", set_wireless_controller_restriction },
    { 106, "SetProtocol", set_protocol },
    { 200, "OpenAccessPoint", open_access_point },
    { 201, "CloseAccessPoint", close_access_point },
    { 202, "CreateNetwork", create_network },
    { 203, "CreateNetworkPrivate", create_network_private },
    { 204, "DestroyNetwork", destroy_network },
    { 205, "Reject", reject },
    { 206, "SetAdvertiseData", set_advertise_data },
    { 207, "SetStationAcceptPolicy", set_station_accept_policy },
    { 208, "AddAcceptFilterEntry", rl_srv_stub_ok },
    { 209, "ClearAcceptFilter", rl_srv_stub_ok },
    { 300, "OpenStation", open_station },
    { 301, "CloseStation", close_station },
    { 302, "Connect", connect_network },
    { 303, "ConnectPrivate", connect_private },
    { 304, "Disconnect", disconnect_network },
    { 400, "InitializeOld", initialize },
    { 401, "Finalize", finalize },
    { 402, "Initialize", initialize },
};

static const rl_ipc_class local_class = { "nn::ldn::detail::IUserLocalCommunicationService", local_cmds,
                                          sizeof local_cmds / sizeof local_cmds[0], 0, svc_destroy };

/** @brief CreateUserLocalCommunicationService (0): a service in state None. */
static rl_result create_local(rl_ipc_req *r)
{
    ldn_svc *s = calloc(1, sizeof *s);
    if (!s)
        return KR_OUT_OF_RESOURCE;
    if (!(s->event = rl_hos_event_create())) {
        free(s);
        return KR_OUT_OF_RESOURCE;
    }
    pthread_mutex_init(&s->lock, NULL);
    rl_ipc_object *o = rl_srv_object(&local_class, s);
    if (!o) {
        svc_destroy(s);
        return KR_OUT_OF_RESOURCE;
    }
    return rl_ipc_push_object(r, o);
}

static const rl_ipc_cmd monitor_cmds[] = {
    { 0, "RegisterClient", rl_srv_stub_ok },
};

static const rl_ipc_class monitor_class = { "nn::ldn::detail::IClientProcessMonitor", monitor_cmds,
                                            sizeof monitor_cmds / sizeof monitor_cmds[0], 0, NULL };

/** @brief CreateClientProcessMonitor (1): a monitor whose RegisterClient takes the process. */
static rl_result create_monitor(rl_ipc_req *r)
{
    rl_ipc_object *o = rl_srv_object(&monitor_class, NULL);
    return o ? rl_ipc_push_object(r, o) : KR_OUT_OF_RESOURCE;
}

static const rl_ipc_cmd creator_cmds[] = {
    { 0, "CreateUserLocalCommunicationService", create_local },
    { 1, "CreateClientProcessMonitor", create_monitor },
};

static const rl_ipc_class creator_class = { "ldn:u", creator_cmds, sizeof creator_cmds / sizeof creator_cmds[0], 0,
                                            NULL };

static rl_ipc_object *creator_factory(void) { return rl_srv_object(&creator_class, NULL); }

void rl_srv_install_ldn(void) { rl_ipc_register_service("ldn:u", creator_factory); }

/** @brief The offline backend: no address, nothing found, hosting and joining fail. */
typedef struct offline {
    ldn_backend b;
} offline;

static bool off_address(ldn_backend *b, uint32_t *ip, uint32_t *mask)
{
    (void)b;
    (void)ip;
    (void)mask;
    return false;
}

static int off_scan(ldn_backend *b, uint16_t channel, const ldn_scan_filter *f, ldn_network_info *out, int max)
{
    (void)b;
    (void)channel;
    (void)f;
    (void)out;
    (void)max;
    return 0;
}

static uint32_t off_create(ldn_backend *b, const ldn_security_config *sec, const ldn_user_config *user,
                           const ldn_network_config *net, const void *adv, size_t adv_size)
{
    (void)b;
    (void)sec;
    (void)user;
    (void)net;
    (void)adv;
    (void)adv_size;
    return LDN_RESULT_DEVICE_NOT_AVAILABLE;
}

static void off_nothing(ldn_backend *b) { (void)b; }

static void off_set_advertise(ldn_backend *b, const void *data, size_t size)
{
    (void)b;
    (void)data;
    (void)size;
}

static uint32_t off_reject(ldn_backend *b, uint32_t ip)
{
    (void)b;
    (void)ip;
    return LDN_RESULT_NODE_NOT_FOUND;
}

static uint32_t off_connect(ldn_backend *b, const ldn_network_info *info, const ldn_security_config *sec,
                            const ldn_user_config *user, uint16_t version)
{
    (void)b;
    (void)info;
    (void)sec;
    (void)user;
    (void)version;
    return LDN_RESULT_CONNECT_NOT_FOUND;
}

static void off_free(ldn_backend *b) { free(b); }

ldn_backend *rl_ldn_offline_backend(ldn_change_fn *change, void *ctx)
{
    (void)change;
    (void)ctx;
    offline *o = calloc(1, sizeof *o);
    if (!o)
        return NULL;
    o->b = (ldn_backend){ off_address, off_scan,    off_create,     off_nothing, off_set_advertise,
                          off_reject,  off_connect, off_nothing,    off_free };
    return &o->b;
}
