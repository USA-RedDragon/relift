/**
 * @file gl_context.h
 * @brief An offscreen OpenGL compatibility context through EGL on the
 * surfaceless platform, made with the scenario's slot calls (eglc), so the
 * native build calls the i386 libEGL and the harness the EGL thunks. The
 * caller picks Mesa's llvmpipe through the environment (gl_check.sh).
 * Needs gaddr, galloc, gp and eglc(name, slots, n) from the includer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief EGL constants. */
enum {
    EGL_NONE_ = 0x3038,
    EGL_PLATFORM_SURFACELESS_MESA_ = 0x31DD,
    EGL_SURFACE_TYPE_ = 0x3033,
    EGL_PBUFFER_BIT_ = 0x0001,
    EGL_RENDERABLE_TYPE_ = 0x3040,
    EGL_OPENGL_BIT_ = 0x0008,
    EGL_RED_SIZE_ = 0x3024,
    EGL_GREEN_SIZE_ = 0x3023,
    EGL_BLUE_SIZE_ = 0x3022,
    EGL_ALPHA_SIZE_ = 0x3021,
    EGL_DEPTH_SIZE_ = 0x3025,
    EGL_WIDTH_ = 0x3057,
    EGL_HEIGHT_ = 0x3056,
    EGL_OPENGL_API_ = 0x30A2,
    EGL_VENDOR_ = 0x3053,
    EGL_CLIENT_APIS_ = 0x308D,
};

#define EGLC(name, ...)                                                                            \
    ({                                                                                             \
        const uint32_t e_[] = {0, __VA_ARGS__};                                                    \
        eglc(name, e_ + 1, sizeof e_ / 4 - 1);                                                     \
    })

static gaddr gl_ints(const int32_t *v, size_t n)
{
    gaddr a = galloc(4 * n);
    memcpy(gp(a), v, 4 * n);
    return a;
}

static uint32_t gl_rd32(gaddr a)
{
    uint32_t v;
    memcpy(&v, gp(a), 4);
    return v;
}

static void gl_context(int w, int h)
{
    gaddr d = EGLC("eglGetPlatformDisplay", EGL_PLATFORM_SURFACELESS_MESA_, 0, 0);
    gaddr maj = galloc(4), min = galloc(4);
    uint32_t ok = EGLC("eglInitialize", d, maj, min);
    if (!d || !ok) {
        fprintf(stderr, "no EGL display\n");
        exit(3);
    }
    printf("EGL %u.%u vendor %s apis %s\n", gl_rd32(maj), gl_rd32(min), (const char *)gp(EGLC("eglQueryString", d, EGL_VENDOR_)),
           (const char *)gp(EGLC("eglQueryString", d, EGL_CLIENT_APIS_)));
    const int32_t attrs[] = {EGL_SURFACE_TYPE_, EGL_PBUFFER_BIT_, EGL_RENDERABLE_TYPE_, EGL_OPENGL_BIT_, EGL_RED_SIZE_, 8,
                             EGL_GREEN_SIZE_, 8, EGL_BLUE_SIZE_, 8, EGL_ALPHA_SIZE_, 8, EGL_DEPTH_SIZE_, 24, EGL_NONE_};
    gaddr cfgs = galloc(8), n = galloc(4);
    ok = EGLC("eglChooseConfig", d, gl_ints(attrs, sizeof attrs / 4), cfgs, 2, n);
    printf("configs %d %u\n", (int)ok, gl_rd32(n) > 0);
    if (!ok || gl_rd32(n) < 1) {
        fprintf(stderr, "no EGL config\n");
        exit(3);
    }
    gaddr cfg = gl_rd32(cfgs);
    const int32_t pb[] = {EGL_WIDTH_, w, EGL_HEIGHT_, h, EGL_NONE_};
    gaddr s = EGLC("eglCreatePbufferSurface", d, cfg, gl_ints(pb, 5));
    EGLC("eglBindAPI", EGL_OPENGL_API_);
    gaddr c = EGLC("eglCreateContext", d, cfg, 0, 0);
    ok = EGLC("eglMakeCurrent", d, s, s, c);
    printf("context %d %d current %d error %x\n", s != 0, c != 0, (int)ok, EGLC("eglGetError"));
    gaddr name = galloc(8);
    memcpy(gp(name), "glClear", 8);
    printf("eglGetProcAddress(glClear) %d\n", EGLC("eglGetProcAddress", name) != 0);
    if (!s || !c || !ok) {
        fprintf(stderr, "no EGL context\n");
        exit(3);
    }
}
