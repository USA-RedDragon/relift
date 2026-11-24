/**
 * @file gl_harness.c
 * @brief The GL scenario on the host through the generated GL thunks. The
 * context comes from the host's EGL directly (EGL is not wrapped yet); every
 * GL call goes through its thunk with i386 slots and guest-window data.
 */
#include "harness.h"

extern const tg_lib tg_lib_gl;
extern const tg_lib tg_lib_egl;

static uint32_t glc(const char *name, const uint32_t *a, unsigned n)
{
    return harness_call(&tg_lib_gl, name, a, n);
}

static uint32_t eglc(const char *name, const uint32_t *a, unsigned n)
{
    return harness_call(&tg_lib_egl, name, a, n);
}

#include "gl_context.h"
#include "gl_scenario.h"

int main(void)
{
    harness_init();
    harness_libs[harness_nlibs++] = &tg_lib_gl;
    harness_libs[harness_nlibs++] = &tg_lib_egl;
    gl_context(W, H);
    scenario();
    return 0;
}
