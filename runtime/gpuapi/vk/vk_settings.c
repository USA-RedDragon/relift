/**
 * @file vk_settings.c
 * @brief The settings of Vulkan forwarding: whether attachments keep their
 * contents and whether compressed textures are decoded.
 */
#include "vk.h"

#include <relift/settings.h>

static const char *const g_keep[] = { "auto", "on", "off" };

static const rl_setting_def g_defs[] = {
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
};

void rl_vk_declare_settings(void)
{
    rl_settings_declare_runtime();
    rl_settings_declare(g_defs, sizeof g_defs / sizeof g_defs[0]);
}
