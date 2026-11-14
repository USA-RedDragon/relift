/**
 * @file audio_svc.c
 * @brief Registration of the audio services and guest memory access for them.
 */
#include "hos_audio.h"

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

void rl_hos_audio_register(void)
{
    rl_ipc_register_service("audren:u", audren_manager_new);
    rl_ipc_register_service("audout:u", audout_manager_new);
    rl_ipc_register_service("hwopus", hwopus_manager_new);
}
