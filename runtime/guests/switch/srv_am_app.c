/**
 * @file srv_am_app.c
 * @brief IApplicationFunctions, IStorage and its accessor, ILibraryAppletCreator
 * with its accessor, IHomeMenuFunctions, ILibraryAppletSelfAccessor and
 * IProcessWindingController.
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

typedef struct am_storage {
    uint8_t *data;
    uint64_t size;
    bool read_only;
} am_storage;

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

static void storage_destroy(void *state)
{
    am_storage *s = state;
    free(s->data);
    free(s);
}

static void accessor_destroy(void *state)
{
    rl_ipc_object_unref(state);
}

static const rl_ipc_class accessor_class;

static rl_result storage_open(rl_ipc_req *r)
{
    rl_ipc_object_ref(r->self);
    rl_ipc_object *acc = rl_srv_object(&accessor_class, r->self);
    if (!acc) {
        rl_ipc_object_unref(r->self);
        return KR_OUT_OF_RESOURCE;
    }
    return rl_ipc_push_object(r, acc);
}

static const rl_ipc_cmd storage_cmds[] = {
    { 0, "Open", storage_open },
};

static const rl_ipc_class storage_class = { "nn::am::service::IStorage", storage_cmds, AM_N(storage_cmds), 0, storage_destroy };

static rl_ipc_object *storage_new(const void *data, uint64_t size, bool read_only)
{
    am_storage *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->data = calloc(1, size ? size : 1);
    if (!s->data) {
        free(s);
        return NULL;
    }
    if (data)
        memcpy(s->data, data, size);
    s->size = size;
    s->read_only = read_only;
    rl_ipc_object *o = rl_srv_object(&storage_class, s);
    if (!o)
        storage_destroy(s);
    return o;
}

static am_storage *accessor_storage(const rl_ipc_req *r) { return ((rl_ipc_object *)r->self->state)->state; }

static rl_result accessor_get_size(rl_ipc_req *r)
{
    *(uint64_t *)rl_ipc_out(r, 8) = accessor_storage(r)->size;
    return KR_SUCCESS;
}

static rl_result accessor_write(rl_ipc_req *r)
{
    am_storage *s = accessor_storage(r);
    AM_IN(uint64_t, offset);
    if (s->read_only)
        return AM_OBJECT_INVALID;
    if (offset > s->size)
        return AM_OUT_OF_BOUNDS;
    rl_ipc_buf b = rl_ipc_auto_in(r, 0);
    uint64_t n = b.size < s->size - offset ? b.size : s->size - offset;
    if (n && !rl_ipc_buf_read(&b, s->data + offset, n))
        return KR_INVALID_MEM_STATE;
    return KR_SUCCESS;
}

static rl_result accessor_read(rl_ipc_req *r)
{
    am_storage *s = accessor_storage(r);
    AM_IN(uint64_t, offset);
    if (offset > s->size)
        return AM_OUT_OF_BOUNDS;
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    uint64_t n = b.size < s->size - offset ? b.size : s->size - offset;
    if (n && !rl_ipc_buf_write(&b, s->data + offset, n))
        return KR_INVALID_MEM_STATE;
    return KR_SUCCESS;
}

static const rl_ipc_cmd accessor_cmds[] = {
    { 0, "GetSize", accessor_get_size },
    { 10, "Write", accessor_write },
    { 11, "Read", accessor_read },
};

static const rl_ipc_class accessor_class = {
    "nn::am::service::IStorageAccessor", accessor_cmds, AM_N(accessor_cmds), 0, accessor_destroy
};

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
        st = storage_new(data, size, false);
        free(data);
    } else if (kind == LAUNCH_KIND_PRESELECTED_USER) {
        uint8_t params[LAUNCH_PARAMS_SIZE] = { 0 };
        uint32_t magic = LAUNCH_PARAMS_MAGIC, selected = 1;
        uint64_t uid[2];
        am_preselected_user(&uid[0], &uid[1]);
        memcpy(params, &magic, 4);
        memcpy(params + 4, &selected, 4);
        memcpy(params + 8, uid, 16);
        st = storage_new(params, sizeof params, false);
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

static rl_result af_get_display_version(rl_ipc_req *r)
{
    rl_ipc_out(r, 16);
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
    if (r->n_in_objects != 1 || !r->in_objects[0] || r->in_objects[0]->cls != &storage_class)
        return SF_RESULT_INVALID_IN_OBJECT;
    const am_storage *s = r->in_objects[0]->state;
    uint8_t *copy = malloc(s->size ? s->size : 1);
    if (!copy)
        return KR_OUT_OF_RESOURCE;
    memcpy(copy, s->data, s->size);
    pthread_mutex_lock(&app_mu);
    if (user_channel_count == USER_CHANNEL_MAX) {
        free(user_channel[0].data);
        memmove(&user_channel[0], &user_channel[1], sizeof user_channel[0] * (USER_CHANNEL_MAX - 1));
        user_channel_count--;
    }
    user_channel[user_channel_count].data = copy;
    user_channel[user_channel_count].size = s->size;
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

typedef struct lac_state {
    hos_revent *state_changed;
    hos_revent *normal_out;
    hos_revent *interactive_out;
    uint64_t indirect_layer;
} lac_state;

static void lac_destroy(void *state)
{
    lac_state *s = state;
    if (s->state_changed)
        hos_kobj_unref(&s->state_changed->obj);
    if (s->normal_out)
        hos_kobj_unref(&s->normal_out->obj);
    if (s->interactive_out)
        hos_kobj_unref(&s->interactive_out->obj);
    free(s);
}

static lac_state *lac(const rl_ipc_req *r) { return r->self->state; }

static rl_result lac_get_state_changed_event(rl_ipc_req *r) { return am_push_event(r, lac(r)->state_changed); }
static rl_result lac_get_pop_out_data_event(rl_ipc_req *r) { return am_push_event(r, lac(r)->normal_out); }
static rl_result lac_get_pop_interactive_out_data_event(rl_ipc_req *r) { return am_push_event(r, lac(r)->interactive_out); }

static rl_result lac_request_exit(rl_ipc_req *r)
{
    if (lac(r)->state_changed)
        rl_hos_event_signal(lac(r)->state_changed);
    return KR_SUCCESS;
}

static rl_result lac_get_result(rl_ipc_req *r)
{
    (void)r;
    return KR_SUCCESS;
}

static rl_result lac_push_in_data(rl_ipc_req *r)
{
    if (r->n_in_objects != 1 || !r->in_objects[0] || r->in_objects[0]->cls != &storage_class)
        return SF_RESULT_INVALID_IN_OBJECT;
    return KR_SUCCESS;
}

static rl_result lac_pop_out_data(rl_ipc_req *r)
{
    (void)r;
    return AM_NOT_AVAILABLE;
}

static rl_result lac_stubbed(rl_ipc_req *r)
{
    (void)r;
    return AM_STUBBED;
}

static rl_result lac_get_indirect_layer_consumer_handle(rl_ipc_req *r)
{
    lac_state *s = lac(r);
    if (!s->indirect_layer)
        s->indirect_layer = 0x1000 + am_resource_user_id(rl_srv_pid(r));
    *(uint64_t *)rl_ipc_out(r, 8) = s->indirect_layer;
    return KR_SUCCESS;
}

static const rl_ipc_cmd lac_cmds[] = {
    { 0, "GetAppletStateChangedEvent", lac_get_state_changed_event },
    RL_SRV_STUB(10, "Start"),
    { 20, "RequestExit", lac_request_exit },
    { 30, "GetResult", lac_get_result },
    RL_SRV_STUB(60, "PresetLibraryAppletGpuTimeSliceZero"),
    { 100, "PushInData", lac_push_in_data },
    { 101, "PopOutData", lac_pop_out_data },
    { 103, "PushInteractiveInData", lac_push_in_data },
    { 104, "PopInteractiveOutData", lac_pop_out_data },
    { 105, "GetPopOutDataEvent", lac_get_pop_out_data_event },
    { 106, "GetPopInteractiveOutDataEvent", lac_get_pop_interactive_out_data_event },
    { 110, "NeedsToExitProcess", lac_stubbed },
    { 150, "RequestForAppletToGetForeground", lac_stubbed },
    { 160, "GetIndirectLayerConsumerHandle", lac_get_indirect_layer_consumer_handle },
};

static const rl_ipc_class lac_class = {
    "nn::am::service::ILibraryAppletAccessor", lac_cmds, AM_N(lac_cmds), 0, lac_destroy
};

static rl_ipc_object *lac_new(void)
{
    lac_state *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->state_changed = rl_hos_event_create();
    s->normal_out = rl_hos_event_create();
    s->interactive_out = rl_hos_event_create();
    rl_ipc_object *o = rl_srv_object(&lac_class, s);
    if (!o)
        lac_destroy(s);
    return o;
}

static rl_result lc_create_library_applet(rl_ipc_req *r)
{
    struct {
        uint32_t applet_id;
        uint32_t mode;
    } q;
    if (!rl_srv_in(r, &q, sizeof q))
        return SF_RESULT_INVALID_HEADER;
    return am_push_object(r, lac_new());
}

static rl_result lc_create_library_applet_ex(rl_ipc_req *r)
{
    struct {
        uint32_t applet_id;
        uint32_t mode;
        uint64_t thread_id;
    } q;
    if (!rl_srv_in(r, &q, sizeof q))
        return SF_RESULT_INVALID_HEADER;
    return am_push_object(r, lac_new());
}

static rl_result lc_create_storage(rl_ipc_req *r)
{
    AM_IN(int64_t, size);
    if (size <= 0)
        return AM_OBJECT_INVALID;
    if (size > (int64_t)1 << 30)
        return AM_RESULT(600);
    return am_push_object(r, storage_new(NULL, (uint64_t)size, false));
}

static const rl_ipc_cmd lc_cmds[] = {
    { 0, "CreateLibraryApplet", lc_create_library_applet },
    { 3, "CreateLibraryAppletEx", lc_create_library_applet_ex },
    { 10, "CreateStorage", lc_create_storage },
};

static const rl_ipc_class lc_class = {
    "nn::am::service::ILibraryAppletCreator", lc_cmds, AM_N(lc_cmds), 0, NULL
};

rl_ipc_object *am_new_library_applet_creator(void) { return rl_srv_object(&lc_class, NULL); }

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
