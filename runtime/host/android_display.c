/**
 * @file android_display.c
 * @brief Windows on the displays besides the app's own, for guests with more than one screen (a dual-screen
 * handheld, a foldable, a phone with a connected display). SDL3 on Android supports one window, so the app's
 * Java side (MainActivity, Displays) puts a Presentation with a SurfaceView on the display and hands its surface
 * and touches to these functions; frames are scaled in software into the surface's buffer.
 */
#ifdef __ANDROID__

#include "core.h"
#include "hostlib.h"

#include <relift/window.h>

#include <SDL3/SDL.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <math.h>
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
    uint32_t rx, ry, rw, rh;
    bool fit;
    finger fingers[MAX_FINGERS];
    uint32_t *xmap;
    size_t xmap_cap;
} xdisplay;

static const rl_window_backend g_extra;
static xdisplay g_disp[MAX_DISPLAYS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;

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
    if (!call_int("reliftDisplayOpen", "(I)I", (int)index, NULL)) {
        d->used = false;
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
    pthread_mutex_lock(&g_lock);
    if (d->nw)
        ANativeWindow_release(d->nw);
    d->nw = NULL;
    memset(d->fingers, 0, sizeof d->fingers);
    free(d->xmap);
    d->xmap = NULL;
    d->xmap_cap = 0;
    d->used = false;
    pthread_mutex_unlock(&g_lock);
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

static void x_present(rl_window *h, const uint8_t *rgb, uint32_t width, uint32_t height, uint32_t shown_width,
                      uint32_t shown_height)
{
    xdisplay *d = (xdisplay *)h;
    (void)shown_width;
    (void)shown_height;
    pthread_mutex_lock(&g_lock);
    ANativeWindow *nw = d->nw;
    if (!nw || !width || !height) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    ANativeWindow_acquire(nw);
    bool fit = d->fit;
    pthread_mutex_unlock(&g_lock);

    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(nw, &buf, NULL) == 0) {
        uint32_t bw = (uint32_t)buf.width, bh = (uint32_t)buf.height;
        float s = fminf((float)bw / (float)width, (float)bh / (float)height);
        if (!fit && s >= 1.0f)
            s = floorf(s);
        uint32_t rw = (uint32_t)((float)width * s), rh = (uint32_t)((float)height * s);
        if (rw > bw)
            rw = bw;
        if (rh > bh)
            rh = bh;
        uint32_t rx = (bw - rw) / 2, ry = (bh - rh) / 2;
        if (buf.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM || buf.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM) {
            if (d->xmap_cap < rw) {
                free(d->xmap);
                d->xmap = malloc(rw * sizeof *d->xmap);
                d->xmap_cap = d->xmap ? rw : 0;
            }
            uint32_t *xm = d->xmap;
            if (xm) {
                for (uint32_t x = 0; x < rw; x++)
                    xm[x] = (uint32_t)((uint64_t)x * width / rw);
                uint32_t *base = buf.bits;
                for (uint32_t y = 0; y < bh; y++) {
                    uint32_t *row = base + (size_t)y * (uint32_t)buf.stride;
                    if (y < ry || y >= ry + rh) {
                        memset(row, 0, (size_t)bw * 4u);
                        continue;
                    }
                    memset(row, 0, (size_t)rx * 4u);
                    memset(row + rx + rw, 0, (size_t)(bw - rx - rw) * 4u);
                    const uint8_t *src = rgb + (size_t)((uint64_t)(y - ry) * height / rh) * width * 3u;
                    uint32_t *dst = row + rx;
                    for (uint32_t x = 0; x < rw; x++) {
                        const uint8_t *p = src + (size_t)xm[x] * 3u;
                        dst[x] = 0xFF000000u | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
                    }
                }
            }
        }
        ANativeWindow_unlockAndPost(nw);
        pthread_mutex_lock(&g_lock);
        d->frame_w = width;
        d->frame_h = height;
        d->rx = rx;
        d->ry = ry;
        d->rw = rw;
        d->rh = rh;
        pthread_mutex_unlock(&g_lock);
    }
    ANativeWindow_release(nw);
}

static uint32_t x_touches(rl_window *h, rl_window_touch *out, uint32_t max)
{
    xdisplay *d = (xdisplay *)h;
    uint32_t n = 0;
    pthread_mutex_lock(&g_lock);
    if (d->rw && d->rh) {
        for (int i = 0; i < MAX_FINGERS && n < max; i++) {
            if (!d->fingers[i].on)
                continue;
            float fx = (d->fingers[i].x - (float)d->rx) / (float)d->rw, fy = (d->fingers[i].y - (float)d->ry) / (float)d->rh;
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
    .present_rgb = x_present,
    .set_fit = x_set_fit,
    .touches = x_touches,
};

JNIEXPORT void JNICALL Java_dev_relift_app_MainActivity_nativeDisplaySurface(JNIEnv *env, jclass cls, jint index,
                                                                             jobject surface, jint w, jint h);
JNIEXPORT void JNICALL Java_dev_relift_app_MainActivity_nativeDisplayTouch(JNIEnv *env, jclass cls, jint index,
                                                                           jint id, jint action, jfloat x, jfloat y);

JNIEXPORT void JNICALL Java_dev_relift_app_MainActivity_nativeDisplaySurface(JNIEnv *env, jclass cls, jint index,
                                                                             jobject surface, jint w, jint h)
{
    (void)cls;
    if (index < 0 || index >= MAX_DISPLAYS)
        return;
    xdisplay *d = &g_disp[index];
    ANativeWindow *nw = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    if (nw)
        ANativeWindow_setBuffersGeometry(nw, 0, 0, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    pthread_mutex_lock(&g_lock);
    if (d->nw && d->nw != nw)
        ANativeWindow_release(d->nw);
    d->nw = nw;
    d->width = (uint32_t)(w > 0 ? w : 0);
    d->height = (uint32_t)(h > 0 ? h : 0);
    d->head.generation++;
    memset(d->fingers, 0, sizeof d->fingers);
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
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
