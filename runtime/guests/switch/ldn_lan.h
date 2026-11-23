/**
 * @file ldn_lan.h
 * @brief The ldn_mitm LAN protocol (github.com/spacemeowx2/ldn_mitm, as
 * Ryujinx's LdnMitm client speaks it): packets of a 12-byte header and an
 * optionally zero-run compressed body, over UDP for discovery and TCP for a
 * network's members. Exposed for tests.
 */
#ifndef RELIFT_LDN_LAN_H
#define RELIFT_LDN_LAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The port of discovery (UDP) and of a network's members (TCP). */
#define LDN_LAN_PORT 11452
/** @brief Header magic. */
#define LDN_LAN_MAGIC 0x11451400u
/** @brief Header bytes. */
#define LDN_LAN_HEADER 12
/** @brief Most bytes of a packet, header included, and of a decompressed body. */
#define LDN_LAN_BUFFER 2048

/** @brief Packet types. */
enum { LDN_LAN_SCAN = 0, LDN_LAN_SCAN_RESPONSE = 1, LDN_LAN_CONNECT = 2, LDN_LAN_SYNC_NETWORK = 3 };

/** @brief Zero-run compression: a zero byte, then how many more zeros follow (at most 255). Returns the size, or 0 when it would not fit in cap. */
size_t rl_ldn_lan_compress(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
/** @brief Undoes rl_ldn_lan_compress; returns the size, or 0 when the input is cut or the output passes cap. */
size_t rl_ldn_lan_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
/** @brief Builds a packet of type with body (compressed when it has one) into out; returns its size, 0 when it does not fit. */
size_t rl_ldn_lan_pack(uint8_t type, const void *body, size_t n, uint8_t *out, size_t cap);

/** @brief Called with each whole packet a reader takes in: its type and decompressed body. */
typedef void rl_ldn_lan_packet_fn(void *ctx, uint8_t type, const uint8_t *body, size_t n);

/** @brief Reassembles packets from a stream (or one datagram at a time). */
typedef struct rl_ldn_lan_reader {
    /** @brief Bytes of the packet being read. */
    uint8_t buf[LDN_LAN_BUFFER];
    /** @brief How many of them are in. */
    size_t len;
} rl_ldn_lan_reader;

/**
 * @brief Feeds n bytes; calls fn for each packet they complete. False when a packet was bad (wrong magic, too
 * big, bad compression): the reader then drops what it had.
 */
bool rl_ldn_lan_feed(rl_ldn_lan_reader *r, const uint8_t *data, size_t n, rl_ldn_lan_packet_fn *fn, void *ctx);

/** @brief Where a LAN backend runs (tests): interface, its own port, and the port it scans and connects to. */
typedef struct rl_ldn_lan_opts {
    /** @brief Interface name or address; NULL takes the net.interface setting. */
    const char *iface;
    /** @brief The port the backend's UDP and TCP sockets bind. */
    uint16_t port;
    /** @brief The port scans and connects go to. */
    uint16_t peer_port;
    /** @brief Where scans go; 0 is the interface's subnet broadcast address. */
    uint32_t scan_ip;
} rl_ldn_lan_opts;

/** @brief Sets where the next LAN backends run; NULL restores net.interface and port 11452. */
void rl_ldn_lan_set_opts(const rl_ldn_lan_opts *o);

#endif
