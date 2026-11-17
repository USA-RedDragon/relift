/**
 * @file srv_am_s2.c
 * @brief The am interfaces only the Switch 2 has: IPerformanceFunctions and
 * IGraphicsScalingParametersFunctions, reached from IApplicationProxy 40
 * and 60. Command ids follow switchbrew.org (Applet Manager services).
 */
#include "srv_am_priv.h"

#include <string.h>

static bool overclock;

static rl_result pf_is_cpu_overclock_enabled(rl_ipc_req *r)
{
    uint8_t *out = rl_ipc_out(r, 1);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = overclock;
    return KR_SUCCESS;
}

static rl_result pf_set_cpu_overclock_enabled(rl_ipc_req *r)
{
    uint8_t on = 0;
    if (!rl_srv_in(r, &on, 1))
        return SF_RESULT_INVALID_HEADER;
    overclock = on & 1;
    return KR_SUCCESS;
}

static const rl_ipc_cmd performance_cmds[] = {
    { 0, "IsCpuOverclockEnabled", pf_is_cpu_overclock_enabled },
    { 1, "SetCpuOverclockEnabled", pf_set_cpu_overclock_enabled },
};

static const rl_ipc_class performance_class = {
    "nn::am::service::IPerformanceFunctions", performance_cmds, AM_N(performance_cmds), 0, NULL
};

/** @brief The scale code nn::oe::GetCurrentGraphicsScale turns into scale 0; it takes 10, 20 and 30 and aborts on any other. */
#define GRAPHICS_SCALE_CODE_0 10u

/**
 * @brief GetGraphicsScalingParameters: 12 bytes, which nn::oe hands back as
 * they come. Games read only the first u32, the scale code, through
 * nn::oe::GetCurrentGraphicsScale; relift reports scale 0 and zero for the
 * rest.
 */
static rl_result gs_get_graphics_scaling_parameters(rl_ipc_req *r)
{
    uint8_t *out = rl_ipc_out(r, 12);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    uint32_t code = GRAPHICS_SCALE_CODE_0;
    memset(out, 0, 12);
    memcpy(out, &code, sizeof code);
    return KR_SUCCESS;
}

static const rl_ipc_cmd scaling_cmds[] = {
    { 10, "GetGraphicsScalingParameters", gs_get_graphics_scaling_parameters },
    { 1050, "SetGraphicsScaleChangedNotificationEnabled", rl_srv_stub_ok },
};

static const rl_ipc_class scaling_class = {
    "nn::am::service::IGraphicsScalingParametersFunctions", scaling_cmds, AM_N(scaling_cmds), 0, NULL
};

rl_ipc_object *am_new_performance_functions(void) { return am_new_empty(&performance_class); }

rl_ipc_object *am_new_graphics_scaling_functions(void) { return am_new_empty(&scaling_class); }

void am_s2_reset(void) { overclock = false; }
