/**
 * @file srv_apm.c
 * @brief The performance and power services: apm, apm:am, apm:sys, apm:p and psm.
 *
 * Command ids, defaults and results follow Ryujinx (MIT) Services/Apm and
 * Services/Ptm/Psm, plus the psm commands of libnx (ISC) psm.c. The host has
 * no battery to report, so psm reports a full battery on a charger.
 */
#include "services.h"

#include <stdlib.h>
#include <string.h>

#define APM_RESULT_INVALID_PARAMETERS RL_MAKE_RESULT(148, 1)

enum { MODE_DEFAULT = 0, MODE_BOOST = 1 };
#define CONFIG_7 0x00020003u
#define CONFIG_8 0x00020004u

#define BATTERY_PERCENT 100u
#define CHARGER_ENOUGH_POWER 1u
#define VOLTAGE_STATE_NORMAL 3u
#define INFO_FIELDS_SIZE 0x54u
#define PSM_MAX_PERCENT_CENTI_MILLI 100000u

static struct {
    uint32_t mode;
    uint32_t cpu_boost;
    uint32_t config[2];
    bool overclock;
    bool charging;
    hos_revent *charge_info_event, *calibrated_event;
} g_apm;

static bool read_u32(const rl_ipc_req *r, uint32_t off, uint32_t *v)
{
    if (r->in_size < off + 4)
        return false;
    memcpy(v, r->in + off, 4);
    return true;
}

static uint32_t config_for(uint32_t mode)
{
    return mode == MODE_BOOST ? g_apm.config[MODE_BOOST] : g_apm.config[MODE_DEFAULT];
}

static rl_result mgr_get_performance_mode(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = g_apm.mode;
    return KR_SUCCESS;
}

static rl_result mgr_is_cpu_overclock_enabled(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = g_apm.overclock;
    return KR_SUCCESS;
}

static rl_result session_set_performance_configuration(rl_ipc_req *r)
{
    uint32_t mode, config;
    if (!read_u32(r, 0, &mode) || !read_u32(r, 4, &config))
        return SF_RESULT_INVALID_HEADER;
    if (mode > MODE_BOOST)
        return APM_RESULT_INVALID_PARAMETERS;
    g_apm.config[mode] = config;
    return KR_SUCCESS;
}

static rl_result session_get_performance_configuration(rl_ipc_req *r)
{
    uint32_t mode;
    if (!read_u32(r, 0, &mode))
        return SF_RESULT_INVALID_HEADER;
    if (mode > MODE_BOOST)
        return APM_RESULT_INVALID_PARAMETERS;
    *(uint32_t *)rl_ipc_out(r, 4) = config_for(mode);
    return KR_SUCCESS;
}

static rl_result session_set_cpu_overclock_enabled(rl_ipc_req *r)
{
    uint8_t v;
    if (!rl_srv_in(r, &v, 1))
        return SF_RESULT_INVALID_HEADER;
    g_apm.overclock = v != 0;
    return KR_SUCCESS;
}

static const rl_ipc_cmd session_cmds[] = {
    { 0, "SetPerformanceConfiguration", session_set_performance_configuration },
    { 1, "GetPerformanceConfiguration", session_get_performance_configuration },
    { 2, "SetCpuOverclockEnabled", session_set_cpu_overclock_enabled },
};

static const rl_ipc_class session_class = { "apm::ISession", session_cmds, sizeof session_cmds / sizeof session_cmds[0],
                                            0, NULL };

static rl_result open_session(rl_ipc_req *r) { return rl_ipc_push_object(r, rl_srv_object(&session_class, NULL)); }

static const rl_ipc_cmd manager_cmds[] = {
    { 0, "OpenSession", open_session },
    { 1, "GetPerformanceMode", mgr_get_performance_mode },
    { 6, "IsCpuOverclockEnabled", mgr_is_cpu_overclock_enabled },
};

static const rl_ipc_class manager_class = { "apm", manager_cmds, sizeof manager_cmds / sizeof manager_cmds[0], 0, NULL };

static rl_ipc_object *manager_factory(void) { return rl_srv_object(&manager_class, NULL); }

static const rl_ipc_cmd privileged_cmds[] = {
    { 0, "OpenSession", open_session },
};

static const rl_ipc_class privileged_class = { "apm:p", privileged_cmds,
                                               sizeof privileged_cmds / sizeof privileged_cmds[0], 0, NULL };

static rl_ipc_object *privileged_factory(void) { return rl_srv_object(&privileged_class, NULL); }

static rl_result sys_request_performance_mode(rl_ipc_req *r)
{
    uint32_t mode;
    if (!read_u32(r, 0, &mode))
        return SF_RESULT_INVALID_HEADER;
    g_apm.mode = mode;
    return KR_SUCCESS;
}

static rl_result sys_set_cpu_boost_mode(rl_ipc_req *r)
{
    uint32_t mode;
    if (!read_u32(r, 0, &mode))
        return SF_RESULT_INVALID_HEADER;
    g_apm.cpu_boost = mode;
    return KR_SUCCESS;
}

static rl_result sys_get_current_performance_configuration(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = config_for(g_apm.mode);
    return KR_SUCCESS;
}

static const rl_ipc_cmd system_cmds[] = {
    { 0, "RequestPerformanceMode", sys_request_performance_mode },
    { 6, "SetCpuBoostMode", sys_set_cpu_boost_mode },
    { 7, "GetCurrentPerformanceConfiguration", sys_get_current_performance_configuration },
};

static const rl_ipc_class system_class = { "apm:sys", system_cmds, sizeof system_cmds / sizeof system_cmds[0], 0, NULL };

static rl_ipc_object *system_factory(void) { return rl_srv_object(&system_class, NULL); }

typedef struct psm_session {
    hos_revent *event;
} psm_session;

static void psm_session_destroy(void *state)
{
    psm_session *s = state;
    if (s->event)
        hos_kobj_unref(&s->event->obj);
    free(s);
}

static rl_result psm_bind_state_change_event(rl_ipc_req *r)
{
    psm_session *s = r->self->state;
    if (!s->event)
        s->event = rl_hos_event_create();
    if (!s->event)
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_copy(r, &s->event->obj);
}

static rl_result psm_unbind_state_change_event(rl_ipc_req *r)
{
    psm_session *s = r->self->state;
    if (s->event)
        hos_kobj_unref(&s->event->obj);
    s->event = NULL;
    return KR_SUCCESS;
}

static const rl_ipc_cmd psm_session_cmds[] = {
    { 0, "BindStateChangeEvent", psm_bind_state_change_event },
    { 1, "UnbindStateChangeEvent", psm_unbind_state_change_event },
    RL_SRV_STUB(2, "SetChargerTypeChangeEventEnabled"),
    RL_SRV_STUB(3, "SetPowerSupplyChangeEventEnabled"),
    RL_SRV_STUB(4, "SetBatteryVoltageStateChangeEventEnabled"),
};

static const rl_ipc_class psm_session_class = { "psm::IPsmSession", psm_session_cmds,
                                                sizeof psm_session_cmds / sizeof psm_session_cmds[0], 0,
                                                psm_session_destroy };

static rl_result psm_open_session(rl_ipc_req *r)
{
    psm_session *s = calloc(1, sizeof *s);
    if (!s)
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_object(r, rl_srv_object(&psm_session_class, s));
}

static rl_result psm_get_battery_charge_percentage(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = BATTERY_PERCENT;
    return KR_SUCCESS;
}

static rl_result psm_get_charger_type(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = CHARGER_ENOUGH_POWER;
    return KR_SUCCESS;
}

static rl_result psm_enable_battery_charging(rl_ipc_req *r)
{
    g_apm.charging = true;
    (void)r;
    return KR_SUCCESS;
}

static rl_result psm_disable_battery_charging(rl_ipc_req *r)
{
    g_apm.charging = false;
    (void)r;
    return KR_SUCCESS;
}

static rl_result psm_is_battery_charging_enabled(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = g_apm.charging;
    return KR_SUCCESS;
}

static rl_result psm_get_battery_voltage_state(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = VOLTAGE_STATE_NORMAL;
    return KR_SUCCESS;
}

static rl_result psm_get_raw_battery_charge_percentage(rl_ipc_req *r)
{
    *(double *)rl_ipc_out(r, 8) = (double)BATTERY_PERCENT;
    return KR_SUCCESS;
}

static rl_result psm_is_enough_power_supplied(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = 1;
    return KR_SUCCESS;
}

static rl_result psm_get_battery_age_percentage(rl_ipc_req *r)
{
    *(double *)rl_ipc_out(r, 8) = (double)BATTERY_PERCENT;
    return KR_SUCCESS;
}

static rl_result psm_event(rl_ipc_req *r, hos_revent **slot)
{
    if (!*slot)
        *slot = rl_hos_event_create();
    if (!*slot)
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_copy(r, &(*slot)->obj);
}

static rl_result psm_get_battery_charge_info_event(rl_ipc_req *r) { return psm_event(r, &g_apm.charge_info_event); }

static rl_result psm_get_battery_charge_calibrated_event(rl_ipc_req *r)
{
    return psm_event(r, &g_apm.calibrated_event);
}

static rl_result psm_get_battery_charge_info_fields(rl_ipc_req *r)
{
    uint32_t *f = rl_ipc_out(r, INFO_FIELDS_SIZE);
    f[0] = 2000;
    f[1] = 0;
    f[2] = 1536;
    f[3] = 4208;
    f[4] = CHARGER_ENOUGH_POWER;
    ((uint8_t *)&f[5])[1] = g_apm.charging;
    f[6] = 2;
    f[7] = 25000;
    f[8] = PSM_MAX_PERCENT_CENTI_MILLI;
    f[9] = 4150;
    f[10] = PSM_MAX_PERCENT_CENTI_MILLI;
    return KR_SUCCESS;
}

static const rl_ipc_cmd psm_cmds[] = {
    { 0, "GetBatteryChargePercentage", psm_get_battery_charge_percentage },
    { 1, "GetChargerType", psm_get_charger_type },
    { 2, "EnableBatteryCharging", psm_enable_battery_charging },
    { 3, "DisableBatteryCharging", psm_disable_battery_charging },
    { 4, "IsBatteryChargingEnabled", psm_is_battery_charging_enabled },
    RL_SRV_STUB(5, "AcquireControllerPowerSupply"),
    RL_SRV_STUB(6, "ReleaseControllerPowerSupply"),
    { 7, "OpenSession", psm_open_session },
    RL_SRV_STUB(8, "EnableEnoughPowerChargeEmulation"),
    RL_SRV_STUB(9, "DisableEnoughPowerChargeEmulation"),
    RL_SRV_STUB(10, "EnableFastBatteryCharging"),
    RL_SRV_STUB(11, "DisableFastBatteryCharging"),
    { 12, "GetBatteryVoltageState", psm_get_battery_voltage_state },
    { 13, "GetRawBatteryChargePercentage", psm_get_raw_battery_charge_percentage },
    { 14, "IsEnoughPowerSupplied", psm_is_enough_power_supplied },
    { 15, "GetBatteryAgePercentage", psm_get_battery_age_percentage },
    { 16, "GetBatteryChargeInfoEvent", psm_get_battery_charge_info_event },
    { 17, "GetBatteryChargeInfoFields", psm_get_battery_charge_info_fields },
    { 18, "GetBatteryChargeCalibratedEvent", psm_get_battery_charge_calibrated_event },
};

static const rl_ipc_class psm_class = { "psm", psm_cmds, sizeof psm_cmds / sizeof psm_cmds[0], 0, NULL };

static rl_ipc_object *psm_factory(void) { return rl_srv_object(&psm_class, NULL); }

static void apm_reset(void)
{
    hos_revent *ev[2] = { g_apm.charge_info_event, g_apm.calibrated_event };
    for (int i = 0; i < 2; i++)
        if (ev[i])
            hos_kobj_unref(&ev[i]->obj);
    memset(&g_apm, 0, sizeof g_apm);
    g_apm.config[MODE_DEFAULT] = CONFIG_7;
    g_apm.config[MODE_BOOST] = CONFIG_8;
    g_apm.charging = true;
}

void rl_srv_install_apm(void)
{
    apm_reset();
    rl_ipc_register_service("apm", manager_factory);
    rl_ipc_register_service("apm:am", manager_factory);
    rl_ipc_register_service("apm:sys", system_factory);
    rl_ipc_register_service("apm:p", privileged_factory);
    rl_ipc_register_service("psm", psm_factory);
}
