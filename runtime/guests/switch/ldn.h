/**
 * @file ldn.h
 * @brief Local wireless play (nn::ldn): the console's structures, the
 * service's states and results, and the backend the service drives (offline,
 * or the LAN protocol of ldn_lan.c). Layouts and values follow Ryujinx (MIT,
 * Copyright (c) Ryujinx Team and Contributors) Services/Ldn/Types and
 * switchbrew.org (LDN services).
 */
#ifndef RELIFT_LDN_H
#define RELIFT_LDN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The ldn error module and its results. */
#define LDN_MODULE 203
#define LDN_RESULT(d) ((uint32_t)LDN_MODULE | ((uint32_t)(d) << 9))
#define LDN_RESULT_DEVICE_NOT_AVAILABLE LDN_RESULT(16)
#define LDN_RESULT_DEVICE_DISABLED LDN_RESULT(22)
#define LDN_RESULT_INVALID_STATE LDN_RESULT(32)
#define LDN_RESULT_NODE_NOT_FOUND LDN_RESULT(48)
#define LDN_RESULT_CONNECT_FAILURE LDN_RESULT(64)
#define LDN_RESULT_CONNECT_NOT_FOUND LDN_RESULT(65)
#define LDN_RESULT_CONNECT_TIMEOUT LDN_RESULT(66)
#define LDN_RESULT_CONNECT_REJECTED LDN_RESULT(67)
#define LDN_RESULT_INVALID_ARGUMENT LDN_RESULT(96)
#define LDN_RESULT_INVALID_OBJECT LDN_RESULT(97)
#define LDN_RESULT_VERSION_TOO_LOW LDN_RESULT(113)
#define LDN_RESULT_VERSION_TOO_HIGH LDN_RESULT(114)
#define LDN_RESULT_TOO_MANY_PLAYERS LDN_RESULT(144)

/** @brief Most nodes of a network (the host is node 0). */
#define LDN_NODE_MAX 8
/** @brief Most bytes of advertise data. */
#define LDN_ADVERTISE_MAX 0x180
/** @brief Most bytes of a passphrase. */
#define LDN_PASSPHRASE_MAX 0x40
/** @brief Bytes of an SSID name. */
#define LDN_SSID_MAX 0x20

/** @brief nn::ldn::State. */
enum ldn_state {
    LDN_STATE_NONE = 0,
    LDN_STATE_INITIALIZED = 1,
    LDN_STATE_ACCESS_POINT = 2,
    LDN_STATE_ACCESS_POINT_CREATED = 3,
    LDN_STATE_STATION = 4,
    LDN_STATE_STATION_CONNECTED = 5,
    LDN_STATE_ERROR = 6
};

/** @brief nn::ldn::DisconnectReason. */
enum ldn_disconnect_reason {
    LDN_DISCONNECT_NONE = 0,
    LDN_DISCONNECT_BY_USER = 1,
    LDN_DISCONNECT_BY_SYSTEM = 2,
    LDN_DESTROYED_BY_USER = 3,
    LDN_DESTROYED_BY_SYSTEM = 4,
    LDN_DISCONNECT_REJECTED = 5,
    LDN_DISCONNECT_SIGNAL_LOST = 6
};

/** @brief ScanFilter flag bits. */
enum {
    LDN_FILTER_LOCAL_COMMUNICATION_ID = 1 << 0,
    LDN_FILTER_SESSION_ID = 1 << 1,
    LDN_FILTER_NETWORK_TYPE = 1 << 2,
    LDN_FILTER_MAC = 1 << 3,
    LDN_FILTER_SSID = 1 << 4,
    LDN_FILTER_SCENE_ID = 1 << 5,
    LDN_FILTER_ALL = 0x3f
};

/** @brief NodeLatestUpdate flag bits. */
enum { LDN_NODE_CONNECT = 1 << 0, LDN_NODE_DISCONNECT = 1 << 1 };

/** @brief nn::ldn::IntentId (0x10). */
typedef struct ldn_intent_id {
    uint64_t local_communication_id;
    uint16_t reserved1;
    uint16_t scene_id;
    uint32_t reserved2;
} ldn_intent_id;

/** @brief nn::ldn::NetworkId (0x20). */
typedef struct ldn_network_id {
    ldn_intent_id intent;
    uint8_t session_id[16];
} ldn_network_id;

/** @brief nn::ldn::Ssid (0x22). */
typedef struct ldn_ssid {
    uint8_t length;
    char name[LDN_SSID_MAX + 1];
} ldn_ssid;

/** @brief nn::ldn::NodeInfo (0x40). The address is a u32 whose value reads as a.b.c.d from the top byte down. */
typedef struct ldn_node_info {
    uint32_t ipv4;
    uint8_t mac[6];
    uint8_t node_id;
    uint8_t is_connected;
    char user_name[33];
    uint8_t reserved1;
    uint16_t local_communication_version;
    uint8_t reserved2[16];
} ldn_node_info;

/** @brief nn::ldn::CommonNetworkInfo (0x30). */
typedef struct ldn_common_info {
    uint8_t bssid[6];
    ldn_ssid ssid;
    uint16_t channel;
    uint8_t link_level;
    uint8_t network_type;
    uint32_t reserved;
} ldn_common_info;

/** @brief nn::ldn::LdnNetworkInfo (0x430). */
typedef struct ldn_ldn_info {
    uint8_t security_parameter[16];
    uint16_t security_mode;
    uint8_t station_accept_policy;
    uint8_t reserved1;
    uint16_t reserved2;
    uint8_t node_count_max;
    uint8_t node_count;
    ldn_node_info nodes[LDN_NODE_MAX];
    uint16_t reserved3;
    uint16_t advertise_size;
    uint8_t advertise[LDN_ADVERTISE_MAX];
    uint8_t reserved4[140];
    uint64_t authentication_id;
} ldn_ldn_info;

/** @brief nn::ldn::NetworkInfo (0x480): what a scan finds and what every node of a network shares. */
typedef struct ldn_network_info {
    ldn_network_id id;
    ldn_common_info common;
    ldn_ldn_info ldn;
} ldn_network_info;

/** @brief nn::ldn::ScanFilter (0x60). */
typedef struct ldn_scan_filter {
    ldn_network_id id;
    uint32_t network_type;
    uint8_t bssid[6];
    ldn_ssid ssid;
    uint8_t reserved[16];
    uint32_t flags;
} ldn_scan_filter;

/** @brief nn::ldn::SecurityConfig (0x44). */
typedef struct ldn_security_config {
    uint16_t mode;
    uint16_t passphrase_size;
    uint8_t passphrase[LDN_PASSPHRASE_MAX];
} ldn_security_config;

/** @brief nn::ldn::UserConfig (0x30). */
typedef struct ldn_user_config {
    char user_name[33];
    uint8_t reserved[15];
} ldn_user_config;

/** @brief nn::ldn::NetworkConfig (0x20). */
typedef struct ldn_network_config {
    ldn_intent_id intent;
    uint16_t channel;
    uint8_t node_count_max;
    uint8_t reserved1;
    uint16_t local_communication_version;
    uint8_t reserved2[10];
} ldn_network_config;

_Static_assert(sizeof(ldn_intent_id) == 0x10, "IntentId");
_Static_assert(sizeof(ldn_network_id) == 0x20, "NetworkId");
_Static_assert(sizeof(ldn_ssid) == 0x22, "Ssid");
_Static_assert(sizeof(ldn_node_info) == 0x40, "NodeInfo");
_Static_assert(sizeof(ldn_common_info) == 0x30, "CommonNetworkInfo");
_Static_assert(sizeof(ldn_ldn_info) == 0x430, "LdnNetworkInfo");
_Static_assert(offsetof(ldn_ldn_info, nodes) == 0x18, "LdnNetworkInfo nodes");
_Static_assert(offsetof(ldn_ldn_info, advertise_size) == 0x21a, "LdnNetworkInfo advertise size");
_Static_assert(offsetof(ldn_ldn_info, authentication_id) == 0x428, "LdnNetworkInfo authentication id");
_Static_assert(sizeof(ldn_network_info) == 0x480, "NetworkInfo");
_Static_assert(offsetof(ldn_network_info, ldn) == 0x50, "NetworkInfo ldn");
_Static_assert(sizeof(ldn_scan_filter) == 0x60, "ScanFilter");
_Static_assert(offsetof(ldn_scan_filter, flags) == 0x5c, "ScanFilter flags");
_Static_assert(sizeof(ldn_security_config) == 0x44, "SecurityConfig");
_Static_assert(sizeof(ldn_user_config) == 0x30, "UserConfig");
_Static_assert(sizeof(ldn_network_config) == 0x20, "NetworkConfig");

/**
 * @brief Told by a backend when its network changed: info is the network as
 * it is now, connected false when the process left it (reason says why).
 * Called from any host thread, never while the backend holds its own lock.
 */
typedef void ldn_change_fn(void *ctx, const ldn_network_info *info, bool connected, uint32_t reason);

/** @brief What carries a process's local network: the service drives one per Initialize. */
typedef struct ldn_backend ldn_backend;
struct ldn_backend {
    /** @brief The process's address and subnet mask on the local network; false when it has none. */
    bool (*address)(ldn_backend *b, uint32_t *ip, uint32_t *mask);
    /** @brief Fills up to max networks that match filter; returns how many. May block about a second. */
    int (*scan)(ldn_backend *b, uint16_t channel, const ldn_scan_filter *filter, ldn_network_info *out, int max);
    /** @brief Hosts a network as node 0; reports it through the change callback. Returns an ldn result. */
    uint32_t (*create)(ldn_backend *b, const ldn_security_config *sec, const ldn_user_config *user,
                       const ldn_network_config *net, const void *advertise, size_t advertise_size);
    /** @brief Ends the hosted network and drops its stations. */
    void (*destroy)(ldn_backend *b);
    /** @brief Replaces the hosted network's advertise data and tells the stations. */
    void (*set_advertise)(ldn_backend *b, const void *data, size_t size);
    /** @brief Drops the station at ipv4 from the hosted network. Returns an ldn result. */
    uint32_t (*reject)(ldn_backend *b, uint32_t ipv4);
    /** @brief Joins the network info describes; blocks until the host answers or a timeout. Returns an ldn result. */
    uint32_t (*connect)(ldn_backend *b, const ldn_network_info *info, const ldn_security_config *sec,
                        const ldn_user_config *user, uint16_t local_communication_version);
    /** @brief Leaves the joined network. */
    void (*disconnect)(ldn_backend *b);
    /** @brief Stops everything and frees the backend. */
    void (*free)(ldn_backend *b);
};

/** @brief Makes a backend that reports changes to change(ctx, ...); NULL when it cannot start. */
typedef ldn_backend *ldn_backend_factory(ldn_change_fn *change, void *ctx);

/** @brief The backend of a console with its radio on and nobody near: scans find nothing, hosting and joining fail. */
ldn_backend *rl_ldn_offline_backend(ldn_change_fn *change, void *ctx);
/** @brief The ldn_mitm LAN protocol backend on the host interface the settings name (ldn_lan.c). */
ldn_backend *rl_ldn_lan_backend(ldn_change_fn *change, void *ctx);
/** @brief Replaces the factory Initialize uses (tests); NULL restores the one net.mode selects. */
void rl_ldn_set_backend_factory(ldn_backend_factory *f);

/**
 * @brief The network the process is in, for the socket layer: true with its info (info may be NULL), the
 * process's address and subnet mask while it hosts or has joined one; false otherwise.
 */
bool rl_ldn_route(ldn_network_info *info, uint32_t *ip, uint32_t *mask);

/** @brief True when a network info's fields pass filter (the scan rules of nn::ldn). */
bool rl_ldn_filter_match(const ldn_scan_filter *filter, const ldn_network_info *info);

#endif
