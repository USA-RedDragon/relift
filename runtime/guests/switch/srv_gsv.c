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

#include <stdlib.h>
#include <string.h>

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

/**
 * @brief SyncptSession 1 (unnamed): a u32 below 0x400 that the SDK passes
 * back, capped at 63, to command 21. Its meaning is not known; 0 here.
 */
static rl_result syncpt_cmd_1(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

/** @brief SyncptSession 21 (unnamed): takes the u32 from command 1. */
static rl_result syncpt_cmd_21(rl_ipc_req *r)
{
    uint32_t v;
    return rl_srv_in(r, &v, 4) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

static const rl_ipc_cmd syncpt_cmds[] = {
    { 0, "Ping", syncpt_ping },
    { 1, "Cmd1", syncpt_cmd_1 },
    { 21, "Cmd21", syncpt_cmd_21 },
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

/** @brief Header of the sync point area gsv 204 lays out in the second (read-only) transfer memory. */
typedef struct syncpt_header {
    /** @brief Layout version; the SDK aborts unless it is 1. */
    uint32_t version;
    uint32_t reserved;
    /** @brief Counters whose values the area holds; the SDK asks SyncptSession for the others. */
    uint32_t counters;
    /** @brief Offset in this area of the u64 value per counter (all ones: no value, a wait ends at once). */
    uint32_t values;
    /** @brief Offset in the first (writable) transfer memory of an area the client writes. */
    uint32_t client0;
    /** @brief Offset in this area of a per-counter bitmap. */
    uint32_t bitmap0;
    /** @brief Offset in the first transfer memory of a second client area. */
    uint32_t client1;
    /** @brief Offset in this area of a second per-counter bitmap. */
    uint32_t bitmap1;
} syncpt_header;

/**
 * @brief Command 204 (syncpt context, unnamed): transfer memories W (0x1000
 * bytes, writable) and R (0x2000 bytes, read-only, right after W), u32 1,
 * u32 0x1ff and their sizes; answers two events. The server lays out R:
 * a syncpt_header, then the tables it points at. Layout read from the
 * SDK's nn::syncpt::sdk, which reads counters below `counters` from R and
 * asks SyncptSession for the rest. relift's GPU signals no counter through
 * this area, so it holds none.
 */
static rl_result syncpt_cmd_204(rl_ipc_req *r)
{
    struct {
        uint32_t a, b;
        uint64_t size0, size1;
    } in;
    if (!rl_srv_in(r, &in, sizeof in) || r->n_in_copy < 2)
        return SF_RESULT_INVALID_HEADER;
    hos_lock();
    hos_tmem *ro = (hos_tmem *)(void *)hos_handle_get(r->in_copy[1], KOBJ_TRANSFER_MEMORY);
    hos_unlock();
    if (!ro)
        return KR_INVALID_HANDLE;
    rl_result rc = KR_SUCCESS;
    if (ro->size < 0x400) {
        rc = SF_RESULT_INVALID_HEADER;
    } else {
        uint8_t area[0x400];
        memset(area, 0, sizeof area);
        syncpt_header h = { 1, 0, 0, 0x100, 0, 0x200, 0x800, 0x300 };
        memcpy(area, &h, sizeof h);
        memset(area + h.values, 0xff, 8);
        rl_vm_kernel_write(&hos.vm, ro->addr, area, sizeof area);
    }
    hos_kobj_unref(&ro->obj);
    for (int i = 0; rc == KR_SUCCESS && i < 2; i++) {
        hos_revent *e = rl_hos_event_create();
        if (!e)
            return KR_OUT_OF_RESOURCE;
        rc = rl_ipc_push_copy(r, &e->obj);
        hos_kobj_unref(&e->obj);
    }
    return rc;
}

/**
 * @brief BootNativeMemoryManagement: takes the u64 MemoryBlockModeBits the
 * SDK asks for and answers the bits in effect, which nn::gs keeps and tests
 * (bit 1) before each native memory block. The modes asked for are granted.
 */
static rl_result boot_native_memory_management(rl_ipc_req *r)
{
    uint64_t bits;
    if (!rl_srv_in(r, &bits, 8))
        return SF_RESULT_INVALID_HEADER;
    uint64_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = bits;
    return KR_SUCCESS;
}

/** @brief The GpuSession user version the SDKs seen use (nn::gpu::impl::user::UserBootFunction::MyUserVersion). */
#define GPU_USER_VERSION 4u

/**
 * @brief GpuSession 11 (unnamed): no input; the lowest and highest user
 * versions the server takes, from which nn::gpu builds a mask of the
 * versions it may boot with. Only the one the SDK sends is taken.
 */
static rl_result gpu_user_versions(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    out[0] = GPU_USER_VERSION;
    out[1] = GPU_USER_VERSION;
    return KR_SUCCESS;
}

/** @brief A GpuSession: the event that tells its client the user session is ready. */
typedef struct gpu_session {
    hos_revent *ready;
} gpu_session;

static void gpu_session_destroy(void *state)
{
    gpu_session *s = state;
    if (s->ready)
        hos_kobj_unref(&s->ready->obj);
    free(s);
}

static const rl_ipc_class gpu_user_class = { "nn::gsv::GpuUserSession", NULL, 0, 0, NULL };

/**
 * @brief GpuSession 12 (unnamed): the u32 user version to boot with; the
 * readable end of an event (moved), which nn::gpu attaches to a system event
 * and waits on after command 21.
 */
static rl_result gpu_open_user(rl_ipc_req *r)
{
    uint32_t version;
    if (!rl_srv_in(r, &version, 4))
        return SF_RESULT_INVALID_HEADER;
    if (version != GPU_USER_VERSION)
        return SF_RESULT_NOT_SUPPORTED;
    gpu_session *s = r->self->state;
    if (!s->ready && !(s->ready = rl_hos_event_create()))
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_move(r, &s->ready->obj);
}

/** @brief GpuSession 21 (unnamed): the u32 user version; asks for its user session and signals the event of command 12 when it is ready. */
static rl_result gpu_request_user(rl_ipc_req *r)
{
    uint32_t version;
    if (!rl_srv_in(r, &version, 4))
        return SF_RESULT_INVALID_HEADER;
    gpu_session *s = r->self->state;
    if (version != GPU_USER_VERSION || !s->ready)
        return SF_RESULT_NOT_SUPPORTED;
    rl_hos_event_signal(s->ready);
    return KR_SUCCESS;
}

/** @brief GpuSession 22 (unnamed): no input; the user session command 21 readied. */
static rl_result gpu_take_user(rl_ipc_req *r) { return push_session(r, &gpu_user_class); }

static const rl_ipc_cmd gpu_session_cmds[] = {
    { 11, "GetUserVersionRange", gpu_user_versions },
    { 12, "OpenUser", gpu_open_user },
    { 21, "RequestUser", gpu_request_user },
    { 22, "TakeUser", gpu_take_user },
};

static const rl_ipc_class gpu_session_class = { "nn::gsv::GpuSession", gpu_session_cmds,
                                                sizeof gpu_session_cmds / sizeof gpu_session_cmds[0], 0,
                                                gpu_session_destroy };

/** @brief DisplaySession 3 (unnamed): no input; a u32 below 8 (sdk 0x560fe0 aborts otherwise), the display relift has: 0. */
static rl_result display_first_id(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

/** @brief DisplaySession 1 (unnamed): no input; a u32 below 0x400 (sdk 0x561288 aborts otherwise): 0. */
static rl_result display_count(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

/**
 * @brief DisplaySession 11 (unnamed): a u32; one moved handle (sdk 0x56133c),
 * which relift makes the readable end of an event it never signals.
 */
static rl_result display_event(rl_ipc_req *r)
{
    uint32_t id;
    if (!rl_srv_in(r, &id, 4))
        return SF_RESULT_INVALID_HEADER;
    hos_revent *e = rl_hos_event_create();
    if (!e)
        return KR_OUT_OF_RESOURCE;
    rl_result rc = rl_ipc_push_move(r, &e->obj);
    hos_kobj_unref(&e->obj);
    return rc;
}

/**
 * @brief DisplaySession 32 (unnamed): a u32 and a copied handle, the readable
 * end of a system event the client owns; one handle back, which the client
 * attaches as a readable system event (SDK 22 sdk 0x5e3af0, called through
 * slot +72 of the display client table by 0x5eaef0). relift gives the
 * readable end of an event it never signals, as for 11.
 */
static rl_result display_linked_event(rl_ipc_req *r)
{
    if (r->n_in_copy < 1)
        return SF_RESULT_INVALID_HEADER;
    return display_event(r);
}

/** @brief DisplaySession 212 (unnamed): a u64; a u64 (sdk 0x5620c4), relift: 0. */
static rl_result display_u64_zero(rl_ipc_req *r)
{
    uint64_t in;
    if (!rl_srv_in(r, &in, 8))
        return SF_RESULT_INVALID_HEADER;
    uint64_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

/**
 * @brief DisplaySession 5 (unnamed): no input; how many timestamp recorders
 * nn::vi2 may use, below 8 (sdk 0x562ea0; the pool at 0x5696a0 keeps at most
 * 7 and reports them all taken when it gets 0). relift: 7.
 */
static rl_result display_recorder_count(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 7;
    return KR_SUCCESS;
}

static const rl_ipc_cmd display_session_cmds[] = {
    { 1, "GetCount", display_count },
    { 3, "GetDisplayId", display_first_id },
    { 5, "GetTimestampRecorderCount", display_recorder_count },
    { 11, "GetEvent", display_event },
    { 21, "Command21", rl_srv_stub_ok },
    { 31, "GetEvent2", display_event },
    { 32, "Command32", display_linked_event },
    { 51, "SetTimestampRecorderCount", rl_srv_stub_ok },
    { 101, "OpenDisplay", rl_srv_stub_ok },
    { 102, "CloseDisplay", rl_srv_stub_ok },
    { 111, "Command111", rl_srv_stub_ok },
    { 112, "Command112", rl_srv_stub_ok },
    { 113, "Command113", rl_srv_stub_ok },
    { 201, "Command201", rl_srv_stub_ok },
    { 202, "Command202", rl_srv_stub_ok },
    { 212, "Command212", display_u64_zero },
    { 251, "Command251", rl_srv_stub_ok },
    { 252, "Command252", rl_srv_stub_ok },
    { 261, "Command261", rl_srv_stub_ok },
    { 262, "Command262", rl_srv_stub_ok },
    { 263, "Command263", rl_srv_stub_ok },
    { 264, "Command264", rl_srv_stub_ok },
    { 265, "Command265", rl_srv_stub_ok },
    { 266, "Command266", rl_srv_stub_ok },
    { 267, "Command267", rl_srv_stub_ok },
    { 268, "Command268", rl_srv_stub_ok },
    { 269, "Command269", rl_srv_stub_ok },
    { 270, "Command270", rl_srv_stub_ok },
    { 271, "Command271", rl_srv_stub_ok },
    { 272, "Command272", rl_srv_stub_ok },
    { 301, "Command301", rl_srv_stub_ok },
    { 302, "Command302", rl_srv_stub_ok },
    { 351, "Command351", rl_srv_stub_ok },
    { 352, "Command352", rl_srv_stub_ok },
    { 353, "Command353", rl_srv_stub_ok },
};

static const rl_ipc_class display_session_class = { "nn::gsv::DisplaySession", display_session_cmds,
                                                    sizeof display_session_cmds / sizeof display_session_cmds[0], 0,
                                                    NULL };

/** @brief InitializeDisplayContext: u64 version (0x300010000 on 19.0.0, per switchbrew); a DisplaySession. */
static rl_result initialize_display_context(rl_ipc_req *r)
{
    uint64_t version;
    if (!rl_srv_in(r, &version, 8))
        return SF_RESULT_INVALID_HEADER;
    return push_session(r, &display_session_class);
}

/** @brief CreateGpuSessionWithRemoteMemory: u64 version; a GpuSession. */
static rl_result create_gpu_session(rl_ipc_req *r)
{
    uint64_t version;
    if (!rl_srv_in(r, &version, 8))
        return SF_RESULT_INVALID_HEADER;
    gpu_session *s = calloc(1, sizeof *s);
    if (!s)
        return KR_OUT_OF_RESOURCE;
    rl_ipc_object *o = rl_srv_object(&gpu_session_class, s);
    if (!o) {
        free(s);
        return KR_OUT_OF_RESOURCE;
    }
    uint32_t h;
    rl_result rc = rl_ipc_session_open(o, &h);
    rl_ipc_object_unref(o);
    if (rc == KR_SUCCESS)
        r->out_move[r->n_out_move++] = h;
    return rc;
}

static const rl_ipc_cmd user_session_cmds[] = {
    { 1, "BootWithTransferMemory", boot_with_transfer_memory },
    { 4, "BootWithRemoteMemory", boot_with_remote_memory },
    { 11, "SetCurrentProcessHandle", set_current_process_handle },
    { 101, "CreateContext", rl_srv_stub_ok },
    { 115, "BootNativeMemoryManagement", boot_native_memory_management },
    { 201, "CreateSyncptContext", rl_srv_stub_ok },
    { 202, "InitializeSyncptContext", initialize_syncpt_context },
    { 204, "Cmd204", syncpt_cmd_204 },
    { 301, "CreateDisplayContext", rl_srv_stub_ok },
    { 303, "DestroyDisplayContext", rl_srv_stub_ok },
    { 305, "InitializeDisplayContext", initialize_display_context },
    { 401, "CreateGpuContext", rl_srv_stub_ok },
    { 402, "InitializeGpuContext", rl_srv_stub_ok },
    { 403, "DestroyGpuContext", rl_srv_stub_ok },
    { 601, "CreateCameraContext", rl_srv_stub_ok },
    { 603, "DestroyCameraContext", rl_srv_stub_ok },
    { 901, "CreateVideoDecoderContext", rl_srv_stub_ok },
    { 903, "DestroyVideoDecoderContext", rl_srv_stub_ok },
    { 497, "CreateGpuSessionWithRemoteMemory", create_gpu_session },
    { 1001, "CreateVideoEncoderContext", rl_srv_stub_ok },
    { 1003, "DestroyVideoEncoderContext", rl_srv_stub_ok },
    { 1101, "CreateGraphicsFrameContext", rl_srv_stub_ok },
    { 1102, "InitializeGraphicsFrameContext", rl_srv_stub_ok },
    { 1111, "SetGraphicsFrameLengthMode", rl_srv_stub_ok },
    { 1112, "SetGraphicsFrameRestartMode", rl_srv_stub_ok },
    { 1113, "SetGraphicsFrameGpuTimesliceMode", rl_srv_stub_ok },
    { 1201, "CreateChatContext", rl_srv_stub_ok },
    { 1203, "DestroyChatContext", rl_srv_stub_ok },
    { 1301, "CreateAdrawContext", rl_srv_stub_ok },
    { 1302, "InitializeAdrawContext", rl_srv_stub_ok },
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
