/**
 * @file srv_chat.c
 * @brief The Switch 2's GameChat user service chat:u (nn::chat::IUserService)
 * of a console that is not in a chat. Command ids and layouts follow
 * switchbrew.org (Switch 2: Chat services).
 */
#include "hos.h"
#include "services.h"

/**
 * @brief IUserShimScopedObject 1: no input; one handle, the readable end of an
 * event (verified: nn::chat::detail::InitializeUserService, SDK 20.5 sdk
 * 0x636644, calls it as the object's first command and attaches the handle
 * to a readable system event). A console not in a chat never signals it.
 */
static rl_result shim_event(rl_ipc_req *r)
{
    hos_revent *e = rl_hos_event_create();
    if (!e)
        return KR_OUT_OF_RESOURCE;
    rl_result rc = rl_ipc_push_copy(r, &e->obj);
    hos_kobj_unref(&e->obj);
    return rc;
}

static const rl_ipc_cmd shim_cmds[] = {
    { 1, "GetEvent", shim_event },
};

static const rl_ipc_class shim_class = { "nn::chat::IUserShimScopedObject", shim_cmds,
                                         sizeof shim_cmds / sizeof shim_cmds[0], 0, NULL };

/** @brief Cmd0: no input; an IUserShimScopedObject. */
static rl_result create_shim(rl_ipc_req *r)
{
    rl_ipc_object *o = rl_srv_object(&shim_class, NULL);
    return o ? rl_ipc_push_object(r, o) : KR_OUT_OF_RESOURCE;
}

static const rl_ipc_cmd user_cmds[] = {
    { 0, "CreateUserShimScopedObject", create_shim },
};

static const rl_ipc_class user_class = { "chat:u", user_cmds, sizeof user_cmds / sizeof user_cmds[0], 0, NULL };

static rl_ipc_object *user_factory(void) { return rl_srv_object(&user_class, NULL); }

void rl_srv_install_chat(void)
{
    if (hos_switch2())
        rl_ipc_register_service("chat:u", user_factory);
}
