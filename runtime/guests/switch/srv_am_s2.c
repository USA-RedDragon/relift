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

/** @brief The scale codes nn::oe::GetCurrentGraphicsScale turns into scales 0, 1 and 2; it aborts on any other. */
#define GRAPHICS_SCALE_CODE_0 10u
#define GRAPHICS_SCALE_CODE_1 20u
#define GRAPHICS_SCALE_CODE_2 30u

/**
 * @brief The scale code for the video.render_resolution setting. Ocarina (Switch 2) fixes its display at 2560x1440 in
 * the game and sizes its frame from that times the factor of the scale this call reports (scale 0 x1, scale 1 x0.8,
 * scale 2 x0.667, so 1440, 1152 and 960 lines); it never asks the display services. The first scale whose height is
 * within the cap is chosen, the smallest when none is: 1080p and 720p both give scale 2, the lowest the game offers.
 */
uint32_t am_graphics_scale_code(uint32_t cap_lines)
{
    static const uint32_t codes[] = { GRAPHICS_SCALE_CODE_0, GRAPHICS_SCALE_CODE_1, GRAPHICS_SCALE_CODE_2 };
    static const uint32_t lines[] = { 1440, 1152, 960 };
    if (cap_lines == 0)
        return GRAPHICS_SCALE_CODE_0;
    for (size_t i = 0; i < 3; i++)
        if (lines[i] <= cap_lines)
            return codes[i];
    return GRAPHICS_SCALE_CODE_2;
}

/**
 * @brief GetGraphicsScalingParameters: 12 bytes, which nn::oe hands back as
 * they come. Games read only the first u32, the scale code, through
 * nn::oe::GetCurrentGraphicsScale; relift reports scale 0 and zero for the
 * rest. The code follows the video.render_resolution setting (am_graphics_scale_code).
 */
static rl_result gs_get_graphics_scaling_parameters(rl_ipc_req *r)
{
    uint8_t *out = rl_ipc_out(r, 12);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    uint32_t code = am_graphics_scale_code(rl_am_display_cap());
    rl_log(RL_LOG_INFO, "am", "GetGraphicsScalingParameters: scale code %u (video.render_resolution cap %u)", code,
           rl_am_display_cap());
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
