/**
 * @file test_nvn_stamp.c
 * @brief The translator stamp of the shader tables (nvn_shader.c): the runtime accepts tables whose table.stamp is the
 * one the build expects, refuses any other (or none) with a named report, and lets a deliberate A/B run through.
 */
#include "nvn/nvn_module.h"
#include "nvn/nvn_priv.h"
#include "tc.h"

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

static void write_stamp(const char *dir, const char *text)
{
    char path[PATH_MAX + 16];
    snprintf(path, sizeof path, "%s/table.stamp", dir);
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
