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
#include "services.h"

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
