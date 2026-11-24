/**
 * @file srv_am.c
 * @brief appletOE, appletAE, their proxy objects and the applet manager state
 * (message queue, focus, operation mode). The operation mode starts as the
 * system.console_mode setting says, handheld by default on every host.
 *
 * Command ids and behavior follow Ryujinx (MIT, Copyright (c) Ryujinx Team
 * and Contributors) and libnx (ISC, Copyright (c) libnx Authors).
 */
#include "srv_am_priv.h"

#include <relift/settings.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define AM_QUEUE 64
#define AM_PIDS 32

static pthread_mutex_t am_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t am_queue[AM_QUEUE];
static uint32_t am_qhead;
static uint32_t am_qcount;
static hos_revent *am_msg_event;
static hos_revent *am_res_event;
static uint32_t am_focus = RL_AM_FOCUS_IN;
static bool am_docked;
static uint64_t am_language = RL_AM_LANGUAGE_CODE_EN_US;
static char am_display_version_str[16];
static bool am_display_version_known;
static uint64_t am_user_high = 1;
static uint64_t am_user_low;
static uint64_t am_pids[AM_PIDS];
static uint32_t am_npids;
static bool am_exit_locked, am_exit_requested;

rl_result (*rl_srv_vi_layer_hook)(uint64_t *layer_id);

#ifdef RL_SRV_VI_HOOK_DEFAULT
rl_result rl_srv_vi_create_managed_layer(uint64_t *layer_id)
{
    *layer_id = 1;
    return KR_SUCCESS;
}
#endif

static void ensure_events(void)
{
    if (!am_msg_event)
        am_msg_event = rl_hos_event_create();
    if (!am_res_event)
        am_res_event = rl_hos_event_create();
}

static void sync_event(void)
{
    if (!am_msg_event)
        return;
    if (am_qcount)
        rl_hos_event_signal(am_msg_event);
    else
        rl_hos_event_clear(am_msg_event);
}

static void enqueue(uint32_t msg)
{
    if (am_qcount == AM_QUEUE) {
        am_qhead = (am_qhead + 1) % AM_QUEUE;
        am_qcount--;
    }
    am_queue[(am_qhead + am_qcount) % AM_QUEUE] = msg;
    am_qcount++;
}

void rl_am_push_message(uint32_t msg)
{
    pthread_mutex_lock(&am_mu);
    ensure_events();
    enqueue(msg);
    sync_event();
    pthread_mutex_unlock(&am_mu);
}

void rl_am_set_focus(bool in_focus)
{
    pthread_mutex_lock(&am_mu);
    ensure_events();
    am_focus = in_focus ? RL_AM_FOCUS_IN : RL_AM_FOCUS_OUT;
    enqueue(RL_AM_MSG_FOCUS_STATE_CHANGED);
    if (in_focus)
        enqueue(RL_AM_MSG_CHANGE_INTO_FOREGROUND);
    sync_event();
    pthread_mutex_unlock(&am_mu);
}

void rl_am_set_operation_mode(bool docked)
{
    pthread_mutex_lock(&am_mu);
    ensure_events();
    if (docked != am_docked) {
        am_docked = docked;
        enqueue(RL_AM_MSG_OPERATION_MODE_CHANGED);
        enqueue(RL_AM_MSG_PERFORMANCE_MODE_CHANGED);
        sync_event();
        rl_hos_event_signal(am_res_event);
        rl_log(RL_LOG_INFO, "am", "console mode: %s (operation and performance mode change messages queued)",
               docked ? "docked" : "handheld");
    }
    pthread_mutex_unlock(&am_mu);
}

bool rl_am_is_docked(void)
{
    pthread_mutex_lock(&am_mu);
    bool d = am_docked;
    pthread_mutex_unlock(&am_mu);
    return d;
}

bool rl_am_request_exit(void)
{
    pthread_mutex_lock(&am_mu);
    bool locked = am_exit_locked;
    if (locked) {
        am_exit_requested = true;
        ensure_events();
        enqueue(RL_AM_MSG_EXIT);
        sync_event();
    }
    pthread_mutex_unlock(&am_mu);
    if (!locked)
        rl_process_exit(0);
    return locked;
}

void rl_am_lock_exit(bool lock)
{
    pthread_mutex_lock(&am_mu);
    bool end = am_exit_requested && (lock || am_exit_locked);
    if (!end)
        am_exit_locked = lock;
    pthread_mutex_unlock(&am_mu);
    if (end)
        rl_process_exit(0);
}

void rl_am_set_desired_language(uint64_t code)
{
    pthread_mutex_lock(&am_mu);
    am_language = code;
    pthread_mutex_unlock(&am_mu);
}

void rl_am_set_display_version(const char *version)
{
    pthread_mutex_lock(&am_mu);
    memset(am_display_version_str, 0, sizeof am_display_version_str);
    am_display_version_known = version && *version;
    if (am_display_version_known)
        strncpy(am_display_version_str, version, sizeof am_display_version_str - 1);
    pthread_mutex_unlock(&am_mu);
}

bool am_display_version(uint8_t out[16])
{
    pthread_mutex_lock(&am_mu);
    memcpy(out, am_display_version_str, sizeof am_display_version_str);
    bool known = am_display_version_known;
    pthread_mutex_unlock(&am_mu);
    return known;
}

void rl_am_set_preselected_user(uint64_t high, uint64_t low)
{
    pthread_mutex_lock(&am_mu);
    am_user_high = high;
    am_user_low = low;
    pthread_mutex_unlock(&am_mu);
}

bool am_msg_pop(uint32_t *msg)
{
    pthread_mutex_lock(&am_mu);
    ensure_events();
    bool ok = am_qcount != 0;
    if (ok) {
        *msg = am_queue[am_qhead];
        am_qhead = (am_qhead + 1) % AM_QUEUE;
        am_qcount--;
    }
    sync_event();
    pthread_mutex_unlock(&am_mu);
    return ok;
}

hos_revent *am_message_event(void)
{
    pthread_mutex_lock(&am_mu);
    ensure_events();
    hos_revent *e = am_msg_event;
    pthread_mutex_unlock(&am_mu);
    return e;
}

hos_revent *am_resolution_event(void)
{
    pthread_mutex_lock(&am_mu);
    ensure_events();
    hos_revent *e = am_res_event;
    pthread_mutex_unlock(&am_mu);
    return e;
}

uint32_t am_focus_state(void)
{
    pthread_mutex_lock(&am_mu);
    uint32_t f = am_focus;
    pthread_mutex_unlock(&am_mu);
    return f;
}

uint64_t am_language_code(void)
{
    pthread_mutex_lock(&am_mu);
    uint64_t v = am_language;
    pthread_mutex_unlock(&am_mu);
    return v;
}

void am_preselected_user(uint64_t *high, uint64_t *low)
{
    pthread_mutex_lock(&am_mu);
    *high = am_user_high;
    *low = am_user_low;
    pthread_mutex_unlock(&am_mu);
}

uint64_t am_resource_user_id(uint64_t pid)
{
    pthread_mutex_lock(&am_mu);
    uint32_t i = 0;
    while (i < am_npids && am_pids[i] != pid)
        i++;
    if (i == am_npids) {
        if (am_npids == AM_PIDS)
            i = AM_PIDS - 1;
        else
            am_npids++;
        am_pids[i] = pid;
    }
    pthread_mutex_unlock(&am_mu);
    return i + 1;
}

rl_result am_push_event(rl_ipc_req *r, hos_revent *e)
{
    if (!e)
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_copy(r, &e->obj);
}

rl_result am_push_object(rl_ipc_req *r, rl_ipc_object *o)
{
    if (!o)
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_object(r, o);
}

rl_ipc_object *am_new_empty(const rl_ipc_class *cls) { return rl_srv_object(cls, NULL); }

const rl_ipc_class am_debug_functions_class = { "nn::am::service::IDebugFunctions", NULL, 0, 0, NULL };
const rl_ipc_class am_global_state_controller_class = { "nn::am::service::IGlobalStateController", NULL, 0, 0, NULL };
const rl_ipc_class am_application_creator_class = { "nn::am::service::IApplicationCreator", NULL, 0, 0, NULL };
const rl_ipc_class am_applet_common_functions_class = { "nn::am::service::IAppletCommonFunctions", NULL, 0, 0, NULL };

static uint64_t proxy_pid(const rl_ipc_req *r) { return *(const uint64_t *)r->self->state; }

static rl_result px_common_state_getter(rl_ipc_req *r) { return am_push_object(r, am_new_common_state_getter()); }
static rl_result px_self_controller(rl_ipc_req *r) { return am_push_object(r, am_new_self_controller(proxy_pid(r))); }
static rl_result px_window_controller(rl_ipc_req *r) { return am_push_object(r, am_new_window_controller(proxy_pid(r))); }
static rl_result px_audio_controller(rl_ipc_req *r) { return am_push_object(r, am_new_audio_controller()); }
static rl_result px_display_controller(rl_ipc_req *r) { return am_push_object(r, am_new_display_controller()); }
static rl_result px_library_applet_creator(rl_ipc_req *r) { return am_push_object(r, am_new_library_applet_creator()); }
static rl_result px_application_functions(rl_ipc_req *r) { return am_push_object(r, am_new_application_functions()); }
static rl_result px_home_menu_functions(rl_ipc_req *r) { return am_push_object(r, am_new_home_menu_functions()); }
static rl_result px_self_accessor(rl_ipc_req *r) { return am_push_object(r, am_new_library_self_accessor()); }
static rl_result px_winding_controller(rl_ipc_req *r) { return am_push_object(r, am_new_process_winding_controller()); }
static rl_result px_debug_functions(rl_ipc_req *r) { return am_push_object(r, am_new_empty(&am_debug_functions_class)); }
static rl_result px_global_state_controller(rl_ipc_req *r)
{
    return am_push_object(r, am_new_empty(&am_global_state_controller_class));
}
static rl_result px_application_creator(rl_ipc_req *r)
{
    return am_push_object(r, am_new_empty(&am_application_creator_class));
}
static rl_result px_applet_common_functions(rl_ipc_req *r)
{
    return am_push_object(r, am_new_empty(&am_applet_common_functions_class));
}

static const rl_ipc_cmd application_proxy_cmds[] = {
    { 0, "GetCommonStateGetter", px_common_state_getter },
    { 1, "GetSelfController", px_self_controller },
    { 2, "GetWindowController", px_window_controller },
    { 3, "GetAudioController", px_audio_controller },
    { 4, "GetDisplayController", px_display_controller },
    { 11, "GetLibraryAppletCreator", px_library_applet_creator },
    { 20, "GetApplicationFunctions", px_application_functions },
    { 1000, "GetDebugFunctions", px_debug_functions },
};

static rl_result px_performance_functions(rl_ipc_req *r) { return am_push_object(r, am_new_performance_functions()); }
static rl_result px_graphics_scaling_functions(rl_ipc_req *r)
{
    return am_push_object(r, am_new_graphics_scaling_functions());
}

static const rl_ipc_cmd application_proxy2_cmds[] = {
    { 0, "GetCommonStateGetter", px_common_state_getter },
    { 1, "GetSelfController", px_self_controller },
    { 2, "GetWindowController", px_window_controller },
    { 3, "GetAudioController", px_audio_controller },
    { 4, "GetDisplayController", px_display_controller },
    { 11, "GetLibraryAppletCreator", px_library_applet_creator },
    { 20, "GetApplicationFunctions", px_application_functions },
    { 40, "GetPerformanceFunctions", px_performance_functions },
    { 60, "GetGraphicsScalingParametersFunctions", px_graphics_scaling_functions },
    { 1000, "GetDebugFunctions", px_debug_functions },
};

static const rl_ipc_cmd library_proxy_cmds[] = {
    { 0, "GetCommonStateGetter", px_common_state_getter },
    { 1, "GetSelfController", px_self_controller },
    { 2, "GetWindowController", px_window_controller },
    { 3, "GetAudioController", px_audio_controller },
    { 4, "GetDisplayController", px_display_controller },
    { 10, "GetProcessWindingController", px_winding_controller },
    { 11, "GetLibraryAppletCreator", px_library_applet_creator },
    { 20, "OpenLibraryAppletSelfAccessor", px_self_accessor },
    { 21, "GetAppletCommonFunctions", px_applet_common_functions },
    { 1000, "GetDebugFunctions", px_debug_functions },
};

static const rl_ipc_cmd system_proxy_cmds[] = {
    { 0, "GetCommonStateGetter", px_common_state_getter },
    { 1, "GetSelfController", px_self_controller },
    { 2, "GetWindowController", px_window_controller },
    { 3, "GetAudioController", px_audio_controller },
    { 4, "GetDisplayController", px_display_controller },
    { 11, "GetLibraryAppletCreator", px_library_applet_creator },
    { 20, "GetHomeMenuFunctions", px_home_menu_functions },
    { 21, "GetGlobalStateController", px_global_state_controller },
    { 22, "GetApplicationCreator", px_application_creator },
    { 23, "GetAppletCommonFunctions", px_applet_common_functions },
    { 1000, "GetDebugFunctions", px_debug_functions },
};

static const rl_ipc_class application_proxy_class = {
    "nn::am::service::IApplicationProxy", application_proxy_cmds, AM_N(application_proxy_cmds), 0, free
};
static const rl_ipc_class application_proxy2_class = {
    "nn::am::service::IApplicationProxy", application_proxy2_cmds, AM_N(application_proxy2_cmds), 0, free
};
static const rl_ipc_class library_proxy_class = {
    "nn::am::service::ILibraryAppletProxy", library_proxy_cmds, AM_N(library_proxy_cmds), 0, free
};
static const rl_ipc_class system_proxy_class = {
    "nn::am::service::ISystemAppletProxy", system_proxy_cmds, AM_N(system_proxy_cmds), 0, free
};

static rl_result open_proxy(rl_ipc_req *r, const rl_ipc_class *cls)
{
    uint64_t *pid = malloc(sizeof *pid);
    if (!pid)
        return KR_OUT_OF_RESOURCE;
    *pid = rl_srv_pid(r);
    rl_ipc_object *o = rl_srv_object(cls, pid);
    if (!o) {
        free(pid);
        return KR_OUT_OF_RESOURCE;
    }
    return rl_ipc_push_object(r, o);
}

static rl_result oe_open_application_proxy(rl_ipc_req *r)
{
    return open_proxy(r, hos_switch2() ? &application_proxy2_class : &application_proxy_class);
}
static rl_result ae_open_system_applet_proxy(rl_ipc_req *r) { return open_proxy(r, &system_proxy_class); }
static rl_result ae_open_library_applet_proxy(rl_ipc_req *r) { return open_proxy(r, &library_proxy_class); }
static rl_result ae_open_system_application_proxy(rl_ipc_req *r) { return open_proxy(r, &application_proxy_class); }

static const rl_ipc_cmd oe_cmds[] = {
    { 0, "OpenApplicationProxy", oe_open_application_proxy },
};

static const rl_ipc_cmd oe2_cmds[] = {
    { 0, "OpenApplicationProxy", oe_open_application_proxy },
    { 10, "OpenApplicationProxy2", oe_open_application_proxy },
};

static const rl_ipc_cmd ae_cmds[] = {
    { 100, "OpenSystemAppletProxy", ae_open_system_applet_proxy },
    { 200, "OpenLibraryAppletProxyOld", ae_open_library_applet_proxy },
    { 201, "OpenLibraryAppletProxy", ae_open_library_applet_proxy },
    { 350, "OpenSystemApplicationProxy", ae_open_system_application_proxy },
};

static const rl_ipc_class oe_class = { "appletOE", oe_cmds, AM_N(oe_cmds), 0, NULL };
static const rl_ipc_class oe2_class = { "appletOE", oe2_cmds, AM_N(oe2_cmds), 0, NULL };
static const rl_ipc_class ae_class = { "appletAE", ae_cmds, AM_N(ae_cmds), 0, NULL };

static rl_ipc_object *oe_factory(void) { return rl_srv_object(hos_switch2() ? &oe2_class : &oe_class, NULL); }
static rl_ipc_object *ae_factory(void) { return rl_srv_object(&ae_class, NULL); }

void rl_srv_install_am(void)
{
    pthread_mutex_lock(&am_mu);
    if (am_msg_event)
        hos_kobj_unref(&am_msg_event->obj);
    if (am_res_event)
        hos_kobj_unref(&am_res_event->obj);
    am_msg_event = NULL;
    am_res_event = NULL;
    am_qhead = 0;
    am_qcount = 0;
    am_focus = RL_AM_FOCUS_IN;
    am_docked = rl_setting_enum("system.console_mode") == 1;
    rl_log(RL_LOG_INFO, "am", "console mode at start: %s", am_docked ? "docked" : "handheld");
    am_language = RL_AM_LANGUAGE_CODE_EN_US;
    am_user_high = 1;
    am_user_low = 0;
    am_npids = 0;
    ensure_events();
    enqueue(RL_AM_MSG_FOCUS_STATE_CHANGED);
    enqueue(RL_AM_MSG_CHANGE_INTO_FOREGROUND);
    sync_event();
    pthread_mutex_unlock(&am_mu);
    am_app_reset();
    am_s2_reset();
    rl_ipc_register_service("appletOE", oe_factory);
    rl_ipc_register_service("appletAE", ae_factory);
}
