/**
 * @file test_gpu_driver.c
 * @brief Driver packages: names, meta.json, the installed list and the
 * video.gpu_driver setting.
 */
#include "gpu_driver.h"
#include "hostlib.h"
#include "runner.h"

#include <relift/settings.h>

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_root[512];

static void put(const char *pkg, const char *file, const char *text)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", g_root, pkg);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/%s/%s", g_root, pkg, file);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void make_root(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(g_root, sizeof g_root, "%s/rl_gpu_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(g_root))
        g_root[0] = 0;
    setenv(RL_GPU_DRIVERS_ENV, g_root, 1);
    put("turnip", "meta.json",
        "{\n  \"schemaVersion\": 1,\n  \"name\": \"Mesa Turnip\",\n  \"vendor\": \"Mesa\",\n"
        "  \"libraryName\" : \"vulkan.ad07xx.so\"\n}\n");
    put("turnip", "vulkan.ad07xx.so", "x");
    put("blob", "meta.json", "{\"libraryName\": \"vulkan.adreno.so\", \"name\": \"libraryName\"}");
    put("blob", "vulkan.adreno.so", "x");
    put("nolib", "meta.json", "{\"libraryName\": \"missing.so\"}");
    put("escape", "meta.json", "{\"libraryName\": \"../../evil.so\"}");
    put("nometa", "vulkan.x.so", "x");
}

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static void remove_root(void)
{
    if (nftw(g_root, remove_one, 16, FTW_DEPTH | FTW_PHYS) != 0)
        fprintf(stderr, "could not remove %s\n", g_root);
}

TEST(gpu_driver_names)
{
    CHECK(rl_gpu_driver_name_ok("Turnip_v26.0.0_R8"));
    CHECK(rl_gpu_driver_name_ok("a-b"));
    CHECK(!rl_gpu_driver_name_ok("system"));
    CHECK(!rl_gpu_driver_name_ok(""));
    CHECK(!rl_gpu_driver_name_ok(NULL));
    CHECK(!rl_gpu_driver_name_ok(".hidden"));
    CHECK(!rl_gpu_driver_name_ok("../x"));
    CHECK(!rl_gpu_driver_name_ok("a b"));
}

TEST(gpu_drivers_live_under_home_unless_the_env_names_a_folder)
{
    char d[600];
    unsetenv(RL_GPU_DRIVERS_ENV);
    setenv("HOME", "/data/app/files", 1);
    CHECK(rl_gpu_drivers_dir(d, sizeof d));
    CHECK(!strcmp(d, "/data/app/files/gpu_drivers"));
    setenv(RL_GPU_DRIVERS_ENV, "/x/drivers/", 1);
    CHECK(rl_gpu_drivers_dir(d, sizeof d));
    CHECK(!strcmp(d, "/x/drivers"));
}

TEST(gpu_driver_reads_meta_json)
{
    make_root();
    CHECK(g_root[0]);
    rl_gpu_driver d;
    bool turnip = rl_gpu_driver_find("turnip", &d);
    char dir[700];
    snprintf(dir, sizeof dir, "%s/turnip/", g_root);
    bool blob = turnip && !strcmp(d.library, "vulkan.ad07xx.so") && !strcmp(d.dir, dir) &&
                rl_gpu_driver_find("blob", &d) && !strcmp(d.library, "vulkan.adreno.so");
    bool bad = rl_gpu_driver_find("nolib", &d) || rl_gpu_driver_find("escape", &d) ||
               rl_gpu_driver_find("nometa", &d) || rl_gpu_driver_find("absent", &d) || rl_gpu_driver_find("..", &d);
    remove_root();
    CHECK(turnip);
    CHECK(blob);
    CHECK(!bad);
}

TEST(gpu_driver_list_names_usable_packages_sorted)
{
    make_root();
    CHECK(g_root[0]);
    char names[4][RL_GPU_DRIVER_NAME_MAX];
    size_t n = rl_gpu_driver_list(names, 4);
    char one[1][RL_GPU_DRIVER_NAME_MAX];
    size_t m = rl_gpu_driver_list(one, 1);
    remove_root();
    CHECK_EQ(n, 2);
    CHECK(!strcmp(names[0], "blob"));
    CHECK(!strcmp(names[1], "turnip"));
    CHECK_EQ(m, 2);
}

TEST(gpu_driver_setting_defaults_to_the_system_driver)
{
    rl_settings_reset();
    unsetenv("RELIFT_GPU_DRIVER");
    rl_settings_declare_runtime();
    CHECK(!strcmp(rl_setting_str(RL_GPU_DRIVER_KEY), RL_GPU_DRIVER_SYSTEM));
    CHECK_EQ(rl_settings_set(RL_GPU_DRIVER_KEY, "turnip"), RL_SETTING_OK);
    CHECK(!strcmp(rl_setting_str(RL_GPU_DRIVER_KEY), "turnip"));
    setenv("RELIFT_GPU_DRIVER", "blob", 1);
    CHECK(!strcmp(rl_setting_str(RL_GPU_DRIVER_KEY), "blob"));
}

TEST(gpu_driver_choice_is_ignored_when_the_app_offers_none)
{
    rl_settings_reset();
    rl_settings_declare_runtime();
    rl_hostlib_test_reset_driver_choice();
    CHECK(rl_hostlib_selected_driver() == NULL);
    CHECK_EQ(rl_settings_set("video.gpu_driver", "Turnip_v26.0.0_R8"), RL_SETTING_OK);
    CHECK(rl_hostlib_selected_driver() && !strcmp(rl_hostlib_selected_driver(), "Turnip_v26.0.0_R8"));
    /* an app with no driver choice runs on the system driver whatever the setting says */
    rl_hostlib_set_driver_choice(false);
    CHECK(rl_hostlib_selected_driver() == NULL);
    CHECK(rl_hostlib_selected_driver() == NULL);
    CHECK_EQ(rl_settings_set("video.gpu_driver", "system"), RL_SETTING_OK);
    CHECK(rl_hostlib_selected_driver() == NULL);
    rl_hostlib_test_reset_driver_choice();
    rl_settings_reset();
}
