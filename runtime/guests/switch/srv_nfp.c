/**
 * @file srv_nfp.c
 * @brief The NFC service nfp:user (nn::nfc::detail::IUserManager and IUser,
 * which a Switch 2 SDK's nn::nfp client opens) of a console with no tag in
 * its reader: a client initializes and finalizes, and the interface reports
 * the initialized state. Command ids come from the SDK's own client (the
 * proxy tables behind nn::nfp::Initialize). The commands a game calls that
 * this does not answer stop with relift's unknown command report.
 */
#include "services.h"

/** @brief nn::nfp State: the interface is initialized. */
#define NFP_STATE_INIT 1

/** @brief GetState (2): a u32, initialized (inferred: the SDK's Initialize succeeded before it asks). */
static rl_result user_get_state(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = NFP_STATE_INIT;
    return KR_SUCCESS;
}

static const rl_ipc_cmd user_cmds[] = {
    { 0, "Initialize", rl_srv_stub_ok },
    { 1, "Finalize", rl_srv_stub_ok },
    { 2, "GetState", user_get_state },
};

static const rl_ipc_class user_class = { "nn::nfc::detail::IUser", user_cmds, sizeof user_cmds / sizeof user_cmds[0], 0,
                                         NULL };

/** @brief CreateUserInterface (0): an IUser. */
static rl_result create_user_interface(rl_ipc_req *r)
{
    rl_ipc_object *o = rl_srv_object(&user_class, NULL);
    return o ? rl_ipc_push_object(r, o) : KR_OUT_OF_RESOURCE;
}

static const rl_ipc_cmd manager_cmds[] = {
    { 0, "CreateUserInterface", create_user_interface },
};

static const rl_ipc_class manager_class = { "nn::nfc::detail::IUserManager", manager_cmds,
                                            sizeof manager_cmds / sizeof manager_cmds[0], 0, NULL };

static rl_ipc_object *manager_factory(void) { return rl_srv_object(&manager_class, NULL); }

void rl_srv_install_nfp(void) { rl_ipc_register_service("nfp:user", manager_factory); }
