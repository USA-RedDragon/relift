/**
 * @file gpu_floor.c
 * @brief The GPU clock floor (gpu_floor.h).
 */
#include "gpu_floor.h"

#include <relift/settings.h>

#include "core.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__ANDROID__)
#include "hostlib.h"

#include <unistd.h>
#endif

/** @brief A span this long or longer is a pause, not a frame. */
#define SPAN_MAX_NS 1000000000ull

static rl_gpu_floor_kind g_kind = RL_GPU_FLOOR_OFF;
static uint64_t g_start_ns;

rl_gpu_floor_kind rl_gpu_floor_pick(bool on, bool android, bool queue_hint, bool adpf)
{
    if (!on)
        return RL_GPU_FLOOR_OFF;
    if (!android)
        return RL_GPU_FLOOR_NONE;
    if (queue_hint)
        return RL_GPU_FLOOR_QUEUE_HINT;
    return adpf ? RL_GPU_FLOOR_ADPF : RL_GPU_FLOOR_NONE;
}

const char *rl_gpu_floor_name(rl_gpu_floor_kind kind)
{
    switch (kind) {
    case RL_GPU_FLOOR_OFF:
        return "off";
    case RL_GPU_FLOOR_QUEUE_HINT:
        return "VK_QCOM_queue_perf_hint at the highest clock";
    case RL_GPU_FLOOR_ADPF:
        return "an ADPF performance hint session";
    default:
        return "none on this host";
    }
}

int64_t rl_gpu_floor_span(uint64_t start_ns, uint64_t end_ns)
{
    if (!start_ns || end_ns < start_ns || end_ns - start_ns >= SPAN_MAX_NS)
        return 0;
    return (int64_t)(end_ns - start_ns);
}

static uint64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

#if defined(__ANDROID__)
/** @brief VK_STRUCTURE_TYPE_PERF_HINT_INFO_QCOM. */
#define ST_PERF_HINT_INFO 1000302000
/** @brief VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_QUEUE_PERF_HINT_FEATURES_QCOM. */
#define ST_QUEUE_PERF_HINT_FEATURES 1000302001
/** @brief VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2. */
#define ST_FEATURES_2 1000059000
/** @brief VK_PERF_HINT_TYPE_FREQUENCY_MAX_QCOM: the queue's work runs at the highest clock. */
#define PERF_HINT_FREQUENCY_MAX 2u

/** @brief VkPhysicalDeviceQueuePerfHintFeaturesQCOM. */
typedef struct hint_features {
    int32_t sType;
    void *next;
    uint32_t queuePerfHint;
} hint_features;

/** @brief VkPhysicalDeviceFeatures2 with the 55 core feature flags kept out of the way. */
typedef struct features2 {
    int32_t sType;
    void *next;
    uint32_t core[55];
} features2;

/** @brief VkPerfHintInfoQCOM. */
typedef struct hint_info {
    int32_t sType;
    const void *next;
    uint32_t type;
    uint32_t scale;
} hint_info;

typedef rlvk_result(RLVK_CALL *set_hint_fn)(rlvk_handle queue, const hint_info *info);

static hint_features g_hint_feat = { ST_QUEUE_PERF_HINT_FEATURES, NULL, 1 };
static bool g_hint_asked;

/** @brief Whether the physical device lists the extension. */
static bool has_ext(const rlvk_dev *d, const char *name)
{
    uint32_t n = 0;
    if (d->vkEnumerateDeviceExtensionProperties(d->phys, NULL, &n, NULL) != RLVK_SUCCESS || !n)
        return false;
    rlvk_extension_properties *e = calloc(n, sizeof *e);
    if (!e)
        return false;
    bool found = false;
    if (d->vkEnumerateDeviceExtensionProperties(d->phys, NULL, &n, e) >= RLVK_SUCCESS)
        for (uint32_t i = 0; i < n && !found; i++)
            found = !strcmp(e[i].extensionName, name);
    free(e);
    return found;
}

/** @brief Whether the driver has VK_QCOM_queue_perf_hint with its queuePerfHint feature. */
static bool queue_hint_offered(const rlvk_dev *d)
{
    if (!has_ext(d, "VK_QCOM_queue_perf_hint"))
        return false;
    typedef void(RLVK_CALL * features2_fn)(rlvk_handle, features2 *);
    features2_fn get = (features2_fn)d->gipa(d->instance, "vkGetPhysicalDeviceFeatures2");
    if (!get)
        get = (features2_fn)d->gipa(d->instance, "vkGetPhysicalDeviceFeatures2KHR");
    if (!get)
        return false;
    hint_features f = { ST_QUEUE_PERF_HINT_FEATURES, NULL, 0 };
    features2 all = { .sType = ST_FEATURES_2, .next = &f };
    get(d->phys, &all);
    return f.queuePerfHint != 0;
}

/** @brief The ADPF entry points of libandroid (API 33), looked up so a build for older Android still loads. */
static struct {
    void *(*get_manager)(void);
    void *(*create)(void *manager, const int32_t *tids, size_t n, int64_t target_ns);
    int (*report)(void *session, int64_t actual_ns);
    void *session;
    bool tried;
} adpf;

static bool adpf_offered(void)
{
    if (!adpf.tried) {
        adpf.tried = true;
        static const char *const names[] = { "libandroid.so", NULL };
        void *lib = rl_hostlib_open(names);
        if (lib) {
            adpf.get_manager = (void *(*)(void))rl_hostlib_sym(lib, "APerformanceHint_getManager");
            adpf.create = (void *(*)(void *, const int32_t *, size_t, int64_t))rl_hostlib_sym(lib, "APerformanceHint_createSession");
            adpf.report = (int (*)(void *, int64_t))rl_hostlib_sym(lib, "APerformanceHint_reportActualWorkDuration");
        }
    }
    return adpf.get_manager && adpf.create && adpf.report && adpf.get_manager();
}

/** @brief Reports a frame's span; the session is made on the first frame, for the thread that hands frames over. */
static void adpf_report(int64_t span)
{
    if (!adpf.session) {
        int32_t tid = (int32_t)gettid();
        adpf.session = adpf.create(adpf.get_manager(), &tid, 1, RL_GPU_FLOOR_PERIOD_NS);
        if (!adpf.session) {
            rl_log(RL_LOG_WARN, "gpu", "GPU clock floor: the platform made no ADPF session; the floor is off");
            __atomic_store_n(&g_kind, RL_GPU_FLOOR_NONE, __ATOMIC_RELAXED);
            return;
        }
    }
    adpf.report(adpf.session, span);
}
#endif

void rl_gpu_floor_build(const rlvk_dev *d, rl_vkdev_build *b)
{
#if defined(__ANDROID__)
    g_hint_asked = false;
    if (rl_setting_bool(RL_GPU_FLOOR_KEY) && queue_hint_offered(d) && rl_vkdev_add_ext(b, "VK_QCOM_queue_perf_hint")) {
        g_hint_feat.next = NULL;
        rl_vkdev_chain(b, &g_hint_feat);
        g_hint_asked = true;
    }
#else
    (void)d;
    (void)b;
#endif
}

void rl_gpu_floor_start(const rlvk_dev *d)
{
#if defined(__ANDROID__)
    set_hint_fn set = g_hint_asked ? (set_hint_fn)d->vkGetDeviceProcAddr(d->device, "vkQueueSetPerfHintQCOM") : NULL;
    bool on = rl_setting_bool(RL_GPU_FLOOR_KEY);
    rl_gpu_floor_kind k = rl_gpu_floor_pick(on, true, set != NULL, on && !set && adpf_offered());
    if (k == RL_GPU_FLOOR_QUEUE_HINT) {
        hint_info h = { ST_PERF_HINT_INFO, NULL, PERF_HINT_FREQUENCY_MAX, 0 };
        rlvk_result r = set(d->queue, &h);
        if (r == RLVK_SUCCESS && d->guest_queue != d->queue)
            r = set(d->guest_queue, &h);
        if (r != RLVK_SUCCESS) {
            rl_log(RL_LOG_WARN, "gpu", "GPU clock floor: vkQueueSetPerfHintQCOM failed (VkResult %d)", (int)r);
            k = adpf_offered() ? RL_GPU_FLOOR_ADPF : RL_GPU_FLOOR_NONE;
        }
    }
    __atomic_store_n(&g_kind, k, __ATOMIC_RELAXED);
    if (k == RL_GPU_FLOOR_OFF)
        rl_log(RL_LOG_INFO, "gpu", "GPU clock floor: off (gpu.clock_floor)");
    else
        rl_log(RL_LOG_INFO, "gpu", "GPU clock floor: %s%s", rl_gpu_floor_name(k),
               k == RL_GPU_FLOOR_ADPF ? ", each frame's work span against 16.7 ms" : "");
#else
    (void)d;
    __atomic_store_n(&g_kind, rl_gpu_floor_pick(rl_setting_bool(RL_GPU_FLOOR_KEY), false, false, false),
                     __ATOMIC_RELAXED);
    rl_log(RL_LOG_DEBUG, "gpu", "GPU clock floor: none on this host (Android only)");
#endif
}

rl_gpu_floor_kind rl_gpu_floor_active(void) { return __atomic_load_n(&g_kind, __ATOMIC_RELAXED); }

void rl_gpu_floor_work(void)
{
    if (rl_gpu_floor_active() != RL_GPU_FLOOR_ADPF)
        return;
    uint64_t zero = 0;
    __atomic_compare_exchange_n(&g_start_ns, &zero, mono_ns(), false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

void rl_gpu_floor_frame(void)
{
    if (rl_gpu_floor_active() != RL_GPU_FLOOR_ADPF)
        return;
    int64_t span = rl_gpu_floor_span(__atomic_exchange_n(&g_start_ns, 0, __ATOMIC_RELAXED), mono_ns());
#if defined(__ANDROID__)
    if (span > 0)
        adpf_report(span);
#else
    (void)span;
#endif
}
