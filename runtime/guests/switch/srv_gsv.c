/**
 * @file srv_gsv.c
 * @brief The Switch 2's graphics supervisor (gsv): the TIPC UserSession an
 * application gets from am (ISelfController 2000 GetGraphicsSupervisor).
 * It multiplexes the boot of the graphics system and the contexts of its
 * clients (sync points, display, GPU, NvDrv2, camera, codecs).
 *
 * Command ids and arguments follow switchbrew.org (Switch 2: GSV services).
 * relift's GPU is reached through the nvn layer, not through the
 * supervisor's GPU sessions, so the supervisor only has to boot and accept
 * the contexts its clients create.
 */
#include "services.h"

/** @brief BootWithTransferMemory: transfer memory handle, aruid, size. */
static rl_result boot_with_transfer_memory(rl_ipc_req *r)
{
    uint64_t in[2];
    if (!rl_srv_in(r, in, sizeof in) || r->n_in_copy < 1)
        return SF_RESULT_INVALID_HEADER;
    return KR_SUCCESS;
}

/** @brief BootWithRemoteMemory: process handle, aruid and five u64s (version, three zero, remote memory usage). */
static rl_result boot_with_remote_memory(rl_ipc_req *r)
{
    uint64_t in[6];
    if (!rl_srv_in(r, in, sizeof in) || r->n_in_copy < 1)
        return SF_RESULT_INVALID_HEADER;
    return KR_SUCCESS;
}

static rl_result set_current_process_handle(rl_ipc_req *r)
{
    return r->n_in_copy < 1 ? SF_RESULT_INVALID_HEADER : KR_SUCCESS;
}

/** @brief CreateNvDrv2UserSessionWithRemoteMemory: no input, an INvDrv2User session. */
static rl_result create_nvdrv2_user_session(rl_ipc_req *r)
{
    uint32_t h;
    rl_result rc = rl_srv_nv2_open(&h);
    if (rc == KR_SUCCESS)
        r->out_move[r->n_out_move++] = h;
    return rc;
}

/** @brief SyncptSession Ping: answers the u64 it gets (the SDK sends the system tick). */
static rl_result syncpt_ping(rl_ipc_req *r)
{
    uint64_t v;
    if (!rl_srv_in(r, &v, 8))
        return SF_RESULT_INVALID_HEADER;
    uint64_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = v;
    return KR_SUCCESS;
}

static const rl_ipc_cmd syncpt_cmds[] = {
    { 0, "Ping", syncpt_ping },
};

static const rl_ipc_class syncpt_class = { "nn::gsv::SyncptSession", syncpt_cmds,
                                           sizeof syncpt_cmds / sizeof syncpt_cmds[0], 0, NULL };

static rl_result push_session(rl_ipc_req *r, const rl_ipc_class *cls)
{
    rl_ipc_object *o = rl_srv_object(cls, NULL);
    if (!o)
        return KR_OUT_OF_RESOURCE;
    uint32_t h;
    rl_result rc = rl_ipc_session_open(o, &h);
    rl_ipc_object_unref(o);
    if (rc == KR_SUCCESS)
        r->out_move[r->n_out_move++] = h;
    return rc;
}

/** @brief InitializeSyncptContext: no input, a SyncptSession. */
static rl_result initialize_syncpt_context(rl_ipc_req *r) { return push_session(r, &syncpt_class); }

/**
 * @brief Command 204 (syncpt context, unnamed): two transfer memories (the
 * SDK lends 0x1000 bytes read-write and 0x2000 read-only), u32 1, u32
 * 0x1ff and their two sizes; answers two handles. The SDK keeps them as
 * events of the sync point context, so they are readable events here,
 * never signaled until relift's sync points use them.
 */
static rl_result syncpt_cmd_204(rl_ipc_req *r)
{
    struct {
        uint32_t a, b;
        uint64_t size0, size1;
    } in;
    if (!rl_srv_in(r, &in, sizeof in) || r->n_in_copy < 2)
        return SF_RESULT_INVALID_HEADER;
    for (int i = 0; i < 2; i++) {
        hos_revent *e = rl_hos_event_create();
        if (!e)
            return KR_OUT_OF_RESOURCE;
        rl_result rc = rl_ipc_push_copy(r, &e->obj);
        hos_kobj_unref(&e->obj);
        if (rc != KR_SUCCESS)
            return rc;
    }
    return KR_SUCCESS;
}

static const rl_ipc_cmd user_session_cmds[] = {
    { 1, "BootWithTransferMemory", boot_with_transfer_memory },
    { 4, "BootWithRemoteMemory", boot_with_remote_memory },
    { 11, "SetCurrentProcessHandle", set_current_process_handle },
    { 101, "CreateContext", rl_srv_stub_ok },
    { 201, "CreateSyncptContext", rl_srv_stub_ok },
    { 202, "InitializeSyncptContext", initialize_syncpt_context },
    { 204, "Cmd204", syncpt_cmd_204 },
    { 1402, "CreateNvDrv2UserSessionWithRemoteMemory", create_nvdrv2_user_session },
    { 1501, "CreateInterModuleImageContext", rl_srv_stub_ok },
    { 1502, "InitializeInterModuleImageContext", rl_srv_stub_ok },
};

static const rl_ipc_class user_session_class = { "nn::gsv::UserSession", user_session_cmds,
                                                 sizeof user_session_cmds / sizeof user_session_cmds[0], 0, NULL };

rl_result rl_gsv_open_user_session(uint32_t *handle)
{
    rl_ipc_object *o = rl_srv_object(&user_session_class, NULL);
    if (!o)
        return KR_OUT_OF_RESOURCE;
    rl_result rc = rl_ipc_session_open(o, handle);
    rl_ipc_object_unref(o);
    return rc;
}
