/**
 * @file settings.c
 * @brief The settings of the Game Boy layer: whether the picture is shown
 * with the colour correction of the LCD.
 */
#include "gbmain.h"

#include <relift/settings.h>

static const rl_setting_def g_defs[] = {
    {
        .key = "video.gb_color_correction",
        .type = RL_SETTING_BOOL,
        .label = "Colour correction",
        .description = "Shows the colours as the LCD of the console does. Off: the plain expansion of the "
                       "5-bit channels the game writes",
        .env = "RELIFT_GB_COLOR_CORRECTION",
        .def_int = 1,
    },
};

void rl_gb_declare_settings(void)
{
    rl_settings_declare_runtime();
    rl_settings_declare(g_defs, sizeof g_defs / sizeof g_defs[0]);
}
