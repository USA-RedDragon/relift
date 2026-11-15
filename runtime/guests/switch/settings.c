/**
 * @file settings.c
 * @brief The settings of the Switch layer: where saves go and which host
 * Vulkan device the guest sees.
 */
#include "services.h"

#include <relift/settings.h>

static const rl_setting_def g_defs[] = {
    {
        .key = "save.dir",
        .type = RL_SETTING_STRING,
        .label = "Save folder",
        .description = "Where the game's save data is kept. Empty: <user folder>/save when the program names an app, "
                       "else ./save",
        .env = "RELIFT_SAVE",
    },
    {
        .key = "vk.device",
        .type = RL_SETTING_STRING,
        .label = "Graphics device",
        .description = "A part of the name of the host graphics device the game uses. Empty: the first device that "
                       "can present",
        .env = "RELIFT_VK_DEVICE",
    },
    {
        .key = "stats.every",
        .type = RL_SETTING_INT,
        .label = "Frame stats",
        .description = "Every this many guest frames, log the frames per second, the process CPU and the CPU the "
                       "presenting thread spent per frame over them. 0: off",
        .env = "RELIFT_STATS",
        .def_int = 0,
        .min_int = 0,
        .max_int = 1000000,
    },
};

void rl_switch_declare_settings(void)
{
    rl_settings_declare_runtime();
    rl_settings_declare(g_defs, sizeof g_defs / sizeof g_defs[0]);
}
