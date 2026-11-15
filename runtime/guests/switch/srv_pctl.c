/**
 * @file srv_pctl.c
 * @brief The parental control services: pctl, pctl:a, pctl:r and pctl:s.
 *
 * Command ids, permission bits and results follow Ryujinx (MIT)
 * Services/Pctl. The host has no parental controls set, so nothing is
 * restricted: free communication is allowed and stereo vision permitted.
 */
#include "services.h"

#include <stdlib.h>

#define PCTL_RESULT_STEREO_VISION_DENIED RL_MAKE_RESULT(142, 104)
#define PCTL_RESULT_PERMISSION_DENIED RL_MAKE_RESULT(142, 133)

enum {
    PERM_PCTL = 0x303,
    PERM_PCTL_A = 0x83BE,
    PERM_PCTL_R = 0x8040,
    PERM_PCTL_S = 0x838E,
};

typedef struct pctl_session {
    uint32_t permission;
    bool stereo_restricted;
} pctl_session;

static uint32_t permission_of(const rl_ipc_req *r) { return ((const pctl_session *)r->self->state)->permission; }

static rl_result pctl_initialize(rl_ipc_req *r)
{
    if (!(permission_of(r) & 0x8001))
        return PCTL_RESULT_PERMISSION_DENIED;
    return KR_SUCCESS;
}

static rl_result stereo_vision_permitted(const pctl_session *s)
{
    return s->stereo_restricted ? PCTL_RESULT_STEREO_VISION_DENIED : KR_SUCCESS;
}

static rl_result pctl_confirm_stereo_vision_permission(rl_ipc_req *r)
{
    return stereo_vision_permitted(r->self->state);
}

static rl_result pctl_is_restriction_enabled(rl_ipc_req *r)
{
    if (!(permission_of(r) & 0x140))
        return PCTL_RESULT_PERMISSION_DENIED;
    *(uint8_t *)rl_ipc_out(r, 1) = 0;
    return KR_SUCCESS;
}

static rl_result pctl_confirm_stereo_vision_restriction_configurable(rl_ipc_req *r)
{
    if (!(permission_of(r) & 2))
        return PCTL_RESULT_PERMISSION_DENIED;
    return KR_SUCCESS;
}

static rl_result pctl_get_stereo_vision_restriction(rl_ipc_req *r)
{
    if (!(permission_of(r) & 0x200))
        return PCTL_RESULT_PERMISSION_DENIED;
    *(uint8_t *)rl_ipc_out(r, 1) = ((pctl_session *)r->self->state)->stereo_restricted;
    return KR_SUCCESS;
}

static rl_result pctl_set_stereo_vision_restriction(rl_ipc_req *r)
{
    if (!(permission_of(r) & 0x200))
        return PCTL_RESULT_PERMISSION_DENIED;
    uint8_t v;
    if (!rl_srv_in(r, &v, 1))
        return SF_RESULT_INVALID_HEADER;
    ((pctl_session *)r->self->state)->stereo_restricted = v != 0;
    return KR_SUCCESS;
}

static rl_result pctl_is_stereo_vision_permitted(rl_ipc_req *r)
{
    rl_result rc = stereo_vision_permitted(r->self->state);
    *(uint8_t *)rl_ipc_out(r, 1) = rc == KR_SUCCESS;
    return rc;
}

static const rl_ipc_cmd service_cmds[] = {
    { 1, "Initialize", pctl_initialize },
    RL_SRV_STUB(1001, "CheckFreeCommunicationPermission"),
    { 1013, "ConfirmStereoVisionPermission", pctl_confirm_stereo_vision_permission },
    RL_SRV_STUB(1017, "EndFreeCommunication"),
    RL_SRV_STUB(1018, "IsFreeCommunicationAvailable"),
    { 1031, "IsRestrictionEnabled", pctl_is_restriction_enabled },
    { 1061, "ConfirmStereoVisionRestrictionConfigurable", pctl_confirm_stereo_vision_restriction_configurable },
    { 1062, "GetStereoVisionRestriction", pctl_get_stereo_vision_restriction },
    { 1063, "SetStereoVisionRestriction", pctl_set_stereo_vision_restriction },
    RL_SRV_STUB(1064, "ResetConfirmedStereoVisionPermission"),
    { 1065, "IsStereoVisionPermitted", pctl_is_stereo_vision_permitted },
};

static const rl_ipc_class service_class = { "pctl::detail::ipc::IParentalControlService", service_cmds,
                                            sizeof service_cmds / sizeof service_cmds[0], 0, free };

static rl_result create(rl_ipc_req *r, bool initialize)
{
    pctl_session *s = calloc(1, sizeof *s);
    if (!s)
        return KR_OUT_OF_RESOURCE;
    s->permission = (uint32_t)(uintptr_t)r->self->state;
    if (initialize && !(s->permission & 0x8001)) {
        free(s);
        return PCTL_RESULT_PERMISSION_DENIED;
    }
    return rl_ipc_push_object(r, rl_srv_object(&service_class, s));
}

static rl_result create_service(rl_ipc_req *r) { return create(r, true); }

static rl_result create_service_without_initialize(rl_ipc_req *r) { return create(r, false); }

static const rl_ipc_cmd factory_cmds[] = {
    { 0, "CreateService", create_service },
    { 1, "CreateServiceWithoutInitialize", create_service_without_initialize },
};

static const rl_ipc_class factory_class = { "pctl", factory_cmds, sizeof factory_cmds / sizeof factory_cmds[0], 0,
                                            NULL };

static rl_ipc_object *factory(uint32_t permission)
{
    return rl_srv_object(&factory_class, (void *)(uintptr_t)permission);
}

static rl_ipc_object *pctl_factory(void) { return factory(PERM_PCTL); }

static rl_ipc_object *pctl_a_factory(void) { return factory(PERM_PCTL_A); }

static rl_ipc_object *pctl_r_factory(void) { return factory(PERM_PCTL_R); }

static rl_ipc_object *pctl_s_factory(void) { return factory(PERM_PCTL_S); }

void rl_srv_install_pctl(void)
{
    rl_ipc_register_service("pctl", pctl_factory);
    rl_ipc_register_service("pctl:a", pctl_a_factory);
    rl_ipc_register_service("pctl:r", pctl_r_factory);
    rl_ipc_register_service("pctl:s", pctl_s_factory);
}
