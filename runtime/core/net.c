/**
 * @file net.c
 * @brief Host IPv4 sockets, see relift/net.h: BSD sockets on POSIX hosts,
 * Winsock on Windows. Every socket is nonblocking.
 */
#include <relift/net.h>

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET host_sock;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
typedef int sock_len;
static bool would_block(void)
{
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
static bool net_start(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static bool ok;
    BOOL pending;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        WSADATA w;
        ok = WSAStartup(MAKEWORD(2, 2), &w) == 0;
        InitOnceComplete(&once, 0, NULL);
    }
    return ok;
}
static bool set_nonblock(host_sock s)
{
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int host_sock;
#define BAD_SOCK (-1)
#define close_sock close
typedef socklen_t sock_len;
static bool would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS; }
static bool net_start(void) { return true; }
static bool set_nonblock(host_sock s)
{
    int fl = fcntl(s, F_GETFL, 0);
    return fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0 && fcntl(s, F_SETFD, FD_CLOEXEC) == 0;
}
#endif

#if defined(MSG_NOSIGNAL)
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

struct rl_net_sock {
    host_sock fd;
};

static struct sockaddr_in to_sa(uint32_t ip, uint16_t port)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(ip);
    return sa;
}

static rl_net_sock *wrap(host_sock fd)
{
    if (fd == BAD_SOCK)
        return NULL;
    if (!set_nonblock(fd)) {
        close_sock(fd);
        return NULL;
    }
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    rl_net_sock *s = malloc(sizeof *s);
    if (!s) {
        close_sock(fd);
        return NULL;
    }
    s->fd = fd;
    return s;
}

static void reuse(host_sock fd)
{
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
}

static rl_net_sock *bound(int type, uint32_t ip, uint16_t port)
{
    if (!net_start())
        return NULL;
    host_sock fd = socket(AF_INET, type, 0);
    if (fd == BAD_SOCK)
        return NULL;
    reuse(fd);
    if (type == SOCK_DGRAM) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    }
    struct sockaddr_in sa = to_sa(ip, port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close_sock(fd);
        return NULL;
    }
    return wrap(fd);
}

rl_net_sock *rl_net_udp(uint32_t ip, uint16_t port) { return bound(SOCK_DGRAM, ip, port); }

rl_net_sock *rl_net_tcp_listen(uint32_t ip, uint16_t port, int backlog)
{
    rl_net_sock *s = bound(SOCK_STREAM, ip, port);
    if (s && listen(s->fd, backlog) != 0) {
        rl_net_close(s);
        return NULL;
    }
    return s;
}

static void no_delay(host_sock fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

rl_net_sock *rl_net_tcp_connect(uint32_t local_ip, uint32_t ip, uint16_t port)
{
    if (!net_start())
        return NULL;
    host_sock fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == BAD_SOCK)
        return NULL;
    if (local_ip) {
        struct sockaddr_in la = to_sa(local_ip, 0);
        if (bind(fd, (struct sockaddr *)&la, sizeof la) != 0) {
            close_sock(fd);
            return NULL;
        }
    }
    no_delay(fd);
    rl_net_sock *s = wrap(fd);
    if (!s)
        return NULL;
    struct sockaddr_in sa = to_sa(ip, port);
    if (connect(s->fd, (struct sockaddr *)&sa, sizeof sa) != 0 && !would_block()) {
        rl_net_close(s);
        return NULL;
    }
    return s;
}

rl_net_sock *rl_net_tcp_accept(rl_net_sock *s, uint32_t *ip, uint16_t *port)
{
    struct sockaddr_in sa;
    sock_len len = sizeof sa;
    host_sock fd = accept(s->fd, (struct sockaddr *)&sa, &len);
    if (fd == BAD_SOCK)
        return NULL;
    no_delay(fd);
    if (ip)
        *ip = ntohl(sa.sin_addr.s_addr);
    if (port)
        *port = ntohs(sa.sin_port);
    return wrap(fd);
}

int rl_net_error(rl_net_sock *s)
{
    int e = 0;
    sock_len len = sizeof e;
    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, (char *)&e, &len) != 0)
        return -1;
    return e;
}

static long result(long n) { return n >= 0 ? n : (would_block() ? RL_NET_AGAIN : RL_NET_FAILED); }

long rl_net_send(rl_net_sock *s, const void *data, size_t len)
{
    return result((long)send(s->fd, (const char *)data, (int)len, SEND_FLAGS));
}

long rl_net_sendto(rl_net_sock *s, const void *data, size_t len, uint32_t ip, uint16_t port)
{
    struct sockaddr_in sa = to_sa(ip, port);
    return result((long)sendto(s->fd, (const char *)data, (int)len, SEND_FLAGS, (struct sockaddr *)&sa, sizeof sa));
}

long rl_net_recv(rl_net_sock *s, void *buf, size_t cap) { return result((long)recv(s->fd, (char *)buf, (int)cap, 0)); }

long rl_net_recvfrom(rl_net_sock *s, void *buf, size_t cap, uint32_t *ip, uint16_t *port)
{
    struct sockaddr_in sa;
    sock_len len = sizeof sa;
    memset(&sa, 0, sizeof sa);
    long n = result((long)recvfrom(s->fd, (char *)buf, (int)cap, 0, (struct sockaddr *)&sa, &len));
    if (n >= 0) {
        if (ip)
            *ip = ntohl(sa.sin_addr.s_addr);
        if (port)
            *port = ntohs(sa.sin_port);
    }
    return n;
}

bool rl_net_local(rl_net_sock *s, uint32_t *ip, uint16_t *port)
{
    struct sockaddr_in sa;
    sock_len len = sizeof sa;
    if (getsockname(s->fd, (struct sockaddr *)&sa, &len) != 0)
        return false;
    if (ip)
        *ip = ntohl(sa.sin_addr.s_addr);
    if (port)
        *port = ntohs(sa.sin_port);
    return true;
}

int rl_net_poll(rl_net_sock *const *socks, const uint8_t *events, uint8_t *revents, int n, int timeout_ms)
{
#if defined(_WIN32)
    WSAPOLLFD fds[64];
#else
    struct pollfd fds[64];
#endif
    if (n > 64)
        n = 64;
    for (int i = 0; i < n; i++) {
        fds[i].fd = socks[i] ? socks[i]->fd : BAD_SOCK;
        fds[i].events = (short)((events[i] & RL_NET_IN ? POLLIN : 0) | (events[i] & RL_NET_OUT ? POLLOUT : 0));
        fds[i].revents = 0;
    }
#if defined(_WIN32)
    int r = n ? WSAPoll(fds, (ULONG)n, timeout_ms) : (Sleep((DWORD)timeout_ms), 0);
#else
    int r = poll(fds, (nfds_t)n, timeout_ms);
#endif
    for (int i = 0; i < n; i++) {
        short re = fds[i].revents;
        revents[i] = (uint8_t)((re & POLLIN ? RL_NET_IN : 0) | (re & POLLOUT ? RL_NET_OUT : 0) |
                               (re & (POLLERR | POLLHUP | POLLNVAL) ? RL_NET_ERR : 0));
    }
    return r < 0 ? -1 : r;
}

void rl_net_close(rl_net_sock *s)
{
    if (!s)
        return;
    close_sock(s->fd);
    free(s);
}

int rl_net_ifaces(rl_net_iface *out, int max)
{
    int n = 0;
#if defined(_WIN32)
    if (!net_start())
        return 0;
    host_sock fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == BAD_SOCK)
        return 0;
    INTERFACE_INFO list[32];
    DWORD bytes = 0;
    if (WSAIoctl(fd, SIO_GET_INTERFACE_LIST, NULL, 0, list, sizeof list, &bytes, NULL, NULL) == 0) {
        int count = (int)(bytes / sizeof list[0]);
        for (int i = 0; i < count && n < max; i++) {
            if (!(list[i].iiFlags & IFF_UP) || list[i].iiAddress.Address.sa_family != AF_INET)
                continue;
            memset(&out[n], 0, sizeof out[n]);
            out[n].ip = ntohl(list[i].iiAddress.AddressIn.sin_addr.s_addr);
            out[n].mask = ntohl(list[i].iiNetmask.AddressIn.sin_addr.s_addr);
            out[n].loopback = (list[i].iiFlags & IFF_LOOPBACK) != 0;
            n++;
        }
    }
    closesocket(fd);
#else
    struct ifaddrs *all;
    if (getifaddrs(&all) != 0)
        return 0;
    for (struct ifaddrs *a = all; a && n < max; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || !a->ifa_netmask)
            continue;
        memset(&out[n], 0, sizeof out[n]);
        strncpy(out[n].name, a->ifa_name, sizeof out[n].name - 1);
        out[n].ip = ntohl(((struct sockaddr_in *)(void *)a->ifa_addr)->sin_addr.s_addr);
        out[n].mask = ntohl(((struct sockaddr_in *)(void *)a->ifa_netmask)->sin_addr.s_addr);
        out[n].loopback = (a->ifa_flags & IFF_LOOPBACK) != 0;
        n++;
    }
    freeifaddrs(all);
#endif
    return n;
}

/** @brief Parses a.b.c.d; false when want is not one. */
static bool parse_ip(const char *want, uint32_t *ip)
{
    struct in_addr a;
    if (inet_pton(AF_INET, want, &a) != 1)
        return false;
    *ip = ntohl(a.s_addr);
    return true;
}

bool rl_net_pick_iface(const char *want, rl_net_iface *out)
{
    rl_net_iface all[32];
    int n = rl_net_ifaces(all, 32);
    uint32_t ip = 0;
    bool by_ip = want && *want && parse_ip(want, &ip);
    for (int i = 0; i < n; i++) {
        bool ok = !want || !*want ? !all[i].loopback : by_ip ? all[i].ip == ip : strcmp(all[i].name, want) == 0;
        if (ok) {
            *out = all[i];
            return true;
        }
    }
    return false;
}
