/**
 * @file srv_vi.c
 * @brief vi:u, vi:s and vi:m, the display services they hand out and the
 * vsync event.
 *
 * Ported from Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors),
 * Ryujinx.HLE/HOS/Services/Vi. Request layouts follow libnx (ISC) services/vi.c.
 */
#include "vi.h"

#include "srv_link.h"

#include <stdlib.h>
#include <time.h>

enum { VI_TYPE_APPLICATION = 0, VI_TYPE_MANAGER = 1, VI_TYPE_SYSTEM = 2 };

#define VI_DISPLAYS 5
#define VI_NAME_SIZE 0x40u
#define VI_DISPLAY_INFO 0x60u

static const char *const display_names[VI_DISPLAYS] = { "Default", "External", "Edid", "Internal", "Null" };

static uint32_t res_w = 1280, res_h = 720;

static pthread_mutex_t vsync_lock = PTHREAD_MUTEX_INITIALIZER;
static hos_revent *vsync_ev;
static pthread_t vsync_thread;
static int64_t vsync_epoch;
static bool vsync_running;
static bool vsync_quit;

typedef struct disp_state {
    int type;
    bool opened[VI_DISPLAYS];
    bool vsync_taken[VI_DISPLAYS];
} disp_state;

typedef struct sub_state {
    int type;
} sub_state;

void rl_vi_set_resolution(uint32_t width, uint32_t height)
{
    __atomic_store_n(&res_w, width, __ATOMIC_RELAXED);
    __atomic_store_n(&res_h, height, __ATOMIC_RELAXED);
}

void vi_get_resolution(uint32_t *w, uint32_t *h)
{
    *w = __atomic_load_n(&res_w, __ATOMIC_RELAXED);
    *h = __atomic_load_n(&res_h, __ATOMIC_RELAXED);
}

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

int64_t vi_vsync_after(int64_t t)
{
    int64_t e = __atomic_load_n(&vsync_epoch, __ATOMIC_ACQUIRE);
    if (!e) {
        int64_t mine = mono_ns();
        e = __atomic_compare_exchange_n(&vsync_epoch, &e, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? mine : e;
    }
    if (t < e)
        return e;
    return e + ((t - e) / VI_VSYNC_NS + 1) * VI_VSYNC_NS;
}

static void *vsync_main(void *arg)
{
    (void)arg;
    int64_t next = vi_vsync_after(mono_ns());
    for (;;) {
        int64_t now = mono_ns();
        if (next > now) {
            struct timespec d = { (time_t)((next - now) / 1000000000ll), (long)((next - now) % 1000000000ll) };
            nanosleep(&d, NULL);
        }
        pthread_mutex_lock(&vsync_lock);
        bool quit = vsync_quit;
        hos_revent *e = vsync_ev;
        if (!quit && e)
            hos_kobj_ref(&e->obj);
        pthread_mutex_unlock(&vsync_lock);
        if (quit)
            return NULL;
        if (e) {
            rl_hos_event_signal(e);
            hos_kobj_unref(&e->obj);
        }
        int64_t done = mono_ns();
        next = vi_vsync_after(done > next ? done : next);
    }
}

hos_revent *vi_vsync_event(bool start)
{
    pthread_mutex_lock(&vsync_lock);
    if (!vsync_ev)
        vsync_ev = rl_hos_event_create();
    if (start && !vsync_running && !vsync_quit && vsync_ev && pthread_create(&vsync_thread, NULL, vsync_main, NULL) == 0)
        vsync_running = true;
    hos_revent *e = vsync_ev;
    pthread_mutex_unlock(&vsync_lock);
    return e;
}

void rl_vi_vsync_now(void)
{
    hos_revent *e = vi_vsync_event(false);
    if (e)
        rl_hos_event_signal(e);
}

void rl_vi_stop(void)
{
    pthread_mutex_lock(&vsync_lock);
    vsync_quit = true;
    bool join = vsync_running;
    vsync_running = false;
    pthread_mutex_unlock(&vsync_lock);
    if (join)
        pthread_join(vsync_thread, NULL);
    vi_layers_deactivate();
}

static void vsync_reset(void)
{
    rl_vi_stop();
    pthread_mutex_lock(&vsync_lock);
    vsync_quit = false;
    hos_revent *old = vsync_ev;
    vsync_ev = NULL;
    pthread_mutex_unlock(&vsync_lock);
    if (old)
        hos_kobj_unref(&old->obj);
}

static rl_result write_window(rl_ipc_req *r, int32_t binder, uint64_t *len)
{
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    if (!out.size)
        return SF_RESULT_INVALID_HEADER;
    return vi_native_window(binder, out, len);
}

static rl_result create_stray_layer(rl_ipc_req *r)
{
    struct {
        uint32_t flags, pad;
        uint64_t display;
    } a;
    if (!rl_srv_in(r, &a, sizeof a))
        return SF_RESULT_INVALID_HEADER;
    if (!rl_ipc_auto_out(r, 0).size)
        return SF_RESULT_INVALID_HEADER;
    uint64_t id;
    rl_result rc = vi_layer_create(rl_srv_pid(r), VI_LAYER_STRAY, &id);
    if (rc != KR_SUCCESS)
        return rc;
    uint64_t len = 0;
    rc = write_window(r, vi_layer_binder(id), &len);
    if (rc != KR_SUCCESS) {
        vi_layer_destroy_stray(id);
        return rc;
    }
    uint64_t *o = rl_ipc_out(r, 16);
    o[0] = id;
    o[1] = len;
    return KR_SUCCESS;
}

static rl_result get_display_resolution(rl_ipc_req *r)
{
    uint64_t id;
    if (!rl_srv_in(r, &id, 8))
        return SF_RESULT_INVALID_HEADER;
    uint32_t w, h;
    vi_get_resolution(&w, &h);
    uint64_t *o = rl_ipc_out(r, 16);
    o[0] = w;
    o[1] = h;
    return KR_SUCCESS;
}

static rl_result sys_create_stray(rl_ipc_req *r) { return create_stray_layer(r); }

static rl_result sys_get_display_mode(rl_ipc_req *r)
{
    uint64_t id;
    if (!rl_srv_in(r, &id, 8))
        return SF_RESULT_INVALID_HEADER;
    uint32_t w, h;
    vi_get_resolution(&w, &h);
    float hz = 60.0f;
    uint8_t *o = rl_ipc_out(r, 16);
    memcpy(o, &w, 4);
    memcpy(o + 4, &h, 4);
    memcpy(o + 8, &hz, 4);
    return KR_SUCCESS;
}

static rl_result sys_set_layer_size(rl_ipc_req *r)
{
    uint64_t a[3];
    if (!rl_srv_in(r, a, sizeof a))
        return SF_RESULT_INVALID_HEADER;
    return vi_layer_set_size(a[0], a[1], a[2]);
}

static const rl_ipc_cmd system_cmds[] = {
    { 2203, "SetLayerSize", sys_set_layer_size },
    RL_SRV_STUB(2205, "SetLayerZ"),
    RL_SRV_STUB(2207, "SetLayerVisibility"),
    { 2312, "CreateStrayLayer", sys_create_stray },
    { 3200, "GetDisplayMode", sys_get_display_mode },
};

static const rl_ipc_class system_class = { "ISystemDisplayService", system_cmds, sizeof system_cmds / sizeof system_cmds[0], 0, free };

static rl_result mgr_create_managed(rl_ipc_req *r)
{
    struct {
        uint32_t flags, pad;
        uint64_t display, aruid;
    } a;
    if (!rl_srv_in(r, &a, sizeof a))
        return SF_RESULT_INVALID_HEADER;
    uint64_t id;
    rl_result rc = vi_layer_create(rl_srv_pid(r), VI_LAYER_MANAGED_CLOSED, &id);
    if (rc != KR_SUCCESS)
        return rc;
    *(uint64_t *)rl_ipc_out(r, 8) = id;
    return KR_SUCCESS;
}

static rl_result mgr_destroy_managed(rl_ipc_req *r)
{
    uint64_t id;
    if (!rl_srv_in(r, &id, 8))
        return SF_RESULT_INVALID_HEADER;
    return vi_layer_destroy_managed(id);
}

static const rl_ipc_cmd manager_cmds[] = {
    { 1102, "GetDisplayResolution", get_display_resolution },
    { 2010, "CreateManagedLayer", mgr_create_managed },
    { 2011, "DestroyManagedLayer", mgr_destroy_managed },
    { 2012, "CreateStrayLayer", sys_create_stray },
    RL_SRV_STUB(4201, "SetDisplayAlpha"),
    RL_SRV_STUB(4205, "SetDisplayPowerState"),
    RL_SRV_STUB(6000, "AddToLayerStack"),
    RL_SRV_STUB(6002, "SetLayerVisibility"),
    RL_SRV_STUB(7000, "SetContentVisibility"),
};

static const rl_ipc_class manager_class = { "IManagerDisplayService", manager_cmds, sizeof manager_cmds / sizeof manager_cmds[0], 0, free };

static rl_ipc_object *sub_new(const rl_ipc_class *cls, int type)
{
    sub_state *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->type = type;
    rl_ipc_object *o = rl_srv_object(cls, s);
    if (!o)
        free(s);
    return o;
}

static rl_result push_sub(rl_ipc_req *r, rl_ipc_object *o)
{
    if (!o)
        return KR_OUT_OF_MEMORY;
    return rl_ipc_push_object(r, o);
}

static disp_state *ds(rl_ipc_req *r) { return r->self->state; }

static rl_result d_relay(rl_ipc_req *r) { return push_sub(r, vi_binder_new()); }

static rl_result d_system(rl_ipc_req *r)
{
    if (ds(r)->type == VI_TYPE_APPLICATION)
        return VI_RESULT_PERMISSION_DENIED;
    return push_sub(r, sub_new(&system_class, ds(r)->type));
}

static rl_result d_manager(rl_ipc_req *r)
{
    if (ds(r)->type == VI_TYPE_APPLICATION)
        return VI_RESULT_PERMISSION_DENIED;
    return push_sub(r, sub_new(&manager_class, ds(r)->type));
}

static rl_result d_indirect(rl_ipc_req *r)
{
    if (ds(r)->type == VI_TYPE_APPLICATION)
        return VI_RESULT_PERMISSION_DENIED;
    return push_sub(r, vi_binder_new());
}

static void fill_info(uint8_t *e, int i)
{
    uint32_t w, h;
    vi_get_resolution(&w, &h);
    memset(e, 0, VI_DISPLAY_INFO);
    memcpy(e, display_names[i], strlen(display_names[i]));
    e[0x40] = i != 4;
    uint64_t limit = i != 4, width = i == 2 ? 0 : w, height = i == 2 ? 0 : h;
    memcpy(e + 0x48, &limit, 8);
    memcpy(e + 0x50, &width, 8);
    memcpy(e + 0x58, &height, 8);
}

static rl_result d_list_displays(rl_ipc_req *r)
{
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    uint8_t e[VI_DISPLAY_INFO];
    fill_info(e, 0);
    if (!rl_ipc_buf_write(&out, e, sizeof e))
        return SF_RESULT_INVALID_HEADER;
    *(uint64_t *)rl_ipc_out(r, 8) = 1;
    return KR_SUCCESS;
}

static rl_result open_display(rl_ipc_req *r, const char *name)
{
    if (!name[0])
        return VI_RESULT_INVALID_VALUE;
    int id = -1;
    for (int i = 0; i < VI_DISPLAYS; i++)
        if (strcmp(display_names[i], name) == 0)
            id = i;
    if (id < 0)
        return VI_RESULT_INVALID_VALUE;
    disp_state *s = ds(r);
    if (s->opened[id])
        return VI_RESULT_ALREADY_OPENED;
    s->opened[id] = true;
    *(uint64_t *)rl_ipc_out(r, 8) = (uint64_t)id;
    return KR_SUCCESS;
}

static rl_result d_open_display(rl_ipc_req *r)
{
    char name[VI_NAME_SIZE + 1];
    if (!rl_srv_in(r, name, VI_NAME_SIZE))
        return SF_RESULT_INVALID_HEADER;
    name[VI_NAME_SIZE] = 0;
    return open_display(r, name);
}

static rl_result d_open_default_display(rl_ipc_req *r) { return open_display(r, "Default"); }

static rl_result d_close_display(rl_ipc_req *r)
{
    uint64_t id;
    if (!rl_srv_in(r, &id, 8))
        return SF_RESULT_INVALID_HEADER;
    disp_state *s = ds(r);
    if (id >= VI_DISPLAYS || !s->opened[id])
        return VI_RESULT_INVALID_VALUE;
    s->opened[id] = false;
    s->vsync_taken[id] = false;
    return KR_SUCCESS;
}

static rl_result d_open_layer(rl_ipc_req *r)
{
    struct {
        char name[VI_NAME_SIZE];
        uint64_t layer, aruid;
    } a;
    if (!rl_srv_in(r, &a, sizeof a))
        return SF_RESULT_INVALID_HEADER;
    if (!rl_ipc_auto_out(r, 0).size)
        return SF_RESULT_INVALID_HEADER;
    int32_t binder;
    rl_result rc = vi_layer_open(a.layer, &binder);
    if (rc != KR_SUCCESS)
        return rc;
    uint64_t len = 0;
    rc = write_window(r, binder, &len);
    if (rc != KR_SUCCESS) {
        vi_layer_close(a.layer);
        return rc;
    }
    *(uint64_t *)rl_ipc_out(r, 8) = len;
    return KR_SUCCESS;
}

static rl_result d_close_layer(rl_ipc_req *r)
{
    uint64_t id;
    if (!rl_srv_in(r, &id, 8))
        return SF_RESULT_INVALID_HEADER;
    return vi_layer_close(id);
}

static rl_result d_destroy_stray_layer(rl_ipc_req *r)
{
    uint64_t id;
    if (!rl_srv_in(r, &id, 8))
        return SF_RESULT_INVALID_HEADER;
    return vi_layer_destroy_stray(id);
}

static rl_result d_convert_scaling_mode(rl_ipc_req *r)
{
    uint32_t mode;
    if (!rl_srv_in(r, &mode, 4))
        return SF_RESULT_INVALID_HEADER;
    if (mode > 4)
        return VI_RESULT_INVALID_ARGUMENTS;
    if (mode != 2 && mode != 4)
        return VI_RESULT_INVALID_SCALING_MODE;
    *(uint64_t *)rl_ipc_out(r, 8) = mode == 2 ? 1 : 4;
    return KR_SUCCESS;
}

static rl_result d_layer_memory_info(rl_ipc_req *r)
{
    uint64_t in[2];
    if (!rl_srv_in(r, in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    int32_t width = (int32_t)in[0], height = (int32_t)in[1];
    if (width < 0 || height < 0)
        return VI_RESULT_INVALID_LAYER_SIZE;
    uint64_t pitch = (((uint64_t)width * 4u) + 63u) & ~63ull;
    uint64_t mem = pitch * (((uint64_t)height + 63u) & ~63ull);
    uint64_t req = (mem + 0xFFFu) & ~0xFFFull;
    uint64_t *o = rl_ipc_out(r, 16);
    o[0] = (req + 0x1FFFFu) / 0x20000u * 0x20000u;
    o[1] = 0x1000;
    return KR_SUCCESS;
}

static rl_result vsync_event(rl_ipc_req *r, bool debug)
{
    uint64_t id;
    if (!rl_srv_in(r, &id, 8))
        return SF_RESULT_INVALID_HEADER;
    disp_state *s = ds(r);
    if (id >= VI_DISPLAYS || !s->opened[id])
        return VI_RESULT_INVALID_VALUE;
    if (!debug) {
        if (s->vsync_taken[id])
            return VI_RESULT_PERMISSION_DENIED;
        s->vsync_taken[id] = true;
    }
    hos_revent *e = vi_vsync_event(true);
    if (!e)
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_copy(r, &e->obj);
}

static rl_result d_vsync(rl_ipc_req *r) { return vsync_event(r, false); }

static rl_result d_vsync_debug(rl_ipc_req *r) { return vsync_event(r, true); }

static const rl_ipc_cmd display_cmds[] = {
    { 100, "GetRelayService", d_relay },
    { 101, "GetSystemDisplayService", d_system },
    { 102, "GetManagerDisplayService", d_manager },
    { 103, "GetIndirectDisplayTransactionService", d_indirect },
    { 1000, "ListDisplays", d_list_displays },
    { 1010, "OpenDisplay", d_open_display },
    { 1011, "OpenDefaultDisplay", d_open_default_display },
    { 1020, "CloseDisplay", d_close_display },
    RL_SRV_STUB(1101, "SetDisplayEnabled"),
    { 1102, "GetDisplayResolution", get_display_resolution },
    { 2020, "OpenLayer", d_open_layer },
    { 2021, "CloseLayer", d_close_layer },
    { 2030, "CreateStrayLayer", create_stray_layer },
    { 2031, "DestroyStrayLayer", d_destroy_stray_layer },
    RL_SRV_STUB(2101, "SetLayerScalingMode"),
    { 2102, "ConvertScalingMode", d_convert_scaling_mode },
    RL_SRV_STUB(2450, "GetIndirectLayerImageMap"),
    { 2460, "GetIndirectLayerImageRequiredMemoryInfo", d_layer_memory_info },
    { 5202, "GetDisplayVsyncEvent", d_vsync },
    { 5203, "GetDisplayVsyncEventForDebug", d_vsync_debug },
};

static const rl_ipc_class display_class = { "IApplicationDisplayService", display_cmds, sizeof display_cmds / sizeof display_cmds[0], 0, free };

static rl_result get_display_service(rl_ipc_req *r, bool (*allowed)(uint32_t), int type)
{
    uint32_t mode;
    if (!rl_srv_in(r, &mode, 4))
        return SF_RESULT_INVALID_HEADER;
    if (!allowed(mode))
        return VI_RESULT_PERMISSION_DENIED;
    disp_state *s = calloc(1, sizeof *s);
    if (!s)
        return KR_OUT_OF_MEMORY;
    s->type = type;
    rl_ipc_object *o = rl_srv_object(&display_class, s);
    if (!o) {
        free(s);
        return KR_OUT_OF_MEMORY;
    }
    return rl_ipc_push_object(r, o);
}

static bool allow_user(uint32_t m) { return m == VI_TYPE_APPLICATION; }
static bool allow_manager(uint32_t m) { return m == VI_TYPE_MANAGER; }
static bool allow_system(uint32_t m) { return m == VI_TYPE_MANAGER || m == VI_TYPE_SYSTEM; }

static rl_result root_user(rl_ipc_req *r) { return get_display_service(r, allow_user, VI_TYPE_APPLICATION); }
static rl_result root_system(rl_ipc_req *r) { return get_display_service(r, allow_system, VI_TYPE_SYSTEM); }
static rl_result root_manager(rl_ipc_req *r) { return get_display_service(r, allow_manager, VI_TYPE_MANAGER); }

static const rl_ipc_cmd user_cmds[] = { { 0, "GetDisplayService", root_user } };
static const rl_ipc_cmd system_root_cmds[] = { { 1, "GetDisplayService", root_system } };
static const rl_ipc_cmd manager_root_cmds[] = { { 2, "GetDisplayService", root_manager } };

static const rl_ipc_class user_class = { "vi:u", user_cmds, 1, 0, NULL };
static const rl_ipc_class system_root_class = { "vi:s", system_root_cmds, 1, 0, NULL };
static const rl_ipc_class manager_root_class = { "vi:m", manager_root_cmds, 1, 0, NULL };

static rl_ipc_object *user_factory(void) { return rl_srv_object(&user_class, NULL); }
static rl_ipc_object *system_factory(void) { return rl_srv_object(&system_root_class, NULL); }
static rl_ipc_object *manager_factory(void) { return rl_srv_object(&manager_root_class, NULL); }

rl_result rl_srv_vi_create_managed_layer(uint64_t *layer_id)
{
    return vi_layer_create(HOS_PID, VI_LAYER_MANAGED_CLOSED, layer_id);
}

void rl_srv_install_vi(void)
{
    vi_layers_reset();
    vsync_reset();
    rl_srv_vi_layer_hook = rl_srv_vi_create_managed_layer;
    rl_ipc_register_service("vi:m", manager_factory);
    rl_ipc_register_service("vi:s", system_factory);
    rl_ipc_register_service("vi:u", user_factory);
}
