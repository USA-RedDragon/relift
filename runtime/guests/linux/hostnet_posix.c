/**
 * @file hostnet_posix.c
 * @brief Sockets on a POSIX host for the Linux guest: socket addresses,
 * option numbers and values, message flags and ancillary data in the
 * guest's (generic Linux, 64-bit) layout, turned into the host's and back.
 * On a Linux host most of it is the identity; on macOS the families,
 * SOL_SOCKET, its option numbers, sockaddr's length byte and cmsg's
 * header differ.
 */
#include "hostio.h"

#if !defined(_WIN32)

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

int lx_errno(int e);

static int64_t fail(void) { return -(int64_t)lx_errno(errno); }

enum { G_AF_UNIX = 1, G_AF_INET = 2, G_AF_INET6 = 10, G_SOL_SOCKET = 1, G_IPPROTO_IPV6 = 41, G_IPPROTO_TCP = 6, G_IPPROTO_IP = 0 };

static int host_family(int f)
{
    switch (f) {
    case G_AF_UNIX: return AF_UNIX;
    case G_AF_INET: return AF_INET;
    case G_AF_INET6: return AF_INET6;
    case 0: return AF_UNSPEC;
    default: return -1;
    }
}

static int guest_family(int f)
{
    if (f == AF_UNIX)
        return G_AF_UNIX;
    if (f == AF_INET)
        return G_AF_INET;
    if (f == AF_INET6)
        return G_AF_INET6;
    return f == AF_UNSPEC ? 0 : -1;
}

static int host_type(int t)
{
    switch (t & 0xf) {
    case 1: return SOCK_STREAM;
    case 2: return SOCK_DGRAM;
    case 3: return SOCK_RAW;
    case 5: return SOCK_SEQPACKET;
    default: return -1;
    }
}

static int guest_type(int t)
{
    if (t == SOCK_STREAM)
        return 1;
    if (t == SOCK_DGRAM)
        return 2;
    if (t == SOCK_RAW)
        return 3;
    return t == SOCK_SEQPACKET ? 5 : t;
}

/** @brief The guest's sockaddr (Linux layout) as a host sockaddr; the host length or -errno. */
static int64_t to_host_addr(const uint8_t *g, uint32_t glen, struct sockaddr_storage *h)
{
    memset(h, 0, sizeof *h);
    if (glen < 2)
        return -L_EINVAL;
    uint16_t fam;
    memcpy(&fam, g, 2);
    int hf = host_family(fam);
    if (hf < 0)
        return -L_EAFNOSUPPORT;
    switch (fam) {
    case G_AF_INET: {
        if (glen < 16)
            return -L_EINVAL;
        struct sockaddr_in *s = (struct sockaddr_in *)h;
        s->sin_family = AF_INET;
        memcpy(&s->sin_port, g + 2, 2);
        memcpy(&s->sin_addr, g + 4, 4);
#if defined(__APPLE__)
        s->sin_len = sizeof *s;
#endif
        return sizeof *s;
    }
    case G_AF_INET6: {
        if (glen < 24)
            return -L_EINVAL;
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)h;
        s->sin6_family = AF_INET6;
        memcpy(&s->sin6_port, g + 2, 2);
        memcpy(&s->sin6_flowinfo, g + 4, 4);
        memcpy(&s->sin6_addr, g + 8, 16);
        if (glen >= 28)
            memcpy(&s->sin6_scope_id, g + 24, 4);
#if defined(__APPLE__)
        s->sin6_len = sizeof *s;
#endif
        return sizeof *s;
    }
    case G_AF_UNIX: {
        struct sockaddr_un *s = (struct sockaddr_un *)h;
        s->sun_family = AF_UNIX;
        size_t n = glen - 2;
        if (n > sizeof s->sun_path)
            return -L_EINVAL;
        memcpy(s->sun_path, g + 2, n);
#if defined(__APPLE__)
        s->sun_len = (uint8_t)(offsetof(struct sockaddr_un, sun_path) + n);
#endif
        return (int64_t)(offsetof(struct sockaddr_un, sun_path) + n);
    }
    default:
        h->ss_family = AF_UNSPEC;
        return 2;
    }
}

/** @brief The host sockaddr as the guest's into out (at most *len bytes); *len gets its full length. */
static void to_guest_addr(const struct sockaddr_storage *h, socklen_t hlen, uint8_t *out, uint32_t *len)
{
    uint8_t g[128];
    memset(g, 0, sizeof g);
    uint32_t n = 2;
    uint16_t fam = (uint16_t)guest_family(h->ss_family);
    memcpy(g, &fam, 2);
    if (h->ss_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)h;
        memcpy(g + 2, &s->sin_port, 2);
        memcpy(g + 4, &s->sin_addr, 4);
        n = 16;
    } else if (h->ss_family == AF_INET6) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)h;
        memcpy(g + 2, &s->sin6_port, 2);
        memcpy(g + 4, &s->sin6_flowinfo, 4);
        memcpy(g + 8, &s->sin6_addr, 16);
        memcpy(g + 24, &s->sin6_scope_id, 4);
        n = 28;
    } else if (h->ss_family == AF_UNIX) {
        const struct sockaddr_un *s = (const struct sockaddr_un *)h;
        size_t p = hlen > offsetof(struct sockaddr_un, sun_path) ? hlen - offsetof(struct sockaddr_un, sun_path) : 0;
        if (p > 108)
            p = 108;
        memcpy(g + 2, s->sun_path, p);
        n = (uint32_t)(2 + p);
    }
    memcpy(out, g, n < *len ? n : *len);
    *len = n;
}

int64_t hio_socket(int domain, int type, int protocol)
{
    int f = host_family(domain), t = host_type(type);
    if (f < 0 || f == AF_UNSPEC)
        return -L_EAFNOSUPPORT;
    if (t < 0)
        return -L_EINVAL;
    int s = socket(f, t, protocol);
    if (s < 0)
        return fail();
    fcntl(s, F_SETFD, FD_CLOEXEC);
    if (type & 04000)
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return s;
}

int64_t hio_socketpair(int domain, int type, int protocol, int sv[2])
{
    int f = host_family(domain), t = host_type(type);
    if (f < 0 || f == AF_UNSPEC)
        return -L_EAFNOSUPPORT;
    if (t < 0)
        return -L_EINVAL;
    if (socketpair(f, t, protocol, sv) < 0)
        return fail();
    for (int i = 0; i < 2; i++) {
        fcntl(sv[i], F_SETFD, FD_CLOEXEC);
        if (type & 04000)
            fcntl(sv[i], F_SETFL, fcntl(sv[i], F_GETFL) | O_NONBLOCK);
    }
    return 0;
}

int64_t hio_bind(int h, const uint8_t *addr, uint32_t len)
{
    struct sockaddr_storage s;
    int64_t n = to_host_addr(addr, len, &s);
    if (n < 0)
        return n;
    return bind(h, (struct sockaddr *)&s, (socklen_t)n) < 0 ? fail() : 0;
}

int64_t hio_connect(int h, const uint8_t *addr, uint32_t len)
{
    struct sockaddr_storage s;
    int64_t n = to_host_addr(addr, len, &s);
    if (n < 0)
        return n;
    return connect(h, (struct sockaddr *)&s, (socklen_t)n) < 0 ? fail() : 0;
}

int64_t hio_listen(int h, int backlog) { return listen(h, backlog) < 0 ? fail() : 0; }

int64_t hio_accept(int h, uint8_t *addr, uint32_t *len, bool nonblock)
{
    struct sockaddr_storage s;
    socklen_t sl = sizeof s;
    int r = accept(h, (struct sockaddr *)&s, &sl);
    if (r < 0)
        return fail();
    fcntl(r, F_SETFD, FD_CLOEXEC);
    int fl = fcntl(r, F_GETFL);
    fcntl(r, F_SETFL, nonblock ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
    if (addr && len)
        to_guest_addr(&s, sl, addr, len);
    return r;
}

int64_t hio_sockname(int h, bool peer, uint8_t *addr, uint32_t *len)
{
    struct sockaddr_storage s;
    memset(&s, 0, sizeof s);
    socklen_t sl = sizeof s;
    if ((peer ? getpeername(h, (struct sockaddr *)&s, &sl) : getsockname(h, (struct sockaddr *)&s, &sl)) < 0)
        return fail();
    to_guest_addr(&s, sl, addr, len);
    return 0;
}

int64_t hio_shutdown(int h, int how) { return shutdown(h, how) < 0 ? fail() : 0; }

static int host_msg_flags(uint32_t g)
{
    int f = 0;
    if (g & 1)
        f |= MSG_OOB;
    if (g & 2)
        f |= MSG_PEEK;
    if (g & 4)
        f |= MSG_DONTROUTE;
    if (g & 0x40)
        f |= MSG_DONTWAIT;
    if (g & 0x80)
        f |= MSG_EOR;
    if (g & 0x100)
        f |= MSG_WAITALL;
    if (g & 0x20)
        f |= MSG_TRUNC;
#if defined(MSG_NOSIGNAL)
    f |= MSG_NOSIGNAL;
#endif
    return f;
}

static uint32_t guest_msg_flags(int h)
{
    uint32_t g = 0;
    if (h & MSG_OOB)
        g |= 1;
    if (h & MSG_CTRUNC)
        g |= 8;
    if (h & MSG_TRUNC)
        g |= 0x20;
    if (h & MSG_EOR)
        g |= 0x80;
    return g;
}

int64_t hio_sendmsg(int h, const hio_msg *m, uint32_t flags)
{
    struct sockaddr_storage s;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    if (m->name && m->namelen) {
        int64_t n = to_host_addr(m->name, m->namelen, &s);
        if (n < 0)
            return n;
        mh.msg_name = &s;
        mh.msg_namelen = (socklen_t)n;
    }
    struct iovec iov = { (void *)m->data, m->len };
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int) * HIO_MAX_RIGHTS)];
    } cbuf;
    if (m->nrights > 0) {
        mh.msg_control = cbuf.buf;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)m->nrights);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)m->nrights);
        memcpy(CMSG_DATA(c), m->rights, sizeof(int) * (size_t)m->nrights);
    }
    ssize_t r = sendmsg(h, &mh, host_msg_flags(flags));
    return r < 0 ? fail() : r;
}

int64_t hio_recvmsg(int h, hio_msg *m, uint32_t flags)
{
    struct sockaddr_storage s;
    memset(&s, 0, sizeof s);
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_name = &s;
    mh.msg_namelen = sizeof s;
    struct iovec iov = { m->data, m->len };
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int) * HIO_MAX_RIGHTS)];
    } cbuf;
    mh.msg_control = cbuf.buf;
    mh.msg_controllen = sizeof cbuf.buf;
    int hf = host_msg_flags(flags);
#if defined(MSG_CMSG_CLOEXEC)
    hf |= MSG_CMSG_CLOEXEC;
#endif
    ssize_t r = recvmsg(h, &mh, hf);
    if (r < 0)
        return fail();
    m->nrights = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < n && m->nrights < HIO_MAX_RIGHTS; i++) {
                int fd;
                memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                m->rights[m->nrights++] = fd;
            }
        }
    if (m->name) {
        uint32_t len = m->namelen;
        if (mh.msg_namelen > 0 && s.ss_family != AF_UNSPEC)
            to_guest_addr(&s, mh.msg_namelen, m->name, &len);
        else
            len = 0;
        m->namelen = len;
    }
    m->flags = guest_msg_flags(mh.msg_flags);
    return r;
}

/** @brief Guest SOL_SOCKET option numbers and the host's. */
static const struct {
    int g, h;
} sol_opts[] = {
    { 1, SO_DEBUG },      { 2, SO_REUSEADDR }, { 3, SO_TYPE },     { 4, SO_ERROR },       { 5, SO_DONTROUTE },
    { 6, SO_BROADCAST },  { 7, SO_SNDBUF },    { 8, SO_RCVBUF },   { 9, SO_KEEPALIVE },   { 10, SO_OOBINLINE },
    { 13, SO_LINGER },    { 18, SO_RCVLOWAT }, { 19, SO_SNDLOWAT }, { 20, SO_RCVTIMEO },  { 21, SO_SNDTIMEO },
    { 30, SO_ACCEPTCONN },
#if defined(SO_REUSEPORT)
    { 15, SO_REUSEPORT },
#endif
#if defined(__linux__)
    { 16, SO_PASSCRED },  { 17, SO_PEERCRED }, { 38, SO_PROTOCOL }, { 39, SO_DOMAIN },    { 12, SO_PRIORITY },
#endif
};

/** @brief The host level and name of a guest socket option, false when unknown. */
static bool host_opt(int level, int name, int *hl, int *hn)
{
    if (level == G_SOL_SOCKET) {
        *hl = SOL_SOCKET;
        for (size_t i = 0; i < sizeof sol_opts / sizeof *sol_opts; i++)
            if (sol_opts[i].g == name) {
                *hn = sol_opts[i].h;
                return true;
            }
        return false;
    }
    *hl = level;
    *hn = name;
#if defined(__APPLE__)
    if (level == G_IPPROTO_IPV6 && name == 26) {
        *hn = IPV6_V6ONLY;
        return true;
    }
    if (level == G_IPPROTO_TCP && name == 4) {
        *hn = TCP_KEEPALIVE;
        return true;
    }
    return level == G_IPPROTO_TCP ? name == TCP_NODELAY : level == G_IPPROTO_IP || level == G_IPPROTO_IPV6;
#else
    (void)G_IPPROTO_IP;
    return true;
#endif
}

int64_t hio_setsockopt(int h, int level, int name, const void *val, uint32_t len)
{
    int hl, hn;
    if (!host_opt(level, name, &hl, &hn))
        return -L_ENOPROTOOPT;
    if (level == G_SOL_SOCKET && (name == 20 || name == 21)) {
        if (len < 16)
            return -L_EINVAL;
        int64_t tv[2];
        memcpy(tv, val, sizeof tv);
        struct timeval t = { (time_t)tv[0], (suseconds_t)tv[1] };
        return setsockopt(h, hl, hn, &t, sizeof t) < 0 ? fail() : 0;
    }
    return setsockopt(h, hl, hn, val, (socklen_t)len) < 0 ? fail() : 0;
}

int64_t hio_getsockopt(int h, int level, int name, void *val, uint32_t *len)
{
    int hl, hn;
    if (!host_opt(level, name, &hl, &hn))
        return -L_ENOPROTOOPT;
    if (level == G_SOL_SOCKET && (name == 20 || name == 21)) {
        struct timeval t;
        socklen_t sl = sizeof t;
        if (getsockopt(h, hl, hn, &t, &sl) < 0)
            return fail();
        int64_t tv[2] = { (int64_t)t.tv_sec, (int64_t)t.tv_usec };
        uint32_t n = *len < 16 ? *len : 16;
        memcpy(val, tv, n);
        *len = 16;
        return 0;
    }
    uint8_t buf[256];
    socklen_t sl = sizeof buf;
    if (getsockopt(h, hl, hn, buf, &sl) < 0)
        return fail();
    if (level == G_SOL_SOCKET && name == 3 && sl >= 4) {
        int t;
        memcpy(&t, buf, 4);
        t = guest_type(t);
        memcpy(buf, &t, 4);
    }
#if defined(__linux__)
    if (level == G_SOL_SOCKET && name == 39 && sl >= 4) {
        int f;
        memcpy(&f, buf, 4);
        f = guest_family(f);
        memcpy(buf, &f, 4);
    }
#endif
    uint32_t n = *len < (uint32_t)sl ? *len : (uint32_t)sl;
    memcpy(val, buf, n);
    *len = (uint32_t)sl;
    return 0;
}

#endif
