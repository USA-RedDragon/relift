/**
 * @file test_nvn_stamp.c
 * @brief The translator stamp of the shader tables (nvn_shader.c): the runtime
 * accepts tables whose table.stamp is the one the build expects, refuses any
 * other (or none) with a named report, and lets a deliberate A/B run through.
 * With gpu.math at native the run reads the native-math tables
 * (table-native.*), whose stamp ends in -native, and hands a missing one to the
 * table maker.
 */
#include "nvn/nvn_module.h"
#include "nvn/nvn_priv.h"
#include "tc.h"

#include <relift/settings.h>

#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static bool make_scratch(char dir[PATH_MAX])
{
    const char *base = getenv("TMPDIR");
    snprintf(dir, PATH_MAX, "%s/relift-stamp-XXXXXX", base && *base ? base : "/tmp");
    return mkdtemp(dir) != NULL;
}

static void write_named(const char *dir, const char *name, const char *text)
{
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!text) {
        remove(path);
        return;
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void write_stamp(const char *dir, const char *text) { write_named(dir, "table.stamp", text); }

/** @brief Switches gpu.math and has the run pick its tables again. */
static void set_math(const char *math)
{
    rl_settings_declare_runtime();
    CHECK(rl_settings_set("gpu.math", math) == RL_SETTING_OK);
    nvn_shader_tables_repick();
}

static int g_made;
static char g_made_stamp[96];

static bool fake_maker(const char *dir, const char *base, const char *math, const char *stamp)
{
    g_made++;
    snprintf(g_made_stamp, sizeof g_made_stamp, "%s", stamp ? stamp : "");
    if (strcmp(base, "table-native") || strcmp(math, "native"))
        return false;
    write_named(dir, "table-native.rlspv", "RLSPV002");
    char text[100];
    snprintf(text, sizeof text, "%s\n", stamp);
    write_named(dir, "table-native.stamp", text);
    return true;
}

TEST(nvn_native_math_reads_the_native_tables_and_their_stamp)
{
    char dir[PATH_MAX], why[512] = "";
    CHECK(make_scratch(dir));
    setenv("RELIFT_NVN_TABLES", dir, 1);
    unsetenv("RELIFT_NVN_TABLE_STAMP");
    write_stamp(dir, "0123456789abcdef\n");
    write_named(dir, "table-native.rlspv", "RLSPV002");
    write_named(dir, "table-native.stamp", "0123456789abcdef-native\n");
    rl_nvn_set_table_stamp("0123456789abcdef");
    set_math("native");
    CHECK(nvn_shader_math_native());
    CHECK(nvn_shader_stamp_check(why, sizeof why));
    write_named(dir, "table-native.stamp", "0123456789abcdef\n");
    nvn_shader_tables_repick();
    CHECK(!nvn_shader_stamp_check(why, sizeof why));
    CHECK(strstr(why, "0123456789abcdef-native") != NULL);
    set_math("exact");
    CHECK(!nvn_shader_math_native());
    CHECK(nvn_shader_stamp_check(why, sizeof why));
    rl_nvn_set_table_stamp(NULL);
    nvn_shader_tables_repick();
    unsetenv("RELIFT_NVN_TABLES");
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}

TEST(nvn_a_missing_native_table_goes_to_the_table_maker)
{
    char dir[PATH_MAX], why[512] = "";
    CHECK(make_scratch(dir));
    setenv("RELIFT_NVN_TABLES", dir, 1);
    unsetenv("RELIFT_NVN_TABLE_STAMP");
    write_stamp(dir, "0123456789abcdef\n");
    rl_nvn_set_table_stamp("0123456789abcdef");
    set_math("native");
    CHECK(!nvn_shader_stamp_check(why, sizeof why));
    CHECK(strstr(why, "no native-math table") != NULL);
    g_made = 0;
    rl_nvn_set_table_maker(fake_maker);
    nvn_shader_tables_repick();
    CHECK(nvn_shader_stamp_check(why, sizeof why));
    CHECK(g_made == 1);
    CHECK(!strcmp(g_made_stamp, "0123456789abcdef-native"));
    rl_nvn_set_table_maker(NULL);
    set_math("exact");
    rl_nvn_set_table_stamp(NULL);
    nvn_shader_tables_repick();
    unsetenv("RELIFT_NVN_TABLES");
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}

TEST(nvn_tables_of_the_expected_translator_are_accepted)
{
    char dir[PATH_MAX], why[512] = "";
    CHECK(make_scratch(dir));
    setenv("RELIFT_NVN_TABLES", dir, 1);
    unsetenv("RELIFT_NVN_TABLE_STAMP");
    write_stamp(dir, "0123456789abcdef\n");
    rl_nvn_set_table_stamp("0123456789abcdef");
    CHECK(nvn_shader_stamp_check(why, sizeof why));
    rl_nvn_set_table_stamp(NULL);
    unsetenv("RELIFT_NVN_TABLES");
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}

TEST(nvn_tables_of_another_translator_stop_with_a_named_report)
{
    char dir[PATH_MAX], why[512] = "", key[65];
    CHECK(make_scratch(dir));
    setenv("RELIFT_NVN_TABLES", dir, 1);
    unsetenv("RELIFT_NVN_TABLE_STAMP");
    write_stamp(dir, "ffffffffffffffff\n");
    rl_nvn_set_table_stamp("0123456789abcdef");
    CHECK(!nvn_shader_stamp_check(why, sizeof why));
    CHECK(strstr(why, "shader table stamp mismatch") != NULL);
    CHECK(strstr(why, "ffffffffffffffff") != NULL);
    CHECK(strstr(why, "0123456789abcdef") != NULL);
    CHECK(strstr(why, "RELIFT_NVN_TABLE_STAMP=allow") != NULL);
    memset(key, '0', 64);
    key[64] = 0;
    why[0] = 0;
    CHECK(nvn_module_get(key, (int)NVN_STAGE_COMPUTE, why, sizeof why) == NULL);
    CHECK(strstr(why, "shader table stamp mismatch") != NULL);
    rl_nvn_set_table_stamp(NULL);
    unsetenv("RELIFT_NVN_TABLES");
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}

/**
 * @brief An interface table of the old format (RLSPI001, made before relift recorded the storage image arrays each
 * program uses) is refused even with the expected stamp: the runtime would size the image arrays wrongly.
 */
TEST(nvn_tables_without_the_image_array_records_are_refused_by_name)
{
    char dir[PATH_MAX], why[512] = "", path[PATH_MAX + 16];
    CHECK(make_scratch(dir));
    setenv("RELIFT_NVN_TABLES", dir, 1);
    unsetenv("RELIFT_NVN_TABLE_STAMP");
    write_stamp(dir, "0123456789abcdef\n");
    snprintf(path, sizeof path, "%s/table.rlspi", dir);
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite("RLSPI001\0\0\0\0", 1, 12, f) == 12);
    CHECK(fclose(f) == 0);
    rl_nvn_set_table_stamp("0123456789abcdef");
    CHECK(!nvn_shader_stamp_check(why, sizeof why));
    CHECK(strstr(why, "storage image array records") != NULL);
    CHECK(strstr(why, "RLSPI002") != NULL);
    rl_nvn_set_table_stamp(NULL);
    unsetenv("RELIFT_NVN_TABLES");
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}

TEST(nvn_tables_without_a_stamp_are_refused_when_the_build_expects_one)
{
    char dir[PATH_MAX], why[512] = "";
    CHECK(make_scratch(dir));
    setenv("RELIFT_NVN_TABLES", dir, 1);
    unsetenv("RELIFT_NVN_TABLE_STAMP");
    write_stamp(dir, NULL);
    rl_nvn_set_table_stamp("0123456789abcdef");
    CHECK(!nvn_shader_stamp_check(why, sizeof why));
    CHECK(strstr(why, "no translator stamp") != NULL);
    rl_nvn_set_table_stamp(NULL);
    CHECK(nvn_shader_stamp_check(why, sizeof why));
    unsetenv("RELIFT_NVN_TABLES");
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}

TEST(nvn_a_deliberate_run_may_use_tables_of_another_translator)
{
    char dir[PATH_MAX], why[512] = "";
    CHECK(make_scratch(dir));
    setenv("RELIFT_NVN_TABLES", dir, 1);
    setenv("RELIFT_NVN_TABLE_STAMP", "allow", 1);
    write_stamp(dir, "ffffffffffffffff\n");
    rl_nvn_set_table_stamp("0123456789abcdef");
    CHECK(nvn_shader_stamp_check(why, sizeof why));
    rl_nvn_set_table_stamp(NULL);
    unsetenv("RELIFT_NVN_TABLE_STAMP");
    unsetenv("RELIFT_NVN_TABLES");
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}
