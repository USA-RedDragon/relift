/**
 * @file audio_svc.c
 * @brief Registration of the audio services and guest memory access for them.
 * The Switch 2 reaches the audio out and renderer managers through aud:u
 * (nn::audio::IUserClientSession) instead of audout:u and audren:u.
 */
#include "hos_audio.h"
#include "services.h"

#include <string.h>

bool audio_guest_read(uint64_t addr, void *dst, uint64_t size)
{
    if (!size)
        return true;
    hos_lock();
    bool ok = hos_user_range(addr, size, HOS_PERM_R);
    if (ok)
        memcpy(dst, hos.vm.base + addr, size);
    hos_unlock();
    return ok;
}

bool audio_guest_write(uint64_t addr, const void *src, uint64_t size)
{
    if (!size)
        return true;
    hos_lock();
    bool ok = hos_user_range(addr, size, HOS_PERM_W);
    if (ok)
        memcpy(hos.vm.base + addr, src, size);
    hos_unlock();
    return ok;
}

static rl_result push_new(rl_ipc_req *r, rl_ipc_object *o)
{
    if (!o)
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_object(r, o);
}

/** @brief IUserClientSession 1: the IAudioOutManager audout:u gives on the Switch. */
static rl_result user_audout(rl_ipc_req *r) { return push_new(r, audout_manager_new()); }

/** @brief IUserClientSession 2: the IAudioRendererManager audren:u gives on the Switch. */
static rl_result user_audren(rl_ipc_req *r) { return push_new(r, audren_manager_new()); }

static const rl_ipc_cmd user_cmds[] = {
    { 1, "GetAudioOutManager", user_audout },
    { 2, "GetAudioRendererManager", user_audren },
};

static const rl_ipc_class user_class = { "nn::audio::IUserClientSession", user_cmds,
                                         sizeof user_cmds / sizeof user_cmds[0], 0, NULL };

static rl_ipc_object *user_factory(void) { return rl_srv_object(&user_class, NULL); }

void rl_hos_audio_register(void)
{
    if (hos_switch2()) {
        rl_ipc_register_service("aud:u", user_factory);
    } else {
        rl_ipc_register_service("audren:u", audren_manager_new);
        rl_ipc_register_service("audout:u", audout_manager_new);
    }
    rl_ipc_register_service("hwopus", hwopus_manager_new);
}
