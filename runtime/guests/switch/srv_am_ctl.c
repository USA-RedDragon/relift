/**
 * @file srv_am_ctl.c
 * @brief ICommonStateGetter, ISelfController, IWindowController,
 * IAudioController and IDisplayController.
 *
 * Command ids and behavior follow Ryujinx (MIT, Copyright (c) Ryujinx Team
 * and Contributors) and libnx (ISC, Copyright (c) libnx Authors).
 */
#include "srv_am_priv.h"

#include <stdlib.h>

#define PERF_CONFIG_HANDHELD 0x00020003u
#define PERF_CONFIG_DOCKED 0x00020004u
#define PLATFORM_REGION_GLOBAL 1u

typedef struct csg_state {
    hos_revent *sleep_lock_event;
    bool vr_mode;
    bool lcd_backlight_off;
    bool request_exit_to_library_applet;
} csg_state;

static void csg_destroy(void *state)
{
    csg_state *s = state;
    if (s->sleep_lock_event)
        hos_kobj_unref(&s->sleep_lock_event->obj);
    free(s);
}

static csg_state *csg(const rl_ipc_req *r) { return r->self->state; }

static rl_result csg_get_event_handle(rl_ipc_req *r) { return am_push_event(r, am_message_event()); }

static rl_result csg_receive_message(rl_ipc_req *r)
{
    uint32_t msg;
    if (!am_msg_pop(&msg))
        return AM_NO_MESSAGES;
    *(uint32_t *)rl_ipc_out(r, 4) = msg;
    return KR_SUCCESS;
}

static rl_result csg_get_operation_mode(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = rl_am_is_docked() ? 1 : 0;
    return KR_SUCCESS;
}

static rl_result csg_get_performance_mode(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = rl_am_is_docked() ? 1 : 0;
    return KR_SUCCESS;
}

static rl_result csg_get_boot_mode(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = 0;
    return KR_SUCCESS;
}

static rl_result csg_get_current_focus_state(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = (uint8_t)am_focus_state();
    return KR_SUCCESS;
}

static rl_result csg_get_acquired_sleep_lock_event(rl_ipc_req *r) { return am_push_event(r, csg(r)->sleep_lock_event); }

static rl_result csg_is_vr_mode_enabled(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = csg(r)->vr_mode ? 1 : 0;
    return KR_SUCCESS;
}

static rl_result csg_set_vr_mode_enabled(rl_ipc_req *r)
{
    AM_IN(uint8_t, on);
    csg(r)->vr_mode = on != 0;
    return KR_SUCCESS;
}

static rl_result csg_set_lcd_backlight_off_enabled(rl_ipc_req *r)
{
    AM_IN(uint8_t, on);
    csg(r)->lcd_backlight_off = on != 0;
    return KR_SUCCESS;
}

static rl_result csg_begin_vr_mode_ex(rl_ipc_req *r)
{
    csg(r)->vr_mode = true;
    return KR_SUCCESS;
}

static rl_result csg_end_vr_mode_ex(rl_ipc_req *r)
{
    csg(r)->vr_mode = false;
    return KR_SUCCESS;
}

static rl_result csg_get_default_display_resolution(rl_ipc_req *r)
{
    bool docked = rl_am_is_docked();
    uint32_t w = docked ? 1920 : 1280, h = docked ? 1080 : 720;
    rl_am_cap_display(&w, &h);
    uint32_t *o = rl_ipc_out(r, 8);
    o[0] = w;
    o[1] = h;
    return KR_SUCCESS;
}

static rl_result csg_get_default_display_resolution_change_event(rl_ipc_req *r)
{
    return am_push_event(r, am_resolution_event());
}

static rl_result csg_get_hdcp_authentication_state(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = 0;
    return KR_SUCCESS;
}

/**
 * @brief ICommonStateGetter 66: the u32 boost mode. The Switch takes 0 (normal)
 * and 1 (fast load); the Switch 2's SDK sends fast load as 2 and never 1
 * (nn::oe::SetCpuBoostMode, nn::applet::SetCpuBoostMode).
 */
static rl_result csg_set_cpu_boost_mode(rl_ipc_req *r)
{
    AM_IN(uint32_t, mode);
    return mode == 0 || mode == (hos_switch2() ? 2u : 1u) ? KR_SUCCESS : AM_INVALID_PARAMETERS;
}

static rl_result csg_get_current_performance_configuration(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = rl_am_is_docked() ? PERF_CONFIG_DOCKED : PERF_CONFIG_HANDHELD;
    return KR_SUCCESS;
}

static rl_result csg_get_settings_platform_region(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = PLATFORM_REGION_GLOBAL;
    return KR_SUCCESS;
}

static rl_result csg_set_request_exit_to_library_applet(rl_ipc_req *r)
{
    csg(r)->request_exit_to_library_applet = true;
    return KR_SUCCESS;
}

static const rl_ipc_cmd csg_cmds[] = {
    { 0, "GetEventHandle", csg_get_event_handle },
    { 1, "ReceiveMessage", csg_receive_message },
    { 5, "GetOperationMode", csg_get_operation_mode },
    { 6, "GetPerformanceMode", csg_get_performance_mode },
    { 8, "GetBootMode", csg_get_boot_mode },
    { 9, "GetCurrentFocusState", csg_get_current_focus_state },
    RL_SRV_STUB(10, "RequestToAcquireSleepLock"),
    { 13, "GetAcquiredSleepLockEvent", csg_get_acquired_sleep_lock_event },
    { 50, "IsVrModeEnabled", csg_is_vr_mode_enabled },
    { 51, "SetVrModeEnabled", csg_set_vr_mode_enabled },
    { 52, "SetLcdBacklighOffEnabled", csg_set_lcd_backlight_off_enabled },
    { 53, "BeginVrModeEx", csg_begin_vr_mode_ex },
    { 54, "EndVrModeEx", csg_end_vr_mode_ex },
    { 60, "GetDefaultDisplayResolution", csg_get_default_display_resolution },
    { 61, "GetDefaultDisplayResolutionChangeEvent", csg_get_default_display_resolution_change_event },
    { 62, "GetHdcpAuthenticationState", csg_get_hdcp_authentication_state },
    { 66, "SetCpuBoostMode", csg_set_cpu_boost_mode },
    { 91, "GetCurrentPerformanceConfiguration", csg_get_current_performance_configuration },
    { 300, "GetSettingsPlatformRegion", csg_get_settings_platform_region },
    { 900, "SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled", csg_set_request_exit_to_library_applet },
};

static const rl_ipc_class csg_class = {
    "nn::am::service::ICommonStateGetter", csg_cmds, AM_N(csg_cmds), 0, csg_destroy
};

rl_ipc_object *am_new_common_state_getter(void)
{
    csg_state *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->sleep_lock_event = rl_hos_event_create();
    rl_ipc_object *o = rl_srv_object(&csg_class, s);
    if (!o)
        csg_destroy(s);
    return o;
}

typedef struct sc_state {
    uint64_t pid;
    hos_revent *launchable_event;
    hos_revent *tick_event;
    bool tick_event_signaled;
    uint32_t fatal_sections;
    bool screenshot_permission;
    bool operation_mode_notification;
    bool performance_mode_notification;
    bool restart_message;
    bool out_of_focus_suspending;
    bool handles_request_to_display;
    bool auto_sleep_disabled;
    bool album_image_taken_notification;
    bool record_volume_muted;
    uint32_t screenshot_orientation;
    uint32_t idle_time_extension;
} sc_state;

static void sc_destroy(void *state)
{
    sc_state *s = state;
    if (s->launchable_event)
        hos_kobj_unref(&s->launchable_event->obj);
    if (s->tick_event)
        hos_kobj_unref(&s->tick_event->obj);
    free(s);
}

static sc_state *sc(const rl_ipc_req *r) { return r->self->state; }

static rl_result sc_enter_fatal_section(rl_ipc_req *r)
{
    sc(r)->fatal_sections++;
    return KR_SUCCESS;
}

static rl_result sc_exit(rl_ipc_req *r)
{
    (void)r;
    rl_process_exit(0);
    return KR_SUCCESS;
}

static rl_result sc_lock_exit(rl_ipc_req *r)
{
    (void)r;
    rl_am_lock_exit(true);
    return KR_SUCCESS;
}

static rl_result sc_unlock_exit(rl_ipc_req *r)
{
    (void)r;
    rl_am_lock_exit(false);
    return KR_SUCCESS;
}

static rl_result sc_leave_fatal_section(rl_ipc_req *r)
{
    sc_state *s = sc(r);
    if (!s->fatal_sections)
        return AM_UNBALANCED_FATAL_SECTION;
    s->fatal_sections--;
    return KR_SUCCESS;
}

static rl_result sc_get_library_applet_launchable_event(rl_ipc_req *r)
{
    sc_state *s = sc(r);
    if (s->launchable_event)
        rl_hos_event_signal(s->launchable_event);
    return am_push_event(r, s->launchable_event);
}

#define SC_SET_BOOL(fn, field)                                                                                         \
    static rl_result fn(rl_ipc_req *r)                                                                                 \
    {                                                                                                                  \
        AM_IN(uint8_t, v);                                                                                             \
        sc(r)->field = v != 0;                                                                                         \
        return KR_SUCCESS;                                                                                             \
    }

SC_SET_BOOL(sc_set_screenshot_permission, screenshot_permission)
SC_SET_BOOL(sc_set_operation_mode_changed_notification, operation_mode_notification)
SC_SET_BOOL(sc_set_performance_mode_changed_notification, performance_mode_notification)
SC_SET_BOOL(sc_set_restart_message_enabled, restart_message)
SC_SET_BOOL(sc_set_out_of_focus_suspending_enabled, out_of_focus_suspending)
SC_SET_BOOL(sc_set_handles_request_to_display, handles_request_to_display)
SC_SET_BOOL(sc_set_auto_sleep_disabled, auto_sleep_disabled)
SC_SET_BOOL(sc_set_album_image_taken_notification_enabled, album_image_taken_notification)
SC_SET_BOOL(sc_set_record_volume_muted, record_volume_muted)

static rl_result sc_set_focus_handling_mode(rl_ipc_req *r)
{
    struct {
        uint8_t flags[3];
    } v;
    if (!rl_srv_in(r, &v, sizeof v))
        return SF_RESULT_INVALID_HEADER;
    return KR_SUCCESS;
}

static rl_result sc_set_screenshot_image_orientation(rl_ipc_req *r)
{
    AM_IN(uint32_t, v);
    sc(r)->screenshot_orientation = v;
    return KR_SUCCESS;
}

static rl_result create_layer(uint64_t *id, uint64_t fallback)
{
    if (rl_srv_vi_layer_hook)
        return rl_srv_vi_layer_hook(id);
    *id = fallback;
    return KR_SUCCESS;
}

static rl_result sc_create_managed_display_layer(rl_ipc_req *r)
{
    uint64_t id = 0;
    rl_result rc = create_layer(&id, 1);
    if (rc != KR_SUCCESS)
        return rc;
    *(uint64_t *)rl_ipc_out(r, 8) = id;
    return KR_SUCCESS;
}

static rl_result sc_is_system_buffer_sharing_enabled(rl_ipc_req *r)
{
    (void)r;
    return AM_NOT_IMPLEMENTED;
}

static rl_result sc_create_managed_display_separable_layer(rl_ipc_req *r)
{
    uint64_t display = 0, recording = 0;
    rl_result rc = create_layer(&display, 1);
    if (rc == KR_SUCCESS)
        rc = create_layer(&recording, 2);
    if (rc != KR_SUCCESS)
        return rc;
    uint64_t *o = rl_ipc_out(r, 16);
    o[0] = display;
    o[1] = recording;
    return KR_SUCCESS;
}

static rl_result sc_set_idle_time_detection_extension(rl_ipc_req *r)
{
    AM_IN(uint32_t, v);
    sc(r)->idle_time_extension = v;
    return KR_SUCCESS;
}

static rl_result sc_get_idle_time_detection_extension(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = sc(r)->idle_time_extension;
    return KR_SUCCESS;
}

static rl_result sc_is_illuminance_available(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = 1;
    return KR_SUCCESS;
}

static rl_result sc_is_auto_sleep_disabled(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = sc(r)->auto_sleep_disabled ? 1 : 0;
    return KR_SUCCESS;
}

static rl_result sc_get_current_illuminance_ex(rl_ipc_req *r)
{
    uint8_t *o = rl_ipc_out(r, 8);
    float lux = 10000.0f;
    o[0] = 1;
    memcpy(o + 4, &lux, 4);
    return KR_SUCCESS;
}

static rl_result sc_set_wireless_priority_mode(rl_ipc_req *r)
{
    AM_IN(int32_t, mode);
    return (mode < 0 || mode > 2) ? AM_INVALID_PARAMETERS : KR_SUCCESS;
}

static rl_result sc_get_accumulated_suspended_tick_value(rl_ipc_req *r)
{
    *(uint64_t *)rl_ipc_out(r, 8) = 0;
    return KR_SUCCESS;
}

static rl_result sc_get_accumulated_suspended_tick_changed_event(rl_ipc_req *r)
{
    sc_state *s = sc(r);
    if (!s->tick_event_signaled) {
        s->tick_event_signaled = true;
        s->tick_event = rl_hos_event_create();
        if (s->tick_event)
            rl_hos_event_signal(s->tick_event);
    }
    return am_push_event(r, s->tick_event);
}

static rl_result sc_save_current_screenshot(rl_ipc_req *r)
{
    AM_IN(int32_t, option);
    return (option < 0 || option > 3) ? AM_INVALID_PARAMETERS : KR_SUCCESS;
}

/** @brief Switch 2 setters of what the system records or shares (SetRtcScreenSharingAudioEnabled): accepted, nothing to apply. */
static rl_result sc_switch2_setting(rl_ipc_req *r)
{
    uint8_t on;
    if (!hos_switch2())
        return SF_RESULT_UNKNOWN_COMMAND;
    return rl_srv_in(r, &on, 1) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

/** @brief GetGraphicsSupervisor (Switch 2 only): a copy handle to the gsv UserSession. */
static rl_result sc_get_graphics_supervisor(rl_ipc_req *r)
{
    if (!hos_switch2())
        return SF_RESULT_UNKNOWN_COMMAND;
    uint32_t h;
    rl_result rc = rl_gsv_open_user_session(&h);
    if (rc == KR_SUCCESS)
        r->out_copy[r->n_out_copy++] = h;
    return rc;
}

static const rl_ipc_cmd sc_cmds[] = {
    { 0, "Exit", sc_exit },
    { 1, "LockExit", sc_lock_exit },
    { 2, "UnlockExit", sc_unlock_exit },
    { 3, "EnterFatalSection", sc_enter_fatal_section },
    { 4, "LeaveFatalSection", sc_leave_fatal_section },
    { 9, "GetLibraryAppletLaunchableEvent", sc_get_library_applet_launchable_event },
    { 10, "SetScreenShotPermission", sc_set_screenshot_permission },
    { 11, "SetOperationModeChangedNotification", sc_set_operation_mode_changed_notification },
    { 12, "SetPerformanceModeChangedNotification", sc_set_performance_mode_changed_notification },
    { 13, "SetFocusHandlingMode", sc_set_focus_handling_mode },
    { 14, "SetRestartMessageEnabled", sc_set_restart_message_enabled },
    { 16, "SetOutOfFocusSuspendingEnabled", sc_set_out_of_focus_suspending_enabled },
    { 19, "SetScreenShotImageOrientation", sc_set_screenshot_image_orientation },
    { 40, "CreateManagedDisplayLayer", sc_create_managed_display_layer },
    { 41, "IsSystemBufferSharingEnabled", sc_is_system_buffer_sharing_enabled },
    { 44, "CreateManagedDisplaySeparableLayer", sc_create_managed_display_separable_layer },
    { 50, "SetHandlesRequestToDisplay", sc_set_handles_request_to_display },
    { 62, "SetIdleTimeDetectionExtension", sc_set_idle_time_detection_extension },
    { 63, "GetIdleTimeDetectionExtension", sc_get_idle_time_detection_extension },
    RL_SRV_STUB(65, "ReportUserIsActive"),
    { 67, "IsIlluminanceAvailable", sc_is_illuminance_available },
    { 68, "SetAutoSleepDisabled", sc_set_auto_sleep_disabled },
    { 69, "IsAutoSleepDisabled", sc_is_auto_sleep_disabled },
    { 71, "GetCurrentIlluminanceEx", sc_get_current_illuminance_ex },
    { 80, "SetWirelessPriorityMode", sc_set_wireless_priority_mode },
    { 90, "GetAccumulatedSuspendedTickValue", sc_get_accumulated_suspended_tick_value },
    { 91, "GetAccumulatedSuspendedTickChangedEvent", sc_get_accumulated_suspended_tick_changed_event },
    { 100, "SetAlbumImageTakenNotificationEnabled", sc_set_album_image_taken_notification_enabled },
    { 120, "SaveCurrentScreenshot", sc_save_current_screenshot },
    { 130, "SetRecordVolumeMuted", sc_set_record_volume_muted },
    { 2000, "GetGraphicsSupervisor", sc_get_graphics_supervisor },
    { 2400, "SetRtcScreenSharingAudioEnabled", sc_switch2_setting },
};

static const rl_ipc_class sc_class = { "nn::am::service::ISelfController", sc_cmds, AM_N(sc_cmds), 0, sc_destroy };

rl_ipc_object *am_new_self_controller(uint64_t pid)
{
    sc_state *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->pid = pid;
    s->launchable_event = rl_hos_event_create();
    rl_ipc_object *o = rl_srv_object(&sc_class, s);
    if (!o)
        sc_destroy(s);
    return o;
}

static rl_result wc_get_applet_resource_user_id(rl_ipc_req *r)
{
    *(uint64_t *)rl_ipc_out(r, 8) = am_resource_user_id(*(const uint64_t *)r->self->state);
    return KR_SUCCESS;
}

static const rl_ipc_cmd wc_cmds[] = {
    { 1, "GetAppletResourceUserId", wc_get_applet_resource_user_id },
    RL_SRV_STUB(10, "AcquireForegroundRights"),
    RL_SRV_STUB(11, "ReleaseForegroundRights"),
    RL_SRV_STUB(12, "RejectToChangeIntoBackground"),
};

static const rl_ipc_class wc_class = { "nn::am::service::IWindowController", wc_cmds, AM_N(wc_cmds), 0, free };

rl_ipc_object *am_new_window_controller(uint64_t pid)
{
    uint64_t *p = malloc(sizeof *p);
    if (!p)
        return NULL;
    *p = pid;
    rl_ipc_object *o = rl_srv_object(&wc_class, p);
    if (!o)
        free(p);
    return o;
}

typedef struct audio_state {
    float applet_volume;
    float library_applet_volume;
} audio_state;

static audio_state *audio(const rl_ipc_req *r) { return r->self->state; }

static rl_result ac_set_expected_master_volume(rl_ipc_req *r)
{
    AM_IN(audio_state, v);
    *audio(r) = v;
    return KR_SUCCESS;
}

static rl_result ac_get_main_applet_expected_master_volume(rl_ipc_req *r)
{
    *(float *)rl_ipc_out(r, 4) = audio(r)->applet_volume;
    return KR_SUCCESS;
}

static rl_result ac_get_library_applet_expected_master_volume(rl_ipc_req *r)
{
    *(float *)rl_ipc_out(r, 4) = audio(r)->library_applet_volume;
    return KR_SUCCESS;
}

static rl_result ac_change_main_applet_master_volume(rl_ipc_req *r)
{
    struct {
        float volume;
        uint32_t pad;
        uint64_t fade_ns;
    } v;
    if (!rl_srv_in(r, &v, sizeof v))
        return SF_RESULT_INVALID_HEADER;
    audio(r)->applet_volume = v.volume;
    return KR_SUCCESS;
}

static rl_result ac_set_transparent_volume_rate(rl_ipc_req *r)
{
    AM_IN(float, rate);
    return KR_SUCCESS;
}

static const rl_ipc_cmd ac_cmds[] = {
    { 0, "SetExpectedMasterVolume", ac_set_expected_master_volume },
    { 1, "GetMainAppletExpectedMasterVolume", ac_get_main_applet_expected_master_volume },
    { 2, "GetLibraryAppletExpectedMasterVolume", ac_get_library_applet_expected_master_volume },
    { 3, "ChangeMainAppletMasterVolume", ac_change_main_applet_master_volume },
    { 4, "SetTransparentVolumeRate", ac_set_transparent_volume_rate },
};

static const rl_ipc_class ac_class = { "nn::am::service::IAudioController", ac_cmds, AM_N(ac_cmds), 0, free };

rl_ipc_object *am_new_audio_controller(void)
{
    audio_state *s = malloc(sizeof *s);
    if (!s)
        return NULL;
    s->applet_volume = 1.0f;
    s->library_applet_volume = 1.0f;
    rl_ipc_object *o = rl_srv_object(&ac_class, s);
    if (!o)
        free(s);
    return o;
}

typedef struct dc_state {
    bool last_application_acquired;
    bool caller_applet_acquired;
} dc_state;

static rl_result dc_take_screenshot_of_own_layer(rl_ipc_req *r)
{
    struct {
        uint8_t flag;
        uint8_t pad[3];
        int32_t layer_stack;
    } v;
    if (!rl_srv_in(r, &v, sizeof v))
        return SF_RESULT_INVALID_HEADER;
    return KR_SUCCESS;
}

static rl_result dc_release_last_application_capture_buffer(rl_ipc_req *r)
{
    dc_state *s = r->self->state;
    if (!s->last_application_acquired)
        return AM_BUFFER_NOT_ACQUIRED;
    s->last_application_acquired = false;
    return KR_SUCCESS;
}

static rl_result dc_release_caller_applet_capture_buffer(rl_ipc_req *r)
{
    dc_state *s = r->self->state;
    if (!s->caller_applet_acquired)
        return AM_BUFFER_NOT_ACQUIRED;
    s->caller_applet_acquired = false;
    return KR_SUCCESS;
}

static rl_result dc_acquire_capture_buffer(rl_ipc_req *r)
{
    (void)r;
    return AM_NOT_AVAILABLE;
}

static const rl_ipc_cmd dc_cmds[] = {
    { 8, "TakeScreenShotOfOwnLayer", dc_take_screenshot_of_own_layer },
    { 11, "ReleaseLastApplicationCaptureBuffer", dc_release_last_application_capture_buffer },
    { 15, "ReleaseCallerAppletCaptureBuffer", dc_release_caller_applet_capture_buffer },
    { 16, "AcquireLastApplicationCaptureBufferEx", dc_acquire_capture_buffer },
    { 18, "AcquireCallerAppletCaptureBufferEx", dc_acquire_capture_buffer },
};

static const rl_ipc_class dc_class = { "nn::am::service::IDisplayController", dc_cmds, AM_N(dc_cmds), 0, free };

rl_ipc_object *am_new_display_controller(void)
{
    dc_state *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    rl_ipc_object *o = rl_srv_object(&dc_class, s);
    if (!o)
        free(s);
    return o;
}
