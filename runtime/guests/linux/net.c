/**
 * @file net.c
 * @brief Sockets: AF_UNIX, AF_INET and AF_INET6 over the host's, in the
 * guest's descriptor table like any other host descriptor, so read, write,
 * poll, epoll, fcntl and close work on them unchanged.
 *
 * Addresses, option values, message headers and ancillary data are the
 * guest's (generic 64-bit Linux) layouts; hostnet_*.c turns them into the
 * host's. A path of an AF_UNIX address is a guest path and resolves under
 * the guest root like an open; SCM_RIGHTS carries guest descriptors, which
 * become host descriptors on the way out and new guest descriptors on the
 * way in.
 *
 * A blocking call waits for the socket with lx_wait first, so a signal
 * interrupts it as the kernel's: ERESTARTSYS before anything moved
 * (restarted after a handler with SA_RESTART), the count after some bytes
 * of a send went. SO_RCVTIMEO and SO_SNDTIMEO bound those waits (EAGAIN).
 */
#include "hostio.h"
#include "linux_priv.h"

#include <stdlib.h>
#include <string.h>

enum {
    G_AF_UNIX = 1,
    G_SOCK_NONBLOCK = 04000,
    G_SOCK_CLOEXEC = 02000000,
    G_MSG_DONTWAIT = 0x40,
    G_MSG_WAITALL = 0x100,
    G_MSG_NOSIGNAL = 0x4000,
    G_MSG_CMSG_CLOEXEC = 0x40000000,
    G_MSG_CTRUNC = 8,
    G_SOL_SOCKET = 1,
    G_SCM_RIGHTS = 1,
    G_SO_RCVTIMEO = 20,
    G_SO_SNDTIMEO = 21,
    ADDR_CAP = 128,
    MAX_MSG = 0x7ffff000,
};

/** @brief The socket's host descriptor, or -EBADF; -ENOTSOCK for a descriptor that is not a socket. */
static int sock_of(int64_t fd)
{
    int h = lx_host_fd((int)fd);
    if (h < 0)
        return -L_EBADF;
    l_stat st;
    if (hio_fstat(h, &st) == 0 && (st.st_mode & L_S_IFMT) != L_S_IFSOCK)
        return -L_ENOTSOCK;
    return h;
}

static bool blocking(int h)
{
    int64_t fl = hio_get_status_flags(h);
    return fl >= 0 && !(fl & L_O_NONBLOCK);
}

/** @brief Waits until the socket is ready for events within the descriptor's timeout: 0, -ERESTARTSYS for a signal, -EAGAIN when the timeout passes. */
static int64_t wait_for(int fd, int h, uint32_t events)
{
    int64_t t = events == L_POLLIN ? lx.fds[fd].rcv_timeout_ns : lx.fds[fd].snd_timeout_ns;
    int64_t r = lx_wait(h, events | L_POLLERR | L_POLLHUP, t > 0 ? t : -1);
    if (r == -L_EINTR)
        return -L_ERESTARTSYS;
    if (r == 0)
        return -L_EAGAIN;
    return r < 0 ? r : 0;
}

/**
 * @brief The guest address at addr (len bytes) in buf, its AF_UNIX path
 * (not an abstract name) turned into the host path under the guest root;
 * the length or -errno.
 */
static int64_t read_addr(uint64_t addr, uint64_t len, uint8_t buf[ADDR_CAP])
{
    if (len > ADDR_CAP || len < 2)
        return -L_EINVAL;
    if (!lx_get(buf, addr, len))
        return -L_EFAULT;
    uint16_t fam;
    memcpy(&fam, buf, 2);
    if (fam != G_AF_UNIX || len <= 2 || buf[2] == 0)
        return (int64_t)len;
    char path[112], host[4096];
    size_t n = len - 2;
    if (n >= sizeof path)
        return -L_EINVAL;
    memcpy(path, buf + 2, n);
    path[n] = 0;
    int64_t rc = lx_host_path(path, host, sizeof host);
    if (rc < 0)
        return rc;
    size_t hn = strlen(host);
    if (hn >= 108)
        return -L_ENAMETOOLONG;
    memcpy(buf + 2, host, hn + 1);
    return (int64_t)(2 + hn + 1);
}

/** @brief Writes an address the host gave (guest layout, full length len) to the guest's addr and *lenp, an AF_UNIX path below the guest root without the root. */
static int64_t put_addr(uint8_t *g, uint32_t len, uint64_t addr, uint64_t lenp)
{
    if (!addr || !lenp)
        return 0;
    uint32_t cap;
    if (!lx_get(&cap, lenp, 4))
        return -L_EFAULT;
    uint16_t fam;
    memcpy(&fam, g, 2);
    if (fam == G_AF_UNIX && len > 2 && g[2]) {
        char p[128];
        size_t n = len - 2 < sizeof p - 1 ? len - 2 : sizeof p - 1;
        memcpy(p, g + 2, n);
        p[n] = 0;
        n = strlen(p);
        n = lx_guest_name(p, n);
        memcpy(g + 2, p, n + 1);
        len = (uint32_t)(2 + n + 1);
    }
    if (!lx_put(addr, g, cap < len ? cap : len) || !lx_put(lenp, &len, 4))
        return -L_EFAULT;
    return 0;
}

static int64_t sys_socket(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int type = (int)a[1];
    if (type & ~(0xf | G_SOCK_NONBLOCK | G_SOCK_CLOEXEC))
        return -L_EINVAL;
    int64_t h = hio_socket((int)a[0], type, (int)a[2]);
    return h < 0 ? h : lx_fd_install((int)h, (type & G_SOCK_CLOEXEC) != 0);
}

static int64_t sys_socketpair(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int type = (int)a[1];
    if (type & ~(0xf | G_SOCK_NONBLOCK | G_SOCK_CLOEXEC))
        return -L_EINVAL;
    int sv[2];
    int64_t rc = hio_socketpair((int)a[0], type, (int)a[2], sv);
    if (rc < 0)
        return rc;
    bool cx = (type & G_SOCK_CLOEXEC) != 0;
    int64_t g0 = lx_fd_install(sv[0], cx);
    if (g0 < 0) {
        hio_close(sv[1]);
        return g0;
    }
    int64_t g1 = lx_fd_install(sv[1], cx);
    if (g1 < 0) {
        lx_fd_close((int)g0);
        return g1;
    }
    int32_t out[2] = { (int32_t)g0, (int32_t)g1 };
    if (!lx_put(a[3], out, sizeof out)) {
        lx_fd_close((int)g0);
        lx_fd_close((int)g1);
        return -L_EFAULT;
    }
    return 0;
}

static int64_t sys_bind(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = sock_of((int64_t)a[0]);
    if (h < 0)
        return h;
    uint8_t buf[ADDR_CAP];
    int64_t n = read_addr(a[1], a[2], buf);
    return n < 0 ? n : hio_bind(h, buf, (uint32_t)n);
}

static int64_t sys_connect(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = sock_of((int64_t)a[0]);
    if (h < 0)
        return h;
    uint8_t buf[ADDR_CAP];
    int64_t n = read_addr(a[1], a[2], buf);
    return n < 0 ? n : hio_connect(h, buf, (uint32_t)n);
}

static int64_t sys_listen(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = sock_of((int64_t)a[0]);
    return h < 0 ? h : hio_listen(h, (int)a[1]);
}

static int64_t do_accept(int64_t fd, uint64_t addr, uint64_t lenp, uint32_t flags)
{
    if (flags & ~(uint32_t)(G_SOCK_NONBLOCK | G_SOCK_CLOEXEC))
        return -L_EINVAL;
    int h = sock_of(fd);
    if (h < 0)
        return h;
    uint8_t g[ADDR_CAP];
    uint32_t len = sizeof g;
    for (;;) {
        if (blocking(h)) {
            int64_t w = wait_for((int)fd, h, L_POLLIN);
            if (w < 0)
                return w;
        }
        int64_t r = hio_accept(h, g, &len, (flags & G_SOCK_NONBLOCK) != 0);
        if (r == -L_EAGAIN && blocking(h))
            continue;
        if (r < 0)
            return r;
        int64_t nfd = lx_fd_install((int)r, (flags & G_SOCK_CLOEXEC) != 0);
        if (nfd < 0)
            return nfd;
        int64_t rc = put_addr(g, len, addr, lenp);
        if (rc < 0) {
            lx_fd_close((int)nfd);
            return rc;
        }
        return nfd;
    }
}

static int64_t sys_accept(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    return do_accept((int64_t)a[0], a[1], a[2], 0);
}

static int64_t sys_accept4(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    return do_accept((int64_t)a[0], a[1], a[2], (uint32_t)a[3]);
}

static int64_t names(const uint64_t a[6], bool peer)
{
    int h = sock_of((int64_t)a[0]);
    if (h < 0)
        return h;
    uint8_t g[ADDR_CAP];
    uint32_t len = sizeof g;
    int64_t rc = hio_sockname(h, peer, g, &len);
    return rc < 0 ? rc : put_addr(g, len, a[1], a[2]);
}

static int64_t sys_getsockname(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    return names(a, false);
}

static int64_t sys_getpeername(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    return names(a, true);
}

static int64_t sys_shutdown(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = sock_of((int64_t)a[0]);
    return h < 0 ? h : hio_shutdown(h, (int)a[1]);
}

/**
 * @brief Sends m on the socket with the kernel's blocking rules: a stream
 * socket's blocking send waits and goes on until every byte went, a signal
 * after some gives the count; EPIPE raises SIGPIPE unless MSG_NOSIGNAL.
 */
static int64_t send_all(int fd, int h, hio_msg *m, uint32_t flags)
{
    bool block = !(flags & G_MSG_DONTWAIT) && blocking(h);
    size_t done = 0, total = m->len;
    uint8_t *base = m->data;
    for (;;) {
        if (block) {
            int64_t w = wait_for(fd, h, L_POLLOUT);
            if (w < 0)
                return done ? (int64_t)done : w;
        }
        m->data = base + done;
        m->len = total - done;
        int64_t r = hio_sendmsg(h, m, flags | G_MSG_DONTWAIT);
        if (r == -L_EAGAIN && block)
            continue;
        if (r < 0) {
            if (r == -L_EPIPE && !(flags & G_MSG_NOSIGNAL))
                lx_signal_raise(L_SIGPIPE, L_SI_USER);
            return done ? (int64_t)done : r;
        }
        m->nrights = 0;
        done += (size_t)r;
        if (done >= total || !block)
            return (int64_t)done;
    }
}

/** @brief Receives into m with the blocking rules (MSG_WAITALL waits for the whole buffer, a signal after some gives the count). */
static int64_t recv_one(int fd, int h, hio_msg *m, uint32_t flags)
{
    bool block = !(flags & G_MSG_DONTWAIT) && blocking(h);
    size_t done = 0, total = m->len;
    uint8_t *base = m->data;
    for (;;) {
        if (block) {
            int64_t w = wait_for(fd, h, L_POLLIN);
            if (w < 0)
                return done ? (int64_t)done : w;
        }
        m->data = base + done;
        m->len = total - done;
        int64_t r = hio_recvmsg(h, m, (flags & ~(uint32_t)G_MSG_WAITALL) | G_MSG_DONTWAIT);
        if (r == -L_EAGAIN && block)
            continue;
        if (r < 0)
            return done ? (int64_t)done : r;
        done += (size_t)r;
        if (!(flags & G_MSG_WAITALL) || r == 0 || done >= total || !block || (size_t)r > m->len)
            return (int64_t)done;
    }
}

static int64_t sys_sendto(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    int h = sock_of((int64_t)a[0]);
    if (h < 0)
        return h;
    uint64_t len = a[2] > MAX_MSG ? MAX_MSG : a[2];
    if (len && !lx_ok(a[1], len, RL_VM_R))
        return -L_EFAULT;
    uint8_t name[ADDR_CAP];
    hio_msg *m = calloc(1, sizeof *m);
    if (!m)
        return -L_ENOMEM;
    m->data = mem + a[1];
    m->len = len;
    if (a[4]) {
        int64_t n = read_addr(a[4], a[5], name);
        if (n < 0) {
            free(m);
            return n;
        }
        m->name = name;
        m->namelen = (uint32_t)n;
    }
    int64_t r = send_all((int)a[0], h, m, (uint32_t)a[3]);
    free(m);
    return r;
}

static int64_t sys_recvfrom(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    int h = sock_of((int64_t)a[0]);
    if (h < 0)
        return h;
    uint64_t len = a[2] > MAX_MSG ? MAX_MSG : a[2];
    if (len && !lx_ok(a[1], len, RL_VM_W))
        return -L_EFAULT;
    uint8_t name[ADDR_CAP];
    hio_msg *m = calloc(1, sizeof *m);
    if (!m)
        return -L_ENOMEM;
    m->data = mem + a[1];
    m->len = len;
    m->name = name;
    m->namelen = sizeof name;
    int64_t r = recv_one((int)a[0], h, m, (uint32_t)a[3]);
    for (int i = 0; i < m->nrights; i++)
        hio_close(m->rights[i]);
    if (r >= 0 && a[4]) {
        int64_t rc = m->namelen ? put_addr(name, m->namelen, a[4], a[5]) : 0;
        uint32_t zero = 0;
        if (!m->namelen && a[5] && !lx_put(a[5], &zero, 4))
            rc = -L_EFAULT;
        if (rc < 0)
            r = rc;
    }
    free(m);
    return r;
}

/** @brief struct msghdr of the guest. */
typedef struct g_msghdr {
    uint64_t name;
    uint32_t namelen;
    uint32_t pad;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t control;
    uint64_t controllen;
    int32_t flags;
    uint32_t pad2;
} g_msghdr;

/** @brief The iovec array of h in one buffer of *total bytes (gathered from the guest when out, to scatter into when not); NULL on a bad vector. */
static uint8_t *gather(const g_msghdr *h, uint64_t (*v)[2], uint64_t *total, bool out, int64_t *err)
{
    *err = lx_iov_get(h->iov, h->iovlen, v, total, out ? RL_VM_R : RL_VM_W);
    if (*err < 0)
        return NULL;
    uint8_t *buf = malloc(*total ? *total : 1);
    if (!buf) {
        *err = -L_ENOMEM;
        return NULL;
    }
    if (out) {
        uint64_t at = 0;
        for (uint64_t i = 0; i < h->iovlen; i++) {
            memcpy(buf + at, rl_mem_base() + v[i][0], v[i][1]);
            at += v[i][1];
        }
    }
    return buf;
}

enum { IOVS = 1024 };

static int64_t sys_sendmsg(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = sock_of((int64_t)a[0]);
    if (h < 0)
        return h;
    g_msghdr gh;
    if (!lx_get(&gh, a[1], sizeof gh))
        return -L_EFAULT;
    static _Thread_local uint64_t v[IOVS][2];
    uint64_t total;
    int64_t err;
    uint8_t *buf = gather(&gh, v, &total, true, &err);
    if (!buf)
        return err;
    hio_msg *m = calloc(1, sizeof *m);
    uint8_t name[ADDR_CAP];
    int64_t r = m ? 0 : -L_ENOMEM;
    if (r == 0 && gh.name && gh.namelen) {
        int64_t n = read_addr(gh.name, gh.namelen, name);
        if (n < 0)
            r = n;
        m->name = name;
        m->namelen = (uint32_t)(n < 0 ? 0 : n);
    }
    for (uint64_t at = 0; r == 0 && gh.control && at + 16 <= gh.controllen;) {
        uint64_t len;
        int32_t lt[2];
        if (!lx_get(&len, gh.control + at, 8) || !lx_get(lt, gh.control + at + 8, 8)) {
            r = -L_EFAULT;
            break;
        }
        if (len < 16 || at + len > gh.controllen) {
            r = -L_EINVAL;
            break;
        }
        if (lt[0] == G_SOL_SOCKET && lt[1] == G_SCM_RIGHTS) {
            for (uint64_t k = 16; k + 4 <= len; k += 4) {
                int32_t gfd;
                if (!lx_get(&gfd, gh.control + at + k, 4)) {
                    r = -L_EFAULT;
                    break;
                }
                int hfd = lx_host_fd(gfd);
                if (hfd < 0 || m->nrights >= HIO_MAX_RIGHTS) {
                    r = hfd < 0 ? -L_EBADF : -L_EINVAL;
                    break;
                }
                m->rights[m->nrights++] = hfd;
            }
        }
        at += (len + 7) & ~7ull;
    }
    if (r == 0) {
        m->data = buf;
        m->len = total;
        r = send_all((int)a[0], h, m, (uint32_t)a[2]);
    }
    free(m);
    free(buf);
    return r;
}

static int64_t sys_recvmsg(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = sock_of((int64_t)a[0]);
    if (h < 0)
        return h;
    g_msghdr gh;
    if (!lx_get(&gh, a[1], sizeof gh))
        return -L_EFAULT;
    static _Thread_local uint64_t v[IOVS][2];
    uint64_t total;
    int64_t err;
    uint8_t *buf = gather(&gh, v, &total, false, &err);
    if (!buf)
        return err;
    hio_msg *m = calloc(1, sizeof *m);
    if (!m) {
        free(buf);
        return -L_ENOMEM;
    }
    uint8_t name[ADDR_CAP];
    m->data = buf;
    m->len = total;
    m->name = gh.name ? name : NULL;
    m->namelen = sizeof name;
    uint32_t flags = (uint32_t)a[2];
    int64_t r = recv_one((int)a[0], h, m, flags);
    if (r >= 0) {
        uint64_t at = 0, got = (uint64_t)r < total ? (uint64_t)r : total;
        for (uint64_t i = 0; i < gh.iovlen && at < got; i++) {
            uint64_t n = v[i][1] < got - at ? v[i][1] : got - at;
            memcpy(rl_mem_base() + v[i][0], buf + at, n);
            at += n;
        }
        uint32_t out_flags = m->flags;
        uint64_t clen = 0;
        int took = 0;
        if (m->nrights) {
            uint64_t room = gh.controllen >= 16 ? (gh.controllen - 16) / 4 : 0;
            int fit = (int)(room < (uint64_t)m->nrights ? room : (uint64_t)m->nrights);
            int32_t gfds[HIO_MAX_RIGHTS];
            for (; took < fit; took++) {
                int64_t g = lx_fd_install(m->rights[took], (flags & G_MSG_CMSG_CLOEXEC) != 0);
                if (g < 0)
                    break;
                gfds[took] = (int32_t)g;
            }
            for (int i = took; i < m->nrights; i++)
                hio_close(m->rights[i]);
            if (took < m->nrights)
                out_flags |= G_MSG_CTRUNC;
            if (took) {
                uint64_t len = 16 + 4 * (uint64_t)took;
                int32_t lt[2] = { G_SOL_SOCKET, G_SCM_RIGHTS };
                if (!lx_put(gh.control, &len, 8) || !lx_put(gh.control + 8, lt, 8) || !lx_put(gh.control + 16, gfds, 4 * (size_t)took))
                    r = -L_EFAULT;
                clen = (len + 7) & ~7ull;
                if (clen > gh.controllen)
                    clen = len;
            }
        }
        uint32_t nl = gh.name ? m->namelen : 0;
        if (gh.name && nl) {
            uint64_t tmp_len = a[1] + offsetof(g_msghdr, namelen);
            uint32_t cap = gh.namelen;
            if (!lx_put(tmp_len, &cap, 4) || put_addr(name, nl, gh.name, tmp_len) < 0)
                r = -L_EFAULT;
        } else if (!lx_put(a[1] + offsetof(g_msghdr, namelen), &nl, 4)) {
            r = -L_EFAULT;
        }
        int32_t f32 = (int32_t)out_flags;
        if (!lx_put(a[1] + offsetof(g_msghdr, controllen), &clen, 8) || !lx_put(a[1] + offsetof(g_msghdr, flags), &f32, 4))
            r = -L_EFAULT;
    } else {
        for (int i = 0; i < m->nrights; i++)
            hio_close(m->rights[i]);
    }
    free(m);
    free(buf);
    return r;
}

static int64_t sys_setsockopt(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = sock_of((int64_t)a[0]);
    if (h < 0)
        return h;
    uint8_t val[256];
    if (a[4] > sizeof val)
        return -L_EINVAL;
    if (a[4] && !lx_get(val, a[3], a[4]))
        return -L_EFAULT;
    int64_t rc = hio_setsockopt(h, (int)a[1], (int)a[2], val, (uint32_t)a[4]);
    if (rc == 0 && (int)a[1] == G_SOL_SOCKET && ((int)a[2] == G_SO_RCVTIMEO || (int)a[2] == G_SO_SNDTIMEO) && a[4] >= 16) {
        int64_t tv[2];
        memcpy(tv, val, sizeof tv);
        int64_t ns = tv[0] * 1000000000ll + tv[1] * 1000;
        if ((int)a[2] == G_SO_RCVTIMEO)
            lx.fds[a[0]].rcv_timeout_ns = ns;
        else
            lx.fds[a[0]].snd_timeout_ns = ns;
    }
    return rc;
}

static int64_t sys_getsockopt(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = sock_of((int64_t)a[0]);
    if (h < 0)
        return h;
    uint32_t len;
    if (!lx_get(&len, a[4], 4))
        return -L_EFAULT;
    uint8_t val[256];
    uint32_t cap = len > sizeof val ? sizeof val : len;
    uint32_t got = cap;
    int64_t rc = hio_getsockopt(h, (int)a[1], (int)a[2], val, &got);
    if (rc < 0)
        return rc;
    uint32_t n = got < cap ? got : cap;
    if (!lx_put(a[3], val, n) || !lx_put(a[4], &n, 4))
        return -L_EFAULT;
    return 0;
}

int64_t lx_sock_write(int fd, int h, const uint8_t *buf, uint64_t n)
{
    hio_msg *m = calloc(1, sizeof *m);
    if (!m)
        return -L_ENOMEM;
    m->data = (void *)buf;
    m->len = n;
    int64_t r = send_all(fd, h, m, G_MSG_NOSIGNAL);
    free(m);
    return r;
}

lx_sys_fn *lx_net_call(uint32_t nr)
{
    switch (nr) {
    case NR_socket: return sys_socket;
    case NR_socketpair: return sys_socketpair;
    case NR_bind: return sys_bind;
    case NR_listen: return sys_listen;
    case NR_accept: return sys_accept;
    case NR_accept4: return sys_accept4;
    case NR_connect: return sys_connect;
    case NR_getsockname: return sys_getsockname;
    case NR_getpeername: return sys_getpeername;
    case NR_sendto: return sys_sendto;
    case NR_recvfrom: return sys_recvfrom;
    case NR_setsockopt: return sys_setsockopt;
    case NR_getsockopt: return sys_getsockopt;
    case NR_shutdown: return sys_shutdown;
    case NR_sendmsg: return sys_sendmsg;
    case NR_recvmsg: return sys_recvmsg;
    default: return NULL;
    }
}
