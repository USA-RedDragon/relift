/**
 * @file glx_native.c
 * @brief The oracle: the GLX scenario as a native i386 program against the
 * real i386 libGL and libX11. Each GL function is called with the scenario's 32-bit
 * argument slots, which on i386 is the cdecl call itself.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


_Static_assert(sizeof(void *) == 4, "build this for i386");

typedef uint32_t gaddr;

static gaddr galloc(size_t n)
{
    return (gaddr)(uintptr_t)calloc(1, n);
}

static uint8_t *gp(gaddr a)
{
    return (uint8_t *)(uintptr_t)a;
}

typedef uint32_t s;

static uint32_t glc(const char *name, const uint32_t *a, unsigned n)
{
    static void *lib;
    static void *(*gpa)(const unsigned char *);
    if (!lib) {
        lib = dlopen("libGL.so.1", RTLD_NOW);
        gpa = lib ? (void *(*)(const unsigned char *))dlsym(lib, "glXGetProcAddressARB") : NULL;
    }
    void *f = lib ? dlsym(lib, name) : NULL;
    if (!f && gpa)
        f = gpa((const unsigned char *)name);
    if (!f) {
        fprintf(stderr, "no %s\n", name);
        exit(3);
    }
    switch (n) {
    case 0: return ((s(*)(void))f)();
    case 1: return ((s(*)(s))f)(a[0]);
    case 2: return ((s(*)(s, s))f)(a[0], a[1]);
    case 3: return ((s(*)(s, s, s))f)(a[0], a[1], a[2]);
    case 4: return ((s(*)(s, s, s, s))f)(a[0], a[1], a[2], a[3]);
    case 5: return ((s(*)(s, s, s, s, s))f)(a[0], a[1], a[2], a[3], a[4]);
    case 6: return ((s(*)(s, s, s, s, s, s))f)(a[0], a[1], a[2], a[3], a[4], a[5]);
    case 7: return ((s(*)(s, s, s, s, s, s, s))f)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    case 8: return ((s(*)(s, s, s, s, s, s, s, s))f)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    case 9: return ((s(*)(s, s, s, s, s, s, s, s, s))f)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
    case 10: return ((s(*)(s, s, s, s, s, s, s, s, s, s))f)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]);
    case 11: return ((s(*)(s, s, s, s, s, s, s, s, s, s, s))f)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10]);
    case 12: return ((s(*)(s, s, s, s, s, s, s, s, s, s, s, s))f)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
    default:
        fprintf(stderr, "%s: too many slots\n", name);
        exit(3);
    }
}

static void *x11(const char *name)
{
    static void *lib;
    if (!lib)
        lib = dlopen("libX11.so.6", RTLD_NOW);
    void *f = lib ? dlsym(lib, name) : NULL;
    if (!f) {
        fprintf(stderr, "no %s\n", name);
        exit(3);
    }
    return f;
}

static gaddr x_open(void)
{
    return (gaddr)(uintptr_t)((void *(*)(const char *))x11("XOpenDisplay"))(NULL);
}

static int x_screen(gaddr d)
{
    return ((int (*)(void *))x11("XDefaultScreen"))((void *)(uintptr_t)d);
}

#include "gl_scenario.h"
#include "glx_scenario.h"

int main(void)
{
    glx_context();
    scenario();
    glx_teardown();
    return 0;
}
