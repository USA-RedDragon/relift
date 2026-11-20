/**
 * @file srv_bsd.c
 * @brief The socket services bsd:u and bsd:s (nn::socket::sf::IClient) of a
 * console without a network: a client registers, creates sockets and
 * configures them, sockets of the program talk over an in-process loopback
 * (bsd_net.c), and every other address is unreachable. Commands, request
 * layouts and reply words follow the SDK's own client stubs (nn::socket::sf::
 * IClient proxy), switchbrew.org (Sockets services) and Ryujinx (MIT,
 * Copyright (c) Ryujinx Team and Contributors) Services/Sockets/Bsd IClient;
 * the errno values are the console's (FreeBSD). Command 36 is the Switch 2
 * SDK's replacement for RegisterClient: a u32 version, 4 bytes of padding and
 * a u64 process id placeholder; a u32 bsd errno, which the SDK demands be 0.
 */
#include "bsd_net.h"
#include "hos.h"
#include "services.h"

#include <stdlib.h>
#include <string.h>

/** @brief Most descriptors one Poll request carries. */
#define BSD_POLL_MAX 0x10000
/** @brief Largest data one Send, Recv or similar request moves through a service buffer. */
#define BSD_IO_MAX (1u << 20)

static bsd_net *net_of(rl_ipc_req *r) { return r->self->state; }

/** @brief Writes the common reply: i32 ret, i32 errno. */
static rl_result reply(rl_ipc_req *r, int32_t ret, int32_t err)
{
    int32_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    out[0] = ret;
    out[1] = err;
    return KR_SUCCESS;
}

/** @brief Writes a reply of ret, errno and one more u32. */
static rl_result reply3(rl_ipc_req *r, int32_t ret, int32_t err, uint32_t extra)
{
    int32_t *out = rl_ipc_out(r, 12);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    out[0] = ret;
    out[1] = err;
    out[2] = (int32_t)extra;
    return KR_SUCCESS;
}

/** @brief RegisterClient (0): BsdBufferConfig, u64 pid, u64 transfer memory size, the transfer memory; u32 bsd errno. */
static rl_result register_client(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

/** @brief RegisterClientSwitch2 (36): u32 version, padding, u64 pid placeholder (the pid travels in the message); u32 bsd errno 0. */
static rl_result register_client_switch2(rl_ipc_req *r)
{
    struct {
        uint32_t version;
        uint32_t pad;
        uint64_t pid;
    } in;
    if (!rl_srv_in(r, &in, 16))
        return SF_RESULT_INVALID_HEADER;
    return register_client(r);
}

/** @brief Socket (2) and SocketExempt (3): u32 domain, type, protocol; i32 descriptor and errno. */
static rl_result socket_create(rl_ipc_req *r)
{
    uint32_t in[3];
    if (!rl_srv_in(r, in, 12))
        return SF_RESULT_INVALID_HEADER;
    int32_t err;
    int32_t fd = bsd_net_socket(net_of(r), in[0], in[1], in[2], &err);
    return reply(r, fd, err);
}

/** @brief Poll (6): u32 nfds, i32 timeout in ms; the pollfd array (u32 fd, u16 events, u16 revents) as send and receive buffers; i32 ready count and errno. */
static rl_result poll_fds(rl_ipc_req *r)
{
    struct {
        uint32_t nfds;
        int32_t timeout;
    } in;
    if (!rl_srv_in(r, &in, 8) || in.nfds > BSD_POLL_MAX)
        return SF_RESULT_INVALID_HEADER;
    uint64_t size = (uint64_t)in.nfds * 8;
    uint32_t *fds = malloc(size ? size : 1);
    if (!fds)
        return KR_OUT_OF_RESOURCE;
    rl_ipc_buf src = rl_ipc_auto_in(r, 0);
    if (size && !rl_ipc_buf_read(&src, fds, size)) {
        free(fds);
        return SF_RESULT_INVALID_HEADER;
    }
    int64_t waited = 0;
    int32_t ready = 0;
    for (;;) {
        ready = 0;
        for (uint32_t i = 0; i < in.nfds; i++) {
            uint32_t events = fds[i * 2 + 1] & 0xffffu;
            uint16_t rev = 0;
            if ((int32_t)fds[i * 2] >= 0)
                rev = bsd_net_revents(net_of(r), (int32_t)fds[i * 2], (uint16_t)events);
            fds[i * 2 + 1] = events | ((uint32_t)rev << 16);
            if (rev)
                ready++;
        }
        if (ready || in.timeout == 0 || (in.timeout > 0 && waited >= (int64_t)in.timeout))
            break;
        hos_sleep_thread(INT64_C(1000000));
        waited++;
    }
    bool wrote = true;
    if (size) {
        rl_ipc_buf dst = rl_ipc_auto_out(r, 0);
        wrote = rl_ipc_buf_write(&dst, fds, size);
    }
    free(fds);
    if (!wrote)
        return SF_RESULT_INVALID_HEADER;
    return reply(r, ready, 0);
}

/** @brief Reads a whole send buffer (index i) into a fresh allocation. */
static uint8_t *take_in(rl_ipc_req *r, uint32_t i, size_t *len)
{
    rl_ipc_buf b = rl_ipc_auto_in(r, i);
    *len = (size_t)b.size;
    if (b.size > BSD_IO_MAX)
        return NULL;
    uint8_t *p = malloc(b.size ? b.size : 1);
    if (p && b.size && !rl_ipc_buf_read(&b, p, b.size)) {
        free(p);
        return NULL;
    }
    return p;
}

/** @brief Bind (13) and Connect (14): i32 fd; the sockaddr as a send buffer; i32 ret and errno. */
static rl_result bind_or_connect(rl_ipc_req *r, bool connect)
{
    int32_t fd;
    size_t len;
    if (!rl_srv_in(r, &fd, 4))
        return SF_RESULT_INVALID_HEADER;
    uint8_t *sa = take_in(r, 0, &len);
    if (!sa)
        return SF_RESULT_INVALID_HEADER;
    int32_t err;
    int32_t ret = connect ? bsd_net_connect(net_of(r), fd, sa, len, &err) : bsd_net_bind(net_of(r), fd, sa, len, &err);
    free(sa);
    return reply(r, ret, err);
}

static rl_result do_bind(rl_ipc_req *r) { return bind_or_connect(r, false); }

static rl_result do_connect(rl_ipc_req *r) { return bind_or_connect(r, true); }

/** @brief Listen (18): i32 fd, i32 backlog; i32 ret and errno. */
static rl_result do_listen(rl_ipc_req *r)
{
    int32_t in[2];
    if (!rl_srv_in(r, in, 8))
        return SF_RESULT_INVALID_HEADER;
    int32_t err;
    int32_t ret = bsd_net_listen(net_of(r), in[0], in[1], &err);
    return reply(r, ret, err);
}

/** @brief Accept (12): i32 fd; the peer sockaddr as a receive buffer; i32 ret, errno and u32 address length. */
static rl_result do_accept(rl_ipc_req *r)
{
    int32_t fd;
    if (!rl_srv_in(r, &fd, 4))
        return SF_RESULT_INVALID_HEADER;
    uint8_t peer[16];
    int32_t err;
    int32_t ret = bsd_net_accept(net_of(r), fd, peer, &err);
    uint32_t alen = 0;
    if (ret >= 0) {
        rl_ipc_buf b = rl_ipc_auto_out(r, 0);
        alen = b.size < 16 ? (uint32_t)b.size : 16;
        if (alen && !rl_ipc_buf_write(&b, peer, alen))
            return SF_RESULT_INVALID_HEADER;
    }
    return reply3(r, ret, err, alen);
}

/** @brief Send (10), Write (24): i32 fd, (i32 flags for Send); the data as a send buffer; i32 ret and errno. */
static rl_result do_send(rl_ipc_req *r)
{
    int32_t fd;
    size_t len;
    if (!rl_srv_in(r, &fd, 4))
        return SF_RESULT_INVALID_HEADER;
    uint8_t *data = take_in(r, 0, &len);
    if (!data)
        return SF_RESULT_INVALID_HEADER;
    int32_t err;
    int32_t ret = bsd_net_send(net_of(r), fd, data, len, NULL, 0, &err);
    free(data);
    return reply(r, ret, err);
}

/** @brief SendTo (11): i32 fd, i32 flags; the data and the sockaddr as send buffers; i32 ret and errno. */
static rl_result do_sendto(rl_ipc_req *r)
{
    int32_t fd;
    size_t len, alen;
    if (!rl_srv_in(r, &fd, 4))
        return SF_RESULT_INVALID_HEADER;
    uint8_t *data = take_in(r, 0, &len);
    uint8_t *sa = take_in(r, 1, &alen);
    if (!data || !sa) {
        free(data);
        free(sa);
        return SF_RESULT_INVALID_HEADER;
    }
    int32_t err;
    int32_t ret = bsd_net_send(net_of(r), fd, data, len, alen ? sa : NULL, alen, &err);
    free(data);
    free(sa);
    return reply(r, ret, err);
}

/** @brief Recv (8), RecvFrom (9), Read (25): i32 fd, (u32 flags); the data (and the sender's sockaddr) as receive buffers; i32 ret, errno (and u32 address length). */
static rl_result do_recv_common(rl_ipc_req *r, bool from, bool has_flags)
{
    uint32_t in[2] = { 0, 0 };
    if (!rl_srv_in(r, in, has_flags ? 8 : 4))
        return SF_RESULT_INVALID_HEADER;
    rl_ipc_buf db = rl_ipc_auto_out(r, 0);
    size_t cap = db.size > BSD_IO_MAX ? BSD_IO_MAX : (size_t)db.size;
    uint8_t *data = malloc(cap ? cap : 1);
    if (!data)
        return KR_OUT_OF_RESOURCE;
    uint8_t sender[16];
    int32_t err;
    int32_t ret = bsd_net_recv(net_of(r), (int32_t)in[0], data, cap, in[1], from ? sender : NULL, &err);
    uint32_t alen = 0;
    bool ok = true;
    if (ret > 0)
        ok = rl_ipc_buf_write(&db, data, (uint64_t)ret);
    free(data);
    if (ret >= 0 && from) {
        rl_ipc_buf ab = rl_ipc_auto_out(r, 1);
        alen = ab.size < 16 ? (uint32_t)ab.size : 16;
        if (alen)
            ok = ok && rl_ipc_buf_write(&ab, sender, alen);
    }
    if (!ok)
        return SF_RESULT_INVALID_HEADER;
    return from ? reply3(r, ret, err, alen) : reply(r, ret, err);
}

static rl_result do_recv(rl_ipc_req *r) { return do_recv_common(r, false, true); }

static rl_result do_recvfrom(rl_ipc_req *r) { return do_recv_common(r, true, true); }

static rl_result do_read(rl_ipc_req *r) { return do_recv_common(r, false, false); }

/** @brief GetSockName (16) and GetPeerName (15): i32 fd; the sockaddr as a receive buffer; i32 ret, errno and u32 address length. */
static rl_result name_common(rl_ipc_req *r, bool peer)
{
    int32_t fd;
    if (!rl_srv_in(r, &fd, 4))
        return SF_RESULT_INVALID_HEADER;
    uint8_t sa[16];
    int32_t err;
    int32_t ret = bsd_net_name(net_of(r), fd, peer, sa, &err);
    uint32_t alen = 0;
    if (ret >= 0) {
        rl_ipc_buf b = rl_ipc_auto_out(r, 0);
        alen = b.size < 16 ? (uint32_t)b.size : 16;
        if (alen && !rl_ipc_buf_write(&b, sa, alen))
            return SF_RESULT_INVALID_HEADER;
    }
    return reply3(r, ret, err, alen);
}

static rl_result do_getsockname(rl_ipc_req *r) { return name_common(r, false); }

static rl_result do_getpeername(rl_ipc_req *r) { return name_common(r, true); }

/** @brief GetSockOpt (17): i32 fd, u32 level, u32 name; the value as a receive buffer; i32 ret, errno and u32 length. */
static rl_result do_getsockopt(rl_ipc_req *r)
{
    uint32_t in[3];
    if (!rl_srv_in(r, in, 12))
        return SF_RESULT_INVALID_HEADER;
    uint8_t val[8] = { 0 };
    uint32_t len = 0;
    int32_t err;
    int32_t ret = bsd_net_getsockopt(net_of(r), (int32_t)in[0], in[1], in[2], val, sizeof val, &len, &err);
    if (ret >= 0) {
        rl_ipc_buf b = rl_ipc_auto_out(r, 0);
        if (len > b.size)
            len = (uint32_t)b.size;
        if (len && !rl_ipc_buf_write(&b, val, len))
            return SF_RESULT_INVALID_HEADER;
    }
    return reply3(r, ret, err, len);
}

/** @brief SetSockOpt (21): i32 fd, u32 level, u32 name; the value as a send buffer; i32 ret and errno. */
static rl_result do_setsockopt(rl_ipc_req *r)
{
    uint32_t in[3];
    size_t len;
    if (!rl_srv_in(r, in, 12))
        return SF_RESULT_INVALID_HEADER;
    uint8_t *val = take_in(r, 0, &len);
    if (!val)
        return SF_RESULT_INVALID_HEADER;
    int32_t err;
    int32_t ret = bsd_net_setsockopt(net_of(r), (int32_t)in[0], in[1], in[2], val, len, &err);
    free(val);
    return reply(r, ret, err);
}

/** @brief Fcntl (20): i32 fd, cmd, arg; i32 ret and errno. */
static rl_result do_fcntl(rl_ipc_req *r)
{
    int32_t in[3];
    if (!rl_srv_in(r, in, 12))
        return SF_RESULT_INVALID_HEADER;
    int32_t err;
    int32_t ret = bsd_net_fcntl(net_of(r), in[0], in[1], in[2], &err);
    return reply(r, ret, err);
}

/** @brief Shutdown (22): i32 fd, how; i32 ret and errno. */
static rl_result do_shutdown(rl_ipc_req *r)
{
    int32_t in[2];
    if (!rl_srv_in(r, in, 8))
        return SF_RESULT_INVALID_HEADER;
    int32_t err;
    int32_t ret = bsd_net_shutdown(net_of(r), in[0], in[1], &err);
    return reply(r, ret, err);
}

/** @brief Close (26): i32 fd; i32 ret and errno. */
static rl_result do_close(rl_ipc_req *r)
{
    int32_t fd;
    if (!rl_srv_in(r, &fd, 4))
        return SF_RESULT_INVALID_HEADER;
    int32_t err;
    int32_t ret = bsd_net_close(net_of(r), fd, &err);
    return reply(r, ret, err);
}

/** @brief DuplicateSocket (27): i32 fd, u64 reserved; i32 ret and errno. */
static rl_result do_duplicate(rl_ipc_req *r)
{
    struct {
        int32_t fd;
        uint32_t pad;
        uint64_t reserved;
    } in;
    if (!rl_srv_in(r, &in, 16))
        return SF_RESULT_INVALID_HEADER;
    int32_t err;
    int32_t ret = bsd_net_duplicate(net_of(r), in.fd, &err);
    return reply(r, ret, err);
}

/** @brief EventFd (31): u32 flags, u64 initial value; i32 descriptor and errno. */
static rl_result do_eventfd(rl_ipc_req *r)
{
    struct {
        uint32_t flags;
        uint32_t pad;
        uint64_t initval;
    } in;
    if (!rl_srv_in(r, &in, 16))
        return SF_RESULT_INVALID_HEADER;
    int32_t err;
    int32_t ret = bsd_net_eventfd(net_of(r), in.initval, in.flags, &err);
    return reply(r, ret, err);
}

/** @brief Largest descriptor number a Select set can name. */
#define BSD_FD_SETSIZE 1024

/** @brief Ioctl request codes of the console (FreeBSD) that sockets use. */
#define BSD_FIONREAD 0x4004667fu
#define BSD_FIONBIO 0x8004667eu

/**
 * @brief Select (5): i32 nfds, 4 bytes of padding, i64 seconds, i64 microseconds, a u8 that is 1 when the program
 * passed no timeout; the read, write and except sets as send buffers 0 to 2 and again as receive buffers 0 to 2 (an
 * empty buffer is a null set; a set is whole 64-bit words); i32 ready count and errno. A descriptor is ready for
 * reading as Poll's POLLIN says, for writing as POLLOUT says; the except set is always empty.
 */
static rl_result do_select(rl_ipc_req *r)
{
    struct {
        int32_t nfds;
        uint32_t pad;
        int64_t sec;
        int64_t usec;
        uint8_t no_timeout;
    } in;
    if (!rl_srv_in(r, &in, 25))
        return SF_RESULT_INVALID_HEADER;
    if (in.nfds < 0)
        return reply(r, -1, BSD_EINVAL);
    uint32_t nfds = in.nfds > BSD_FD_SETSIZE ? BSD_FD_SETSIZE : (uint32_t)in.nfds;
    size_t bytes = ((size_t)nfds + 63) / 64 * 8;
    uint64_t sets[3][BSD_FD_SETSIZE / 64] = { { 0 } };
    bool present[3] = { false, false, false };
    for (uint32_t i = 0; i < 3; i++) {
        rl_ipc_buf b = rl_ipc_auto_in(r, i);
        if (!b.size || !bytes)
            continue;
        present[i] = true;
        if (!rl_ipc_buf_read(&b, sets[i], b.size < bytes ? b.size : bytes))
            return SF_RESULT_INVALID_HEADER;
    }
    int64_t timeout_ms = in.no_timeout ? -1 : in.sec * 1000 + in.usec / 1000;
    int64_t waited = 0;
    int32_t ready = 0;
    uint64_t out[3][BSD_FD_SETSIZE / 64];
    for (;;) {
        memset(out, 0, sizeof out);
        ready = 0;
        for (uint32_t fd = 0; fd < nfds; fd++) {
            uint64_t bit = UINT64_C(1) << (fd % 64);
            for (uint32_t k = 0; k < 2; k++) {
                if (!(sets[k][fd / 64] & bit))
                    continue;
                uint16_t want = k == 0 ? 1 : 4;
                if (bsd_net_revents(net_of(r), (int32_t)fd, want) & (uint16_t)(want | 0x10)) {
                    out[k][fd / 64] |= bit;
                    ready++;
                }
            }
        }
        if (ready || timeout_ms == 0 || (timeout_ms > 0 && waited >= timeout_ms))
            break;
        hos_sleep_thread(INT64_C(1000000));
        waited++;
    }
    for (uint32_t i = 0; i < 3; i++) {
        if (!present[i])
            continue;
        rl_ipc_buf b = rl_ipc_auto_out(r, i);
        if (b.size && !rl_ipc_buf_write(&b, out[i], b.size < bytes ? b.size : bytes))
            return SF_RESULT_INVALID_HEADER;
    }
    return reply(r, ready, 0);
}

/**
 * @brief Ioctl (19): i32 fd, u32 request, u32 buffer count; the argument as send buffer 0 (and as receive buffer 0);
 * i32 ret and errno. FIONBIO sets the nonblocking mode from the int in the argument, FIONREAD writes the bytes a
 * Recv would return as an int to receive buffer 0; any other request is not a socket ioctl, so ENOTTY.
 */
static rl_result do_ioctl(rl_ipc_req *r)
{
    struct {
        int32_t fd;
        uint32_t request;
        uint32_t count;
    } in;
    if (!rl_srv_in(r, &in, 12))
        return SF_RESULT_INVALID_HEADER;
    int32_t err;
    int32_t ret;
    if (in.request == BSD_FIONBIO) {
        int32_t on = 0;
        rl_ipc_buf b = rl_ipc_auto_in(r, 0);
        if (!rl_ipc_buf_read(&b, &on, 4))
            return SF_RESULT_INVALID_HEADER;
        ret = bsd_net_set_nonblock(net_of(r), in.fd, on != 0, &err);
    } else if (in.request == BSD_FIONREAD) {
        int32_t pending = bsd_net_pending(net_of(r), in.fd, &err);
        ret = pending < 0 ? -1 : 0;
        if (ret == 0) {
            rl_ipc_buf b = rl_ipc_auto_out(r, 0);
            if (!rl_ipc_buf_write(&b, &pending, 4))
                return SF_RESULT_INVALID_HEADER;
        }
    } else {
        ret = -1;
        err = BSD_ENOTTY;
    }
    return reply(r, ret, err);
}

static const rl_ipc_cmd client_cmds[] = {
    { 0, "RegisterClient", register_client },
    { 1, "StartMonitoring", rl_srv_stub_ok },
    { 2, "Socket", socket_create },
    { 3, "SocketExempt", socket_create },
    { 5, "Select", do_select },
    { 6, "Poll", poll_fds },
    { 8, "Recv", do_recv },
    { 9, "RecvFrom", do_recvfrom },
    { 10, "Send", do_send },
    { 11, "SendTo", do_sendto },
    { 12, "Accept", do_accept },
    { 13, "Bind", do_bind },
    { 14, "Connect", do_connect },
    { 15, "GetPeerName", do_getpeername },
    { 16, "GetSockName", do_getsockname },
    { 17, "GetSockOpt", do_getsockopt },
    { 18, "Listen", do_listen },
    { 19, "Ioctl", do_ioctl },
    { 20, "Fcntl", do_fcntl },
    { 21, "SetSockOpt", do_setsockopt },
    { 22, "Shutdown", do_shutdown },
    { 24, "Write", do_send },
    { 25, "Read", do_read },
    { 26, "Close", do_close },
    { 27, "DuplicateSocket", do_duplicate },
    { 31, "EventFd", do_eventfd },
    { 36, "RegisterClientSwitch2", register_client_switch2 },
};

static void client_destroy(void *state) { bsd_net_free(state); }

static const rl_ipc_class client_class = { "nn::socket::sf::IClient", client_cmds,
                                           sizeof client_cmds / sizeof client_cmds[0], 0, client_destroy };

static rl_ipc_object *client_factory(void)
{
    bsd_net *n = bsd_net_new();
    rl_ipc_object *o = n ? rl_srv_object(&client_class, n) : NULL;
    if (n && !o)
        bsd_net_free(n);
    return o;
}

void rl_srv_install_bsd(void)
{
    rl_ipc_register_service("bsd:u", client_factory);
    rl_ipc_register_service("bsd:s", client_factory);
}
