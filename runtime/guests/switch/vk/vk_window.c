/**
 * @file vk_window.c
 * @brief The player side of the forwarding layer: the window surface
 * provider for VK_NN_vi_surface and the loader hook that starts the layer and
 * links guest modules to it.
 *
 * Each vi layer the guest presents on gets one host window, opened at the
 * layer's size when the guest makes its first surface on it. The guest
 * passes an nn::vi::NativeWindowHandle, whose layout is the SDK's; the
 * provider maps a handle it has not seen to the layer the guest opened or
 * created last (rl_vi_render_layer) and remembers the pair. Window close and
 * focus go to am; resizes only bump the window's generation, which the
 * swapchain code (vk_swap.c) follows.
 */
#include "am.h"
#include "vi.h"
#include "vkf.h"

#include <relift/window.h>

#include <stdlib.h>

#define MAX_LAYERS 16u
#define MAX_SURFACES 64u
#define MAP_WINDOW_39 (8ull << 30)
#define MAP_WINDOW_36 (2ull << 30)
#define MAP_ALIGN (2ull << 20)

typedef struct vkw_layer {
    uint64_t layer;
    rl_window *win;
    uint32_t width, height;
} vkw_layer;

typedef struct vkw_surface {
    uint64_t surface;
    uint64_t guest_window;
    vkw_layer *layer;
} vkw_surface;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static vkw_layer g_layers[MAX_LAYERS];
static uint32_t g_nlayers;
static vkw_surface g_surfaces[MAX_SURFACES];
static uint32_t g_nsurfaces;
static int g_closes;

static vkw_layer *layer_of_window(uint64_t guest_window)
{
    for (uint32_t i = g_nsurfaces; i-- > 0;)
        if (g_surfaces[i].guest_window == guest_window)
            return g_surfaces[i].layer;
    return NULL;
}

static vkw_layer *layer_open(const rl_window_backend *b, uint64_t layer)
{
    for (uint32_t i = 0; i < g_nlayers; i++)
        if (g_layers[i].layer == layer)
            return &g_layers[i];
    if (g_nlayers == MAX_LAYERS)
        return NULL;
    uint32_t w, h;
    if (!rl_vi_layer_size(layer, &w, &h))
        vi_get_resolution(&w, &h);
    rl_window *win = b->open("relift", w, h);
    if (!win)
        return NULL;
    vkw_layer *l = &g_layers[g_nlayers++];
    *l = (vkw_layer){ layer, win, w, h };
    rl_log(RL_LOG_INFO, "vk", "vi layer %llu shows in a %ux%u %s window", (unsigned long long)layer, w, h, b->name);
    return l;
}

static int32_t window_create(void *user, void *host_instance, void *gipa, uint64_t guest_window, uint64_t *surface)
{
    (void)user;
    (void)gipa;
    const rl_window_backend *b = rl_window_backend_current();
    if (!b)
        return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&g_lock);
    vkw_layer *l = layer_of_window(guest_window);
    uint64_t layer = 0;
    if (!l) {
        rl_vi_render_layer(&layer);
        l = layer_open(b, layer);
    }
    int32_t r = VK_ERROR_INITIALIZATION_FAILED;
    if (l && g_nsurfaces < MAX_SURFACES) {
        r = b->vulkan_surface(l->win, host_instance, surface);
        if (r >= 0)
            g_surfaces[g_nsurfaces++] = (vkw_surface){ *surface, guest_window, l };
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

static void window_extent(void *user, uint64_t surface, uint32_t guest[2], uint32_t host[2], uint64_t *generation)
{
    (void)user;
    vi_get_resolution(&guest[0], &guest[1]);
    host[0] = guest[0];
    host[1] = guest[1];
    *generation = 0;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = g_nsurfaces; i-- > 0;)
        if (g_surfaces[i].surface == surface) {
            vkw_layer *l = g_surfaces[i].layer;
            guest[0] = l->width;
            guest[1] = l->height;
            l->win->b->size(l->win, &host[0], &host[1]);
            *generation = __atomic_load_n(&l->win->generation, __ATOMIC_ACQUIRE);
            break;
        }
    pthread_mutex_unlock(&g_lock);
}

static int32_t window_recreate(void *user, void *host_instance, void *gipa, uint64_t surface, uint64_t *fresh)
{
    (void)user;
    (void)gipa;
    const rl_window_backend *b = rl_window_backend_current();
    if (!b)
        return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&g_lock);
    int32_t r = VK_ERROR_SURFACE_LOST_KHR;
    for (uint32_t i = g_nsurfaces; i-- > 0;)
        if (g_surfaces[i].surface == surface) {
            r = b->vulkan_surface(g_surfaces[i].layer->win, host_instance, fresh) >= 0 ? VK_SUCCESS : VK_NOT_READY;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    return r;
}

static rl_vk_surface_provider g_window_provider = {
    .create = window_create,
    .extent = window_extent,
    .recreate = window_recreate,
};

const rl_vk_surface_provider *rl_vk_window_surfaces(void)
{
    const rl_window_backend *b = rl_window_backend_current();
    if (!b || !b->vulkan_extensions || !b->vulkan_surface)
        return NULL;
    g_window_provider.instance_extensions = b->vulkan_extensions();
    return g_window_provider.instance_extensions ? &g_window_provider : NULL;
}

static void on_window(void *user, rl_window *w, int event)
{
    (void)user;
    (void)w;
    switch (event) {
    case RL_WINDOW_CLOSE:
        if (__atomic_add_fetch(&g_closes, 1, __ATOMIC_RELAXED) == 1) {
            rl_log(RL_LOG_INFO, "vk", "window closed: asking the guest to exit (close again to quit at once)");
            rl_am_request_exit();
        } else {
            rl_log(RL_LOG_INFO, "vk", "window closed again: quitting");
            rl_process_exit(0);
        }
        break;
    case RL_WINDOW_FOCUS_GAINED:
        rl_am_set_focus(true);
        break;
    case RL_WINDOW_FOCUS_LOST:
        rl_am_set_focus(false);
        break;
    default:
        break;
    }
}

static bool g_up;

static int hook_vm_set(void *user, uint64_t addr, uint64_t size, uint32_t perm, int content)
{
    (void)user;
    return rl_hos_reserved_set(addr, size, perm, content);
}

static int hook_start(void *user, uint64_t *code_free)
{
    (void)user;
    uint64_t bytes = (rl_vk_thunk_bytes() + HOS_PAGE - 1) & ~(HOS_PAGE - 1);
    uint64_t map_size = hos.lay.bits == 39 ? MAP_WINDOW_39 : MAP_WINDOW_36, map_base = 0;
    if (rl_hos_reserve_at(*code_free, bytes) != KR_SUCCESS || rl_hos_reserve(map_size, MAP_ALIGN, &map_base) != KR_SUCCESS) {
        rl_log(RL_LOG_WARN, "vk", "no guest address space for Vulkan forwarding; the guest's own Vulkan runs");
        return 0;
    }
    const rl_window_backend *b = rl_window_backend_current();
    const rl_vk_surface_provider *surfaces = rl_vk_window_surfaces();
    rl_vk_config cfg = {
        .thunk_base = *code_free,
        .vm = &hos.vm,
        .map_base = map_base,
        .map_size = map_size,
        .map_mode = RL_VK_MAP_AUTO,
        .vm_set = hook_vm_set,
        .surfaces = surfaces ? surfaces : rl_vk_headless_surfaces(),
        .gipa = surfaces && b->vulkan_gipa ? b->vulkan_gipa() : NULL,
    };
    *code_free += bytes;
    if (rl_vk_init(&cfg) != 0) {
        rl_log(RL_LOG_WARN, "vk", "no host Vulkan; the guest's own Vulkan runs");
        return 0;
    }
    const rl_unit *u = rl_vk_unit();
    if (rl_units_register(&u, 1) != 0)
        return -1;
    if (surfaces)
        rl_window_set_events(on_window, NULL);
    rl_log(RL_LOG_INFO, "vk", "Vulkan forwarding up: thunks at 0x%llx, mapped memory at 0x%llx, %s surfaces",
           (unsigned long long)cfg.thunk_base, (unsigned long long)map_base, surfaces ? b->name : "headless");
    g_up = true;
    return 0;
}

static uint64_t hook_import(void *user, const char *symbol)
{
    (void)user;
    return g_up ? rl_vk_guest_addr(symbol) : 0;
}

static rl_fn hook_replace(void *user, const char *symbol)
{
    (void)user;
    return g_up && !vkf_swap_unsupported(symbol) ? rl_vk_import(symbol) : NULL;
}

static int hook_wants(void *user, const char *symbol)
{
    (void)user;
    return !strcmp(symbol, "vkGetInstanceProcAddr") || !strcmp(symbol, "vkGetDeviceProcAddr");
}

static const rl_switch_hook g_hook = {
    .name = "vulkan-forwarding",
    .start = hook_start,
    .import = hook_import,
    .replace = hook_replace,
    .wants = hook_wants,
};

const rl_switch_hook *rl_vk_switch_hook(void) { return &g_hook; }

__attribute__((constructor)) static void register_hook(void) { (void)rl_switch_register_auto_hook(&g_hook); }
