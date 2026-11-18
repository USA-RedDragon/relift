/**
 * @file android_display.c
 * @brief Windows on the displays besides the app's own, for guests with more than one screen (a dual-screen
 * handheld, a foldable, a phone with a connected display). An Android window lives on one display, and SDL3 on
 * Android has one window, so the app's Java side (MainActivity, Displays) puts a Presentation with a SurfaceView on
 * each other display and hands its surface and touches to these functions. Frames go to the GPU: each window has a
 * Vulkan swapchain of its own on the core's device, drawn by a presenter thread (core/present.h), scaled and
 * filtered the way the app's main window is.
 *
 * The surface's life comes from the Java UI thread: a new or resized surface tells the presenter to make its
 * swapchain again, and a surface going away returns only after the presenter has let go of it, as Android requires
 * before surfaceDestroyed returns.
 */
#ifdef __ANDROID__

#include "core.h"
#include "hostlib.h"
#include "present.h"

#include <relift/window.h>

#include <SDL3/SDL.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_DISPLAYS 2
#define MAX_FINGERS 4
#define OPEN_WAIT_MS 4000
#define COUNT_CACHE_NS 500000000ull

typedef struct finger {
    bool on;
    int32_t id;
    float x, y;
} finger;

typedef struct xdisplay {
    rl_window head;
    bool used;
    ANativeWindow *nw;
    uint32_t width, height;
    uint32_t frame_w, frame_h;
    rl_present_rect place;
    bool fit;
    finger fingers[MAX_FINGERS];
    rl_present *pres;
} xdisplay;

static const rl_window_backend g_extra;
static xdisplay g_disp[MAX_DISPLAYS];
/** @brief Guards the fields of g_disp. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
/** @brief Held while a presenter is made, closed or told about its surface, so none of them races another. */
static pthread_mutex_t g_pres_lock = PTHREAD_MUTEX_INITIALIZER;

int32_t rl_android_vulkan_surface(void *native_window, void *instance, uint64_t *surface)
{
    *surface = 0;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(uintptr_t)rl_hostlib_vulkan_gipa();
    PFN_vkCreateAndroidSurfaceKHR create =
        gipa ? (PFN_vkCreateAndroidSurfaceKHR)gipa((VkInstance)instance, "vkCreateAndroidSurfaceKHR") : NULL;
    if (!create || !native_window)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkAndroidSurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = native_window,
    };
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create((VkInstance)instance, &info, NULL, &s);
    memcpy(surface, &s, sizeof *surface);
    return r;
}

typedef struct call_req {
    const char *method, *sig;
    int arg, result;
    bool ok;
} call_req;

static void do_call(void *p)
{
    call_req *r = p;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jobject act = SDL_GetAndroidActivity();
    if (!env || !act)
        return;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = c ? (*env)->GetMethodID(env, c, r->method, r->sig) : NULL;
    if (m) {
        r->result = (*env)->CallIntMethod(env, act, m, (jint)r->arg);
        r->ok = true;
    }
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (c)
        (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
}

/**
 * The guest's threads run on fibers whose stacks the Java VM refuses to attach, so every call into the activity
 * runs on SDL's main (event loop) thread.
 */
static bool call_int(const char *method, const char *sig, int arg, int *result)
{
    call_req r = {.method = method, .sig = sig, .arg = arg};
    SDL_RunOnMainThread(do_call, &r, true);
    if (r.ok && result)
        *result = r.result;
    return r.ok;
}

uint32_t rl_android_display_count(void)
{
    static uint64_t checked_ns;
    static uint32_t count;
    uint64_t now = rl_clock_ns();
    if (checked_ns && now - checked_ns < COUNT_CACHE_NS)
        return count;
    int n = 0;
    count = !call_int("reliftDisplayCount", "(I)I", 0, &n) || n < 0 ? 0 : (uint32_t)(n > MAX_DISPLAYS ? MAX_DISPLAYS : n);
    checked_ns = now;
    return count;
}

static void deadline(struct timespec *t, int ms)
{
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += ms / 1000;
    t->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}

static void x_close(rl_window *h);

rl_window *rl_android_display_open(uint32_t index)
{
    if (index >= MAX_DISPLAYS)
        return NULL;
    xdisplay *d = &g_disp[index];
    pthread_mutex_lock(&g_lock);
    if (d->used) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    d->used = true;
    d->head.b = &g_extra;
    d->head.extra = true;
    d->fit = true;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_lock(&g_pres_lock);
    rl_present *pres = rl_present_open(&d->head);
    pthread_mutex_lock(&g_lock);
    d->pres = pres;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_pres_lock);
    if (!pres || !call_int("reliftDisplayOpen", "(I)I", (int)index, NULL)) {
        x_close(&d->head);
        return NULL;
    }
    struct timespec t;
    deadline(&t, OPEN_WAIT_MS);
    pthread_mutex_lock(&g_lock);
    while (!d->nw && pthread_cond_timedwait(&g_cond, &g_lock, &t) == 0)
        ;
    bool ok = d->nw != NULL;
    pthread_mutex_unlock(&g_lock);
    if (!ok) {
        rl_log(RL_LOG_WARN, "window", "display %u: the presentation has no surface", index);
        x_close(&d->head);
        return NULL;
    }
    rl_log(RL_LOG_INFO, "window", "display %u: %ux%u", index, d->width, d->height);
    return &d->head;
}

static void x_close(rl_window *h)
{
    xdisplay *d = (xdisplay *)h;
    int index = (int)(d - g_disp);
    call_int("reliftDisplayClose", "(I)I", index, NULL);
    pthread_mutex_lock(&g_pres_lock);
    pthread_mutex_lock(&g_lock);
    rl_present *pres = d->pres;
    d->pres = NULL;
    pthread_mutex_unlock(&g_lock);
    rl_present_close(pres);
    pthread_mutex_lock(&g_lock);
    if (d->nw)
        ANativeWindow_release(d->nw);
    d->nw = NULL;
    memset(d->fingers, 0, sizeof d->fingers);
    memset(&d->place, 0, sizeof d->place);
    d->used = false;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_pres_lock);
}

static void x_size(rl_window *h, uint32_t *width, uint32_t *height)
{
    xdisplay *d = (xdisplay *)h;
    pthread_mutex_lock(&g_lock);
    *width = d->width;
    *height = d->height;
    pthread_mutex_unlock(&g_lock);
}

static void x_set_fit(rl_window *h, bool fit)
{
    xdisplay *d = (xdisplay *)h;
    pthread_mutex_lock(&g_lock);
    d->fit = fit;
    pthread_mutex_unlock(&g_lock);
}

static int32_t x_vulkan_surface(rl_window *h, void *instance, uint64_t *surface)
{
    xdisplay *d = (xdisplay *)h;
    pthread_mutex_lock(&g_lock);
    int32_t r = d->nw ? rl_android_vulkan_surface(d->nw, instance, surface) : VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_unlock(&g_lock);
    return r;
}

static void x_present(rl_window *h, const uint8_t *rgb, uint32_t width, uint32_t height, uint32_t shown_width,
                      uint32_t shown_height)
{
    xdisplay *d = (xdisplay *)h;
    pthread_mutex_lock(&g_lock);
    rl_present *pres = d->pres;
    bool fit = d->fit;
    uint32_t ww = d->width, wh = d->height;
    pthread_mutex_unlock(&g_lock);
    rl_present_rect place;
    rl_present_frame(pres, rgb, width, height, shown_width, shown_height, fit, ww, wh, &place);
    pthread_mutex_lock(&g_lock);
    d->frame_w = width;
    d->frame_h = height;
    d->place = place;
    pthread_mutex_unlock(&g_lock);
}

static uint32_t x_touches(rl_window *h, rl_window_touch *out, uint32_t max)
{
    xdisplay *d = (xdisplay *)h;
    uint32_t n = 0;
    pthread_mutex_lock(&g_lock);
    rl_present_rect r = d->place;
    if (r.w && r.h) {
        for (int i = 0; i < MAX_FINGERS && n < max; i++) {
            if (!d->fingers[i].on)
                continue;
            float fx = (d->fingers[i].x - (float)r.x) / (float)r.w, fy = (d->fingers[i].y - (float)r.y) / (float)r.h;
            out[n].id = (uint64_t)(uint32_t)d->fingers[i].id;
            out[n].x = fx <= 0.0f ? 0 : fx >= 1.0f ? d->frame_w - 1 : (uint32_t)(fx * (float)d->frame_w);
            out[n].y = fy <= 0.0f ? 0 : fy >= 1.0f ? d->frame_h - 1 : (uint32_t)(fy * (float)d->frame_h);
            n++;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

static const rl_window_backend g_extra = {
    .name = "Android display",
    .close = x_close,
    .size = x_size,
    .vulkan_surface = x_vulkan_surface,
    .present_rgb = x_present,
    .set_fit = x_set_fit,
    .touches = x_touches,
};

JNIEXPORT void JNICALL Java_dev_relift_app_MainActivity_nativeDisplaySurface(JNIEnv *env, jclass cls, jint index,
                                                                             jobject surface, jint w, jint h);
JNIEXPORT void JNICALL Java_dev_relift_app_MainActivity_nativeDisplayTouch(JNIEnv *env, jclass cls, jint index,
                                                                           jint id, jint action, jfloat x, jfloat y);

/**
 * The UI thread's surfaceChanged (surface set) and surfaceDestroyed (surface NULL). A different or no surface first
 * makes the presenter let go of the old one; the old ANativeWindow is released only after that.
 */
JNIEXPORT void JNICALL Java_dev_relift_app_MainActivity_nativeDisplaySurface(JNIEnv *env, jclass cls, jint index,
                                                                             jobject surface, jint w, jint h)
{
    (void)cls;
    if (index < 0 || index >= MAX_DISPLAYS)
        return;
    xdisplay *d = &g_disp[index];
    ANativeWindow *nw = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&g_pres_lock);
    pthread_mutex_lock(&g_lock);
    rl_present *pres = d->pres;
    ANativeWindow *old = d->nw;
    pthread_mutex_unlock(&g_lock);
    if (old && old != nw)
        rl_present_surface(pres, false);
    pthread_mutex_lock(&g_lock);
    d->nw = nw;
    d->width = (uint32_t)(w > 0 ? w : 0);
    d->height = (uint32_t)(h > 0 ? h : 0);
    __atomic_add_fetch(&d->head.generation, 1, __ATOMIC_RELEASE);
    memset(d->fingers, 0, sizeof d->fingers);
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    if (old && old != nw)
        ANativeWindow_release(old);
    else if (old)
        ANativeWindow_release(nw);
    if (nw)
        rl_present_surface(pres, true);
    pthread_mutex_unlock(&g_pres_lock);
    rl_log(RL_LOG_INFO, "window", "display %d: surface %s %dx%d", (int)index, nw ? "set" : "gone", (int)w, (int)h);
}

JNIEXPORT void JNICALL Java_dev_relift_app_MainActivity_nativeDisplayTouch(JNIEnv *env, jclass cls, jint index,
                                                                           jint id, jint action, jfloat x, jfloat y)
{
    (void)env;
    (void)cls;
    if (index < 0 || index >= MAX_DISPLAYS)
        return;
    xdisplay *d = &g_disp[index];
    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < MAX_FINGERS; i++)
        if (d->fingers[i].on && d->fingers[i].id == id)
            slot = i;
    if (action == 0 || action == 2) {
        if (slot < 0)
            for (int i = 0; i < MAX_FINGERS && slot < 0; i++)
                if (!d->fingers[i].on)
                    slot = i;
        if (slot >= 0)
            d->fingers[slot] = (finger){true, id, x, y};
    } else if (slot >= 0) {
        d->fingers[slot].on = false;
    }
    pthread_mutex_unlock(&g_lock);
}

#endif
