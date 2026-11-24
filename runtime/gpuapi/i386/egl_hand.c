/**
 * @file egl_hand.c
 * @brief Hand-written EGL thunks. The guest's X11 is lifted, so a guest
 * Display is a connection the host cannot use: the host opens its own
 * connection to the same server (XIDs are server-wide, so windows and
 * pixmaps pass as they are) and the guest's requests are flushed first.
 * eglGetProcAddress gives guest addresses of thunks.
 */
#include "tg.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/** @brief The EGL soname. */
#define TG_EGL "libEGL.so.1"

/** @brief EGL constants. */
enum {
    TG_EGL_NONE = 0x3038,
    TG_EGL_PLATFORM_X11 = 0x31D5,
    TG_EGL_PLATFORM_SURFACELESS = 0x31DD,
    TG_EGL_PLATFORM_DEVICE = 0x313F,
};

static void *tg_egl_fn(const char *name)
{
    void *f = tg_dlsym(TG_EGL, name, NULL);
    if (!f)
        tg_fail(name, "missing in the host library " TG_EGL);
    return f;
}

/** @brief Guest displays and the host connections opened for them. */
static struct {
    uint32_t guest;
    void *host;
} tg_egl_x[8];
static pthread_mutex_t tg_egl_lock = PTHREAD_MUTEX_INITIALIZER;

/** @brief A host X11 connection to the server of guest Display dpy (flushing the guest's requests first). */
static void *tg_egl_x11(uint32_t dpy, const char *fn)
{
    pthread_mutex_lock(&tg_egl_lock);
    for (int i = 0; i < 8; i++) {
        if (tg_egl_x[i].guest == dpy) {
            void *h = tg_egl_x[i].host;
            pthread_mutex_unlock(&tg_egl_lock);
            return h;
        }
    }
    pthread_mutex_unlock(&tg_egl_lock);
    uint32_t a[2] = {dpy, 0};
    uint32_t name = (uint32_t)tg_guest_libc_in("libX11.so.6", "XDisplayString", a, 1);
    tg_guest_libc_in("libX11.so.6", "XSync", (uint32_t[]){dpy, 0}, 2);
    void *(*open)(const char *) = (void *(*)(const char *))tg_dlsym("libX11.so.6", "XOpenDisplay", NULL);
    if (!open)
        tg_fail(fn, "the host has no libX11 to reach the guest's X server");
    tg_call *k = tg_current(fn);
    void *h = open(name ? (const char *)k->mem + name : NULL);
    if (!h)
        tg_fail(fn, "the host cannot open the guest's X display");
    pthread_mutex_lock(&tg_egl_lock);
    for (int i = 0; i < 8; i++) {
        if (!tg_egl_x[i].guest) {
            tg_egl_x[i].guest = dpy;
            tg_egl_x[i].host = h;
            break;
        }
    }
    pthread_mutex_unlock(&tg_egl_lock);
    return h;
}

uint64_t tgh_eglGetDisplay(rl_ctx *c, uint8_t *mem)
{
    tg_call kk, *k = &kk;
    tg_begin(k, c, mem);
    uint32_t native = tg_arg32(k, 0);
    void *h = native ? tg_egl_x11(native, "eglGetDisplay") : NULL;
    void *d = ((void *(*)(void *))tg_egl_fn("eglGetDisplay"))(h);
    tg_i386_set_eax(c, tg_handle(d));
    return tg_end(k, 0);
}

uint64_t tgh_eglGetPlatformDisplay(rl_ctx *c, uint8_t *mem)
{
    tg_call kk, *k = &kk;
    tg_begin(k, c, mem);
    uint32_t platform = tg_arg32(k, 0), native = tg_arg32(k, 4), attrs = tg_arg32(k, 8);
    void *h = NULL;
    if (native) {
        if (platform == TG_EGL_PLATFORM_X11)
            h = tg_egl_x11(native, "eglGetPlatformDisplay");
        else if (platform == TG_EGL_PLATFORM_DEVICE)
            h = tg_unhandle(native, "eglGetPlatformDisplay");
        else if (platform != TG_EGL_PLATFORM_SURFACELESS)
            tg_fail("eglGetPlatformDisplay", "a native display of a platform the thunks do not convert");
    }
    intptr_t *list = NULL;
    if (attrs) {
        uint32_t n = 0;
        while ((int32_t)tg_ld(mem + attrs + 4 * n, 4) != TG_EGL_NONE)
            n += 2;
        list = tg_alloc(k, 8 * ((size_t)n + 1));
        for (uint32_t i = 0; i <= n; i++)
            list[i] = (int32_t)tg_ld(mem + attrs + 4 * i, 4);
    }
    void *d = ((void *(*)(unsigned, void *, const intptr_t *))tg_egl_fn("eglGetPlatformDisplay"))(platform, h, list);
    tg_i386_set_eax(c, tg_handle(d));
    return tg_end(k, 0);
}

uint64_t tgh_eglGetProcAddress(rl_ctx *c, uint8_t *mem)
{
    tg_call kk, *k = &kk;
    tg_begin(k, c, mem);
    uint32_t name = tg_arg32(k, 0);
    void *h = ((void *(*)(const char *))tg_egl_fn("eglGetProcAddress"))(name ? (const char *)mem + name : NULL);
    uint32_t g = 0;
    if (h) {
        g = tg_guest_symbol(TG_EGL, (const char *)mem + name);
        if (!g)
            g = tg_guest_symbol("libGL.so.1", (const char *)mem + name);
        if (!g)
            fprintf(stderr, "relift: eglGetProcAddress: %s has no thunk; the guest gets NULL\n", (const char *)mem + name);
    }
    tg_i386_set_eax(c, g);
    return tg_end(k, 0);
}
