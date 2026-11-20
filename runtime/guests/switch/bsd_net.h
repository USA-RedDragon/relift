/**
 * @file bsd_net.h
 * @brief The socket layer behind bsd:u and bsd:s: a descriptor table of a
 * console that has no network. Sockets are created and configured as on a
 * console, and sockets of the program itself talk over an in-process
 * loopback (127.0.0.0/8 and 0.0.0.0) that never touches the host; every
 * other address is unreachable. Errno values are the console's (FreeBSD).
 */
#ifndef RELIFT_BSD_NET_H
#define RELIFT_BSD_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Console (FreeBSD) errno values the layer returns. */
enum {
    BSD_EBADF = 9,
    BSD_EMFILE = 24,
    BSD_EPIPE = 32,
    BSD_EWOULDBLOCK = 35,
    BSD_ENOTSOCK = 38,
    BSD_EDESTADDRREQ = 39,
    BSD_EMSGSIZE = 40,
    BSD_EPROTONOSUPPORT = 43,
    BSD_ENOTTY = 25,
    BSD_EOPNOTSUPP = 45,
    BSD_EAFNOSUPPORT = 47,
    BSD_EADDRINUSE = 48,
    BSD_EADDRNOTAVAIL = 49,
    BSD_ENETUNREACH = 51,
    BSD_EISCONN = 56,
    BSD_ENOTCONN = 57,
    BSD_ECONNREFUSED = 61,
    BSD_EINVAL = 22
};

/** @brief poll(2) event bits of the console. */
enum { BSD_POLLIN = 0x1, BSD_POLLOUT = 0x4, BSD_POLLERR = 0x8, BSD_POLLHUP = 0x10, BSD_POLLNVAL = 0x20 };

/** @brief recv(2) flags the layer honors. */
enum { BSD_MSG_PEEK = 0x2, BSD_MSG_DONTWAIT = 0x80 };

typedef struct bsd_net bsd_net;

/** @brief An IPv4 address and port in host order. */
typedef struct bsd_addr {
    uint32_t ip;
    uint16_t port;
} bsd_addr;

/** @brief Creates an empty socket layer. */
bsd_net *bsd_net_new(void);
/** @brief Closes every descriptor and frees the layer. */
void bsd_net_free(bsd_net *n);

/** @brief The functions below return the call's value (-1 on failure) and set *err to the errno (0 on success). */
int32_t bsd_net_socket(bsd_net *n, uint32_t domain, uint32_t type, uint32_t protocol, int32_t *err);
int32_t bsd_net_eventfd(bsd_net *n, uint64_t initval, uint32_t flags, int32_t *err);
int32_t bsd_net_close(bsd_net *n, int32_t fd, int32_t *err);
int32_t bsd_net_duplicate(bsd_net *n, int32_t fd, int32_t *err);
int32_t bsd_net_bind(bsd_net *n, int32_t fd, const uint8_t *sockaddr, size_t len, int32_t *err);
int32_t bsd_net_connect(bsd_net *n, int32_t fd, const uint8_t *sockaddr, size_t len, int32_t *err);
int32_t bsd_net_listen(bsd_net *n, int32_t fd, int32_t backlog, int32_t *err);
/** @brief Accepts a connection; the peer's sockaddr (16 bytes) goes to peer. */
int32_t bsd_net_accept(bsd_net *n, int32_t fd, uint8_t peer[16], int32_t *err);
/** @brief Sends; sockaddr (or NULL) is the destination of a datagram. Blocks as the descriptor's mode says. */
int32_t bsd_net_send(bsd_net *n, int32_t fd, const void *data, size_t len, const uint8_t *sockaddr, size_t alen,
                     int32_t *err);
/** @brief Receives up to cap bytes; the sender's sockaddr (16 bytes) goes to from when from is not NULL. */
int32_t bsd_net_recv(bsd_net *n, int32_t fd, void *data, size_t cap, uint32_t flags, uint8_t from[16], int32_t *err);
int32_t bsd_net_name(bsd_net *n, int32_t fd, bool peer, uint8_t out[16], int32_t *err);
/** @brief Reads an option into out (cap bytes); *len is its size. */
int32_t bsd_net_getsockopt(bsd_net *n, int32_t fd, uint32_t level, uint32_t name, void *out, size_t cap, uint32_t *len,
                           int32_t *err);
int32_t bsd_net_setsockopt(bsd_net *n, int32_t fd, uint32_t level, uint32_t name, const void *val, size_t len,
                           int32_t *err);
int32_t bsd_net_fcntl(bsd_net *n, int32_t fd, int32_t cmd, int32_t arg, int32_t *err);
int32_t bsd_net_shutdown(bsd_net *n, int32_t fd, int32_t how, int32_t *err);
/** @brief Bytes a Recv would return now (FIONREAD), or -1 with *err when fd is not open. */
int32_t bsd_net_pending(bsd_net *n, int32_t fd, int32_t *err);
/** @brief Sets or clears a descriptor's nonblocking mode (FIONBIO). */
int32_t bsd_net_set_nonblock(bsd_net *n, int32_t fd, bool on, int32_t *err);
/** @brief The revents of one descriptor for events (BSD_POLLNVAL for one that is not open). */
uint16_t bsd_net_revents(bsd_net *n, int32_t fd, uint16_t events);

/** @brief Builds a 16-byte BSD sockaddr_in. */
void bsd_addr_pack(uint8_t out[16], bsd_addr a);

#endif
