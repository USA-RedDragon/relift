/**
 * @file glx_harness.c
 * @brief The GLX scenario on the host through the generated GL thunks and
 * the GLX hand thunks. The guest's X11 is lifted in a real run; here
 * stand-ins play it: their own host connection, with i386 XVisualInfo in
 * guest memory.
 */
#include "harness.h"

#include <dlfcn.h>

extern const tg_lib tg_lib_gl;

static uint32_t glc(const char *name, const uint32_t *a, unsigned n)
{
    return harness_call(&tg_lib_gl, name, a, n);
}

static void *x11(const char *name)
{
    static void *lib;
    if (!lib)
        lib = dlopen("libX11.so.6", RTLD_NOW);
    void *f = lib ? dlsym(lib, name) : NULL;
    if (!f) {
        fprintf(stderr, "harness: no %s\n", name);
        exit(2);
    }
    return f;
}

/** @brief The stand-in guest displays: guest token and host connection. */
static uint32_t gdpy_at[4];
static void *gdpy[4];

static void *xd(uint32_t g)
{
    for (int i = 0; i < 4; i++)
        if (gdpy_at[i] == g)
            return gdpy[i];
    fprintf(stderr, "harness: unknown Display %x\n", g);
    exit(2);
}

static gaddr x_open(void)
{
    gdpy[0] = ((void *(*)(const char *))x11("XOpenDisplay"))(NULL);
    if (!gdpy[0]) {
        fprintf(stderr, "harness: no X display\n");
        exit(3);
    }
    gdpy_at[0] = galloc(64);
    return gdpy_at[0];
}

static int x_screen(gaddr d)
{
    return ((int (*)(void *))x11("XDefaultScreen"))(xd(d));
}

static uint32_t stand_in_sync(rl_ctx *c, uint8_t *m)
{
    return (uint32_t)((int (*)(void *, int))x11("XSync"))(xd(harness_arg(c, m, 0)), (int)harness_arg(c, m, 1));
}

static uint32_t stand_in_display_string(rl_ctx *c, uint8_t *m)
{
    const char *s = ((const char *(*)(void *))x11("XDisplayString"))(xd(harness_arg(c, m, 0)));
    gaddr g = galloc(strlen(s) + 1);
    memcpy(gp(g), s, strlen(s) + 1);
    return g;
}

/** @brief XGetVisualInfo: the host's answer written as i386 XVisualInfo (40 bytes each). */
static uint32_t stand_in_get_visual_info(rl_ctx *c, uint8_t *m)
{
    void *d = xd(harness_arg(c, m, 0));
    long mask = (long)harness_arg(c, m, 1);
    uint32_t t = harness_arg(c, m, 2), nout = harness_arg(c, m, 3);
    uint8_t tmpl[64] = {0};
    uint32_t vid, scr;
    memcpy(&vid, m + t + 4, 4);
    memcpy(&scr, m + t + 8, 4);
    unsigned long id = vid;
    memcpy(tmpl + 8, &id, 8);
    memcpy(tmpl + 16, &scr, 4);
    int n = 0;
    uint8_t *v = ((uint8_t * (*)(void *, long, void *, int *)) x11("XGetVisualInfo"))(d, mask, tmpl, &n);
    memcpy(m + nout, &n, 4);
    if (!v)
        return 0;
    gaddr g = galloc(40 * (size_t)n);
    for (int i = 0; i < n; i++) {
        const uint8_t *h = v + 64 * i;
        uint8_t *o = m + g + 40 * i;
        unsigned long l;
        memcpy(&l, h + 8, 8);
        uint32_t w = (uint32_t)l;
        memcpy(o + 4, &w, 4);
        memcpy(o + 8, h + 16, 12);
        for (int j = 0; j < 3; j++) {
            memcpy(&l, h + 32 + 8 * j, 8);
            w = (uint32_t)l;
            memcpy(o + 20 + 4 * j, &w, 4);
        }
        memcpy(o + 32, h + 56, 8);
    }
    ((int (*)(void *))x11("XFree"))(v);
    return g;
}

static uint32_t stand_in_malloc(rl_ctx *c, uint8_t *m)
{
    return galloc(harness_arg(c, m, 0));
}

static void stand_in(const char *soname, const char *name, harness_guest_fn *fn)
{
    harness_libc[harness_nlibc].soname = soname;
    harness_libc[harness_nlibc].name = name;
    harness_libc[harness_nlibc++].at = harness_guest(fn);
}

#include "gl_scenario.h"
#include "glx_scenario.h"

int main(void)
{
    harness_init();
    harness_libs[harness_nlibs++] = &tg_lib_gl;
    stand_in("libX11.so.6", "XSync", stand_in_sync);
    stand_in("libX11.so.6", "XDisplayString", stand_in_display_string);
    stand_in("libX11.so.6", "XGetVisualInfo", stand_in_get_visual_info);
    stand_in("libc.so.6", "malloc", stand_in_malloc);
    glx_context();
    scenario();
    glx_teardown();
    return 0;
}
