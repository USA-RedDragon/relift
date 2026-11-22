/**
 * @file srv_am_app.c
 * @brief IApplicationFunctions, IHomeMenuFunctions, ILibraryAppletSelfAccessor
 * and IProcessWindingController.
 *
 * Command ids and behavior follow Ryujinx (MIT, Copyright (c) Ryujinx Team
 * and Contributors) and libnx (ISC, Copyright (c) libnx Authors).
 */
#include "srv_am_priv.h"

#include <pthread.h>
#include <stdlib.h>

#define LAUNCH_KIND_USER_CHANNEL 1u
#define LAUNCH_KIND_PRESELECTED_USER 2u
#define LAUNCH_PARAMS_SIZE 0x88u
#define LAUNCH_PARAMS_MAGIC 0xC79497CAu
#define USER_CHANNEL_MAX 16
#define DEFAULT_SAVE_SIZE 200000000ll
#define PROGRAM_EXECUTE 0u
#define PROGRAM_RESTART 2u
#define FB_MAX_W 1280
#define FB_MAX_H 720
#define APPLET_ID_QLAUNCH 0x03u
#define QLAUNCH_TITLE 0x0100000000001000ull

typedef struct am_uid {
    uint64_t v[2];
} am_uid;

static pthread_mutex_t app_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint8_t *data;
    uint64_t size;
} user_channel[USER_CHANNEL_MAX];
static uint32_t user_channel_count;
static int32_t program_index;
static int32_t previous_program_index = -1;

static void user_channel_clear_locked(void)
{
    for (uint32_t i = 0; i < user_channel_count; i++)
        free(user_channel[i].data);
    user_channel_count = 0;
}

void am_app_reset(void)
{
    pthread_mutex_lock(&app_mu);
    user_channel_clear_locked();
    program_index = 0;
    previous_program_index = -1;
    pthread_mutex_unlock(&app_mu);
}

enum { EV_GPU_ERROR, EV_FRIEND_INVITATION, EV_NOTIFICATION, EV_HEALTH_WARNING, EV_UNKNOWN, EV_COUNT };

typedef struct af_state {
    hos_revent *events[EV_COUNT];
    int64_t save_size;
    int64_t journal_size;
    bool game_play_recording;
} af_state;

static void af_destroy(void *state)
{
    af_state *s = state;
    for (uint32_t i = 0; i < EV_COUNT; i++)
        if (s->events[i])
            hos_kobj_unref(&s->events[i]->obj);
    free(s);
}

static af_state *af(const rl_ipc_req *r) { return r->self->state; }

static rl_result af_pop_launch_parameter(rl_ipc_req *r)
{
    AM_IN(uint32_t, kind);
    rl_ipc_object *st;
    if (kind == LAUNCH_KIND_USER_CHANNEL) {
        pthread_mutex_lock(&app_mu);
        uint8_t *data = NULL;
        uint64_t size = 0;
        if (user_channel_count) {
            user_channel_count--;
            data = user_channel[user_channel_count].data;
            size = user_channel[user_channel_count].size;
        }
        pthread_mutex_unlock(&app_mu);
        if (!data)
            return AM_NOT_AVAILABLE;
        st = am_storage_new(data, size);
        free(data);
    } else if (kind == LAUNCH_KIND_PRESELECTED_USER) {
        uint8_t params[LAUNCH_PARAMS_SIZE] = { 0 };
        uint32_t magic = LAUNCH_PARAMS_MAGIC, selected = 1;
        uint64_t uid[2];
        am_preselected_user(&uid[0], &uid[1]);
        memcpy(params, &magic, 4);
        memcpy(params + 4, &selected, 4);
        memcpy(params + 8, uid, 16);
        st = am_storage_new(params, sizeof params);
    } else {
        return AM_OBJECT_INVALID;
    }
    return am_push_object(r, st);
}

static rl_result af_create_application_and_request_to_start(rl_ipc_req *r)
{
    AM_IN(uint64_t, title_id);
    return title_id == 0 ? KR_SUCCESS : AM_NOT_IMPLEMENTED;
}

static rl_result af_ensure_save_data(rl_ipc_req *r)
{
    AM_IN(am_uid, uid);
    *(uint64_t *)rl_ipc_out(r, 8) = 0;
    return KR_SUCCESS;
}

static rl_result af_get_desired_language(rl_ipc_req *r)
{
    *(uint64_t *)rl_ipc_out(r, 8) = am_language_code();
    return KR_SUCCESS;
}

static rl_result af_set_terminate_result(rl_ipc_req *r)
{
    AM_IN(uint32_t, result);
    return KR_SUCCESS;
}

/**
 * @brief GetDisplayVersion: the 16-byte display version field of the program's control data (a NUL padded string).
 * Without one the answer is 16 zero bytes, the console's answer for an empty field; the first such call logs that
 * the version is unknown and names the setting that supplies it.
 */
static rl_result af_get_display_version(rl_ipc_req *r)
{
    static bool warned;
    uint8_t *out = rl_ipc_out(r, 16);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    if (!am_display_version(out) && !warned) {
        warned = true;
        rl_log(RL_LOG_WARN, "am",
               "the game's display version is unknown: its control data holds none; set display_version in the [game] "
               "table of relift.toml");
    }
    return KR_SUCCESS;
}

typedef struct save_req {
    uint64_t type;
    uint64_t uid[2];
} save_req;

typedef struct extend_req {
    save_req save;
    int64_t size;
    int64_t journal;
} extend_req;

static rl_result af_extend_save_data(rl_ipc_req *r)
{
    AM_IN(extend_req, q);
    af(r)->save_size = q.size;
    af(r)->journal_size = q.journal;
    *(uint64_t *)rl_ipc_out(r, 8) = 0;
    return KR_SUCCESS;
}

static rl_result af_get_save_data_size(rl_ipc_req *r)
{
    AM_IN(save_req, q);
    int64_t *o = rl_ipc_out(r, 16);
    o[0] = af(r)->save_size;
    o[1] = af(r)->journal_size;
    return KR_SUCCESS;
}

static rl_result af_create_cache_storage(rl_ipc_req *r)
{
    struct {
        uint16_t index;
        uint16_t pad[3];
        int64_t size;
        int64_t journal;
    } q;
    if (!rl_srv_in(r, &q, sizeof q))
        return SF_RESULT_INVALID_HEADER;
    rl_ipc_out(r, 16);
    return KR_SUCCESS;
}

static rl_result af_get_save_data_size_max(rl_ipc_req *r)
{
    int64_t *o = rl_ipc_out(r, 16);
    o[0] = af(r)->save_size;
    o[1] = af(r)->journal_size;
    return KR_SUCCESS;
}

static rl_result af_begin_blocking_home_button(rl_ipc_req *r)
{
    AM_IN(uint64_t, ns);
    return KR_SUCCESS;
}

static rl_result af_notify_running(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = 1;
    return KR_SUCCESS;
}

static rl_result af_get_pseudo_device_id(rl_ipc_req *r)
{
    rl_ipc_out(r, 16);
    return KR_SUCCESS;
}

static rl_result af_set_media_playback_state(rl_ipc_req *r)
{
    AM_IN(uint8_t, enabled);
    return KR_SUCCESS;
}

static rl_result af_is_game_play_recording_supported(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = af(r)->game_play_recording ? 1 : 0;
    return KR_SUCCESS;
}

static rl_result af_set_game_play_recording_state(rl_ipc_req *r)
{
    AM_IN(int32_t, state);
    af(r)->game_play_recording = state != 0;
    return KR_SUCCESS;
}

static rl_result af_enable_application_crash_report(rl_ipc_req *r)
{
    AM_IN(uint8_t, enabled);
    return KR_SUCCESS;
}

static rl_result af_initialize_copyright_frame_buffer(rl_ipc_req *r)
{
    struct {
        int32_t width;
        int32_t height;
        uint64_t size;
    } q;
    if (!rl_srv_in(r, &q, sizeof q))
        return SF_RESULT_INVALID_HEADER;
    if ((q.size & 0x3FFFF) != 0 || q.width > FB_MAX_W || q.height > FB_MAX_H)
        return AM_INVALID_PARAMETERS;
    return KR_SUCCESS;
}

static rl_result af_set_copyright_image(rl_ipc_req *r)
{
    struct {
        int32_t x, y, width, height;
        uint32_t origin;
    } q;
    if (!rl_srv_in(r, &q, sizeof q))
        return SF_RESULT_INVALID_HEADER;
    if ((q.x | q.y) < 0 || q.width < 1 || q.height < 1)
        return AM_INVALID_PARAMETERS;
    return KR_SUCCESS;
}

static rl_result af_set_copyright_visibility(rl_ipc_req *r)
{
    AM_IN(uint8_t, visible);
    return KR_SUCCESS;
}

static void zero_out_buffer(const rl_ipc_buf *b)
{
    static const uint8_t zeros[256];
    for (uint64_t off = 0; off < b->size; off += sizeof zeros) {
        uint64_t n = b->size - off < sizeof zeros ? b->size - off : sizeof zeros;
        rl_ipc_buf part = { b->addr + off, n, b->attr };
        if (!rl_ipc_buf_write(&part, zeros, n))
            return;
    }
}

static rl_result af_query_play_statistics(rl_ipc_req *r)
{
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    zero_out_buffer(&out);
    *(int32_t *)rl_ipc_out(r, 4) = 0;
    return KR_SUCCESS;
}

static rl_result af_query_play_statistics_by_uid(rl_ipc_req *r)
{
    AM_IN(am_uid, uid);
    return af_query_play_statistics(r);
}

static rl_result af_execute_program(rl_ipc_req *r)
{
    struct {
        uint32_t kind;
        uint32_t pad;
        uint64_t value;
    } q;
    if (!rl_srv_in(r, &q, sizeof q))
        return SF_RESULT_INVALID_HEADER;
    if (q.kind != PROGRAM_EXECUTE && q.kind != PROGRAM_RESTART)
        return AM_NOT_IMPLEMENTED;
    pthread_mutex_lock(&app_mu);
    previous_program_index = program_index;
    if (q.kind == PROGRAM_EXECUTE)
        program_index = (int32_t)q.value;
    pthread_mutex_unlock(&app_mu);
    return KR_SUCCESS;
}

static rl_result af_clear_user_channel(rl_ipc_req *r)
{
    (void)r;
    pthread_mutex_lock(&app_mu);
    user_channel_clear_locked();
    pthread_mutex_unlock(&app_mu);
    return KR_SUCCESS;
}

static rl_result af_unpop_to_user_channel(rl_ipc_req *r)
{
    if (r->n_in_objects != 1 || !am_storage_is(r->in_objects[0]))
        return SF_RESULT_INVALID_IN_OBJECT;
    uint64_t size = am_storage_size(r->in_objects[0]);
    uint8_t *copy = malloc(size ? size : 1);
    if (!copy)
        return KR_OUT_OF_RESOURCE;
    if (!am_storage_read(r->in_objects[0], 0, copy, size)) {
        free(copy);
        return AM_INVALID_STORAGE_TYPE;
    }
    pthread_mutex_lock(&app_mu);
    if (user_channel_count == USER_CHANNEL_MAX) {
        free(user_channel[0].data);
        memmove(&user_channel[0], &user_channel[1], sizeof user_channel[0] * (USER_CHANNEL_MAX - 1));
        user_channel_count--;
    }
    user_channel[user_channel_count].data = copy;
    user_channel[user_channel_count].size = size;
    user_channel_count++;
    pthread_mutex_unlock(&app_mu);
    return KR_SUCCESS;
}

static rl_result af_get_previous_program_index(rl_ipc_req *r)
{
    pthread_mutex_lock(&app_mu);
    int32_t v = previous_program_index;
    pthread_mutex_unlock(&app_mu);
    *(int32_t *)rl_ipc_out(r, 4) = v;
    return KR_SUCCESS;
}

static rl_result af_event(rl_ipc_req *r, uint32_t which) { return am_push_event(r, af(r)->events[which]); }
static rl_result af_get_gpu_error_event(rl_ipc_req *r) { return af_event(r, EV_GPU_ERROR); }
static rl_result af_get_friend_invitation_event(rl_ipc_req *r) { return af_event(r, EV_FRIEND_INVITATION); }
static rl_result af_get_notification_event(rl_ipc_req *r) { return af_event(r, EV_NOTIFICATION); }
static rl_result af_get_health_warning_event(rl_ipc_req *r) { return af_event(r, EV_HEALTH_WARNING); }
static rl_result af_get_unknown_event(rl_ipc_req *r) { return af_event(r, EV_UNKNOWN); }

/**
 * @brief Command 320 (20.0.0+, unnamed): no input, two output bytes the SDK
 * hands back as they come. Their meaning is not known; both are zero.
 */
static rl_result af_cmd_320(rl_ipc_req *r)
{
    uint8_t *out = rl_ipc_out(r, 2);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    out[0] = out[1] = 0;
    return KR_SUCCESS;
}

/** @brief Command 321 (Switch 2, unnamed): no input, one output bool, false here. */
static rl_result af_cmd_321(rl_ipc_req *r)
{
    if (!hos_switch2())
        return SF_RESULT_UNKNOWN_COMMAND;
    uint8_t *out = rl_ipc_out(r, 1);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

/**
 * @brief IsLanguageSelectionLimited (330, [20.2.0+], per switchbrew): no input,
 * one bool (nn::oe::IsLanguageSelectionLimited reads one byte). Answered false
 * (inferred: Eglantine answers 0; no document gives the console's value, and a
 * console with every language selectable is the usual case).
 */
static rl_result af_is_language_selection_limited(rl_ipc_req *r)
{
    uint8_t *out = rl_ipc_out(r, 1);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

static rl_result af_try_pop_from_friend_invitation_channel(rl_ipc_req *r)
{
    (void)r;
    return AM_NOT_AVAILABLE;
}

static const rl_ipc_cmd af_cmds[] = {
    { 1, "PopLaunchParameter", af_pop_launch_parameter },
    { 12, "CreateApplicationAndRequestToStart", af_create_application_and_request_to_start },
    { 20, "EnsureSaveData", af_ensure_save_data },
    { 21, "GetDesiredLanguage", af_get_desired_language },
    { 22, "SetTerminateResult", af_set_terminate_result },
    { 23, "GetDisplayVersion", af_get_display_version },
    { 25, "ExtendSaveData", af_extend_save_data },
    { 26, "GetSaveDataSize", af_get_save_data_size },
    { 27, "CreateCacheStorage", af_create_cache_storage },
    { 28, "GetSaveDataSizeMax", af_get_save_data_size_max },
    RL_SRV_STUB(30, "BeginBlockingHomeButtonShortAndLongPressed"),
    RL_SRV_STUB(31, "EndBlockingHomeButtonShortAndLongPressed"),
    { 32, "BeginBlockingHomeButton", af_begin_blocking_home_button },
    RL_SRV_STUB(33, "EndBlockingHomeButton"),
    { 40, "NotifyRunning", af_notify_running },
    { 50, "GetPseudoDeviceId", af_get_pseudo_device_id },
    { 60, "SetMediaPlaybackStateForApplication", af_set_media_playback_state },
    { 65, "IsGamePlayRecordingSupported", af_is_game_play_recording_supported },
    RL_SRV_STUB(66, "InitializeGamePlayRecording"),
    { 67, "SetGamePlayRecordingState", af_set_game_play_recording_state },
    { 90, "EnableApplicationCrashReport", af_enable_application_crash_report },
    { 100, "InitializeApplicationCopyrightFrameBuffer", af_initialize_copyright_frame_buffer },
    { 101, "SetApplicationCopyrightImage", af_set_copyright_image },
    { 102, "SetApplicationCopyrightVisibility", af_set_copyright_visibility },
    { 110, "QueryApplicationPlayStatistics", af_query_play_statistics },
    { 111, "QueryApplicationPlayStatisticsByUid", af_query_play_statistics_by_uid },
    { 120, "ExecuteProgram", af_execute_program },
    { 121, "ClearUserChannel", af_clear_user_channel },
    { 122, "UnpopToUserChannel", af_unpop_to_user_channel },
    { 123, "GetPreviousProgramIndex", af_get_previous_program_index },
    { 130, "GetGpuErrorDetectedSystemEvent", af_get_gpu_error_event },
    { 140, "GetFriendInvitationStorageChannelEvent", af_get_friend_invitation_event },
    { 141, "TryPopFromFriendInvitationStorageChannel", af_try_pop_from_friend_invitation_channel },
    { 150, "GetNotificationStorageChannelEvent", af_get_notification_event },
    { 160, "GetHealthWarningDisappearedSystemEvent", af_get_health_warning_event },
    { 210, "GetUnknownEvent", af_get_unknown_event },
    { 320, "Cmd320", af_cmd_320 },
    { 321, "Cmd321", af_cmd_321 },
    { 330, "IsLanguageSelectionLimited", af_is_language_selection_limited },
    RL_SRV_STUB(1001, "PrepareForJit"),
};

static const rl_ipc_class af_class = {
    "nn::am::service::IApplicationFunctions", af_cmds, AM_N(af_cmds), 0, af_destroy
};

rl_ipc_object *am_new_application_functions(void)
{
    af_state *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    for (uint32_t i = 0; i < EV_COUNT; i++)
        s->events[i] = rl_hos_event_create();
    s->save_size = DEFAULT_SAVE_SIZE;
    s->journal_size = DEFAULT_SAVE_SIZE;
    rl_ipc_object *o = rl_srv_object(&af_class, s);
    if (!o)
        af_destroy(s);
    return o;
}

static rl_result hm_get_pop_from_general_channel_event(rl_ipc_req *r)
{
    return am_push_event(r, r->self->state);
}

static void hm_destroy(void *state)
{
    if (state)
        hos_kobj_unref(&((hos_revent *)state)->obj);
}

static const rl_ipc_cmd hm_cmds[] = {
    RL_SRV_STUB(10, "RequestToGetForeground"),
    { 21, "GetPopFromGeneralChannelEvent", hm_get_pop_from_general_channel_event },
};

static const rl_ipc_class hm_class = {
    "nn::am::service::IHomeMenuFunctions", hm_cmds, AM_N(hm_cmds), 0, hm_destroy
};

rl_ipc_object *am_new_home_menu_functions(void)
{
    hos_revent *e = rl_hos_event_create();
    rl_ipc_object *o = rl_srv_object(&hm_class, e);
    if (!o)
        hm_destroy(e);
    return o;
}

static rl_result lsa_pop_in_data(rl_ipc_req *r)
{
    (void)r;
    return AM_NOT_AVAILABLE;
}

static rl_result lsa_get_library_applet_info(rl_ipc_req *r)
{
    rl_ipc_out(r, 8);
    return KR_SUCCESS;
}

static rl_result lsa_get_caller_applet_identity_info(rl_ipc_req *r)
{
    uint8_t *o = rl_ipc_out(r, 16);
    uint32_t id = APPLET_ID_QLAUNCH;
    uint64_t title = QLAUNCH_TITLE;
    memcpy(o, &id, 4);
    memcpy(o + 8, &title, 8);
    return KR_SUCCESS;
}

static const rl_ipc_cmd lsa_cmds[] = {
    { 0, "PopInData", lsa_pop_in_data },
    { 11, "GetLibraryAppletInfo", lsa_get_library_applet_info },
    { 14, "GetCallerAppletIdentityInfo", lsa_get_caller_applet_identity_info },
};

static const rl_ipc_class lsa_class = {
    "nn::am::service::ILibraryAppletSelfAccessor", lsa_cmds, AM_N(lsa_cmds), 0, NULL
};

rl_ipc_object *am_new_library_self_accessor(void) { return rl_srv_object(&lsa_class, NULL); }

static rl_result pwc_get_launch_reason(rl_ipc_req *r)
{
    rl_ipc_out(r, 4);
    return KR_SUCCESS;
}

static const rl_ipc_cmd pwc_cmds[] = {
    { 0, "GetLaunchReason", pwc_get_launch_reason },
};

static const rl_ipc_class pwc_class = {
    "nn::am::service::IProcessWindingController", pwc_cmds, AM_N(pwc_cmds), 0, NULL
};

rl_ipc_object *am_new_process_winding_controller(void) { return rl_srv_object(&pwc_class, NULL); }
