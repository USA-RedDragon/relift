/**
 * @file test_nvn_module.c
 * @brief Shader modules made from a build's tables (nvn_module.c): what a module learns from its translation's
 * interface records.
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

static void put_le(uint8_t *p, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

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
    snprintf(dir, PATH_MAX, "%s/relift-rlspi-XXXXXX", base && *base ? base : "/tmp");
    return mkdtemp(dir) != NULL;
}

/** An empty compute program (SPIR-V 1.0, one invocation a group). */
static const uint32_t empty_compute[] = {
    0x07230203u, 0x00010000u, 0u, 5u, 0u, 0x00020011u, 1u, 0x0003000eu, 0u, 1u,
    0x0005000fu, 5u, 1u, 0x6e69616du, 0u, 0x00060010u, 1u, 17u, 1u, 1u, 1u,
    0x00020013u, 2u, 0x00030021u, 3u, 2u, 0x00050036u, 2u, 1u, 0u, 3u, 0x000200f8u, 4u, 0x000100fdu, 0x00010038u,
};

/**
 * Writes dir/table.rlspv holding empty_compute under the keys 0xa0.. and 0xb0.., and dir/table.rlspi giving the first
 * a register handle record and the second a bound handle record (c[3] byte 0x10).
 */
static bool write_handle_tables(const char *dir)
{
    enum { WORDS = sizeof empty_compute / 4, FRAME = 4 + 2 + 3 + sizeof empty_compute };
    uint8_t frame[FRAME] = { 0x28, 0xb5, 0x2f, 0xfd, 0x20, sizeof empty_compute };
    put_le(frame + 6, (sizeof empty_compute << 3) | 1, 3);
    memcpy(frame + 9, empty_compute, sizeof empty_compute);
    uint8_t spv[12 + 2 * 48 + 2 * FRAME], spi[12 + 2 * 40 + 2 * 16];
    memset(spv, 0, sizeof spv);
    memset(spi, 0, sizeof spi);
    memcpy(spv, "RLSPV002", 8);
    put_le(spv + 8, 2, 4);
    memcpy(spi, "RLSPI001", 8);
    put_le(spi + 8, 2, 4);
    for (int e = 0; e < 2; e++) {
        uint8_t *v = spv + 12 + 48 * e, *s = spi + 12 + 40 * e, *rec = spi + 12 + 80 + 16 * e;
        for (int i = 0; i < 32; i++)
            v[i] = s[i] = (uint8_t)(0xa0 + 0x10 * e + i);
        put_le(v + 32, 12 + 2 * 48 + (uint64_t)e * FRAME, 8);
        put_le(v + 40, FRAME, 4);
        put_le(v + 44, WORDS, 4);
        memcpy(spv + 12 + 2 * 48 + e * FRAME, frame, FRAME);
        put_le(s + 32, (uint64_t)e, 4);
        put_le(s + 36, 1, 4);
        put_le(rec, e ? NVN_IFACE_TEXTURE : NVN_IFACE_REGISTER_HANDLES, 4);
        put_le(rec + 4, e ? 3 : 0, 4);
        put_le(rec + 8, e ? 0x10 : 0, 4);
    }
    bool ok = true;
    const struct { const char *name; const uint8_t *bytes; size_t n; } files[2] = {
        { "table.rlspv", spv, sizeof spv }, { "table.rlspi", spi, sizeof spi } };
    for (int i = 0; i < 2; i++) {
        char path[PATH_MAX + 16];
        snprintf(path, sizeof path, "%s/%s", dir, files[i].name);
        FILE *f = fopen(path, "wb");
        if (!f)
            return false;
        ok = fwrite(files[i].bytes, 1, files[i].n, f) == files[i].n && ok;
        ok = fclose(f) == 0 && ok;
    }
    return ok;
}

TEST(nvn_programs_that_read_handles_from_registers_say_so)
{
    char dir[PATH_MAX];
    CHECK(make_scratch(dir));
    CHECK(write_handle_tables(dir));
    setenv("RELIFT_NVN_SHADERS", dir, 1);
    if (!nvn_vk_ready()) {
        CHECK(!getenv("RELIFT_VK_REQUIRE"));
        printf("  skipped: no host Vulkan device\n");
        nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
        return;
    }
    char key[65], why[256] = "";
    for (int i = 0; i < 32; i++)
        snprintf(key + 2 * i, 3, "%02x", 0xa0 + i);
    nvn_module *reg = nvn_module_get(key, (int)NVN_STAGE_COMPUTE, why, sizeof why);
    if (!reg)
        printf("  %s\n", why);
    CHECK(reg != NULL);
    for (int i = 0; i < 32; i++)
        snprintf(key + 2 * i, 3, "%02x", 0xb0 + i);
    nvn_module *bound = nvn_module_get(key, (int)NVN_STAGE_COMPUTE, why, sizeof why);
    CHECK(bound != NULL);
    if (!reg || !bound) {
        printf("  %s\n", why);
        nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
        return;
    }
    CHECK(reg->register_handles);
    CHECK(!bound->register_handles);
    CHECK_EQ(bound->nrecs, 1u);
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}
