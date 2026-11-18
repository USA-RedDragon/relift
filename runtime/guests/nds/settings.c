/**
 * @file settings.c
 * @brief The settings of the DS layer: where the save is kept, the backup
 * chip when the save does not name it, and how the two screens are laid out
 * and scaled in the window.
 */
#include "nds.h"

#include <relift/settings.h>

static const char *const g_layouts[] = {"auto",       "stacked",  "side_by_side", "large_top",
                                        "large_bottom", "top_only", "bottom_only"};
static const char *const g_displays[] = {"auto", "swapped", "top_both", "bottom_both", "single"};

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
        .apply = RL_APPLY_LIVE,
        .type = RL_SETTING_ENUM,
        .label = "Screen layout",
        .description = "How the two screens share a window. auto: stacked in a tall window, else stacked or side "
                       "by side, whichever fills the window (or the room the on-screen controller leaves) better; stacked: "
                       "the top screen above the bottom one, as the console holds them; side_by_side: the top screen "
                       "left of the bottom one; large_top and large_bottom: that screen at twice the size and the "
                       "other one small beside or below it; top_only and bottom_only: one screen, swapped with the "
                       "Swap screens control",
        .env = "RELIFT_NDS_LAYOUT",
        .choices = g_layouts,
        .nchoices = 7,
    },
    {
        .key = "video.nds_displays",
        .apply = RL_APPLY_LIVE,
        .type = RL_SETTING_ENUM,
        .label = "Displays",
        .description = "What the displays show when the device has a second one. auto: the top screen on the first "
                       "display and the bottom screen on the second; swapped: the other way round; top_both and "
                       "bottom_both: that screen on both displays; single: only the first display, with the screen "
                       "layout",
        .env = "RELIFT_NDS_DISPLAYS",
        .choices = g_displays,
        .nchoices = 5,
    },
    {
        .key = "video.nds_integer_scale",
        .apply = RL_APPLY_LIVE,
        .type = RL_SETTING_BOOL,
        .label = "Whole-number scaling",
        .description = "Scales the screens by the largest whole multiple that fits the window, so every pixel keeps "
                       "its size. Off: they fill the window with their shape kept (with the on-screen controller, "
                       "a whole multiple is used while it loses less than 15% of the room)",
        .env = "RELIFT_NDS_INTEGER_SCALE",
        .def_int = 0,
    },
};

void rl_nds_declare_settings(void)
{
    rl_settings_declare_runtime();
    rl_settings_declare(g_defs, sizeof g_defs / sizeof g_defs[0]);
}
