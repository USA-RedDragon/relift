/**
 * @file vulkan_native.c
 * @brief The oracle: the Vulkan scenario as a native i386 program against the
 * real i386 libvulkan, each function called with the scenario's 32-bit slots.
 */
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(void *) == 4, "build this for i386");

typedef uint32_t gaddr;
typedef uint32_t s;

static gaddr galloc(size_t n)
{
    return (gaddr)(uintptr_t)calloc(1, n);
}

static uint8_t *gp(gaddr a)
{
    return (uint8_t *)(uintptr_t)a;
}

static void *fn_of(const char *name)
{
    static void *lib;
    if (!lib)
        lib = dlopen("libvulkan.so.1", RTLD_NOW);
    void *f = lib ? dlsym(lib, name) : NULL;
    if (!f) {
        fprintf(stderr, "no %s\n", name);
        exit(3);
    }
    return f;
}

/** @brief A cdecl call with the scenario's slots, padded to 32: extra slots are harmless to a cdecl callee. */
#define SLOTS(T, R)                                                                                \
    static R T(const char *name, const uint32_t *a, unsigned n)                                    \
    {                                                                                              \
        uint32_t x[32] = {0};                                                                      \
        if (n > 32) {                                                                              \
            fprintf(stderr, "%s: %u slots\n", name, n);                                           \
            exit(3);                                                                               \
        }                                                                                          \
        if (n)                                                                                     \
            memcpy(x, a, 4 * n);                                                                   \
        return ((R(*)(s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, \
                      s, s, s))fn_of(name))(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8], x[9], x[10], \
                                             x[11], x[12], x[13], x[14], x[15], x[16], x[17], x[18], x[19],      \
                                             x[20], x[21], x[22], x[23], x[24], x[25], x[26], x[27], x[28],      \
                                             x[29], x[30], x[31]);                                               \
    }

SLOTS(vk, uint32_t)


#include "vulkan_scenario.h"

int main(void)
{
    scenario();
    return 0;
}
