/**
 * @file vk_flip.c
 * @brief When the host display shows each present: with VK_KHR_present_id
 * and VK_KHR_present_wait, a watcher thread waits for every host present the
 * guest swapchains make and hands the time it was shown to vi_host_flip,
 * which locks the vi vsync timeline to the host display.
 *
 * The Vulkan headers relift generates hold only the Switch's API, so the few
 * names of these two extensions are spelled here.
 */
#include "vkf.h"
#include "vi.h"

#include <stdlib.h>
#include <time.h>

#define PRESENT_ID_EXT "VK_KHR_present_id"
#define PRESENT_WAIT_EXT "VK_KHR_present_wait"
#define STYPE_PRESENT_ID ((VkStructureType)1000294000)
#define STYPE_PRESENT_ID_FEATURES ((VkStructureType)1000294001)
#define STYPE_PRESENT_WAIT_FEATURES ((VkStructureType)1000248000)
#define WAIT_NS 100000000ull
#define QUEUE_MAX 8u

typedef struct present_id_features {
    VkStructureType sType;
    void *pNext;
    VkBool32 presentId;
} present_id_features;

typedef struct present_wait_features {
    VkStructureType sType;
    void *pNext;
    VkBool32 presentWait;
} present_wait_features;

typedef VkResult(VKAPI_PTR *wait_fn)(VkDevice, VkSwapchainKHR, uint64_t, uint64_t);

typedef struct flip_job {
    VkDevice dev;
    wait_fn wait;
    VkSwapchainKHR sw;
    uint64_t id;
} flip_job;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static flip_job g_q[QUEUE_MAX];
static uint32_t g_head, g_count;
static VkSwapchainKHR g_waiting;
static bool g_started;

static bool has_ext(const VkExtensionProperties *p, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(p[i].extensionName, name))
            return true;
    return false;
}

void vkf_flip_device_setup(vkf_call *k, void *phys, const char ***exts, uint32_t *n_exts, void **pnext)
{
    VkPhysicalDevice pd = phys;
    *exts = NULL;
    *n_exts = 0;
    *pnext = NULL;
    PFN_vkGetPhysicalDeviceFeatures2 gf2 = VKF_FN(k->fns, vkGetPhysicalDeviceFeatures2);
    uint32_t n = 0;
    VKF_FN(k->fns, vkEnumerateDeviceExtensionProperties)(pd, NULL, &n, NULL);
    VkExtensionProperties *p = vkf_alloc(k, (n + 1) * sizeof *p);
    VKF_FN(k->fns, vkEnumerateDeviceExtensionProperties)(pd, NULL, &n, p);
    if (!gf2 || !has_ext(p, n, PRESENT_ID_EXT) || !has_ext(p, n, PRESENT_WAIT_EXT))
        return;
    present_wait_features w = { .sType = STYPE_PRESENT_WAIT_FEATURES };
    present_id_features id = { .sType = STYPE_PRESENT_ID_FEATURES, .pNext = &w };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &id };
    gf2(pd, &f2);
    if (!id.presentId || !w.presentWait)
        return;
    const char **list = vkf_alloc(k, 2 * sizeof *list);
    list[0] = PRESENT_ID_EXT;
    list[1] = PRESENT_WAIT_EXT;
    present_wait_features *fw = vkf_alloc(k, sizeof *fw);
    *fw = (present_wait_features){ .sType = STYPE_PRESENT_WAIT_FEATURES, .presentWait = VK_TRUE };
    present_id_features *fi = vkf_alloc(k, sizeof *fi);
    *fi = (present_id_features){ .sType = STYPE_PRESENT_ID_FEATURES, .pNext = fw, .presentId = VK_TRUE };
    *exts = list;
    *n_exts = 2;
    *pnext = fi;
}

void vkf_flip_device_ready(vkf_dev *d, VkDevice dev, bool enabled)
{
    d->wait_present = NULL;
    if (!enabled)
        return;
    PFN_vkGetDeviceProcAddr gdpa = VKF_FN(&d->fns, vkGetDeviceProcAddr);
    d->wait_present = gdpa ? (void (*)(void))gdpa(dev, "vkWaitForPresentKHR") : NULL;
    rl_log(RL_LOG_INFO, "vk", "present wait %s: the vi vsync %s the host display", d->wait_present ? "on" : "missing",
           d->wait_present ? "follows" : "runs free of");
}

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static void *watch_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (!g_count)
            pthread_cond_wait(&g_cv, &g_lock);
        flip_job j = g_q[g_head];
        g_head = (g_head + 1) % QUEUE_MAX;
        g_count--;
        g_waiting = j.sw;
        pthread_mutex_unlock(&g_lock);
        VkResult r = j.wait(j.dev, j.sw, j.id, WAIT_NS);
        int64_t t = mono_ns();
        pthread_mutex_lock(&g_lock);
        g_waiting = NULL;
        pthread_cond_broadcast(&g_cv);
        if (r == VK_SUCCESS) {
            pthread_mutex_unlock(&g_lock);
            vi_host_flip(t);
            pthread_mutex_lock(&g_lock);
        }
    }
    return NULL;
}

const void *vkf_flip_chain(const vkf_dev *d, vkf_present_id *p, const void *next, uint64_t id)
{
    if (!d->wait_present)
        return next;
    p->id = id;
    p->sType = STYPE_PRESENT_ID;
    p->pNext = next;
    p->swapchainCount = 1;
    p->pPresentIds = &p->id;
    return p;
}

void vkf_flip_presented(const vkf_dev *d, VkDevice dev, VkSwapchainKHR sw, uint64_t id)
{
    if (!d->wait_present)
        return;
    pthread_mutex_lock(&g_lock);
    if (!g_started) {
        pthread_t t;
        g_started = pthread_create(&t, NULL, watch_main, NULL) == 0;
        if (g_started)
            pthread_detach(t);
    }
    if (g_started && g_count < QUEUE_MAX) {
        g_q[(g_head + g_count) % QUEUE_MAX] = (flip_job){ dev, (wait_fn)d->wait_present, sw, id };
        g_count++;
        pthread_cond_broadcast(&g_cv);
    }
    pthread_mutex_unlock(&g_lock);
}

void vkf_flip_forget(VkSwapchainKHR sw)
{
    pthread_mutex_lock(&g_lock);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g_count; i++) {
        flip_job j = g_q[(g_head + i) % QUEUE_MAX];
        if (j.sw != sw)
            g_q[(g_head + kept++) % QUEUE_MAX] = j;
    }
    g_count = kept;
    while (sw && g_waiting == sw)
        pthread_cond_wait(&g_cv, &g_lock);
    pthread_mutex_unlock(&g_lock);
}
