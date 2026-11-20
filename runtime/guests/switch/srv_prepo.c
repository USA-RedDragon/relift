/**
 * @file srv_prepo.c
 * @brief The play report services prepo:u, prepo:a, prepo:s and prepo:m
 * (nn::prepo::detail::ipc::IPrepoService) of a console that is offline: a
 * saved report is kept for a later upload, which never happens, so saving
 * discards it and succeeds. Command ids come from the SDK's own client (the
 * nn::prepo proxy table); the transmission status (0) and system session id
 * (0) follow Eglantine (GPL-3, src/core/hle/service/prepo/prepo.cpp) as
 * values only. The commands a game calls that this does not answer stop with
 * relift's unknown command report.
 */
#include "services.h"

/** @brief GetTransmissionStatus (10300): an i32 status, 0, nothing is being sent. */
static rl_result get_transmission_status(rl_ipc_req *r)
{
    return rl_ipc_out(r, 4) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

/** @brief GetSystemSessionId (10400): a u64 id, 0 (Eglantine). */
static rl_result get_system_session_id(rl_ipc_req *r)
{
    return rl_ipc_out(r, 8) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

static const rl_ipc_cmd prepo_cmds[] = {
    { 10100, "SaveReportOld", rl_srv_stub_ok },
    { 10101, "SaveReportWithUserOld", rl_srv_stub_ok },
    { 10102, "SaveReportOld2", rl_srv_stub_ok },
    { 10103, "SaveReportWithUserOld2", rl_srv_stub_ok },
    { 10104, "SaveReportOld3", rl_srv_stub_ok },
    { 10105, "SaveReportWithUserOld3", rl_srv_stub_ok },
    { 10106, "SaveReport", rl_srv_stub_ok },
    { 10107, "SaveReportWithUser", rl_srv_stub_ok },
    { 10200, "RequestImmediateTransmission", rl_srv_stub_ok },
    { 10300, "GetTransmissionStatus", get_transmission_status },
    { 10400, "GetSystemSessionId", get_system_session_id },
    { 20100, "SaveSystemReport", rl_srv_stub_ok },
    { 20101, "SaveSystemReportWithUser", rl_srv_stub_ok },
    { 20102, "SaveSystemReport2", rl_srv_stub_ok },
    { 20103, "SaveSystemReportWithUser2", rl_srv_stub_ok },
};

static const rl_ipc_class prepo_class = { "nn::prepo::detail::ipc::IPrepoService", prepo_cmds,
                                          sizeof prepo_cmds / sizeof prepo_cmds[0], 0, NULL };

static rl_ipc_object *prepo_factory(void) { return rl_srv_object(&prepo_class, NULL); }

void rl_srv_install_prepo(void)
{
    rl_ipc_register_service("prepo:u", prepo_factory);
    rl_ipc_register_service("prepo:a", prepo_factory);
    rl_ipc_register_service("prepo:s", prepo_factory);
    rl_ipc_register_service("prepo:m", prepo_factory);
}
