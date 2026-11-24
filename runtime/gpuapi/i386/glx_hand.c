/**
 * @file glx_hand.c
 * @brief GLX, by hand. The guest's libX11 is lifted (it only talks to the X
 * server over its socket), so a guest Display is a connection the host GL
 * driver cannot use. Each guest display gets a host connection to the same
 * server; X resources (windows, pixmaps, GLX drawables, visual IDs) are
 * server-wide IDs and pass as they are. The guest's requests are flushed
 * (XSync on its connection) before the host uses its resources, and the
 * host's are flushed before the guest uses what GLX made. Visuals reach the
 * guest as its own XVisualInfo, from its own XGetVisualInfo; arrays the
 * guest frees with XFree come from its own malloc.
 */
#include "tg.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/** @brief Sonames. */
#define TG_GL "libGL.so.1"
#define TG_X11 "libX11.so.6"

/** @brief Xlib and GLX constants. */
enum {
    TG_VISUAL_ID_MASK = 0x1,
    TG_VISUAL_SCREEN_MASK = 0x2,
    TG_GLX_VISUAL_ID = 0x800B,
    TG_GLX_SCREEN = 0x800C,
    TG_GLX_VI_GUEST_SIZE = 40,
};

static void *tg_glx_fn(const char *name)
{
    void *f = tg_dlsym(TG_GL, name, "glXGetProcAddressARB");
    if (!f)
        tg_fail(name, "missing in the host library " TG_GL);
    return f;
}

static void *tg_x_fn(const char *name)
{
    void *f = tg_dlsym(TG_X11, name, NULL);
    if (!f)
        tg_fail(name, "missing in the host library " TG_X11);
    return f;
}

/** @brief Guest displays and their host connections. */
static struct {
    uint32_t guest;
    void *host;
} tg_glx_dpy[8];
static pthread_mutex_t tg_glx_lock = PTHREAD_MUTEX_INITIALIZER;

/** @brief The guest's XSync(dpy, False). */
static void tg_glx_guest_sync(uint32_t dpy)
{
    uint32_t a[2] = {dpy, 0};
    tg_guest_libc_in(TG_X11, "XSync", a, 2);
}

/** @brief The host's XSync on its connection. */
static void tg_glx_host_sync(void *h)
{
    ((int (*)(void *, int))tg_x_fn("XSync"))(h, 0);
}

/** @brief The host connection for guest display dpy, opened on first use; the guest's requests are flushed. */
static void *tg_glx_host(tg_call *k, uint32_t dpy)
{
    if (!dpy)
        return NULL;
    tg_glx_guest_sync(dpy);
    pthread_mutex_lock(&tg_glx_lock);
    for (int i = 0; i < 8; i++) {
        if (tg_glx_dpy[i].guest == dpy) {
            void *h = tg_glx_dpy[i].host;
            pthread_mutex_unlock(&tg_glx_lock);
            return h;
        }
    }
    pthread_mutex_unlock(&tg_glx_lock);
    uint32_t a[1] = {dpy};
    uint32_t name = (uint32_t)tg_guest_libc_in(TG_X11, "XDisplayString", a, 1);
    void *h = ((void *(*)(const char *))tg_x_fn("XOpenDisplay"))(name ? (const char *)k->mem + name : NULL);
    if (!h)
        tg_fail("GLX", "the host cannot open the guest's X display");
    pthread_mutex_lock(&tg_glx_lock);
    int i = 0;
    while (i < 8 && tg_glx_dpy[i].guest)
        i++;
    if (i == 8)
        tg_fail("GLX", "more than 8 displays");
    tg_glx_dpy[i].guest = dpy;
    tg_glx_dpy[i].host = h;
    pthread_mutex_unlock(&tg_glx_lock);
    return h;
}

/** @brief The guest display of a host connection, 0 for none. */
static uint32_t tg_glx_guest_dpy(const void *h)
{
    uint32_t g = 0;
    pthread_mutex_lock(&tg_glx_lock);
    for (int i = 0; h && i < 8; i++)
        if (tg_glx_dpy[i].host == h)
            g = tg_glx_dpy[i].guest;
    pthread_mutex_unlock(&tg_glx_lock);
    return g;
}

/** @brief The members of a host XVisualInfo the thunks read (its x86-64 layout). */
typedef struct tg_host_vi {
    void *visual;
    unsigned long visualid;
    int screen;
} tg_host_vi;

/** @brief The guest's XVisualInfo for a visual ID on a screen, from its own XGetVisualInfo (0 for none). */
static uint32_t tg_glx_guest_vi(tg_call *k, uint32_t dpy, unsigned long visualid, int screen)
{
    uint32_t at = tg_guest_alloc(TG_GLX_VI_GUEST_SIZE + 4);
    if (!at)
        tg_fail("GLX", "out of guest memory");
    uint8_t *tmpl = k->mem + at;
    uint32_t vid = (uint32_t)visualid;
    memcpy(tmpl + 4, &vid, 4);
    memcpy(tmpl + 8, &screen, 4);
    uint32_t g = (uint32_t)(tmpl - k->mem);
    uint32_t a[4] = {dpy, TG_VISUAL_ID_MASK | TG_VISUAL_SCREEN_MASK, g, g + TG_GLX_VI_GUEST_SIZE};
    uint32_t vi = (uint32_t)tg_guest_libc_in(TG_X11, "XGetVisualInfo", a, 4);
    tg_guest_free(at, TG_GLX_VI_GUEST_SIZE + 4);
    return vi;
}

/** @brief The host XVisualInfo for the guest's (XFree it on the host). */
static tg_host_vi *tg_glx_host_vi(tg_call *k, void *h, uint32_t vi)
{
    if (!vi)
        return NULL;
    uint32_t vid, screen;
    memcpy(&vid, k->mem + vi + 4, 4);
    memcpy(&screen, k->mem + vi + 8, 4);
    uint8_t tmpl[64] = {0};
    unsigned long id = vid;
    memcpy(tmpl + 8, &id, 8);
    memcpy(tmpl + 16, &screen, 4);
    int n = 0;
    return ((tg_host_vi * (*)(void *, long, void *, int *)) tg_x_fn("XGetVisualInfo"))(
        h, TG_VISUAL_ID_MASK | TG_VISUAL_SCREEN_MASK, tmpl, &n);
}

/** @brief Converts a GLX attribute list (ints, ending with None) for the host: the same ints. */
static int *tg_glx_attribs(tg_call *k, uint32_t g)
{
    return g ? (int *)(k->mem + g) : NULL;
}

#define TG_GLX_BEGIN                                                                               \
    tg_call kk, *k = &kk;                                                                          \
    tg_begin(k, c, mem)
#define TG_GLX_RET(v)                                                                              \
    do {                                                                                           \
        tg_i386_set_eax(c, (uint32_t)(v));                                                         \
        return tg_end(k, 0);                                                                       \
    } while (0)
#define A(i) tg_arg32(k, 4 * (i))
#define H(i) tg_glx_host(k, A(i))

/** @brief Every hand GLX thunk. */
#define TG_GLX_THUNK(name) uint64_t tgh_##name(rl_ctx *c, uint8_t *mem)
TG_GLX_THUNK(glXChooseVisual);
TG_GLX_THUNK(glXCreateContext);
TG_GLX_THUNK(glXDestroyContext);
TG_GLX_THUNK(glXMakeCurrent);
TG_GLX_THUNK(glXCopyContext);
TG_GLX_THUNK(glXSwapBuffers);
TG_GLX_THUNK(glXCreateGLXPixmap);
TG_GLX_THUNK(glXDestroyGLXPixmap);
TG_GLX_THUNK(glXQueryExtension);
TG_GLX_THUNK(glXQueryVersion);
TG_GLX_THUNK(glXIsDirect);
TG_GLX_THUNK(glXGetConfig);
TG_GLX_THUNK(glXGetCurrentContext);
TG_GLX_THUNK(glXGetCurrentDrawable);
TG_GLX_THUNK(glXGetCurrentReadDrawable);
TG_GLX_THUNK(glXGetCurrentDisplay);
TG_GLX_THUNK(glXWaitGL);
TG_GLX_THUNK(glXWaitX);
TG_GLX_THUNK(glXUseXFont);
TG_GLX_THUNK(glXQueryExtensionsString);
TG_GLX_THUNK(glXQueryServerString);
TG_GLX_THUNK(glXGetClientString);
TG_GLX_THUNK(glXChooseFBConfig);
TG_GLX_THUNK(glXGetFBConfigs);
TG_GLX_THUNK(glXGetFBConfigAttrib);
TG_GLX_THUNK(glXGetVisualFromFBConfig);
TG_GLX_THUNK(glXCreateWindow);
TG_GLX_THUNK(glXDestroyWindow);
TG_GLX_THUNK(glXCreatePixmap);
TG_GLX_THUNK(glXDestroyPixmap);
TG_GLX_THUNK(glXCreatePbuffer);
TG_GLX_THUNK(glXDestroyPbuffer);
TG_GLX_THUNK(glXQueryDrawable);
TG_GLX_THUNK(glXCreateNewContext);
TG_GLX_THUNK(glXMakeContextCurrent);
TG_GLX_THUNK(glXQueryContext);
TG_GLX_THUNK(glXSelectEvent);
TG_GLX_THUNK(glXGetSelectedEvent);
TG_GLX_THUNK(glXGetProcAddress);
TG_GLX_THUNK(glXGetProcAddressARB);
TG_GLX_THUNK(glXCreateContextAttribsARB);

TG_GLX_THUNK(glXChooseVisual)
{
    TG_GLX_BEGIN;
    void *h = H(0);
    tg_host_vi *vi = ((tg_host_vi * (*)(void *, int, int *)) tg_glx_fn("glXChooseVisual"))(h, (int)A(1), tg_glx_attribs(k, A(2)));
    uint32_t g = 0;
    if (vi) {
        g = tg_glx_guest_vi(k, A(0), vi->visualid, vi->screen);
        ((int (*)(void *))tg_x_fn("XFree"))(vi);
    }
    TG_GLX_RET(g);
}

/** @brief A context call taking (dpy, vis, share, direct). */
TG_GLX_THUNK(glXCreateContext)
{
    TG_GLX_BEGIN;
    void *h = H(0);
    tg_host_vi *vi = tg_glx_host_vi(k, h, A(1));
    void *ctx = ((void *(*)(void *, void *, void *, int))tg_glx_fn("glXCreateContext"))(
        h, vi, tg_unhandle(A(2), "glXCreateContext"), (int)A(3));
    if (vi)
        ((int (*)(void *))tg_x_fn("XFree"))(vi);
    TG_GLX_RET(tg_handle(ctx));
}

TG_GLX_THUNK(glXDestroyContext)
{
    TG_GLX_BEGIN;
    ((void (*)(void *, void *))tg_glx_fn("glXDestroyContext"))(H(0), tg_unhandle(A(1), "glXDestroyContext"));
    return tg_end(k, 0);
}

TG_GLX_THUNK(glXMakeCurrent)
{
    TG_GLX_BEGIN;
    int r = ((int (*)(void *, unsigned long, void *))tg_glx_fn("glXMakeCurrent"))(H(0), A(1), tg_unhandle(A(2), "glXMakeCurrent"));
    TG_GLX_RET(r);
}

TG_GLX_THUNK(glXCopyContext)
{
    TG_GLX_BEGIN;
    ((void (*)(void *, void *, void *, unsigned long))tg_glx_fn("glXCopyContext"))(
        H(0), tg_unhandle(A(1), "glXCopyContext"), tg_unhandle(A(2), "glXCopyContext"), A(3));
    return tg_end(k, 0);
}

TG_GLX_THUNK(glXSwapBuffers)
{
    TG_GLX_BEGIN;
    ((void (*)(void *, unsigned long))tg_glx_fn("glXSwapBuffers"))(H(0), A(1));
    return tg_end(k, 0);
}

TG_GLX_THUNK(glXCreateGLXPixmap)
{
    TG_GLX_BEGIN;
    void *h = H(0);
    tg_host_vi *vi = tg_glx_host_vi(k, h, A(1));
    unsigned long p = ((unsigned long (*)(void *, void *, unsigned long))tg_glx_fn("glXCreateGLXPixmap"))(h, vi, A(2));
    if (vi)
        ((int (*)(void *))tg_x_fn("XFree"))(vi);
    tg_glx_host_sync(h);
    TG_GLX_RET(p);
}

TG_GLX_THUNK(glXDestroyGLXPixmap)
{
    TG_GLX_BEGIN;
    ((void (*)(void *, unsigned long))tg_glx_fn("glXDestroyGLXPixmap"))(H(0), A(1));
    return tg_end(k, 0);
}

/** @brief Calls a host function (dpy, int *, int *) and stores both ints in the guest. */
static int tg_glx_two_ints(tg_call *k, const char *fn)
{
    int a = 0, b = 0;
    int r = ((int (*)(void *, int *, int *))tg_glx_fn(fn))(tg_glx_host(k, A(0)), &a, &b);
    if (A(1))
        tg_put(k, A(1), &a, 4);
    if (A(2))
        tg_put(k, A(2), &b, 4);
    return r;
}

TG_GLX_THUNK(glXQueryExtension)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(tg_glx_two_ints(k, "glXQueryExtension"));
}

TG_GLX_THUNK(glXQueryVersion)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(tg_glx_two_ints(k, "glXQueryVersion"));
}

TG_GLX_THUNK(glXIsDirect)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(((int (*)(void *, void *))tg_glx_fn("glXIsDirect"))(H(0), tg_unhandle(A(1), "glXIsDirect")));
}

TG_GLX_THUNK(glXGetConfig)
{
    TG_GLX_BEGIN;
    void *h = H(0);
    tg_host_vi *vi = tg_glx_host_vi(k, h, A(1));
    int v = 0;
    int r = ((int (*)(void *, void *, int, int *))tg_glx_fn("glXGetConfig"))(h, vi, (int)A(2), &v);
    if (vi)
        ((int (*)(void *))tg_x_fn("XFree"))(vi);
    if (A(3))
        tg_put(k, A(3), &v, 4);
    TG_GLX_RET(r);
}

TG_GLX_THUNK(glXGetCurrentContext)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(tg_handle(((void *(*)(void))tg_glx_fn("glXGetCurrentContext"))()));
}

TG_GLX_THUNK(glXGetCurrentDrawable)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(((unsigned long (*)(void))tg_glx_fn("glXGetCurrentDrawable"))());
}

TG_GLX_THUNK(glXGetCurrentReadDrawable)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(((unsigned long (*)(void))tg_glx_fn("glXGetCurrentReadDrawable"))());
}

TG_GLX_THUNK(glXGetCurrentDisplay)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(tg_glx_guest_dpy(((void *(*)(void))tg_glx_fn("glXGetCurrentDisplay"))()));
}

TG_GLX_THUNK(glXWaitGL)
{
    TG_GLX_BEGIN;
    ((void (*)(void))tg_glx_fn("glXWaitGL"))();
    return tg_end(k, 0);
}

TG_GLX_THUNK(glXWaitX)
{
    TG_GLX_BEGIN;
    pthread_mutex_lock(&tg_glx_lock);
    uint32_t dpys[8];
    for (int i = 0; i < 8; i++)
        dpys[i] = tg_glx_dpy[i].guest;
    pthread_mutex_unlock(&tg_glx_lock);
    for (int i = 0; i < 8; i++)
        if (dpys[i])
            tg_glx_guest_sync(dpys[i]);
    ((void (*)(void))tg_glx_fn("glXWaitX"))();
    return tg_end(k, 0);
}

TG_GLX_THUNK(glXUseXFont)
{
    TG_GLX_BEGIN;
    ((void (*)(unsigned long, int, int, int))tg_glx_fn("glXUseXFont"))(A(0), (int)A(1), (int)A(2), (int)A(3));
    return tg_end(k, 0);
}

TG_GLX_THUNK(glXQueryExtensionsString)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(tg_str_out(k, ((const char *(*)(void *, int))tg_glx_fn("glXQueryExtensionsString"))(H(0), (int)A(1))));
}

TG_GLX_THUNK(glXQueryServerString)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(tg_str_out(k, ((const char *(*)(void *, int, int))tg_glx_fn("glXQueryServerString"))(H(0), (int)A(1), (int)A(2))));
}

TG_GLX_THUNK(glXGetClientString)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(tg_str_out(k, ((const char *(*)(void *, int))tg_glx_fn("glXGetClientString"))(H(0), (int)A(1))));
}

/** @brief A host GLXFBConfig array as a guest array from the guest's malloc (its XFree frees it). */
static uint32_t tg_glx_configs(tg_call *k, void **cfgs, int n, uint32_t nout)
{
    if (nout)
        tg_put(k, nout, &n, 4);
    if (!cfgs)
        return 0;
    uint32_t a[1] = {4u * (uint32_t)(n > 0 ? n : 1)};
    uint32_t g = (uint32_t)tg_guest_libc_in("libc.so.6", "malloc", a, 1);
    for (int i = 0; g && i < n; i++) {
        uint32_t t = tg_handle(cfgs[i]);
        tg_put(k, g + 4 * (uint32_t)i, &t, 4);
    }
    ((int (*)(void *))tg_x_fn("XFree"))(cfgs);
    return g;
}

TG_GLX_THUNK(glXChooseFBConfig)
{
    TG_GLX_BEGIN;
    int n = 0;
    void **cfgs = ((void **(*)(void *, int, const int *, int *))tg_glx_fn("glXChooseFBConfig"))(H(0), (int)A(1), tg_glx_attribs(k, A(2)), &n);
    TG_GLX_RET(tg_glx_configs(k, cfgs, n, A(3)));
}

TG_GLX_THUNK(glXGetFBConfigs)
{
    TG_GLX_BEGIN;
    int n = 0;
    void **cfgs = ((void **(*)(void *, int, int *))tg_glx_fn("glXGetFBConfigs"))(H(0), (int)A(1), &n);
    TG_GLX_RET(tg_glx_configs(k, cfgs, n, A(2)));
}

TG_GLX_THUNK(glXGetFBConfigAttrib)
{
    TG_GLX_BEGIN;
    int v = 0;
    int r = ((int (*)(void *, void *, int, int *))tg_glx_fn("glXGetFBConfigAttrib"))(H(0), tg_unhandle(A(1), "glXGetFBConfigAttrib"), (int)A(2), &v);
    if (A(3))
        tg_put(k, A(3), &v, 4);
    TG_GLX_RET(r);
}

TG_GLX_THUNK(glXGetVisualFromFBConfig)
{
    TG_GLX_BEGIN;
    void *h = H(0);
    tg_host_vi *vi = ((tg_host_vi * (*)(void *, void *)) tg_glx_fn("glXGetVisualFromFBConfig"))(h, tg_unhandle(A(1), "glXGetVisualFromFBConfig"));
    uint32_t g = 0;
    if (vi) {
        g = tg_glx_guest_vi(k, A(0), vi->visualid, vi->screen);
        ((int (*)(void *))tg_x_fn("XFree"))(vi);
    }
    TG_GLX_RET(g);
}

/** @brief A drawable maker taking (dpy, config, X resource, attribs). */
static unsigned long tg_glx_make(tg_call *k, const char *fn)
{
    void *h = tg_glx_host(k, A(0));
    unsigned long d = ((unsigned long (*)(void *, void *, unsigned long, const int *))tg_glx_fn(fn))(
        h, tg_unhandle(A(1), fn), A(2), tg_glx_attribs(k, A(3)));
    tg_glx_host_sync(h);
    return d;
}

TG_GLX_THUNK(glXCreateWindow)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(tg_glx_make(k, "glXCreateWindow"));
}

TG_GLX_THUNK(glXCreatePixmap)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(tg_glx_make(k, "glXCreatePixmap"));
}

TG_GLX_THUNK(glXCreatePbuffer)
{
    TG_GLX_BEGIN;
    void *h = H(0);
    unsigned long d = ((unsigned long (*)(void *, void *, const int *))tg_glx_fn("glXCreatePbuffer"))(
        h, tg_unhandle(A(1), "glXCreatePbuffer"), tg_glx_attribs(k, A(2)));
    tg_glx_host_sync(h);
    TG_GLX_RET(d);
}

/** @brief A destroy call taking (dpy, XID). */
static void tg_glx_destroy(tg_call *k, const char *fn)
{
    ((void (*)(void *, unsigned long))tg_glx_fn(fn))(tg_glx_host(k, A(0)), A(1));
}

TG_GLX_THUNK(glXDestroyWindow)
{
    TG_GLX_BEGIN;
    tg_glx_destroy(k, "glXDestroyWindow");
    return tg_end(k, 0);
}

TG_GLX_THUNK(glXDestroyPixmap)
{
    TG_GLX_BEGIN;
    tg_glx_destroy(k, "glXDestroyPixmap");
    return tg_end(k, 0);
}

TG_GLX_THUNK(glXDestroyPbuffer)
{
    TG_GLX_BEGIN;
    tg_glx_destroy(k, "glXDestroyPbuffer");
    return tg_end(k, 0);
}

TG_GLX_THUNK(glXQueryDrawable)
{
    TG_GLX_BEGIN;
    unsigned v = 0;
    ((void (*)(void *, unsigned long, int, unsigned *))tg_glx_fn("glXQueryDrawable"))(H(0), A(1), (int)A(2), &v);
    if (A(3))
        tg_put(k, A(3), &v, 4);
    return tg_end(k, 0);
}

TG_GLX_THUNK(glXCreateNewContext)
{
    TG_GLX_BEGIN;
    void *ctx = ((void *(*)(void *, void *, int, void *, int))tg_glx_fn("glXCreateNewContext"))(
        H(0), tg_unhandle(A(1), "glXCreateNewContext"), (int)A(2), tg_unhandle(A(3), "glXCreateNewContext"), (int)A(4));
    TG_GLX_RET(tg_handle(ctx));
}

TG_GLX_THUNK(glXMakeContextCurrent)
{
    TG_GLX_BEGIN;
    int r = ((int (*)(void *, unsigned long, unsigned long, void *))tg_glx_fn("glXMakeContextCurrent"))(
        H(0), A(1), A(2), tg_unhandle(A(3), "glXMakeContextCurrent"));
    TG_GLX_RET(r);
}

TG_GLX_THUNK(glXQueryContext)
{
    TG_GLX_BEGIN;
    int v = 0;
    int r = ((int (*)(void *, void *, int, int *))tg_glx_fn("glXQueryContext"))(H(0), tg_unhandle(A(1), "glXQueryContext"), (int)A(2), &v);
    if (A(3))
        tg_put(k, A(3), &v, 4);
    TG_GLX_RET(r);
}

TG_GLX_THUNK(glXSelectEvent)
{
    TG_GLX_BEGIN;
    ((void (*)(void *, unsigned long, unsigned long))tg_glx_fn("glXSelectEvent"))(H(0), A(1), A(2));
    return tg_end(k, 0);
}

TG_GLX_THUNK(glXGetSelectedEvent)
{
    TG_GLX_BEGIN;
    unsigned long v = 0;
    ((void (*)(void *, unsigned long, unsigned long *))tg_glx_fn("glXGetSelectedEvent"))(H(0), A(1), &v);
    uint32_t g = (uint32_t)v;
    if (A(2))
        tg_put(k, A(2), &g, 4);
    return tg_end(k, 0);
}

/** @brief The guest address of the thunk of a GL or GLX function, 0 when the host has none. */
static uint32_t tg_glx_proc(tg_call *k, uint32_t name)
{
    if (!name)
        return 0;
    const char *n = (const char *)k->mem + name;
    void *h = ((void *(*)(const char *))tg_glx_fn("glXGetProcAddressARB"))(n);
    uint32_t g = h ? tg_guest_symbol(TG_GL, n) : 0;
    if (h && !g)
        fprintf(stderr, "relift: glXGetProcAddress: %s has no thunk; the guest gets NULL\n", n);
    return g;
}

TG_GLX_THUNK(glXGetProcAddress)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(tg_glx_proc(k, A(0)));
}

TG_GLX_THUNK(glXGetProcAddressARB)
{
    TG_GLX_BEGIN;
    TG_GLX_RET(tg_glx_proc(k, A(0)));
}

TG_GLX_THUNK(glXCreateContextAttribsARB)
{
    TG_GLX_BEGIN;
    void *ctx = ((void *(*)(void *, void *, void *, int, const int *))tg_glx_fn("glXCreateContextAttribsARB"))(
        H(0), tg_unhandle(A(1), "glXCreateContextAttribsARB"), tg_unhandle(A(2), "glXCreateContextAttribsARB"), (int)A(3),
        tg_glx_attribs(k, A(4)));
    TG_GLX_RET(tg_handle(ctx));
}
