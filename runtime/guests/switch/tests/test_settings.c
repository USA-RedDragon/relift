/**
 * @file test_settings.c
 * @brief The Switch layer's settings: the save folder (setting, environment
 * and the per-user fallback) and the graphics device key.
 */
#include "hos_fs.h"
#include "services.h"
#include "t.h"

#include <relift/settings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *sdmc_dir(void)
{
    static char path[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/rl_sw_settings_sdmc_%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    return path;
}

static void clean(void)
{
    rl_settings_reset();
    unsetenv("RELIFT_SAVE");
    unsetenv("RELIFT_VK_DEVICE");
    setenv("RELIFT_SDMC", sdmc_dir(), 1);
}

TEST(save_dir_defaults_then_flag_then_env)
{
    clean();
    rl_srv_install_fs();
    CHECK(!strcmp(rl_hos_fs_save_dir(), "./save"));
    rl_settings_reset();
    char *argv[] = { "x", "--relift-set", "save.dir=/tmp/rl_a/", NULL };
    rl_switch_declare_settings();
    CHECK_EQ(rl_settings_args(3, argv), 1);
    rl_srv_install_fs();
    CHECK(!strcmp(rl_hos_fs_save_dir(), "/tmp/rl_a"));
    setenv("RELIFT_SAVE", "/tmp/rl_b", 1);
    rl_srv_install_fs();
    CHECK(!strcmp(rl_hos_fs_save_dir(), "/tmp/rl_b"));
    clean();
}

TEST(save_dir_falls_back_to_the_user_folder_of_the_app)
{
    clean();
    setenv("HOME", "/home/someone", 1);
    unsetenv("XDG_DATA_HOME");
    rl_settings_set_app("mygame");
    rl_srv_install_fs();
    CHECK(!strcmp(rl_hos_fs_save_dir(), "/home/someone/.local/share/mygame/save"));
    CHECK_EQ(rl_settings_set("save.dir", "/tmp/rl_c"), RL_SETTING_OK);
    rl_srv_install_fs();
    CHECK(!strcmp(rl_hos_fs_save_dir(), "/tmp/rl_c"));
    clean();
}

TEST(graphics_device_is_a_declared_string)
{
    clean();
    rl_switch_declare_settings();
    CHECK(!strcmp(rl_setting_str("vk.device"), ""));
    CHECK_EQ(rl_settings_set("vk.device", "lavapipe"), RL_SETTING_OK);
    CHECK(!strcmp(rl_setting_str("vk.device"), "lavapipe"));
    setenv("RELIFT_VK_DEVICE", "nvidia", 1);
    CHECK(!strcmp(rl_setting_str("vk.device"), "nvidia"));
    clean();
}
