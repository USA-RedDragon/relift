/**
 * @file srv_ectx.c
 * @brief The error context writer ectx:aw (nn::err::context::IWriterForApplication)
 * and the alarm notifications notif:a. Nothing reads an error context back
 * here, and no alarm ever fires, so both keep no state.
 *
 * Commands follow switchbrew.org (Glue services) and Ryujinx (MIT,
 * Copyright (c) Ryujinx Team and Contributors) Services/Ectx and
 * Services/Notification: IContextRegistrar Complete answers descriptor 0,
 * ListAlarmSettings answers no alarms.
 */
#include "services.h"

static rl_result registrar_complete(rl_ipc_req *r)
{
    int32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

static const rl_ipc_cmd registrar_cmds[] = {
    { 0, "Complete", registrar_complete },
};

static const rl_ipc_class registrar_class = { "nn::err::context::IContextRegistrar", registrar_cmds,
                                              sizeof registrar_cmds / sizeof registrar_cmds[0], 0, NULL };

static rl_result create_context_registrar(rl_ipc_req *r)
{
    rl_ipc_object *o = rl_srv_object(&registrar_class, NULL);
    return o ? rl_ipc_push_object(r, o) : KR_OUT_OF_RESOURCE;
}

static const rl_ipc_cmd writer_cmds[] = {
    { 0, "CreateContextRegistrar", create_context_registrar },
    { 1, "CommitContext", rl_srv_stub_ok },
};

static const rl_ipc_class writer_class = { "ectx:aw", writer_cmds, sizeof writer_cmds / sizeof writer_cmds[0], 0, NULL };

static rl_ipc_object *writer_factory(void) { return rl_srv_object(&writer_class, NULL); }

static rl_result list_alarm_settings(rl_ipc_req *r)
{
    int32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

static const rl_ipc_cmd notif_cmds[] = {
    { 520, "ListAlarmSettings", list_alarm_settings },
    { 1000, "Initialize", rl_srv_stub_ok },
};

static const rl_ipc_class notif_class = { "notif:a", notif_cmds, sizeof notif_cmds / sizeof notif_cmds[0], 0, NULL };

static rl_ipc_object *notif_factory(void) { return rl_srv_object(&notif_class, NULL); }

void rl_srv_install_ectx(void)
{
    rl_ipc_register_service("ectx:aw", writer_factory);
    rl_ipc_register_service("notif:a", notif_factory);
}
