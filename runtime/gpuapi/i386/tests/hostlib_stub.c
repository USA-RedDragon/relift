/**
 * @file hostlib_stub.c
 * @brief The two host library calls tg.c makes, for the harnesses that link tg.c without the runtime core.
 */
#include "hostlib.h"

#include <dlfcn.h>

void *rl_hostlib_open(const char *const *names)
{
    for (; names && *names; names++) {
        void *lib = dlopen(*names, RTLD_NOW | RTLD_LOCAL);
        if (lib)
            return lib;
    }
    return NULL;
}

void *rl_hostlib_sym(void *lib, const char *name) { return lib ? dlsym(lib, name) : NULL; }
