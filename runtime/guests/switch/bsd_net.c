/**
 * @file bsd_net.c
 * @brief The descriptor table and in-process loopback behind bsd:u, see bsd_net.h.
 */
#include "bsd_net.h"

#include "hos.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FDS 1024
#define MAX_BACKLOG 128
#define STREAM_CAP (256u * 1024u)
#define DGRAM_QUEUE 64
#define DGRAM_MAX 65507u
#define MAX_OPTS 16
#define EPHEMERAL_FIRST 49152u
#define LOOPBACK 0x7f000001u

#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define TYPE_FLAGS 0x20000000u
#define TYPE_EVENTFD 100

#define FD_NONBLOCK 0x4
#define EFD_SEMAPHORE 0x1
#define EFD_NONBLOCK 0x4

#define SOL_SOCKET 0xffff
#define SO_SNDBUF 0x1001
#define SO_RCVBUF 0x1002
#define SO_ERROR 0x1007
#define SO_TYPE 0x1008

#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define O_RDWR 2

typedef struct msg {
    bsd_addr from;
    size_t len;
    uint8_t *data;
} msg;

typedef struct sock sock;
struct sock {
    int open;
    int type;
    bool nonblock;
    bool bound;
    bsd_addr local;
    bool listening;
    sock *queue[MAX_BACKLOG];
    int queued;
    int backlog;
    bool connected;
    bsd_addr remote;
    sock *peer;
    bool peer_gone;
    bool eof_in;
    bool shut_wr;
    uint8_t *buf;
    size_t len;
    msg dgrams[DGRAM_QUEUE];
    int ndgrams;
    uint64_t counter;
    bool semaphore;
    struct {
        uint32_t level, name;
        int32_t value;
        bool set;
    } opts[MAX_OPTS];
};

struct bsd_net {
    pthread_mutex_t lock;
    sock *fds[MAX_FDS];
    uint16_t next_port;
};

void bsd_addr_pack(uint8_t out[16], bsd_addr a)
{
    memset(out, 0, 16);
    out[0] = 16;
    out[1] = AF_INET;
    out[2] = (uint8_t)(a.port >> 8);
    out[3] = (uint8_t)a.port;
    out[4] = (uint8_t)(a.ip >> 24);
    out[5] = (uint8_t)(a.ip >> 16);
    out[6] = (uint8_t)(a.ip >> 8);
    out[7] = (uint8_t)a.ip;
}

static bool addr_parse(const uint8_t *sa, size_t len, bsd_addr *a, int32_t *err)
{
    if (!sa || len < 8) {
        *err = BSD_EINVAL;
        return false;
    }
    if (sa[1] != AF_INET) {
        *err = BSD_EAFNOSUPPORT;
        return false;
    }
    a->port = (uint16_t)(sa[2] << 8 | sa[3]);
    a->ip = (uint32_t)sa[4] << 24 | (uint32_t)sa[5] << 16 | (uint32_t)sa[6] << 8 | sa[7];
    return true;
}

static bool is_loopback(uint32_t ip) { return ip == 0 || (ip >> 24) == 127; }

bsd_net *bsd_net_new(void)
{
    bsd_net *n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    pthread_mutex_init(&n->lock, NULL);
    n->next_port = EPHEMERAL_FIRST;
    return n;
}

static void sock_free(sock *s)
{
    free(s->buf);
    for (int i = 0; i < s->ndgrams; i++)
        free(s->dgrams[i].data);
    free(s);
}

static void sock_close(sock *s)
{
    if (s->peer) {
        s->peer->peer = NULL;
        s->peer->peer_gone = true;
        s->peer->eof_in = true;
    }
    for (int i = 0; i < s->queued; i++) {
        sock *q = s->queue[i];
        if (q->peer) {
            q->peer->peer = NULL;
            q->peer->peer_gone = true;
            q->peer->eof_in = true;
        }
        sock_free(q);
    }
    sock_free(s);
}

void bsd_net_free(bsd_net *n)
{
    if (!n)
        return;
    for (int i = 0; i < MAX_FDS; i++) {
        sock *s = n->fds[i];
        if (!s)
            continue;
        for (int j = i; j < MAX_FDS; j++)
            if (n->fds[j] == s)
                n->fds[j] = NULL;
        sock_close(s);
    }
    pthread_mutex_destroy(&n->lock);
    free(n);
}

static sock *get(bsd_net *n, int32_t fd)
{
    return fd >= 0 && fd < MAX_FDS ? n->fds[fd] : NULL;
}

static int32_t fail(int32_t *err, int32_t e)
{
    *err = e;
    return -1;
}

static int32_t alloc_fd(bsd_net *n, sock *s)
{
    for (int i = 0; i < MAX_FDS; i++) {
        if (!n->fds[i]) {
            n->fds[i] = s;
            return i;
        }
    }
    return -1;
}

static bool port_taken(bsd_net *n, int type, uint16_t port)
{
    for (int i = 0; i < MAX_FDS; i++) {
        sock *s = n->fds[i];
        if (s && s->bound && s->local.port == port && (s->type == type || type == 0))
            return true;
    }
    return false;
}

static uint16_t ephemeral(bsd_net *n, int type)
{
    for (uint32_t tries = 0; tries < 16384; tries++) {
        uint16_t p = n->next_port++;
        if (n->next_port < EPHEMERAL_FIRST)
            n->next_port = EPHEMERAL_FIRST;
        if (!port_taken(n, type, p))
            return p;
    }
    return 0;
}

static void autobind(bsd_net *n, sock *s)
{
    if (s->bound)
        return;
    s->bound = true;
    s->local.ip = LOOPBACK;
    s->local.port = ephemeral(n, s->type);
}

static int32_t lookup_opt(sock *s, uint32_t level, uint32_t name)
{
    for (int i = 0; i < MAX_OPTS; i++)
        if (s->opts[i].set && s->opts[i].level == level && s->opts[i].name == name)
            return i;
    return -1;
}

int32_t bsd_net_socket(bsd_net *n, uint32_t domain, uint32_t type, uint32_t protocol, int32_t *err)
{
    (void)protocol;
    *err = 0;
    if (domain != AF_INET)
        return fail(err, BSD_EAFNOSUPPORT);
    uint32_t t = type & ~TYPE_FLAGS;
    if (t != SOCK_STREAM && t != SOCK_DGRAM && t != SOCK_RAW)
        return fail(err, BSD_EPROTONOSUPPORT);
    sock *s = calloc(1, sizeof *s);
    if (!s)
        return fail(err, BSD_EMFILE);
    s->open = 1;
    s->type = (int)t;
    pthread_mutex_lock(&n->lock);
    int32_t fd = alloc_fd(n, s);
    pthread_mutex_unlock(&n->lock);
    if (fd < 0) {
        free(s);
        return fail(err, BSD_EMFILE);
    }
    return fd;
}

int32_t bsd_net_eventfd(bsd_net *n, uint64_t initval, uint32_t flags, int32_t *err)
{
    *err = 0;
    sock *s = calloc(1, sizeof *s);
    if (!s)
        return fail(err, BSD_EMFILE);
    s->open = 1;
    s->type = TYPE_EVENTFD;
    s->counter = initval;
    s->semaphore = (flags & EFD_SEMAPHORE) != 0;
    s->nonblock = (flags & EFD_NONBLOCK) != 0;
    pthread_mutex_lock(&n->lock);
    int32_t fd = alloc_fd(n, s);
    pthread_mutex_unlock(&n->lock);
    if (fd < 0) {
        free(s);
        return fail(err, BSD_EMFILE);
    }
    return fd;
}

int32_t bsd_net_close(bsd_net *n, int32_t fd, int32_t *err)
{
    *err = 0;
    pthread_mutex_lock(&n->lock);
    sock *s = get(n, fd);
    if (!s) {
        pthread_mutex_unlock(&n->lock);
        return fail(err, BSD_EBADF);
    }
    n->fds[fd] = NULL;
    if (--s->open == 0)
        sock_close(s);
    pthread_mutex_unlock(&n->lock);
    return 0;
}

int32_t bsd_net_duplicate(bsd_net *n, int32_t fd, int32_t *err)
{
    *err = 0;
    pthread_mutex_lock(&n->lock);
    sock *s = get(n, fd);
    int32_t r = -1;
    if (!s) {
        *err = BSD_EBADF;
    } else if ((r = alloc_fd(n, s)) < 0) {
        *err = BSD_EMFILE;
    } else {
        s->open++;
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

int32_t bsd_net_bind(bsd_net *n, int32_t fd, const uint8_t *sockaddr, size_t len, int32_t *err)
{
    *err = 0;
    bsd_addr a;
    pthread_mutex_lock(&n->lock);
    sock *s = get(n, fd);
    int32_t r = 0;
    if (!s || s->type == TYPE_EVENTFD) {
        r = fail(err, s ? BSD_ENOTSOCK : BSD_EBADF);
    } else if (!addr_parse(sockaddr, len, &a, err)) {
        r = -1;
    } else if (s->bound) {
        r = fail(err, BSD_EINVAL);
    } else if (!is_loopback(a.ip)) {
        r = fail(err, BSD_EADDRNOTAVAIL);
    } else {
        if (a.port == 0)
            a.port = ephemeral(n, s->type);
        else if (port_taken(n, s->type, a.port))
            r = fail(err, BSD_EADDRINUSE);
        if (r == 0) {
            s->bound = true;
            s->local = a;
        }
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

int32_t bsd_net_listen(bsd_net *n, int32_t fd, int32_t backlog, int32_t *err)
{
    *err = 0;
    pthread_mutex_lock(&n->lock);
    sock *s = get(n, fd);
    int32_t r = 0;
    if (!s)
        r = fail(err, BSD_EBADF);
    else if (s->type != SOCK_STREAM)
        r = fail(err, BSD_EOPNOTSUPP);
    else if (s->connected)
        r = fail(err, BSD_EINVAL);
    else {
        autobind(n, s);
        s->listening = true;
        s->backlog = backlog < 1 ? 1 : backlog > MAX_BACKLOG ? MAX_BACKLOG : backlog;
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

static sock *find_listener(bsd_net *n, uint16_t port)
{
    for (int i = 0; i < MAX_FDS; i++) {
        sock *s = n->fds[i];
        if (s && s->listening && s->local.port == port)
            return s;
    }
    return NULL;
}

static sock *find_dgram(bsd_net *n, uint16_t port)
{
    for (int i = 0; i < MAX_FDS; i++) {
        sock *s = n->fds[i];
        if (s && s->type == SOCK_DGRAM && s->bound && s->local.port == port)
            return s;
    }
    return NULL;
}

int32_t bsd_net_connect(bsd_net *n, int32_t fd, const uint8_t *sockaddr, size_t len, int32_t *err)
{
    *err = 0;
    bsd_addr a;
    pthread_mutex_lock(&n->lock);
    sock *s = get(n, fd);
    int32_t r = 0;
    if (!s || s->type == TYPE_EVENTFD) {
        r = fail(err, s ? BSD_ENOTSOCK : BSD_EBADF);
    } else if (!addr_parse(sockaddr, len, &a, err)) {
        r = -1;
    } else if (s->type == SOCK_STREAM && s->connected) {
        r = fail(err, BSD_EISCONN);
    } else if (s->listening) {
        r = fail(err, BSD_EINVAL);
    } else if (!is_loopback(a.ip) || s->type == SOCK_RAW) {
        r = fail(err, BSD_ENETUNREACH);
    } else if (s->type == SOCK_DGRAM) {
        autobind(n, s);
        s->connected = true;
        s->remote = a;
    } else {
        sock *l = find_listener(n, a.port);
        if (!l || l->queued >= l->backlog) {
            r = fail(err, BSD_ECONNREFUSED);
        } else {
            sock *srv = calloc(1, sizeof *srv);
            if (!srv) {
                r = fail(err, BSD_EMFILE);
            } else {
                autobind(n, s);
                srv->open = 1;
                srv->type = SOCK_STREAM;
                srv->bound = true;
                srv->local.ip = LOOPBACK;
                srv->local.port = a.port;
                srv->connected = true;
                srv->remote = s->local;
                srv->peer = s;
                s->peer = srv;
                s->connected = true;
                s->remote.ip = LOOPBACK;
                s->remote.port = a.port;
                l->queue[l->queued++] = srv;
            }
        }
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

/** @brief Waits one step for a blocking call; false when the descriptor is nonblocking. */
static bool wait_step(bsd_net *n, bool nonblock)
{
    if (nonblock)
        return false;
    pthread_mutex_unlock(&n->lock);
    hos_sleep_thread(INT64_C(1000000));
    pthread_mutex_lock(&n->lock);
    return true;
}

int32_t bsd_net_accept(bsd_net *n, int32_t fd, uint8_t peer[16], int32_t *err)
{
    *err = 0;
    pthread_mutex_lock(&n->lock);
    int32_t r = -1;
    for (;;) {
        sock *s = get(n, fd);
        if (!s) {
            *err = BSD_EBADF;
            break;
        }
        if (!s->listening) {
            *err = BSD_EINVAL;
            break;
        }
        if (s->queued > 0) {
            sock *c = s->queue[0];
            int32_t nfd = alloc_fd(n, c);
            if (nfd < 0) {
                *err = BSD_EMFILE;
                break;
            }
            memmove(s->queue, s->queue + 1, (size_t)(s->queued - 1) * sizeof s->queue[0]);
            s->queued--;
            c->open = 1;
            bsd_addr_pack(peer, c->remote);
            r = nfd;
            break;
        }
        if (!wait_step(n, s->nonblock)) {
            *err = BSD_EWOULDBLOCK;
            break;
        }
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

static int32_t dgram_send(bsd_net *n, sock *s, const void *data, size_t len, bsd_addr dest, int32_t *err)
{
    if (len > DGRAM_MAX)
        return fail(err, BSD_EMSGSIZE);
    if (!is_loopback(dest.ip))
        return fail(err, BSD_ENETUNREACH);
    autobind(n, s);
    sock *t = find_dgram(n, dest.port);
    if (t && t->ndgrams < DGRAM_QUEUE) {
        uint8_t *copy = malloc(len ? len : 1);
        if (!copy)
            return fail(err, BSD_EMFILE);
        memcpy(copy, data, len);
        t->dgrams[t->ndgrams].from = s->local;
        t->dgrams[t->ndgrams].len = len;
        t->dgrams[t->ndgrams].data = copy;
        t->ndgrams++;
    }
    return (int32_t)len;
}

int32_t bsd_net_send(bsd_net *n, int32_t fd, const void *data, size_t len, const uint8_t *sockaddr, size_t alen,
                     int32_t *err)
{
    *err = 0;
    pthread_mutex_lock(&n->lock);
    sock *s = get(n, fd);
    int32_t r = -1;
    bsd_addr dest;
    if (!s || s->type == TYPE_EVENTFD) {
        *err = s ? BSD_ENOTSOCK : BSD_EBADF;
    } else if (s->type == SOCK_RAW) {
        *err = BSD_ENETUNREACH;
    } else if (s->type == SOCK_DGRAM) {
        if (sockaddr) {
            if (addr_parse(sockaddr, alen, &dest, err))
                r = dgram_send(n, s, data, len, dest, err);
        } else if (s->connected) {
            r = dgram_send(n, s, data, len, s->remote, err);
        } else {
            *err = BSD_EDESTADDRREQ;
        }
    } else if (!s->connected) {
        *err = BSD_ENOTCONN;
    } else {
        size_t sent = 0;
        for (;;) {
            s = get(n, fd);
            if (!s) {
                r = -1;
                *err = BSD_EBADF;
                break;
            }
            if (s->shut_wr || s->peer_gone || !s->peer) {
                r = sent ? (int32_t)sent : -1;
                if (!sent)
                    *err = BSD_EPIPE;
                break;
            }
            sock *p = s->peer;
            size_t room = STREAM_CAP - p->len;
            size_t take = len - sent < room ? len - sent : room;
            if (take) {
                uint8_t *nb = realloc(p->buf, p->len + take);
                if (!nb) {
                    *err = BSD_EMFILE;
                    break;
                }
                p->buf = nb;
                memcpy(p->buf + p->len, (const uint8_t *)data + sent, take);
                p->len += take;
                sent += take;
            }
            if (sent == len) {
                r = (int32_t)sent;
                break;
            }
            if (!wait_step(n, s->nonblock)) {
                r = sent ? (int32_t)sent : -1;
                if (!sent)
                    *err = BSD_EWOULDBLOCK;
                break;
            }
        }
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

int32_t bsd_net_recv(bsd_net *n, int32_t fd, void *data, size_t cap, uint32_t flags, uint8_t from[16], int32_t *err)
{
    *err = 0;
    pthread_mutex_lock(&n->lock);
    int32_t r = -1;
    for (;;) {
        sock *s = get(n, fd);
        if (!s) {
            *err = BSD_EBADF;
            break;
        }
        bool nonblock = s->nonblock || (flags & BSD_MSG_DONTWAIT);
        if (s->type == TYPE_EVENTFD) {
            if (cap < 8) {
                *err = BSD_EINVAL;
                break;
            }
            if (s->counter) {
                uint64_t v = s->semaphore ? 1 : s->counter;
                s->counter -= v;
                memcpy(data, &v, 8);
                r = 8;
                break;
            }
        } else if (s->type == SOCK_DGRAM) {
            if (s->ndgrams) {
                msg m = s->dgrams[0];
                size_t take = m.len < cap ? m.len : cap;
                memcpy(data, m.data, take);
                if (from)
                    bsd_addr_pack(from, m.from);
                if (!(flags & BSD_MSG_PEEK)) {
                    memmove(s->dgrams, s->dgrams + 1, (size_t)(s->ndgrams - 1) * sizeof s->dgrams[0]);
                    s->ndgrams--;
                    free(m.data);
                }
                r = (int32_t)take;
                break;
            }
        } else if (s->type == SOCK_STREAM) {
            if (!s->connected) {
                *err = BSD_ENOTCONN;
                break;
            }
            if (s->len) {
                size_t take = s->len < cap ? s->len : cap;
                memcpy(data, s->buf, take);
                if (from)
                    bsd_addr_pack(from, s->remote);
                if (!(flags & BSD_MSG_PEEK)) {
                    memmove(s->buf, s->buf + take, s->len - take);
                    s->len -= take;
                }
                r = (int32_t)take;
                break;
            }
            if (s->eof_in) {
                r = 0;
                break;
            }
        } else {
            *err = BSD_ENOTCONN;
            break;
        }
        if (nonblock) {
            *err = BSD_EWOULDBLOCK;
            break;
        }
        wait_step(n, false);
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

int32_t bsd_net_name(bsd_net *n, int32_t fd, bool peer, uint8_t out[16], int32_t *err)
{
    *err = 0;
    pthread_mutex_lock(&n->lock);
    sock *s = get(n, fd);
    int32_t r = 0;
    if (!s || s->type == TYPE_EVENTFD) {
        r = fail(err, s ? BSD_ENOTSOCK : BSD_EBADF);
    } else if (peer) {
        if (!s->connected)
            r = fail(err, BSD_ENOTCONN);
        else
            bsd_addr_pack(out, s->remote);
    } else {
        bsd_addr_pack(out, s->bound ? s->local : (bsd_addr){ 0, 0 });
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

int32_t bsd_net_getsockopt(bsd_net *n, int32_t fd, uint32_t level, uint32_t name, void *out, size_t cap, uint32_t *len,
                           int32_t *err)
{
    *err = 0;
    pthread_mutex_lock(&n->lock);
    sock *s = get(n, fd);
    int32_t r = 0;
    if (!s) {
        r = fail(err, BSD_EBADF);
    } else {
        int32_t v = 0;
        int i = lookup_opt(s, level, name);
        if (i >= 0)
            v = s->opts[i].value;
        else if (level == SOL_SOCKET && name == SO_TYPE)
            v = s->type == TYPE_EVENTFD ? 0 : s->type;
        else if (level == SOL_SOCKET && (name == SO_SNDBUF || name == SO_RCVBUF))
            v = 65536;
        else if (level == SOL_SOCKET && name == SO_ERROR)
            v = 0;
        size_t take = cap < 4 ? cap : 4;
        memcpy(out, &v, take);
        *len = (uint32_t)take;
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

int32_t bsd_net_setsockopt(bsd_net *n, int32_t fd, uint32_t level, uint32_t name, const void *val, size_t len,
                           int32_t *err)
{
    *err = 0;
    pthread_mutex_lock(&n->lock);
    sock *s = get(n, fd);
    int32_t r = 0;
    if (!s) {
        r = fail(err, BSD_EBADF);
    } else {
        int32_t v = 0;
        memcpy(&v, val, len < 4 ? len : 4);
        int i = lookup_opt(s, level, name);
        if (i < 0)
            for (i = 0; i < MAX_OPTS && s->opts[i].set; i++) {
            }
        if (i < MAX_OPTS) {
            s->opts[i].set = true;
            s->opts[i].level = level;
            s->opts[i].name = name;
            s->opts[i].value = v;
        }
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

int32_t bsd_net_fcntl(bsd_net *n, int32_t fd, int32_t cmd, int32_t arg, int32_t *err)
{
    *err = 0;
    pthread_mutex_lock(&n->lock);
    sock *s = get(n, fd);
    int32_t r = 0;
    if (!s) {
        r = fail(err, BSD_EBADF);
    } else if (cmd == F_GETFL) {
        r = O_RDWR | (s->nonblock ? FD_NONBLOCK : 0);
    } else if (cmd == F_SETFL) {
        s->nonblock = (arg & FD_NONBLOCK) != 0;
    } else if (cmd != F_GETFD && cmd != F_SETFD) {
        r = fail(err, BSD_EINVAL);
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

int32_t bsd_net_shutdown(bsd_net *n, int32_t fd, int32_t how, int32_t *err)
{
    *err = 0;
    pthread_mutex_lock(&n->lock);
    sock *s = get(n, fd);
    int32_t r = 0;
    if (!s || s->type == TYPE_EVENTFD) {
        r = fail(err, s ? BSD_ENOTSOCK : BSD_EBADF);
    } else if (how < 0 || how > 2) {
        r = fail(err, BSD_EINVAL);
    } else if (s->type == SOCK_STREAM && !s->connected) {
        r = fail(err, BSD_ENOTCONN);
    } else {
        if (how == 0 || how == 2)
            s->eof_in = true;
        if ((how == 1 || how == 2) && s->type == SOCK_STREAM) {
            s->shut_wr = true;
            if (s->peer)
                s->peer->eof_in = true;
        }
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

uint16_t bsd_net_revents(bsd_net *n, int32_t fd, uint16_t events)
{
    pthread_mutex_lock(&n->lock);
    sock *s = get(n, fd);
    uint16_t rev = 0;
    if (!s) {
        rev = BSD_POLLNVAL;
    } else if (s->type == TYPE_EVENTFD) {
        if (s->counter)
            rev |= BSD_POLLIN;
        rev |= BSD_POLLOUT;
    } else if (s->type == SOCK_DGRAM) {
        if (s->ndgrams)
            rev |= BSD_POLLIN;
        rev |= BSD_POLLOUT;
    } else if (s->type == SOCK_STREAM) {
        if (s->listening) {
            if (s->queued)
                rev |= BSD_POLLIN;
        } else if (s->connected) {
            if (s->len || s->eof_in)
                rev |= BSD_POLLIN;
            if (s->peer_gone)
                rev |= BSD_POLLHUP;
            else if (s->peer && s->peer->len < STREAM_CAP && !s->shut_wr)
                rev |= BSD_POLLOUT;
        }
    }
    pthread_mutex_unlock(&n->lock);
    return rev & (uint16_t)(events | BSD_POLLERR | BSD_POLLHUP | BSD_POLLNVAL);
}
