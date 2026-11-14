/**
 * @file srv_csrng.c
 * @brief The random number service csrng (spl's ISplRandomInterface): its
 * one command fills the client's buffer with random bytes from the host.
 *
 * The command follows Ryujinx (MIT) Services/Spl/IRandomInterface: command
 * 0 writes the receive (B) buffer whole.
 */
#include "services.h"

#define CSRNG_CHUNK 0x1000u

static rl_result generate_random_bytes(rl_ipc_req *r)
{
    uint8_t chunk[CSRNG_CHUNK];
    rl_ipc_buf out = r->nb ? r->b[0] : (rl_ipc_buf){ 0 };
    for (uint64_t at = 0; at < out.size;) {
        uint64_t n = out.size - at < CSRNG_CHUNK ? out.size - at : CSRNG_CHUNK;
        if (!rl_host_random(chunk, (size_t)n))
            return KR_INVALID_STATE;
        rl_ipc_buf part = { out.addr + at, n, out.attr };
        if (!rl_ipc_buf_write(&part, chunk, n))
            return KR_INVALID_ADDRESS;
        at += n;
    }
    return KR_SUCCESS;
}

static const rl_ipc_cmd service_cmds[] = {
    { 0, "GenerateRandomBytes", generate_random_bytes },
};

static const rl_ipc_class service_class = { "csrng", service_cmds, sizeof service_cmds / sizeof service_cmds[0], 0,
                                            NULL };

static rl_ipc_object *service_factory(void) { return rl_srv_object(&service_class, NULL); }

void rl_srv_install_csrng(void) { rl_ipc_register_service("csrng", service_factory); }
