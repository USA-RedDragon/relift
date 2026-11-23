/**
 * @file test_nvn_module.c
 * @brief Shader modules made from a build's tables (nvn_module.c): what a module learns from its translation's
 * interface records.
 */
#include "nvn/nvn_module.h"
#include "nvn/nvn_priv.h"
#include "nvn/nvn_vk.h"
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
    memcpy(spi, "RLSPI002", 8);
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

/** A vertex program that reads a vec4 from a storage buffer at set 1 binding 64 (the translation's slot array). */
static const uint32_t slot_vertex[] = {
    0x07230203u, 0x00010300u, 0x00070000u, 0x00000012u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0003000eu,
    0x00000000u, 0x00000001u, 0x0006000fu, 0x00000000u, 0x00000001u, 0x6e69616du, 0x00000000u, 0x00000002u,
    0x00040047u, 0x00000002u, 0x0000000bu, 0x00000000u, 0x00040047u, 0x00000003u, 0x00000006u, 0x00000010u,
    0x00030047u, 0x00000004u, 0x00000002u, 0x00050048u, 0x00000004u, 0x00000000u, 0x00000023u, 0x00000000u,
    0x00040047u, 0x00000005u, 0x00000022u, 0x00000001u, 0x00040047u, 0x00000005u, 0x00000021u, 0x00000040u,
    0x00020013u, 0x00000006u, 0x00030021u, 0x00000007u, 0x00000006u, 0x00030016u, 0x00000008u, 0x00000020u,
    0x00040017u, 0x00000009u, 0x00000008u, 0x00000004u, 0x00040015u, 0x0000000au, 0x00000020u, 0x00000000u,
    0x0003001du, 0x00000003u, 0x00000009u, 0x0003001eu, 0x00000004u, 0x00000003u, 0x00040020u, 0x0000000bu,
    0x0000000cu, 0x00000004u, 0x0004003bu, 0x0000000bu, 0x00000005u, 0x0000000cu, 0x00040020u, 0x0000000cu,
    0x0000000cu, 0x00000009u, 0x00040020u, 0x0000000du, 0x00000003u, 0x00000009u, 0x0004003bu, 0x0000000du,
    0x00000002u, 0x00000003u, 0x0004002bu, 0x0000000au, 0x0000000eu, 0x00000000u, 0x00050036u, 0x00000006u,
    0x00000001u, 0x00000000u, 0x00000007u, 0x000200f8u, 0x0000000fu, 0x00060041u, 0x0000000cu, 0x00000010u,
    0x00000005u, 0x0000000eu, 0x0000000eu, 0x0004003du, 0x00000009u, 0x00000011u, 0x00000010u, 0x0003003eu,
    0x00000002u, 0x00000011u, 0x000100fdu, 0x00010038u,
};

/** A geometry program that writes Layer 1 and emits one point (written with spirv-as). */
static const uint32_t layer_geometry[] = {
    0x07230203u, 0x00010000u, 0x00070000u, 0x00000009u, 0x00000000u, 0x00020011u, 0x00000002u, 0x0003000eu,
    0x00000000u, 0x00000001u, 0x0006000fu, 0x00000003u, 0x00000001u, 0x6e69616du, 0x00000000u, 0x00000002u,
    0x00030010u, 0x00000001u, 0x00000016u, 0x00040010u, 0x00000001u, 0x00000000u, 0x00000001u, 0x00030010u,
    0x00000001u, 0x0000001bu, 0x00040010u, 0x00000001u, 0x0000001au, 0x00000001u, 0x00040047u, 0x00000002u,
    0x0000000bu, 0x00000009u, 0x00020013u, 0x00000003u, 0x00030021u, 0x00000004u, 0x00000003u, 0x00040015u,
    0x00000005u, 0x00000020u, 0x00000001u, 0x00040020u, 0x00000006u, 0x00000003u, 0x00000005u, 0x0004003bu,
    0x00000006u, 0x00000002u, 0x00000003u, 0x0004002bu, 0x00000005u, 0x00000007u, 0x00000001u, 0x00050036u,
    0x00000003u, 0x00000001u, 0x00000000u, 0x00000004u, 0x000200f8u, 0x00000008u, 0x0003003eu, 0x00000002u,
    0x00000007u, 0x000100dau, 0x000100fdu, 0x00010038u,
};

/**
 * Writes dir/table.rlspv holding the program words (bytes long, 256 to 65791) under the key key0, key0 + 1, ...,
 * with an empty dir/table.rlspi entry.
 */
static bool write_one_table(const char *dir, const uint32_t *words, size_t bytes, uint8_t key0)
{
    size_t frame_n = 4 + 3 + 3 + bytes;
    uint8_t *frame = calloc(1, frame_n), *spv = calloc(1, 12 + 48 + frame_n);
    uint8_t spi[12 + 40];
    if (!frame || !spv) {
        free(frame);
        free(spv);
        return false;
    }
    memcpy(frame, (const uint8_t[]){ 0x28, 0xb5, 0x2f, 0xfd, 0x60 }, 5);
    put_le(frame + 5, bytes - 256, 2);
    put_le(frame + 7, (bytes << 3) | 1, 3);
    memcpy(frame + 10, words, bytes);
    memset(spi, 0, sizeof spi);
    memcpy(spv, "RLSPV002", 8);
    put_le(spv + 8, 1, 4);
    memcpy(spi, "RLSPI002", 8);
    put_le(spi + 8, 1, 4);
    for (int i = 0; i < 32; i++)
        spv[12 + i] = spi[12 + i] = (uint8_t)(key0 + i);
    put_le(spv + 12 + 32, 12 + 48, 8);
    put_le(spv + 12 + 40, frame_n, 4);
    put_le(spv + 12 + 44, bytes / 4, 4);
    memcpy(spv + 12 + 48, frame, frame_n);
    free(frame);
    bool ok = true;
    const struct { const char *name; const uint8_t *bytes; size_t n; } files[2] = {
        { "table.rlspv", spv, 12 + 48 + frame_n }, { "table.rlspi", spi, sizeof spi } };
    for (int i = 0; i < 2; i++) {
        char path[PATH_MAX + 16];
        snprintf(path, sizeof path, "%s/%s", dir, files[i].name);
        FILE *f = fopen(path, "wb");
        if (!f) {
            ok = false;
            continue;
        }
        ok = fwrite(files[i].bytes, 1, files[i].n, f) == files[i].n && ok;
        ok = fclose(f) == 0 && ok;
    }
    free(spv);
    return ok;
}

/** Whether words (n of them) put a variable at set 1 binding b. */
static bool binds_at(const uint32_t *w, size_t n, uint32_t b)
{
    for (size_t i = 5; i + 3 < n && (w[i] >> 16); i += w[i] >> 16)
        if ((w[i] & 0xffff) == 71 && w[i + 2] == 33 && w[i + 3] == b)
            return true;
    return false;
}

TEST(nvn_storage_slot_arrays_of_every_stage_are_their_own)
{
    for (uint32_t s = 0; s < NVN_STAGES; s++) {
        CHECK(nvn_slot_binding(s) >= DRIVER_BINDINGS * NVN_STAGES);
        CHECK(nvn_slot_binding(s) < NVN_COMPUTE_BASE_TABLE_BINDING);
        for (uint32_t t = 0; t < s; t++)
            CHECK(nvn_slot_binding(s) != nvn_slot_binding(t));
    }
    CHECK(binds_at(slot_vertex, sizeof slot_vertex / 4, NVN_SLOT_BINDING_BASE));
    char dir[PATH_MAX];
    CHECK(make_scratch(dir));
    CHECK(write_one_table(dir, slot_vertex, sizeof slot_vertex, 0xd0));
    setenv("RELIFT_NVN_SHADERS", dir, 1);
    if (!nvn_vk_ready()) {
        CHECK(!getenv("RELIFT_VK_REQUIRE"));
        printf("  skipped: no host Vulkan device\n");
        nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
        return;
    }
    char key[65], why[256] = "";
    for (int i = 0; i < 32; i++)
        snprintf(key + 2 * i, 3, "%02x", 0xd0 + i);
    nvn_module *m = nvn_module_get(key, 1, why, sizeof why);
    if (!m)
        printf("  %s\n", why);
    CHECK(m != NULL);
    if (m) {
        CHECK(m->slot_array);
        CHECK_EQ(m->storage_slots, 0u);
        CHECK_EQ(m->driver, 0u);
        CHECK(!m->other_sets);
    }
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}

TEST(nvn_a_compute_base_binding_becomes_a_slot_of_the_compute_layout)
{
    uint32_t words[sizeof slot_vertex / 4];
    memcpy(words, slot_vertex, sizeof words);
    bool moved = false;
    for (size_t i = 5; i + 3 < sizeof words / 4; i += words[i] >> 16)
        if ((words[i] & 0xffff) == 71 && words[i + 2] == 33 && words[i + 3] == NVN_SLOT_BINDING_BASE) {
            words[i + 3] = NVN_COMPUTE_BASE_BINDING + 0x1b8 / 8;
            moved = true;
        }
    CHECK(moved);
    char dir[PATH_MAX];
    CHECK(make_scratch(dir));
    CHECK(write_one_table(dir, words, sizeof words, 0xe0));
    setenv("RELIFT_NVN_SHADERS", dir, 1);
    if (!nvn_vk_ready()) {
        CHECK(!getenv("RELIFT_VK_REQUIRE"));
        printf("  skipped: no host Vulkan device\n");
        nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
        return;
    }
    char key[65], why[256] = "";
    for (int i = 0; i < 32; i++)
        snprintf(key + 2 * i, 3, "%02x", 0xe0 + i);
    nvn_module *m = nvn_module_get(key, (int)NVN_STAGE_COMPUTE, why, sizeof why);
    if (!m)
        printf("  %s\n", why);
    CHECK(m != NULL);
    if (m) {
        CHECK_EQ(m->ncbase, 1u);
        CHECK_EQ(m->cbase[0], 0x1b8u);
        CHECK_EQ(m->storage_slots, 0u);
        CHECK(!m->other_sets);
        CHECK_EQ(m->driver, 0u);
    }
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}

TEST(nvn_a_geometry_program_that_writes_layer_draws_layered_and_feeds_the_fragment_program)
{
    char dir[PATH_MAX];
    CHECK(make_scratch(dir));
    CHECK(write_one_table(dir, layer_geometry, sizeof layer_geometry, 0xd0));
    setenv("RELIFT_NVN_SHADERS", dir, 1);
    if (!nvn_vk_ready()) {
        CHECK(!getenv("RELIFT_VK_REQUIRE"));
        printf("  skipped: no host Vulkan device\n");
        nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
        return;
    }
    char key[65], why[256] = "";
    for (int i = 0; i < 32; i++)
        snprintf(key + 2 * i, 3, "%02x", 0xd0 + i);
    nvn_module *gs = nvn_module_get(key, 2, why, sizeof why);
    if (!gs)
        printf("  %s\n", why);
    CHECK(gs != NULL);
    if (gs) {
        CHECK(gs->writes_layer);
        CHECK(!gs->writes_viewport);
        nvn_module vs = { .stage = 0 }, te = { .stage = 4 };
        nvn_module *mods[5] = { &vs, NULL, NULL, NULL, NULL };
        CHECK(nvn_module_last_stage(mods) == &vs);
        mods[4] = &te;
        CHECK(nvn_module_last_stage(mods) == &te);
        mods[2] = gs;
        CHECK(nvn_module_last_stage(mods) == gs);
    }
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}

TEST(nvn_a_geometry_program_runs_after_triangles_only)
{
    CHECK(nvn_draw_geometry_input_ok(NVN_DRAW_TRIANGLES));
    CHECK(nvn_draw_geometry_input_ok(NVN_DRAW_TRIANGLE_STRIP));
    CHECK(nvn_draw_geometry_input_ok(NVN_DRAW_TRIANGLE_FAN));
    for (uint32_t mode = 0; mode < 4; mode++)
        CHECK(!nvn_draw_geometry_input_ok(mode));
    for (uint32_t mode = 10; mode <= NVN_DRAW_PATCHES; mode++)
        CHECK(!nvn_draw_geometry_input_ok(mode));
}
