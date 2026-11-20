/**
 * @file srv_nsd.c
 * @brief The network settings services nsd:u and nsd:a (nn::nsd::detail::IManager)
 * of a retail console that is offline. Command ids and layouts come from the
 * SDK's own client (the proxy that nn::nsd::detail::ipc calls); the retail
 * environment name "lp1" and the server environment type lp follow Eglantine
 * (GPL-3, src/core/hle/service/sockets/nsd.cpp) as values only. The commands a
 * game calls that this does not answer stop with relift's unknown command report.
 */
#include "services.h"

#include <string.h>

/** @brief nn::oe::ServerEnvironmentType lp, the retail environment. */
#define NSD_SERVER_ENVIRONMENT_LP 1
/** @brief Size of an nn::nsd::Fqdn, SettingName and Url buffer. */
#define NSD_NAME_SIZE 0x100

/** @brief Writes size bytes of src (zero padded to size) to the output buffer; the SDK reads a whole fixed size structure. */
static rl_result put_text(rl_ipc_req *r, const char *src, uint32_t size)
{
    uint8_t text[NSD_NAME_SIZE] = { 0 };
    size_t n = strlen(src);
    if (size > sizeof text || n >= size)
        return SF_RESULT_INVALID_HEADER;
    memcpy(text, src, n);
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    return rl_ipc_buf_write(&b, text, size) ? KR_SUCCESS : KR_INVALID_MEM_STATE;
}

/** @brief GetSettingUrl (5): an nn::nsd::Url (0x100 bytes); a console that uses the retail settings has no setting url, so it is empty (inferred). */
static rl_result get_setting_url(rl_ipc_req *r) { return put_text(r, "", NSD_NAME_SIZE); }

/** @brief GetSettingName (10): an nn::nsd::SettingName (0x100 bytes), "lp1" on a retail console (inferred from the environment identifier). */
static rl_result get_setting_name(rl_ipc_req *r) { return put_text(r, "lp1", NSD_NAME_SIZE); }

/** @brief GetEnvironmentIdentifier (11): an nn::nsd::EnvironmentIdentifier (char[8]), "lp1" on a retail console. */
static rl_result get_environment_identifier(rl_ipc_req *r) { return put_text(r, "lp1", 8); }

/** @brief GetDeviceId (12): an nn::nsd::DeviceId (16 raw bytes); only a console knows its own, so zeros (inferred). */
static rl_result get_device_id(rl_ipc_req *r)
{
    return rl_ipc_out(r, 16) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

/** @brief SetChangeEnvironmentIdentifierDisabled (15): a bool; retail firmware keeps the identifier fixed either way. */
static rl_result set_change_environment_identifier_disabled(rl_ipc_req *r)
{
    uint8_t disabled;
    return rl_srv_in(r, &disabled, 1) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

/**
 * @brief Copies the input Fqdn to the output Fqdn: a retail console with no setting overrides leaves a name as it is
 * (inferred: Eglantine's nsd.cpp answers the same).
 */
static rl_result resolve_name(rl_ipc_req *r)
{
    char name[NSD_NAME_SIZE];
    rl_ipc_buf in = rl_ipc_auto_in(r, 0);
    if (!rl_ipc_buf_read(&in, name, sizeof name))
        return SF_RESULT_INVALID_HEADER;
    name[sizeof name - 1] = 0;
    return put_text(r, name, NSD_NAME_SIZE);
}

/** @brief ResolveEx (21): Resolve, then a second result of the substitution (success) in the output data. */
static rl_result resolve_name_ex(rl_ipc_req *r)
{
    rl_result rc = resolve_name(r);
    if (rc != KR_SUCCESS)
        return rc;
    return rl_ipc_out(r, 4) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

/** @brief IsChangeEnvironmentIdentifierDisabled (63): a bool, false (inferred). */
static rl_result is_change_environment_identifier_disabled(rl_ipc_req *r)
{
    return rl_ipc_out(r, 1) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

/** @brief GetApplicationServerEnvironmentType (100): an nn::oe::ServerEnvironmentType byte, lp. */
static rl_result get_application_server_environment_type(rl_ipc_req *r)
{
    uint8_t *out = rl_ipc_out(r, 1);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = NSD_SERVER_ENVIRONMENT_LP;
    return KR_SUCCESS;
}

static const rl_ipc_cmd manager_cmds[] = {
    { 5, "GetSettingUrl", get_setting_url },
    { 10, "GetSettingName", get_setting_name },
    { 11, "GetEnvironmentIdentifier", get_environment_identifier },
    { 12, "GetDeviceId", get_device_id },
    { 15, "SetChangeEnvironmentIdentifierDisabled", set_change_environment_identifier_disabled },
    { 20, "Resolve", resolve_name },
    { 21, "ResolveEx", resolve_name_ex },
    { 63, "IsChangeEnvironmentIdentifierDisabled", is_change_environment_identifier_disabled },
    { 100, "GetApplicationServerEnvironmentType", get_application_server_environment_type },
};

static const rl_ipc_class manager_class = { "nn::nsd::detail::IManager", manager_cmds,
                                            sizeof manager_cmds / sizeof manager_cmds[0], 0, NULL };

static rl_ipc_object *manager_factory(void) { return rl_srv_object(&manager_class, NULL); }

void rl_srv_install_nsd(void)
{
    rl_ipc_register_service("nsd:u", manager_factory);
    rl_ipc_register_service("nsd:a", manager_factory);
}
