/**
 * @file test_flashwatch.c
 * @brief The flash watch: one bright frame between dark ones writes its folder with the three pictures and the
 * differences; a cut to a new picture writes nothing.
 */
#include "runner.h"

#include "flashwatch.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define W 32u
#define H 18u

static void fill(uint8_t *px, uint8_t v)
{
    for (uint32_t i = 0; i < W * H; i++) {
        px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = v;
        px[i * 4 + 3] = 255;
    }
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/** @brief Removes one entry of the scratch tree (system() is not available on iOS). */
static int remove_entry(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
    (void)sb;
    (void)type;
    (void)ftw;
    return remove(path);
}

TEST(flashwatch_writes_a_one_frame_flash_and_not_a_cut)
{
    char dir[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/rl_flash_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    CHECK(mkdtemp(dir) != NULL);
    char spec[600];
    snprintf(spec, sizeof spec, "%s/out:10", dir);
    rl_flashwatch f;
    CHECK(rl_flashwatch_open(&f, spec));
    static uint8_t dark[W * H * 4], bright[W * H * 4], grey[W * H * 4];
    fill(dark, 20);
    fill(bright, 230);
    fill(grey, 128);
    CHECK_EQ(rl_flashwatch_push(&f, 1, dark, W, H, W * 4, false), 0);
    CHECK_EQ(rl_flashwatch_push(&f, 2, dark, W, H, W * 4, false), 0);
    CHECK_EQ(rl_flashwatch_push(&f, 3, bright, W, H, W * 4, false), 0);
    CHECK_EQ(rl_flashwatch_push(&f, 4, dark, W, H, W * 4, false), 3);
    char path[700];
    snprintf(path, sizeof path, "%s/out/flash-000003/flash.json", dir);
    CHECK(exists(path));
    for (unsigned n = 2; n <= 4; n++) {
        snprintf(path, sizeof path, "%s/out/flash-000003/present-%06u.png", dir, n);
        CHECK(exists(path));
    }
    CHECK_EQ(rl_flashwatch_push(&f, 5, grey, W, H, W * 4, false), 0);
    CHECK_EQ(rl_flashwatch_push(&f, 6, grey, W, H, W * 4, false), 0);
    CHECK_EQ(rl_flashwatch_push(&f, 7, grey, W, H, W * 4, false), 0);
    snprintf(path, sizeof path, "%s/out/flash-000005", dir);
    CHECK(!exists(path));
    CHECK_EQ(f.saved, 1);
    rl_flashwatch_close(&f);
    CHECK(!rl_flashwatch_on(&f));
    CHECK_EQ(nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS), 0);
}
