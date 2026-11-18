/**
 * @file settings.c
 * @brief The settings of the DS layer: where the save is kept, the backup
 * chip when the save does not name it, and how the two screens are laid out
 * and scaled in the window.
 */
#include "nds.h"

#include <relift/settings.h>

static const char *const g_layouts[] = {"stacked", "side_by_side"};

static const rl_setting_def g_defs[] = {
    {
        .key = "save.dir",
        .type = RL_SETTING_STRING,
        .label = "Save folder",
        .description = "Where the game's save file is kept. Empty: <user folder>/save when the program names an app, "
                       "else next to the ROM",
        .env = "RELIFT_SAVE",
    },
    {
        .key = "save.type",
        .type = RL_SETTING_STRING,
        .label = "Backup chip",
        .description = "The cartridge's backup chip when no save file names it by its length: auto, eeprom512, eeprom8k, "
                       "eeprom64k, eeprom128k, fram8k, fram32k, flash256k, flash512k or flash1m",
        .env = "RELIFT_SAVE_TYPE",
    },
    {
        .key = "video.nds_layout",
        .type = RL_SETTING_ENUM,
        .label = "Screen layout",
        .description = "stacked: the top screen above the bottom one, as the console holds them; side_by_side: the top "
                       "screen left of the bottom one",
        .env = "RELIFT_NDS_LAYOUT",
        .choices = g_layouts,
        .nchoices = 2,
    },
    {
        .key = "video.nds_integer_scale",
        .type = RL_SETTING_BOOL,
        .label = "Whole-number scaling",
        .description = "Scales the screens by the largest whole multiple that fits the window, so every pixel keeps "
                       "its size. Off: they fill the window with their shape kept",
        .env = "RELIFT_NDS_INTEGER_SCALE",
        .def_int = 1,
    },
};

void rl_nds_declare_settings(void)
{
    rl_settings_declare_runtime();
    rl_settings_declare(g_defs, sizeof g_defs / sizeof g_defs[0]);
}
