/**
 * @file vulkan_harness.c
 * @brief The Vulkan scenario on the host through the generated thunks.
 */
#include "harness.h"

extern const tg_lib tg_lib_vulkan;

static uint32_t vk(const char *name, const uint32_t *a, unsigned n)
{
    return harness_call(&tg_lib_vulkan, name, a, n);
}

#include "vulkan_scenario.h"

int main(void)
{
    harness_init();
    harness_libs[harness_nlibs++] = &tg_lib_vulkan;
    scenario();
    return 0;
}
