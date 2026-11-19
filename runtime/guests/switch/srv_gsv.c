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
#include "am.h"
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
static rl_result initialize_syncpt_context(rl_ipc_req *r) { return rl_gsv_open_syncpt_session(r); }

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

/**
 * @brief UserSession 126 (unnamed): no input, one u32. SDK 22.2.8's
 * nn::gsv::BootWithRemoteMemory (sdk 0x4915a0) sends it right after command 4,
 * keeps the u32 only for one compare and sends 120 (no input or output) when it
 * is 1; it stores the value nowhere, and the boot succeeds either way. Answered
 * 0, the path without 120 (inferred: the console's value is not known, and no
 * SDK path reads anything that depends on it). 120 stays unimplemented.
 */
static rl_result boot_flag(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
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

/** @brief Native event slots a DisplaySession hands out (the client's slot index is below this; sdk 0x56133c). */
#define DISPLAY_EVENT_SLOTS 16

/** @brief State of a DisplaySession: the events of its native event slots. */
typedef struct display_session {
    hos_revent *slot[DISPLAY_EVENT_SLOTS];
} display_session;

static void display_session_destroy(void *state)
{
    display_session *s = state;
    for (uint32_t i = 0; i < DISPLAY_EVENT_SLOTS; i++)
        if (s->slot[i])
            hos_kobj_unref(&s->slot[i]->obj);
    free(s);
}

/** @brief The event of slot index of a DisplaySession, made on first use; NULL for an index past the slots. */
static hos_revent *display_slot(rl_ipc_req *r, uint32_t index)
{
    display_session *s = r->self->state;
    if (index >= DISPLAY_EVENT_SLOTS)
        return NULL;
    if (!s->slot[index])
        s->slot[index] = rl_hos_event_create();
    return s->slot[index];
}

/**
 * @brief DisplaySession 31 (unnamed): a u32 slot index; one moved handle, the readable end of the slot's event. The
 * slot is later bound to a display change by 263 to 271 (nn::vi's Create*Event).
 */
static rl_result display_acquire_slot(rl_ipc_req *r)
{
    uint32_t index;
    if (!rl_srv_in(r, &index, 4))
        return SF_RESULT_INVALID_HEADER;
    hos_revent *e = display_slot(r, index);
    if (!e)
        return SF_RESULT_INVALID_HEADER;
    return rl_ipc_push_move(r, &e->obj);
}

/**
 * @brief DisplaySession 263, 265, 267, 269 and 271 (nn::vi Create*Event for the refresh rate, HDR status, HDR paper
 * white, buffer max resolution and mode capability): a u64 display name and a u32 slot index. None of these change on
 * the host display, so the slot's event is signaled once, when it is bound, so the client reads the current value.
 * Inferred: Eglantine does the same; the console signals only on a change.
 */
static rl_result display_bind_change(rl_ipc_req *r)
{
    struct {
        uint64_t name;
        uint32_t index;
    } in;
    if (!rl_srv_in(r, &in, 12))
        return SF_RESULT_INVALID_HEADER;
    hos_revent *e = display_slot(r, in.index);
    if (!e)
        return SF_RESULT_INVALID_HEADER;
    rl_hos_event_signal(e);
    return KR_SUCCESS;
}

/** @brief DisplaySession 103 (nn::vi2::QueryDisplayRefreshRate): a u64 display name; a u32 rate in Hz and a bool. */
static rl_result display_query_refresh_rate(rl_ipc_req *r)
{
    uint64_t name;
    if (!rl_srv_in(r, &name, 8))
        return SF_RESULT_INVALID_HEADER;
    uint32_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    out[0] = 60;
    out[1] = 0;
    return KR_SUCCESS;
}

/**
 * @brief DisplaySession 104 (nn::vi2::QueryDisplayHdrStatus): a u64 display name; a u64 output format (the SDK takes 1
 * as SDR and 2 as HDR and fails the query on any other value) and a u16 peak luminance in nits. relift reports SDR
 * with a typical SDR panel's peak (inferred: Eglantine).
 */
static rl_result display_query_hdr_status(rl_ipc_req *r)
{
    uint64_t name;
    if (!rl_srv_in(r, &name, 8))
        return SF_RESULT_INVALID_HEADER;
    uint32_t *out = rl_ipc_out(r, 12);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    out[0] = 1;
    out[1] = 0;
    out[2] = 400;
    return KR_SUCCESS;
}

/** @brief DisplaySession 105 (nn::vi2::QueryDisplayHdrPaperWhite): a u64 display name; a u16 paper white in nits, the BT.2408 reference white (inferred: Eglantine). */
static rl_result display_query_paper_white(rl_ipc_req *r)
{
    uint64_t name;
    if (!rl_srv_in(r, &name, 8))
        return SF_RESULT_INVALID_HEADER;
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 203;
    return KR_SUCCESS;
}

/**
 * @brief DisplaySession 106 (nn::vi2::QueryDisplayBufferMaxResolution): a u64 display name; a u64 mode of the
 * highest buffer resolution (1 is 1280x720, 2 is 1920x1080, 4 is 2560x1440), 4 docked and 2 handheld (inferred:
 * Eglantine). The name must be "default".
 */
static rl_result display_query_buffer_max_resolution(rl_ipc_req *r)
{
    uint64_t name;
    if (!rl_srv_in(r, &name, 8))
        return SF_RESULT_INVALID_HEADER;
    if (name != UINT64_C(0x00746c7561666564))
        return SF_RESULT_NOT_SUPPORTED;
    uint64_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = rl_am_is_docked() ? 4 : 2;
    return KR_SUCCESS;
}

static const rl_ipc_cmd display_session_cmds[] = {
    { 1, "GetCount", display_count },
    { 3, "GetDisplayId", display_first_id },
    { 5, "GetTimestampRecorderCount", display_recorder_count },
    { 11, "GetEvent", display_event },
    { 21, "Command21", rl_srv_stub_ok },
    { 31, "GetEvent2", display_acquire_slot },
    { 32, "Command32", display_linked_event },
    { 51, "SetTimestampRecorderCount", rl_srv_stub_ok },
    { 101, "OpenDisplay", rl_srv_stub_ok },
    { 102, "CloseDisplay", rl_srv_stub_ok },
    { 103, "QueryDisplayRefreshRate", display_query_refresh_rate },
    { 104, "QueryDisplayHdrStatus", display_query_hdr_status },
    { 105, "QueryDisplayHdrPaperWhite", display_query_paper_white },
    { 106, "QueryDisplayBufferMaxResolution", display_query_buffer_max_resolution },
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
    { 263, "BindDisplayRefreshRateEvent", display_bind_change },
    { 264, "Command264", rl_srv_stub_ok },
    { 265, "BindDisplayHdrStatusEvent", display_bind_change },
    { 266, "Command266", rl_srv_stub_ok },
    { 267, "BindDisplayHdrPaperWhiteEvent", display_bind_change },
    { 268, "Command268", rl_srv_stub_ok },
    { 269, "BindDisplayBufferMaxResolutionEvent", display_bind_change },
    { 270, "Command270", rl_srv_stub_ok },
    { 271, "BindDisplayModeCapabilityEvent", display_bind_change },
    { 272, "Command272", rl_srv_stub_ok },
    { 301, "Command301", rl_srv_stub_ok },
    { 302, "Command302", rl_srv_stub_ok },
    { 351, "Command351", rl_srv_stub_ok },
    { 352, "Command352", rl_srv_stub_ok },
    { 353, "Command353", rl_srv_stub_ok },
};

static const rl_ipc_class display_session_class = { "nn::gsv::DisplaySession", display_session_cmds,
                                                    sizeof display_session_cmds / sizeof display_session_cmds[0], 0,
                                                    display_session_destroy };

/** @brief InitializeDisplayContext: u64 version (0x300010000 on 19.0.0, per switchbrew); a DisplaySession. */
static rl_result initialize_display_context(rl_ipc_req *r)
{
    uint64_t version;
    if (!rl_srv_in(r, &version, 8))
        return SF_RESULT_INVALID_HEADER;
    display_session *s = calloc(1, sizeof *s);
    if (!s)
        return KR_OUT_OF_RESOURCE;
    rl_ipc_object *o = rl_srv_object(&display_session_class, s);
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

/**
 * @brief ChatSession 1: no input; a u32 N, which nn::chat::gsv's session init
 * (SDK 20.5 sdk 0x639c30) requires to be at most 0x3ff, passes to 11 and
 * sizes a bitmap of (N + 32) / 32 words with. relift answers 0 (inferred:
 * Eglantine's nx2_gsv answers 0; no SDK path or document gives the console's
 * value, and 0 is within the SDK's bound).
 */
static rl_result chat_event_id(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

/**
 * @brief ChatSession 11: the u32 from 1; one handle, which the SDK attaches
 * as a readable system event (verified: sdk 0x63a100). relift gives an event
 * it never signals, as GameChat never runs here.
 */
static rl_result chat_event(rl_ipc_req *r)
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
 * @brief ChatSession 103: a u64 (the first eight bytes of the display name)
 * and a u32 slot; the result only (verified: nn::chat::gsv::OpenVirtualDisplay,
 * SDK 20.5 sdk 0x638380 into 0x63a604). A console not in a chat has no chat
 * displays to feed, so relift accepts it (inferred: nothing comes back to
 * the game; Eglantine accepts it the same way).
 */
static rl_result chat_open_virtual_display(rl_ipc_req *r)
{
    uint8_t in[12];
    return rl_srv_in(r, in, sizeof in) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

static const rl_ipc_cmd chat_session_cmds[] = {
    { 1, "GetEventId", chat_event_id },
    { 11, "GetEvent", chat_event },
    { 103, "OpenVirtualDisplay", chat_open_virtual_display },
};

static const rl_ipc_class chat_session_class = { "nn::gsv::ChatSession", chat_session_cmds,
                                                 sizeof chat_session_cmds / sizeof chat_session_cmds[0], 0, NULL };

/**
 * @brief InitializeChatContext (switchbrew's name): no input; one moved
 * session handle (verified: SDK 20.5 sdk 0x63a03c sends 1202 with no data
 * and keeps the handle at TLS+12 as the session it sends 11 and on to).
 */
static rl_result initialize_chat_context(rl_ipc_req *r) { return push_session(r, &chat_session_class); }

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

/**
 * @brief UserSession 1510 CalculateRequirements: a 0x40-byte inter-module
 * image preference and a u32 in; the image's u64 size and u64 alignment out.
 * The NVN2 driver's own nvnTextureBuilderGetStorageSize and
 * nvnTextureBuilderGetStorageAlignment ask it for display textures; the
 * client raises the alignment to at least 4 KiB and rounds the size up to it.
 * A block-linear preference holds (width - 1) | (height - 1) << 16 at word 2,
 * the log2 of a block's rows at word 4 and the row pitch in 64-byte GOB
 * columns at word 5. The size is the pitch times the height rounded up to
 * whole blocks, the layout every block-linear surface has. The alignment is
 * one block, at least 4 KiB (inferred: Eglantine, Lichii and Orchid all
 * answer it; the console's value is not known). A preference outside that
 * shape stops.
 */
static rl_result calculate_requirements(rl_ipc_req *r)
{
    uint32_t pref[16], count;
    if (!rl_srv_in(r, pref, sizeof pref) || !rl_srv_in(r, &count, 4))
        return SF_RESULT_INVALID_HEADER;
    uint64_t width = (pref[2] & 0xffffu) + 1, height = (pref[2] >> 16) + 1, rows_log2 = pref[4];
    uint64_t pitch = (uint64_t)pref[5] * 64;
    if (rows_log2 < 3 || rows_log2 > 8 || !pitch || pitch > 4096 * 64) {
        rl_ipc_stop(r, "gsv",
                    "UserSession 1510 CalculateRequirements for a %llux%llu image with block rows log2 %llu and pitch "
                    "0x%llx is not answered yet",
                    (unsigned long long)width, (unsigned long long)height, (unsigned long long)rows_log2,
                    (unsigned long long)pitch);
        return KR_SUCCESS;
    }
    uint64_t rows = (uint64_t)1 << rows_log2;
    uint64_t *out = rl_ipc_out(r, 16);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    out[0] = pitch * ((height + rows - 1) / rows * rows);
    out[1] = 64 * rows > 0x1000 ? 64 * rows : 0x1000;
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "gsv", "CalculateRequirements kind %u %llux%llu rows %llu pitch 0x%llx format %u count %u: "
               "size 0x%llx alignment 0x%llx", pref[1], (unsigned long long)width, (unsigned long long)height,
               (unsigned long long)rows, (unsigned long long)pitch, pref[6], count, (unsigned long long)out[0],
               (unsigned long long)out[1]);
    return KR_SUCCESS;
}

static const rl_ipc_cmd user_session_cmds[] = {
    { 1, "BootWithTransferMemory", boot_with_transfer_memory },
    { 4, "BootWithRemoteMemory", boot_with_remote_memory },
    { 11, "SetCurrentProcessHandle", set_current_process_handle },
    { 101, "CreateContext", rl_srv_stub_ok },
    { 115, "BootNativeMemoryManagement", boot_native_memory_management },
    { 126, "Cmd126", boot_flag },
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
    { 1202, "InitializeChatContext", initialize_chat_context },
    { 1203, "DestroyChatContext", rl_srv_stub_ok },
    { 1301, "CreateAdrawContext", rl_srv_stub_ok },
    { 1302, "InitializeAdrawContext", rl_srv_stub_ok },
    { 1402, "CreateNvDrv2UserSessionWithRemoteMemory", create_nvdrv2_user_session },
    { 1501, "CreateInterModuleImageContext", rl_srv_stub_ok },
    { 1502, "InitializeInterModuleImageContext", rl_srv_stub_ok },
    { 1510, "CalculateRequirements", calculate_requirements },
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
