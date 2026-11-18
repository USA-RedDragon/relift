/**
 * @file srv_nifm.c
 * @brief The network interface service nifm:u (nn::nifm::detail::IStaticService)
 * of a console that is offline. Command ids and interfaces follow
 * switchbrew.org (Network Interface services); the commands a game calls
 * that this does not answer stop with relift's unknown command report.
 */
#include "hos.h"
#include "services.h"

static const rl_ipc_class general_class = { "nn::nifm::detail::IGeneralService", NULL, 0, 0, NULL };

static rl_result push(rl_ipc_req *r, const rl_ipc_class *cls)
{
    rl_ipc_object *o = rl_srv_object(cls, NULL);
    return o ? rl_ipc_push_object(r, o) : KR_OUT_OF_RESOURCE;
}

/** @brief IStaticService 4 and 5: no input, or a PID and a reserved u64; an IGeneralService. */
static rl_result create_general_service(rl_ipc_req *r) { return push(r, &general_class); }

static const rl_ipc_cmd static_cmds[] = {
    { 4, "CreateGeneralServiceOld", create_general_service },
    { 5, "CreateGeneralService", create_general_service },
};

static const rl_ipc_class static_class = { "nifm:u", static_cmds, sizeof static_cmds / sizeof static_cmds[0], 0, NULL };

static rl_ipc_object *static_factory(void) { return rl_srv_object(&static_class, NULL); }

void rl_srv_install_nifm(void) { rl_ipc_register_service("nifm:u", static_factory); }
