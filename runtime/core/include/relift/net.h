/**
 * @file net.h
 * @brief Host network sockets for the guest layers: IPv4 UDP and TCP, always
 * nonblocking, with a poll and the host's interface list. POSIX (Linux,
 * macOS, Android, iOS) and Winsock. Addresses are u32 values that read
 * a.b.c.d from the top byte down, ports are host order.
 */
#ifndef RELIFT_NET_H
#define RELIFT_NET_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief A host socket. */
typedef struct rl_net_sock rl_net_sock;

/** @brief Results below zero of the send and receive calls. */
enum {
    /** @brief Nothing to read, or no room to send, now. */
    RL_NET_AGAIN = -1,
    /** @brief The socket failed or the peer reset it. */
    RL_NET_FAILED = -2
};

/** @brief Poll event bits. */
enum { RL_NET_IN = 1, RL_NET_OUT = 2, RL_NET_ERR = 4 };

/** @brief One host IPv4 interface. */
typedef struct rl_net_iface {
    /** @brief Interface name ("" where the host does not name them). */
    char name[64];
    /** @brief Address. */
    uint32_t ip;
    /** @brief Subnet mask. */
    uint32_t mask;
    /** @brief True for a loopback interface. */
    bool loopback;
} rl_net_iface;

/** @brief Fills up to max interfaces that are up and have an IPv4 address; returns how many. */
RL_RUNTIME_API int rl_net_ifaces(rl_net_iface *out, int max);
/**
 * @brief Picks an interface: want names one (or one of its addresses); empty picks the first up, non-loopback
 * one. False when none matches.
 */
RL_RUNTIME_API bool rl_net_pick_iface(const char *want, rl_net_iface *out);

/** @brief A UDP socket bound to ip:port (port 0 picks one), with SO_REUSEADDR and broadcast sending allowed. */
RL_RUNTIME_API rl_net_sock *rl_net_udp(uint32_t ip, uint16_t port);
/** @brief A TCP socket listening on ip:port (port 0 picks one), with SO_REUSEADDR. */
RL_RUNTIME_API rl_net_sock *rl_net_tcp_listen(uint32_t ip, uint16_t port, int backlog);
/** @brief Starts a TCP connection from local_ip (0: any) to ip:port; rl_net_poll's RL_NET_OUT says when it is up. */
RL_RUNTIME_API rl_net_sock *rl_net_tcp_connect(uint32_t local_ip, uint32_t ip, uint16_t port);
/** @brief Takes a pending connection of a listening socket (NULL when none), with the peer's address. */
RL_RUNTIME_API rl_net_sock *rl_net_tcp_accept(rl_net_sock *s, uint32_t *ip, uint16_t *port);
/** @brief The pending error of a socket (0 none), as SO_ERROR says; a connect that failed shows here. */
RL_RUNTIME_API int rl_net_error(rl_net_sock *s);

/** @brief Sends on a connected socket; bytes sent, or RL_NET_AGAIN or RL_NET_FAILED. */
RL_RUNTIME_API long rl_net_send(rl_net_sock *s, const void *data, size_t len);
/** @brief Sends a datagram to ip:port; bytes sent, or RL_NET_AGAIN or RL_NET_FAILED. */
RL_RUNTIME_API long rl_net_sendto(rl_net_sock *s, const void *data, size_t len, uint32_t ip, uint16_t port);
/** @brief Receives up to cap bytes; 0 when the peer closed, or RL_NET_AGAIN or RL_NET_FAILED. */
RL_RUNTIME_API long rl_net_recv(rl_net_sock *s, void *buf, size_t cap);
/** @brief Receives one datagram and its sender; bytes, or RL_NET_AGAIN or RL_NET_FAILED. */
RL_RUNTIME_API long rl_net_recvfrom(rl_net_sock *s, void *buf, size_t cap, uint32_t *ip, uint16_t *port);
/** @brief The address a socket is bound to. */
RL_RUNTIME_API bool rl_net_local(rl_net_sock *s, uint32_t *ip, uint16_t *port);

/**
 * @brief Waits up to timeout_ms (0: no wait) until one of n sockets has one of the events asked in events[i];
 * revents[i] gets what each has. Returns how many have something, or -1.
 */
RL_RUNTIME_API int rl_net_poll(rl_net_sock *const *socks, const uint8_t *events, uint8_t *revents, int n,
                               int timeout_ms);
/** @brief Closes a socket (NULL is fine). */
RL_RUNTIME_API void rl_net_close(rl_net_sock *s);

#endif
