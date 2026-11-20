/**
 * @file test_srv_bsd.c
 * @brief The socket services as nn::socket uses them on a console without a
 * network: sockets are created and configured, sockets of the program talk
 * over the in-process loopback, and every other address is unreachable.
 */
#include "tc.h"

#include <string.h>

enum { CMD_SOCKET = 2, CMD_POLL = 6, CMD_RECV = 8, CMD_RECVFROM = 9, CMD_SEND = 10, CMD_SENDTO = 11, CMD_ACCEPT = 12,
       CMD_BIND = 13, CMD_CONNECT = 14, CMD_PEERNAME = 15, CMD_SOCKNAME = 16, CMD_GETOPT = 17, CMD_LISTEN = 18,
       CMD_FCNTL = 20, CMD_SETOPT = 21, CMD_SHUTDOWN = 22, CMD_CLOSE = 26, CMD_DUP = 27, CMD_EVENTFD = 31 };

enum { E_BADF = 9, E_PIPE = 32, E_WOULDBLOCK = 35, E_DESTADDRREQ = 39, E_AFNOSUPPORT = 47, E_ADDRINUSE = 48,
       E_NETUNREACH = 51, E_NOTCONN = 57, E_CONNREFUSED = 61 };

typedef struct fx {
    rl_ctx *c;
    tc_obj s;
    uint64_t heap;
} fx;

static fx fx_open(const char *name)
{
    fx f;
    f.c = t_boot(NULL);
    f.heap = t_heap(f.c, 0x200000);
    f.s = tc_open_service(f.c, f.heap, name);
    return f;
}

#define A_BUF(f) ((f).heap + 0x100000)
#define A_BUF2(f) ((f).heap + 0x101000)
#define B_BUF(f) ((f).heap + 0x102000)
#define B_BUF2(f) ((f).heap + 0x103000)

static void put_addr(fx f, uint64_t at, uint32_t ip, uint16_t port)
{
    uint8_t *p = t_mem() + at;
    memset(p, 0, 16);
    p[0] = 16;
    p[1] = 2;
    p[2] = (uint8_t)(port >> 8);
    p[3] = (uint8_t)port;
    p[4] = (uint8_t)(ip >> 24);
    p[5] = (uint8_t)(ip >> 16);
    p[6] = (uint8_t)(ip >> 8);
    p[7] = (uint8_t)ip;
    (void)f;
}

static tc_res call(fx f, tc_req q)
{
    tc_res r = tc_send(f.c, f.s, q);
    if (r.result)
        memset(r.data, 0xee, sizeof r.data);
    return r;
}

static int32_t ret_of(const tc_res *r) { return (int32_t)tc_u32(r, 0); }

static int32_t errno_of(const tc_res *r) { return (int32_t)tc_u32(r, 4); }

static int32_t sock_new(fx f, uint32_t type)
{
    tc_req q = tc_new(CMD_SOCKET);
    tc_put32(&q, 2);
    tc_put32(&q, type);
    tc_put32(&q, 0);
    tc_res r = call(f, q);
    return errno_of(&r) == 0 ? ret_of(&r) : -1000;
}

static tc_res op_fd(fx f, uint32_t cmd, int32_t fd, int32_t arg)
{
    tc_req q = tc_new(cmd);
    tc_put32(&q, (uint32_t)fd);
    tc_put32(&q, (uint32_t)arg);
    return call(f, q);
}

static tc_res op_addr(fx f, uint32_t cmd, int32_t fd, uint32_t ip, uint16_t port)
{
    put_addr(f, A_BUF(f), ip, port);
    tc_req q = tc_new(cmd);
    tc_put32(&q, (uint32_t)fd);
    q.na = 1;
    q.a_addr[0] = A_BUF(f);
    q.a_size[0] = 16;
    return call(f, q);
}

static tc_res op_send(fx f, int32_t fd, const char *text)
{
    memcpy(t_mem() + A_BUF(f), text, strlen(text));
    tc_req q = tc_new(CMD_SEND);
    tc_put32(&q, (uint32_t)fd);
    tc_put32(&q, 0);
    q.na = 1;
    q.a_addr[0] = A_BUF(f);
    q.a_size[0] = strlen(text);
    return call(f, q);
}

static tc_res op_recv(fx f, int32_t fd, uint32_t cap, uint32_t flags)
{
    tc_req q = tc_new(CMD_RECV);
    tc_put32(&q, (uint32_t)fd);
    tc_put32(&q, flags);
    q.nb = 1;
    q.b_addr[0] = B_BUF(f);
    q.b_size[0] = cap;
    return call(f, q);
}

static tc_res poll1(fx f, int32_t fd, uint16_t events, int32_t timeout, uint16_t *revents)
{
    uint32_t *in = (uint32_t *)(void *)(t_mem() + A_BUF(f));
    in[0] = (uint32_t)fd;
    in[1] = events;
    tc_req q = tc_new(CMD_POLL);
    tc_put32(&q, 1);
    tc_put32(&q, (uint32_t)timeout);
    q.na = 1;
    q.a_addr[0] = A_BUF(f);
    q.a_size[0] = 8;
    q.nb = 1;
    q.b_addr[0] = B_BUF(f);
    q.b_size[0] = 8;
    tc_res r = call(f, q);
    *revents = (uint16_t)(((uint32_t *)(void *)(t_mem() + B_BUF(f)))[1] >> 16);
    return r;
}

#define LOOPBACK 0x7f000001u
#define NONBLOCK 4

TEST(bsd_registers_with_either_command)
{
    fx f = fx_open("bsd:s");
    CHECK(f.s.handle != 0);
    tc_req q = tc_new(0);
    for (unsigned i = 0; i < 8; i++)
        tc_put32(&q, 0);
    tc_put64(&q, 0);
    tc_put64(&q, 0x10000);
    tc_res r = call(f, q);
    CHECK_EQ(tc_u32(&r, 0), 0);
    q = tc_new(36);
    tc_put32(&q, 5);
    tc_put32(&q, 0);
    tc_put64(&q, 0);
    r = call(f, q);
    CHECK_EQ(tc_u32(&r, 0), 0);
    CHECK(tc_open_service(f.c, f.heap, "bsd:u").handle != 0);
}

TEST(bsd_sockets_are_created_numbered_and_closed)
{
    fx f = fx_open("bsd:u");
    int32_t a = sock_new(f, 1);
    int32_t b = sock_new(f, 2);
    CHECK_EQ(b, a + 1);
    tc_req q = tc_new(CMD_SOCKET);
    tc_put32(&q, 30);
    tc_put32(&q, 1);
    tc_put32(&q, 0);
    tc_res r = call(f, q);
    CHECK_EQ(ret_of(&r), -1);
    CHECK_EQ(errno_of(&r), E_AFNOSUPPORT);
    r = op_fd(f, CMD_CLOSE, a, 0);
    CHECK_EQ(ret_of(&r), 0);
    r = op_fd(f, CMD_CLOSE, a, 0);
    CHECK_EQ(errno_of(&r), E_BADF);
    CHECK_EQ(sock_new(f, 1), a);
}

TEST(bsd_other_addresses_are_unreachable)
{
    fx f = fx_open("bsd:u");
    int32_t s = sock_new(f, 1);
    tc_res r = op_addr(f, CMD_CONNECT, s, 0x08080808u, 443);
    CHECK_EQ(ret_of(&r), -1);
    CHECK_EQ(errno_of(&r), E_NETUNREACH);
    int32_t d = sock_new(f, 2);
    r = op_addr(f, CMD_CONNECT, d, 0xc0a80101u, 53);
    CHECK_EQ(errno_of(&r), E_NETUNREACH);
    r = op_send(f, s, "x");
    CHECK_EQ(errno_of(&r), E_NOTCONN);
    r = op_addr(f, CMD_CONNECT, s, LOOPBACK, 9);
    CHECK_EQ(errno_of(&r), E_CONNREFUSED);
}

TEST(bsd_stream_sockets_talk_over_loopback)
{
    fx f = fx_open("bsd:u");
    int32_t l = sock_new(f, 1);
    tc_res r = op_addr(f, CMD_BIND, l, LOOPBACK, 4000);
    CHECK_EQ(ret_of(&r), 0);
    r = op_addr(f, CMD_BIND, sock_new(f, 1), LOOPBACK, 4000);
    CHECK_EQ(errno_of(&r), E_ADDRINUSE);
    r = op_fd(f, CMD_LISTEN, l, 4);
    CHECK_EQ(ret_of(&r), 0);
    uint16_t rev;
    poll1(f, l, 1, 0, &rev);
    CHECK_EQ(rev, 0);
    r = op_fd(f, CMD_FCNTL, l, 3);
    r = op_fd(f, CMD_FCNTL, l, 4);
    tc_req q = tc_new(CMD_FCNTL);
    tc_put32(&q, (uint32_t)l);
    tc_put32(&q, 4);
    tc_put32(&q, NONBLOCK);
    call(f, q);
    q = tc_new(CMD_ACCEPT);
    tc_put32(&q, (uint32_t)l);
    q.nb = 1;
    q.b_addr[0] = B_BUF(f);
    q.b_size[0] = 16;
    r = call(f, q);
    CHECK_EQ(ret_of(&r), -1);
    CHECK_EQ(errno_of(&r), E_WOULDBLOCK);

    int32_t c = sock_new(f, 1);
    r = op_addr(f, CMD_CONNECT, c, LOOPBACK, 4000);
    CHECK_EQ(ret_of(&r), 0);
    r = op_addr(f, CMD_CONNECT, c, LOOPBACK, 4000);
    CHECK_EQ(errno_of(&r), 56);
    poll1(f, l, 1, 0, &rev);
    CHECK_EQ(rev, 1);
    q = tc_new(CMD_ACCEPT);
    tc_put32(&q, (uint32_t)l);
    q.nb = 1;
    q.b_addr[0] = B_BUF(f);
    q.b_size[0] = 16;
    r = call(f, q);
    int32_t a = ret_of(&r);
    CHECK(a >= 0);
    CHECK_EQ(tc_u32(&r, 8), 16);
    CHECK_EQ(t_mem()[B_BUF(f) + 4], 127);

    r = op_send(f, c, "hello");
    CHECK_EQ(ret_of(&r), 5);
    poll1(f, a, 1, 0, &rev);
    CHECK_EQ(rev, 1);
    r = op_recv(f, a, 64, 2);
    CHECK_EQ(ret_of(&r), 5);
    r = op_recv(f, a, 64, 0);
    CHECK_EQ(ret_of(&r), 5);
    CHECK_EQ(memcmp(t_mem() + B_BUF(f), "hello", 5), 0);
    poll1(f, a, 1, 0, &rev);
    CHECK_EQ(rev, 0);
    poll1(f, a, 4, 0, &rev);
    CHECK_EQ(rev, 4);
    r = op_fd(f, CMD_SHUTDOWN, c, 1);
    CHECK_EQ(ret_of(&r), 0);
    r = op_recv(f, a, 64, 0);
    CHECK_EQ(ret_of(&r), 0);
    r = op_send(f, c, "late");
    CHECK_EQ(errno_of(&r), E_PIPE);
    r = op_fd(f, CMD_CLOSE, c, 0);
    r = op_send(f, a, "gone");
    CHECK_EQ(errno_of(&r), E_PIPE);
}

TEST(bsd_datagram_sockets_talk_over_loopback)
{
    fx f = fx_open("bsd:u");
    int32_t a = sock_new(f, 2);
    int32_t b = sock_new(f, 2);
    op_addr(f, CMD_BIND, a, LOOPBACK, 5000);
    memcpy(t_mem() + A_BUF(f), "ping", 4);
    put_addr(f, A_BUF2(f), LOOPBACK, 5000);
    tc_req q = tc_new(CMD_SENDTO);
    tc_put32(&q, (uint32_t)b);
    tc_put32(&q, 0);
    q.na = 2;
    q.a_addr[0] = A_BUF(f);
    q.a_size[0] = 4;
    q.a_addr[1] = A_BUF2(f);
    q.a_size[1] = 16;
    tc_res r = call(f, q);
    CHECK_EQ(ret_of(&r), 4);
    uint16_t rev;
    poll1(f, a, 1, 0, &rev);
    CHECK_EQ(rev, 1);
    q = tc_new(CMD_RECVFROM);
    tc_put32(&q, (uint32_t)a);
    tc_put32(&q, 0);
    q.nb = 2;
    q.b_addr[0] = B_BUF(f);
    q.b_size[0] = 64;
    q.b_addr[1] = B_BUF2(f);
    q.b_size[1] = 16;
    r = call(f, q);
    CHECK_EQ(ret_of(&r), 4);
    CHECK_EQ(tc_u32(&r, 8), 16);
    CHECK_EQ(memcmp(t_mem() + B_BUF(f), "ping", 4), 0);
    CHECK_EQ(t_mem()[B_BUF2(f) + 4], 127);
    r = op_send(f, b, "x");
    CHECK_EQ(errno_of(&r), E_DESTADDRREQ);
}

TEST(bsd_options_names_and_duplicates)
{
    fx f = fx_open("bsd:u");
    int32_t s = sock_new(f, 1);
    uint32_t v = 1;
    memcpy(t_mem() + A_BUF(f), &v, 4);
    tc_req q = tc_new(CMD_SETOPT);
    tc_put32(&q, (uint32_t)s);
    tc_put32(&q, 0xffff);
    tc_put32(&q, 4);
    q.na = 1;
    q.a_addr[0] = A_BUF(f);
    q.a_size[0] = 4;
    tc_res r = call(f, q);
    CHECK_EQ(ret_of(&r), 0);
    q = tc_new(CMD_GETOPT);
    tc_put32(&q, (uint32_t)s);
    tc_put32(&q, 0xffff);
    tc_put32(&q, 0x1008);
    q.nb = 1;
    q.b_addr[0] = B_BUF(f);
    q.b_size[0] = 4;
    r = call(f, q);
    CHECK_EQ(ret_of(&r), 0);
    CHECK_EQ(tc_u32(&r, 8), 4);
    CHECK_EQ(*(uint32_t *)(void *)(t_mem() + B_BUF(f)), 1);
    r = op_fd(f, CMD_SOCKNAME, s, 0);
    q = tc_new(CMD_PEERNAME);
    tc_put32(&q, (uint32_t)s);
    q.nb = 1;
    q.b_addr[0] = B_BUF(f);
    q.b_size[0] = 16;
    r = call(f, q);
    CHECK_EQ(errno_of(&r), E_NOTCONN);
    q = tc_new(CMD_DUP);
    tc_put32(&q, (uint32_t)s);
    tc_put32(&q, 0);
    tc_put64(&q, 0);
    r = call(f, q);
    int32_t d = ret_of(&r);
    CHECK(d != s && d >= 0);
    op_fd(f, CMD_CLOSE, s, 0);
    r = op_fd(f, CMD_SHUTDOWN, d, 2);
    CHECK_EQ(errno_of(&r), E_NOTCONN);
}

TEST(bsd_eventfd_counts)
{
    fx f = fx_open("bsd:u");
    tc_req q = tc_new(CMD_EVENTFD);
    tc_put32(&q, NONBLOCK);
    tc_put32(&q, 0);
    tc_put64(&q, 3);
    tc_res r = call(f, q);
    int32_t e = ret_of(&r);
    CHECK(e >= 0);
    uint16_t rev;
    poll1(f, e, 1, 0, &rev);
    CHECK_EQ(rev, 1);
    r = op_recv(f, e, 8, 0);
    CHECK_EQ(ret_of(&r), 8);
    CHECK_EQ(*(uint64_t *)(void *)(t_mem() + B_BUF(f)), 3);
    r = op_recv(f, e, 8, 0);
    CHECK_EQ(errno_of(&r), E_WOULDBLOCK);
}

TEST(bsd_poll_reports_what_is_ready_and_waits_out_the_timeout)
{
    fx f = fx_open("bsd:u");
    int32_t s = sock_new(f, 1);
    uint16_t rev;
    tc_res r = poll1(f, s, 1, 2, &rev);
    CHECK_EQ(ret_of(&r), 0);
    CHECK_EQ(rev, 0);
    r = poll1(f, 99, 1, 0, &rev);
    CHECK_EQ(ret_of(&r), 1);
    CHECK_EQ(rev, 0x20);
    r = poll1(f, -1, 1, 0, &rev);
    CHECK_EQ(ret_of(&r), 0);
    tc_req q = tc_new(CMD_POLL);
    tc_put32(&q, 0);
    tc_put32(&q, 1);
    r = call(f, q);
    CHECK_EQ(ret_of(&r), 0);
    CHECK_EQ(errno_of(&r), 0);
}

TEST(bsd_select_reports_ready_sets)
{
    fx f = fx_open("bsd:u");
    int32_t l = sock_new(f, 1);
    op_addr(f, CMD_BIND, l, LOOPBACK, 4100);
    op_fd(f, CMD_LISTEN, l, 2);
    int32_t c = sock_new(f, 1);
    op_addr(f, CMD_CONNECT, c, LOOPBACK, 4100);
    uint64_t *rd = (uint64_t *)(void *)(t_mem() + A_BUF(f));
    uint64_t *wr = (uint64_t *)(void *)(t_mem() + A_BUF2(f));
    rd[0] = UINT64_C(1) << l;
    wr[0] = UINT64_C(1) << c;
    tc_req q = tc_new(5);
    tc_put32(&q, 16);
    tc_put32(&q, 0);
    tc_put64(&q, 0);
    tc_put64(&q, 0);
    tc_put32(&q, 0);
    q.na = 2;
    q.a_addr[0] = A_BUF(f);
    q.a_size[0] = 8;
    q.a_addr[1] = A_BUF2(f);
    q.a_size[1] = 8;
    q.nb = 2;
    q.b_addr[0] = A_BUF(f);
    q.b_size[0] = 8;
    q.b_addr[1] = A_BUF2(f);
    q.b_size[1] = 8;
    tc_res r = call(f, q);
    CHECK_EQ(ret_of(&r), 2);
    CHECK_EQ(rd[0], UINT64_C(1) << l);
    CHECK_EQ(wr[0], UINT64_C(1) << c);
    int32_t idle = sock_new(f, 1);
    rd[0] = UINT64_C(1) << idle;
    q.a_size[1] = 0;
    q.b_size[1] = 0;
    q.nb = 1;
    r = call(f, q);
    CHECK_EQ(ret_of(&r), 0);
    CHECK_EQ(rd[0], 0);
}

TEST(bsd_ioctl_sets_nonblocking_and_counts_pending_bytes)
{
    fx f = fx_open("bsd:u");
    int32_t a = sock_new(f, 2);
    int32_t b = sock_new(f, 2);
    op_addr(f, CMD_BIND, a, LOOPBACK, 5100);
    int32_t on = 1;
    memcpy(t_mem() + A_BUF(f), &on, 4);
    tc_req q = tc_new(19);
    tc_put32(&q, (uint32_t)a);
    tc_put32(&q, 0x8004667eu);
    tc_put32(&q, 1);
    q.na = 1;
    q.a_addr[0] = A_BUF(f);
    q.a_size[0] = 4;
    tc_res r = call(f, q);
    CHECK_EQ(ret_of(&r), 0);
    r = op_recv(f, a, 8, 0);
    CHECK_EQ(errno_of(&r), E_WOULDBLOCK);
    memcpy(t_mem() + A_BUF(f), "abc", 3);
    put_addr(f, A_BUF2(f), LOOPBACK, 5100);
    q = tc_new(CMD_SENDTO);
    tc_put32(&q, (uint32_t)b);
    tc_put32(&q, 0);
    q.na = 2;
    q.a_addr[0] = A_BUF(f);
    q.a_size[0] = 3;
    q.a_addr[1] = A_BUF2(f);
    q.a_size[1] = 16;
    call(f, q);
    q = tc_new(19);
    tc_put32(&q, (uint32_t)a);
    tc_put32(&q, 0x4004667fu);
    tc_put32(&q, 1);
    q.nb = 1;
    q.b_addr[0] = B_BUF(f);
    q.b_size[0] = 4;
    r = call(f, q);
    CHECK_EQ(ret_of(&r), 0);
    CHECK_EQ(*(int32_t *)(void *)(t_mem() + B_BUF(f)), 3);
    q = tc_new(19);
    tc_put32(&q, (uint32_t)a);
    tc_put32(&q, 0x1234u);
    tc_put32(&q, 0);
    r = call(f, q);
    CHECK_EQ(errno_of(&r), 25);
}
