/**
 * @file hostnet_win.c
 * @brief Sockets on a Windows host: not yet carried over Winsock, so every
 * socket call reports that the family is not supported.
 */
#include "hostio.h"

#if defined(_WIN32)

int64_t hio_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    return -L_EAFNOSUPPORT;
}

int64_t hio_socketpair(int domain, int type, int protocol, int sv[2])
{
    (void)domain;
    (void)type;
    (void)protocol;
    (void)sv;
    return -L_EAFNOSUPPORT;
}

int64_t hio_bind(int h, const uint8_t *addr, uint32_t len)
{
    (void)h;
    (void)addr;
    (void)len;
    return -L_ENOTSOCK;
}

int64_t hio_connect(int h, const uint8_t *addr, uint32_t len)
{
    (void)h;
    (void)addr;
    (void)len;
    return -L_ENOTSOCK;
}

int64_t hio_listen(int h, int backlog)
{
    (void)h;
    (void)backlog;
    return -L_ENOTSOCK;
}

int64_t hio_accept(int h, uint8_t *addr, uint32_t *len, bool nonblock)
{
    (void)h;
    (void)addr;
    (void)len;
    (void)nonblock;
    return -L_ENOTSOCK;
}

int64_t hio_sockname(int h, bool peer, uint8_t *addr, uint32_t *len)
{
    (void)h;
    (void)peer;
    (void)addr;
    (void)len;
    return -L_ENOTSOCK;
}

int64_t hio_shutdown(int h, int how)
{
    (void)h;
    (void)how;
    return -L_ENOTSOCK;
}

int64_t hio_sendmsg(int h, const hio_msg *m, uint32_t flags)
{
    (void)h;
    (void)m;
    (void)flags;
    return -L_ENOTSOCK;
}

int64_t hio_recvmsg(int h, hio_msg *m, uint32_t flags)
{
    (void)h;
    (void)m;
    (void)flags;
    return -L_ENOTSOCK;
}

int64_t hio_setsockopt(int h, int level, int name, const void *val, uint32_t len)
{
    (void)h;
    (void)level;
    (void)name;
    (void)val;
    (void)len;
    return -L_ENOTSOCK;
}

int64_t hio_getsockopt(int h, int level, int name, void *val, uint32_t *len)
{
    (void)h;
    (void)level;
    (void)name;
    (void)val;
    (void)len;
    return -L_ENOTSOCK;
}

#endif
