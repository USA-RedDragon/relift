/**
 * @file services.c
 * @brief Shared helpers of the Horizon services and their installation.
 */
#include "services.h"

#include "hos_audio.h"

rl_result rl_srv_stub_ok(rl_ipc_req *r)
{
    (void)r;
    return KR_SUCCESS;
}

rl_ipc_object *rl_srv_object(const rl_ipc_class *cls, void *state) { return rl_ipc_object_new(cls, state); }

bool rl_srv_in(const rl_ipc_req *r, void *dst, uint32_t size) { return rl_ipc_in(r, dst, size); }

uint64_t rl_srv_pid(const rl_ipc_req *r) { return r->has_pid ? r->pid : HOS_PID; }

void rl_srv_install_all(void)
{
    rl_srv_install_set();
    rl_srv_install_am();
    rl_srv_install_apm();
    rl_srv_install_hid();
    rl_srv_install_time();
    rl_srv_install_vi();
    rl_srv_install_nv();
    rl_hos_audio_register();
}
