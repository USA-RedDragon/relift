/**
 * @file test_nvn_xlate.c
 * @brief Shader tables made on the device (nvn_xlate.c). Without the translator linked the runtime keeps the build's
 * tables. With it (make XLATE_LIB=..., run by scripts/ci/tests-gpu-xlate.sh with RELIFT_XLATE_KAT set to a shader
 * folder of programs/ and device.json, and RELIFT_XLATE_KAT_RUST to the tables relift-gpu-table made from them), the
 * first start makes the tables, byte for byte the Rust ones, a second start keeps them, a changed device.json makes
 * them again, and a translator of another stamp is left alone. A table read before the tables were made (it found
 * none) does not hide them.
 */
#include "nvn/nvn_priv.h"
#include "tc.h"

#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef RL_NVN_XLATE
#include "rlx.h"
#endif

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
    snprintf(dir, PATH_MAX, "%s/relift-xlate-XXXXXX", base && *base ? base : "/tmp");
    return mkdtemp(dir) != NULL;
}

TEST(nvn_without_the_translator_the_build_tables_stay)
{
#ifndef RL_NVN_XLATE
    char dir[PATH_MAX];
    CHECK(make_scratch(dir));
    setenv("RELIFT_NVN_SHADERS", dir, 1);
    unsetenv("RELIFT_NVN_TABLES");
    nvn_xlate_start();
    nvn_xlate_wait();
    CHECK(nvn_xlate_table_dir() == NULL);
    CHECK(!strcmp(nvn_shader_table_dir(), dir));
    unsetenv("RELIFT_NVN_SHADERS");
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
#endif
}

#ifdef RL_NVN_XLATE

/** @brief Whether files a and b hold the same bytes. */
static bool same(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    bool ok = fa && fb;
    while (ok) {
        int ca = fgetc(fa), cb = fgetc(fb);
        if (ca != cb)
            ok = false;
        if (ca == EOF || cb == EOF)
            break;
    }
    if (fa)
        fclose(fa);
    if (fb)
        fclose(fb);
    return ok;
}

static bool copy(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    bool ok = a && b;
    char buf[65536];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, a)) > 0)
        ok = fwrite(buf, 1, n, b) == n;
    if (a)
        fclose(a);
    if (b)
        fclose(b);
    return ok;
}

/** @brief Appends a mark to path, which a translation that writes the file again removes. */
static void mark(const char *path)
{
    FILE *f = fopen(path, "ab");
    if (f) {
        fputs("mark\n", f);
        fclose(f);
    }
}

/** @brief Whether path still ends with the mark. */
static bool marked(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char buf[8] = "";
    bool ok = fseek(f, -5, SEEK_END) == 0 && fread(buf, 1, 5, f) == 5 && !memcmp(buf, "mark\n", 5);
    fclose(f);
    return ok;
}

TEST(nvn_the_device_makes_its_tables_and_keeps_them_until_the_device_changes)
{
    const char *kat = getenv("RELIFT_XLATE_KAT"), *rust = getenv("RELIFT_XLATE_KAT_RUST");
    if (!kat || !rust) {
        fprintf(stderr, "RELIFT_XLATE_KAT and RELIFT_XLATE_KAT_RUST unset: skipped\n");
        return;
    }
    char dir[PATH_MAX], path[PATH_MAX + 64], from[PATH_MAX + 64], table[PATH_MAX + 64];
    CHECK(make_scratch(dir));
    snprintf(path, sizeof path, "%s/programs", dir);
    snprintf(from, sizeof from, "%s/programs", kat);
    CHECK(symlink(from, path) == 0);
    snprintf(from, sizeof from, "%s/device.json", kat);
    snprintf(path, sizeof path, "%s/device.json", dir);
    CHECK(copy(from, path));
    setenv("RELIFT_NVN_SHADERS", dir, 1);
    unsetenv("RELIFT_NVN_TABLES");
    unsetenv("RELIFT_NVN_XLATE");
    rl_nvn_set_table_stamp(rlx_stamp());
    CHECK(nvn_shader_count() == 0);

    nvn_xlate_start();
    nvn_xlate_wait();
    snprintf(path, sizeof path, "%s/tables", dir);
    CHECK(nvn_xlate_table_dir() && !strcmp(nvn_xlate_table_dir(), path));
    CHECK(!strcmp(nvn_shader_table_dir(), path));
    static const char *const files[] = { "table.rlspv", "table.rlspi", "table.stamp", "failed.txt" };
    for (int i = 0; i < 4; i++) {
        snprintf(table, sizeof table, "%s/tables/%s", dir, files[i]);
        snprintf(from, sizeof from, "%s/%s", rust, files[i]);
        CHECK(same(table, from));
    }
    snprintf(table, sizeof table, "%s/tables/table.device", dir);
    snprintf(from, sizeof from, "%s/device.json", dir);
    CHECK(same(table, from));
    char why[512] = "";
    CHECK(nvn_shader_stamp_check(why, sizeof why));
    CHECK(nvn_shader_count() > 0);

    snprintf(table, sizeof table, "%s/tables/failed.txt", dir);
    mark(table);
    nvn_xlate_start();
    nvn_xlate_wait();
    CHECK(marked(table));

    FILE *f = fopen(from, "ab");
    CHECK(f != NULL);
    fputs(" ", f);
    fclose(f);
    nvn_xlate_start();
    nvn_xlate_wait();
    snprintf(path, sizeof path, "%s/tables/table.device", dir);
    CHECK(same(path, from));
    CHECK(!marked(table));

    rl_nvn_set_table_stamp("0000000000000000");
    remove(path);
    mark(table);
    nvn_xlate_start();
    nvn_xlate_wait();
    CHECK(marked(table));
    CHECK(fopen(path, "rb") == NULL);

    rl_nvn_set_table_stamp(NULL);
    unsetenv("RELIFT_NVN_SHADERS");
    nftw(dir, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
}

#endif
