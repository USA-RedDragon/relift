/**
 * @file settings.c
 * @brief The settings of the Switch layer: where saves go, which host
 * Vulkan device the guest sees, whether attachments keep their contents and
 * whether compressed textures are decoded.
 */
#include "hid_priv.h"
#include "services.h"

#include <relift/settings.h>
#include <relift/touchpad.h>

static const char *const g_keep[] = { "auto", "on", "off" };
static const char *const g_layout[] = { "compact", "console" };

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
    {
        .key = "vk.keep_attachments",
        .type = RL_SETTING_ENUM,
        .label = "Keep attachment contents",
        .description = "Pass the game's DONT_CARE attachment load and store ops as LOAD and STORE, as the console keeps "
                       "the contents. auto: only on host GPUs known to lose them (Apple)",
        .env = "RELIFT_VK_KEEP_ATTACHMENTS",
        .choices = g_keep,
        .nchoices = 3,
    },
    {
        .key = "vk.decode_bc",
        .type = RL_SETTING_ENUM,
        .label = "Decode compressed textures",
        .description = "Give the host GPU BC1 to BC7 textures as uncompressed ones, decoded on the CPU at upload. auto: "
                       "only the formats the host GPU cannot sample (Apple, some Adreno and Mali)",
        .env = "RELIFT_VK_DECODE_BC",
        .choices = g_keep,
        .nchoices = 3,
    },
    {
        .key = "kernel.layout",
        .type = RL_SETTING_ENUM,
        .label = "Address space layout",
        .description = "compact: the regions the game uses packed at the lowest bases, so less host address space is "
                       "reserved; console: the console's exact region sizes",
        .env = "RELIFT_KERNEL_LAYOUT",
        .choices = g_layout,
        .nchoices = 2,
    },
};

void rl_switch_declare_settings(void)
{
    rl_settings_declare_runtime();
    rl_settings_declare(g_defs, sizeof g_defs / sizeof g_defs[0]);
    rl_touchpad_declare(rl_switch_touchpad());
}
