/**
 * @file srv_bsd.c
 * @brief The socket services bsd:u and bsd:s (nn::socket::sf::IClient)
 * without a network: a client registers and starts monitoring, and every
 * socket it asks for fails with ENETDOWN, as on a console that is offline.
 *
 * Commands follow switchbrew.org (Sockets services) and Ryujinx (MIT,
 * Copyright (c) Ryujinx Team and Contributors) Services/Sockets/Bsd
 * IClient: RegisterClient answers bsd errno 0. Command 36 follows the Switch 2 SDK's own client stub and its caller,
 * nn::socket::detail::Initialize.
 */
#include "hos.h"
#include "services.h"

#include <stdlib.h>


/** @brief Most descriptors one Poll request carries. */
#define BSD_POLL_MAX 0x10000

/** @brief The BSD errno for a network that is down. */
#define BSD_ENETDOWN 50

/** @brief RegisterClient: BsdBufferConfig, u64 pid, u64 transfer memory size, the transfer memory; u32 bsd errno. */
static rl_result register_client(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

/**
 * @brief Poll: u32 nfds, i32 timeout in milliseconds, the pollfd array (u32 fd, u16 events, u16 revents each) as a
 * send buffer and again as a receive buffer; i32 ready count and a bsd errno. Without a network no descriptor ever
 * becomes ready: every revents is zero, the count is zero, and the call waits out the timeout (forever for a negative
 * one), as a console does for sockets that have nothing to report. Whether a descriptor is a valid socket does not
 * change that answer, so it holds whether or not Socket creates one.
 */
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
    for (uint32_t i = 0; i < in.nfds; i++)
        fds[i * 2 + 1] &= 0xffffu;
    int32_t ready = 0;
    if (in.timeout != 0) {
        if (in.timeout < 0) {
            for (;;)
                hos_sleep_thread(INT64_C(1000000000));
        }
        hos_sleep_thread((int64_t)in.timeout * INT64_C(1000000));
    }
    if (size) {
        rl_ipc_buf dst = rl_ipc_auto_out(r, 0);
        bool wrote = rl_ipc_buf_write(&dst, fds, size);
        free(fds);
        if (!wrote)
            return SF_RESULT_INVALID_HEADER;
    } else {
        free(fds);
    }
    int32_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    out[0] = ready;
    out[1] = 0;
    return KR_SUCCESS;
}

/** @brief Socket and SocketExempt: u32 domain, type, protocol; i32 -1 and ENETDOWN. */
static rl_result socket_down(rl_ipc_req *r)
{
    int32_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    out[0] = -1;
    out[1] = BSD_ENETDOWN;
    return KR_SUCCESS;
}

/**
 * @brief RegisterClientSwitch2 (command 36, a Switch 2 SDK's replacement for RegisterClient): a u32 version (the
 * first word of nn::socket's configuration), 4 bytes of padding and a u64 process id placeholder (0; the pid
 * travels in the message's process id descriptor); a u32 bsd errno. The SDK fails nn::socket::Initialize unless the
 * errno is 0, and then starts monitoring, so a client registers on an offline console exactly as with command 0.
 */
static rl_result register_client_switch2(rl_ipc_req *r)
{
    struct {
        uint32_t version;
        uint32_t pad;
        uint64_t pid;
    } in;
    if (!rl_srv_in(r, &in, 16))
        return SF_RESULT_INVALID_HEADER;
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

static const rl_ipc_cmd client_cmds[] = {
    { 0, "RegisterClient", register_client },
    { 1, "StartMonitoring", rl_srv_stub_ok },
    { 2, "Socket", socket_down },
    { 3, "SocketExempt", socket_down },
    { 6, "Poll", poll_fds },
    { 36, "RegisterClientSwitch2", register_client_switch2 },
};

static const rl_ipc_class client_class = { "nn::socket::sf::IClient", client_cmds,
                                           sizeof client_cmds / sizeof client_cmds[0], 0, NULL };

static rl_ipc_object *client_factory(void) { return rl_srv_object(&client_class, NULL); }

void rl_srv_install_bsd(void)
{
    rl_ipc_register_service("bsd:u", client_factory);
    rl_ipc_register_service("bsd:s", client_factory);
}
