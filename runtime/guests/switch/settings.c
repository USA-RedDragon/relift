/**
 * @file settings.c
 * @brief The settings of the Switch layer: the vsync, the graphics memory
 * pools, the address space layout, the fixed-step clock for tests, local
 * wireless play and pointer authentication.
 */
#include "hid_priv.h"
#include "services.h"
#include "am.h"
#include "vk.h"

#include <relift/arm64/pac.h>
#include <relift/settings.h>
#include <relift/touchpad.h>

static const char *const g_layout[] = { "compact", "console" };
static const char *const g_memory[] = { "auto", "import", "mirror", "placed" };

static const char *const g_pac_choices[] = { "fast", "qarma5" };
static const char *const g_net_modes[] = { "off", "lan" };
static const char *const g_console_modes[] = { "handheld", "docked" };

static const rl_setting_def g_defs[] = {
    {
        .key = "vi.follow_display",
        .type = RL_SETTING_BOOL,
        .label = "Follow the display's refresh",
        .description = "Keep the game's 60 Hz vsync in step with the display, where the host reports when it shows a "
                       "frame, so no frame is shown twice or dropped when the display runs slightly off 60 Hz",
        .env = "RELIFT_VI_FOLLOW_DISPLAY",
        .def_int = 1,
    },
    {
        .key = "nvn.memory",
        .type = RL_SETTING_ENUM,
        .label = "Graphics memory pools",
        .description = "How the game's graphics memory pools reach the host graphics device. import: the device uses the "
                       "game's pages in place (needs VK_EXT_external_memory_host); placed: device memory takes the "
                       "place of the game's pages at their own addresses (needs VK_EXT_map_memory_placed), so the "
                       "game and the device share them as with an import; mirror: each pool gets a device copy "
                       "that the layer keeps equal at the game's sync points, for devices with neither (some "
                       "Android drivers, MoltenVK) and for GPU capture tools that hide them. auto: import where the "
                       "device has it, else placed, else mirror",
        .env = "RELIFT_NVN_MEMORY",
        .choices = g_memory,
        .nchoices = 4,
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
    {
        .key = "clock.fixed_step",
        .type = RL_SETTING_BOOL,
        .label = "Fixed-step clock (testing)",
        .description = "For tests: run one guest thread at a time on a guest clock that only the game's progress moves, "
                       "so two runs with the same input script show the same frames. Slower; vi.follow_display is off",
        .env = "RELIFT_FIXED_STEP",
        .def_int = 0,
    },
    {
        .key = "net.mode",
        .type = RL_SETTING_ENUM,
        .label = "Local wireless play",
        .description = "off: the game finds no one to play with nearby. lan: the game's local wireless sessions run over "
                       "the host's network with the ldn_mitm LAN protocol, so other relift instances and consoles "
                       "running ldn_mitm on the same network can join; the game's sockets reach only the members of "
                       "the session it is in",
        .env = "RELIFT_NET",
        .choices = g_net_modes,
        .nchoices = 2,
    },
    {
        .key = "net.interface",
        .type = RL_SETTING_STRING,
        .label = "Local play network interface",
        .description = "The host network interface (a name or one of its IPv4 addresses) local wireless play uses. "
                       "Empty: the first interface that is up and has an IPv4 address that is not loopback",
        .env = "RELIFT_NET_IF",
    },
    {
        .key = "cpu.pac",
        .type = RL_SETTING_ENUM,
        .label = "Pointer authentication cipher",
        .description = "fast is a keyed hash; qarma5 is the console's QARMA5. A game sees the same behavior with "
                       "either, since it cannot learn the keys",
        .env = "RELIFT_PAC",
        .choices = g_pac_choices,
        .nchoices = 2,
    },
    {
        .key = "cpu.pac_seed",
        .type = RL_SETTING_INT,
        .label = "Pointer authentication key seed",
        .description = "The seed of the process's pointer authentication keys, which the console picks at random",
        .env = "RELIFT_PAC_SEED",
        .min_int = 0,
        .max_int = INT64_MAX,
    },
    {
        .key = "system.console_mode",
        .type = RL_SETTING_ENUM,
        .label = "Console mode",
        .description = "Whether the console is docked to a TV or held in the hands, which the game reads as the "
                       "console's operation and performance mode. docked: the game renders for a TV, usually sharper "
                       "and costlier on the GPU; handheld: it renders for the built-in screen, usually lighter. The "
                       "same default on every host. A run can switch while the game runs (rl_am_set_operation_mode, or "
                       "the input script controls console.docked and console.handheld)",
        .env = "RELIFT_CONSOLE_MODE",
        .choices = g_console_modes,
        .nchoices = 2,
    },
};

void rl_switch_apply_pac_settings(void)
{
    rl_pac_seed((uint64_t)rl_setting_int("cpu.pac_seed"));
    rl_pac_mode = rl_setting_enum("cpu.pac") == 1 ? RL_PAC_QARMA5 : RL_PAC_FAST;
}

void rl_switch_declare_settings(void)
{
    rl_settings_declare_runtime();
    rl_vk_declare_settings();
    rl_settings_declare(g_defs, sizeof g_defs / sizeof g_defs[0]);
    rl_render_sizes_register(rl_switch_render_sizes);
    rl_touchpad_declare(rl_switch_touchpad());
}
