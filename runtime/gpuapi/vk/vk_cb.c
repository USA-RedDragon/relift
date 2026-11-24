/**
 * @file vk_cb.c
 * @brief Guest callbacks: debug messengers and debug report callbacks run the
 * guest's function through the lifted-code ABI, with the callback data
 * copied onto the guest stack.
 *
 * The host driver calls a host trampoline; the trampoline finds the thunk
 * call running on this thread (the callback fires inside a guest Vulkan
 * call), copies the data below the guest sp, saves the guest registers,
 * calls the guest function and restores them. A callback from a driver
 * thread with no guest call running is logged and not delivered.
 *
 * The data stays inside the writable guest stack below sp and leaves
 * CB_RESERVE bytes of it for the guest function's frames: each string gets
 * at most half of the room still free and is cut short to fit (a host
 * validation layer reports messages of hundreds of KiB, longer than a
 * console thread's stack), and a callback whose fixed data does not fit is
 * logged and not delivered.
 * VkAllocationCallbacks are never forwarded: the host driver allocates with
 * its own allocator.
 */
#include "vkf.h"

#include "arm64.h"

#include <stdlib.h>
#include <string.h>

/** @brief One guest callback the host holds a pointer to. */
typedef struct vkf_cb {
    /** @brief Guest function. */
    uint64_t fn;
    /** @brief The guest's pUserData, handed back unchanged. */
    uint64_t user;
    /** @brief Host handle that owns the record (messenger, callback or instance); 0 until adopted. */
    uint64_t owner;
    /** @brief Next record. */
    struct vkf_cb *next;
} vkf_cb;

/** @brief Guest stack bytes below the callback data kept free for the guest function's frames. */
#define CB_RESERVE (64u << 10)

static vkf_cb *g_cbs;
static pthread_mutex_t g_cb_lock = PTHREAD_MUTEX_INITIALIZER;

static vkf_cb *cb_new(uint64_t fn, uint64_t user)
{
    vkf_cb *cb = calloc(1, sizeof *cb);
    if (!cb)
        abort();
    cb->fn = fn;
    cb->user = user;
    pthread_mutex_lock(&g_cb_lock);
    cb->next = g_cbs;
    g_cbs = cb;
    pthread_mutex_unlock(&g_cb_lock);
    return cb;
}

void vkf_cb_release(uint64_t owner)
{
    pthread_mutex_lock(&g_cb_lock);
    for (vkf_cb **p = &g_cbs; *p;) {
        if ((*p)->owner == owner) {
            vkf_cb *dead = *p;
            *p = dead->next;
            free(dead);
        } else
            p = &(*p)->next;
    }
    pthread_mutex_unlock(&g_cb_lock);
}

void vkf_cb_adopt(uint64_t owner)
{
    pthread_mutex_lock(&g_cb_lock);
    for (vkf_cb *cb = g_cbs; cb; cb = cb->next)
        if (!cb->owner)
            cb->owner = owner;
    pthread_mutex_unlock(&g_cb_lock);
}

vkf_gstack vkf_gstack_at(uint64_t sp)
{
    uint64_t lo = vkf_writable_below(sp);
    uint64_t lim = sp - lo > CB_RESERVE ? lo + CB_RESERVE : sp;
    return (vkf_gstack){ .sp = sp, .lim = lim, .fits = true };
}

static uint64_t room(const vkf_gstack *st) { return st->sp - st->lim > 15 ? (st->sp - st->lim - 15) : 0; }

uint64_t vkf_push(vkf_call *k, vkf_gstack *st, const void *src, size_t n)
{
    if (n > room(st)) {
        st->fits = false;
        return 0;
    }
    st->sp = (st->sp - n) & ~(uint64_t)15;
    memcpy(k->mem + st->sp, src, n);
    return st->sp;
}

static uint64_t push_str(vkf_call *k, vkf_gstack *st, const char *s)
{
    if (!s)
        return 0;
    size_t n = strlen(s), cap = (size_t)(room(st) / 2);
    if (!cap) {
        st->fits = false;
        return 0;
    }
    if (n >= cap) {
        n = cap - 1;
        if (rl_once(0x4342534852540000ull))
            rl_log(RL_LOG_INFO, "vk", "a debug message longer than the guest stack holds reaches the guest cut to %zu bytes", n);
    }
    st->sp = (st->sp - (n + 1)) & ~(uint64_t)15;
    memcpy(k->mem + st->sp, s, n);
    k->mem[st->sp + n] = 0;
    return st->sp;
}

uint64_t vkf_guest_call(vkf_call *k, uint64_t fn, const uint64_t *args, unsigned n, uint64_t sp)
{
    rl_ctx *c = k->c;
    uint8_t saved[offsetof(rl_ctx, pend)];
    memcpy(saved, c, sizeof saved);
    for (unsigned i = 0; i < n && i < 8; i++)
        c->x[i] = args[i];
    c->sp = sp & ~(uint64_t)15;
    c->x[30] = 0;
    int rc = rl_call_guest(c, k->mem, fn);
    uint64_t ret = c->x[0];
    memcpy(c, saved, sizeof saved);
    if (rc != RL_OK)
        k->rc = rc;
    return ret;
}

static void *as_ptr(uint64_t g) { return (void *)(uintptr_t)g; }

static uint64_t guest_object(VkObjectType t, uint64_t h)
{
    uint32_t kind = t == VK_OBJECT_TYPE_INSTANCE          ? VKG_KIND_VkInstance
                    : t == VK_OBJECT_TYPE_PHYSICAL_DEVICE ? VKG_KIND_VkPhysicalDevice
                    : t == VK_OBJECT_TYPE_DEVICE          ? VKG_KIND_VkDevice
                    : t == VK_OBJECT_TYPE_QUEUE           ? VKG_KIND_VkQueue
                    : t == VK_OBJECT_TYPE_COMMAND_BUFFER  ? VKG_KIND_VkCommandBuffer
                                                          : 0;
    if (!kind)
        return h;
    vkf_obj *o = vkf_obj_find_host(kind, as_ptr(h));
    return o ? vkf_id(o) : 0;
}

static uint64_t push_labels(vkf_call *k, vkf_gstack *st, const VkDebugUtilsLabelEXT *l, uint32_t n)
{
    if (!l || !n)
        return 0;
    VkDebugUtilsLabelEXT *g = vkf_alloc(k, n * sizeof *g);
    for (uint32_t i = 0; i < n; i++) {
        g[i] = l[i];
        g[i].pNext = NULL;
        g[i].pLabelName = as_ptr(push_str(k, st, l[i].pLabelName));
    }
    return vkf_push(k, st, g, n * sizeof *g);
}

static VkBool32 VKAPI_PTR utils_tramp(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT types,
                                      const VkDebugUtilsMessengerCallbackDataEXT *d, void *user)
{
    vkf_cb *cb = user;
    vkf_call *k = vkf_current();
    if (!k || !cb || !cb->fn || !d) {
        rl_log(RL_LOG_INFO, "vk", "debug message on a host thread: %s", d && d->pMessage ? d->pMessage : "");
        return VK_FALSE;
    }
    vkf_gstack st = vkf_gstack_at(k->c->sp);
    VkDebugUtilsMessengerCallbackDataEXT g = *d;
    g.pNext = NULL;
    g.pMessageIdName = as_ptr(push_str(k, &st, d->pMessageIdName));
    g.pMessage = as_ptr(push_str(k, &st, d->pMessage));
    g.pQueueLabels = as_ptr(push_labels(k, &st, d->pQueueLabels, d->queueLabelCount));
    g.pCmdBufLabels = as_ptr(push_labels(k, &st, d->pCmdBufLabels, d->cmdBufLabelCount));
    if (d->pObjects && d->objectCount) {
        VkDebugUtilsObjectNameInfoEXT *o = vkf_alloc(k, d->objectCount * sizeof *o);
        for (uint32_t i = 0; i < d->objectCount; i++) {
            o[i] = d->pObjects[i];
            o[i].pNext = NULL;
            o[i].objectHandle = guest_object(o[i].objectType, o[i].objectHandle);
            o[i].pObjectName = as_ptr(push_str(k, &st, d->pObjects[i].pObjectName));
        }
        g.pObjects = as_ptr(vkf_push(k, &st, o, d->objectCount * sizeof *o));
    } else
        g.pObjects = NULL;
    uint64_t gd = vkf_push(k, &st, &g, sizeof g);
    if (!st.fits) {
        rl_log(RL_LOG_INFO, "vk", "debug message too large for the guest stack, not delivered: %s", d->pMessage ? d->pMessage : "");
        return VK_FALSE;
    }
    uint64_t args[4] = { (uint32_t)sev, types, gd, cb->user };
    return (VkBool32)vkf_guest_call(k, cb->fn, args, 4, st.sp);
}

static VkBool32 VKAPI_PTR report_tramp(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT type, uint64_t object, size_t location,
                                       int32_t code, const char *prefix, const char *message, void *user)
{
    vkf_cb *cb = user;
    vkf_call *k = vkf_current();
    if (!k || !cb || !cb->fn) {
        rl_log(RL_LOG_INFO, "vk", "debug report on a host thread: %s", message ? message : "");
        return VK_FALSE;
    }
    vkf_gstack st = vkf_gstack_at(k->c->sp);
    uint64_t gp = push_str(k, &st, prefix);
    uint64_t gm = push_str(k, &st, message);
    if (!st.fits) {
        rl_log(RL_LOG_INFO, "vk", "debug report too large for the guest stack, not delivered: %s", message ? message : "");
        return VK_FALSE;
    }
    uint64_t args[8] = { flags, (uint32_t)type, guest_object((VkObjectType)type, object), location, (uint32_t)code, gp, gm, cb->user };
    return (VkBool32)vkf_guest_call(k, cb->fn, args, 8, st.sp);
}

void vkh_in_VkDebugUtilsMessengerCreateInfoEXT(vkf_call *k, VkDebugUtilsMessengerCreateInfoEXT *h, uint64_t g)
{
    memcpy(h, k->mem + g, sizeof *h);
    h->pNext = vkg_chain_in(k, vkf_g64(k, g + offsetof(VkDebugUtilsMessengerCreateInfoEXT, pNext)));
    uint64_t fn = vkf_g64(k, g + offsetof(VkDebugUtilsMessengerCreateInfoEXT, pfnUserCallback));
    uint64_t user = vkf_g64(k, g + offsetof(VkDebugUtilsMessengerCreateInfoEXT, pUserData));
    h->pfnUserCallback = utils_tramp;
    h->pUserData = cb_new(fn, user);
}

void vkh_out_VkDebugUtilsMessengerCreateInfoEXT(vkf_call *k, uint64_t g, const VkDebugUtilsMessengerCreateInfoEXT *h)
{
    (void)k;
    (void)g;
    (void)h;
}

void vkh_in_VkDebugReportCallbackCreateInfoEXT(vkf_call *k, VkDebugReportCallbackCreateInfoEXT *h, uint64_t g)
{
    memcpy(h, k->mem + g, sizeof *h);
    h->pNext = vkg_chain_in(k, vkf_g64(k, g + offsetof(VkDebugReportCallbackCreateInfoEXT, pNext)));
    uint64_t fn = vkf_g64(k, g + offsetof(VkDebugReportCallbackCreateInfoEXT, pfnCallback));
    uint64_t user = vkf_g64(k, g + offsetof(VkDebugReportCallbackCreateInfoEXT, pUserData));
    h->pfnCallback = report_tramp;
    h->pUserData = cb_new(fn, user);
}

void vkh_out_VkDebugReportCallbackCreateInfoEXT(vkf_call *k, uint64_t g, const VkDebugReportCallbackCreateInfoEXT *h)
{
    (void)k;
    (void)g;
    (void)h;
}

uint64_t vkh_vkCreateDebugUtilsMessengerEXT(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkInstance inst = vkf_first(&k, VKG_KIND_VkInstance, c->x[0]);
    if (!inst)
        return vkf_bad_handle(&k, VKG_vkCreateDebugUtilsMessengerEXT, c->x[0]);
    PFN_vkCreateDebugUtilsMessengerEXT f = VKF_FN(k.fns, vkCreateDebugUtilsMessengerEXT);
    if (!f)
        return vkf_no_fn(&k, VKG_vkCreateDebugUtilsMessengerEXT);
    const VkDebugUtilsMessengerCreateInfoEXT *ci = vkg_ain_VkDebugUtilsMessengerCreateInfoEXT(&k, c->x[1], 1);
    VkDebugUtilsMessengerEXT m = NULL;
    VkResult r = f(inst, ci, NULL, &m);
    if (r >= 0) {
        vkf_cb_adopt((uint64_t)(uintptr_t)m);
        vkf_s64(&k, c->x[3], (uint64_t)(uintptr_t)m);
    } else
        vkf_cb_release(0);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateDebugUtilsMessengerEXT, r);
    return vkf_end(&k);
}

uint64_t vkh_vkDestroyDebugUtilsMessengerEXT(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkInstance inst = vkf_first(&k, VKG_KIND_VkInstance, c->x[0]);
    if (!inst)
        return vkf_bad_handle(&k, VKG_vkDestroyDebugUtilsMessengerEXT, c->x[0]);
    PFN_vkDestroyDebugUtilsMessengerEXT f = VKF_FN(k.fns, vkDestroyDebugUtilsMessengerEXT);
    if (f && c->x[1]) {
        f(inst, (VkDebugUtilsMessengerEXT)(uintptr_t)c->x[1], NULL);
        vkf_cb_release(c->x[1]);
    }
    return vkf_end(&k);
}

uint64_t vkh_vkCreateDebugReportCallbackEXT(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkInstance inst = vkf_first(&k, VKG_KIND_VkInstance, c->x[0]);
    if (!inst)
        return vkf_bad_handle(&k, VKG_vkCreateDebugReportCallbackEXT, c->x[0]);
    PFN_vkCreateDebugReportCallbackEXT f = VKF_FN(k.fns, vkCreateDebugReportCallbackEXT);
    if (!f)
        return vkf_no_fn(&k, VKG_vkCreateDebugReportCallbackEXT);
    const VkDebugReportCallbackCreateInfoEXT *ci = vkg_ain_VkDebugReportCallbackCreateInfoEXT(&k, c->x[1], 1);
    VkDebugReportCallbackEXT cb = NULL;
    VkResult r = f(inst, ci, NULL, &cb);
    if (r >= 0) {
        vkf_cb_adopt((uint64_t)(uintptr_t)cb);
        vkf_s64(&k, c->x[3], (uint64_t)(uintptr_t)cb);
    } else
        vkf_cb_release(0);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateDebugReportCallbackEXT, r);
    return vkf_end(&k);
}

uint64_t vkh_vkDestroyDebugReportCallbackEXT(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkInstance inst = vkf_first(&k, VKG_KIND_VkInstance, c->x[0]);
    if (!inst)
        return vkf_bad_handle(&k, VKG_vkDestroyDebugReportCallbackEXT, c->x[0]);
    PFN_vkDestroyDebugReportCallbackEXT f = VKF_FN(k.fns, vkDestroyDebugReportCallbackEXT);
    if (f && c->x[1]) {
        f(inst, (VkDebugReportCallbackEXT)(uintptr_t)c->x[1], NULL);
        vkf_cb_release(c->x[1]);
    }
    return vkf_end(&k);
}
