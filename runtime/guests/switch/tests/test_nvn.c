/**
 * @file test_nvn.c
 * @brief relift's NVN2 procs called as lifted code calls them: texture
 * layout, device queries, syncs and command recording.
 */
#include "nvn/nvn_priv.h"
#include "tc.h"
#include "vi.h"

#include <relift/ports.h>

#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define RET 0x7000000ull

static uint64_t call(rl_ctx *c, rl_fn_t *fn, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3)
{
    c->x[0] = x0;
    c->x[1] = x1;
    c->x[2] = x2;
    c->x[3] = x3;
    c->x[30] = RET;
    return fn(c, t_mem());
}

TEST(nvn_block_linear_layout)
{
    nvn_tex_desc d = { .target = 1, .format = 0x38, .width = 1920, .height = 1080, .depth = 1, .levels = 1, .samples = 0 };
    CHECK(nvn_tex_layout(&d));
    CHECK_EQ(d.block_h, 16);
    CHECK_EQ(d.size, 7680ull * 1152);
    CHECK_EQ(d.align, 512);
    nvn_tex_desc m = { .target = 1, .format = 0x25, .width = 256, .height = 256, .levels = 3 };
    CHECK(nvn_tex_layout(&m));
    CHECK_EQ(m.level_offset[1], 1024ull * 256);
    CHECK_EQ(m.size, 1024ull * 256 + 512ull * 128 + 256ull * 64);
    nvn_tex_desc a = { .target = 4, .format = 0x25, .width = 16, .height = 16, .depth = 3, .levels = 1 };
    CHECK(nvn_tex_layout(&a));
    CHECK_EQ(a.block_h, 2);
    CHECK_EQ(a.layer_stride, 1024);
    CHECK_EQ(a.size, 3072);
    nvn_tex_desc lut = { .target = 2, .format = 0x25, .width = 32, .height = 32, .depth = 32, .levels = 1 };
    CHECK(nvn_tex_layout(&lut));
    CHECK_EQ(lut.block_h, 1);
    CHECK_EQ(lut.block_d, 16);
    CHECK_EQ(lut.size, 128ull * 32 * 32);
    nvn_tex_desc h20 = { .target = 1, .format = 0x25, .width = 16, .height = 20, .levels = 1 };
    CHECK(nvn_tex_layout(&h20));
    CHECK_EQ(h20.block_h, 2);
    CHECK_EQ(h20.size, 64ull * 32);
    nvn_tex_desc h10 = { .target = 1, .format = 0x25, .width = 16, .height = 10, .levels = 1 };
    CHECK(nvn_tex_layout(&h10));
    CHECK_EQ(h10.block_h, 1);
    nvn_tex_desc bc = { .target = 1, .format = 0x41, .width = 64, .height = 64, .levels = 1 };
    CHECK(nvn_tex_layout(&bc));
    CHECK_EQ(bc.size, 128ull * 16);
    nvn_tex_desc lin = { .target = 1, .format = 0x25, .width = 100, .height = 10, .levels = 1, .flags = 1u << 4 };
    CHECK(nvn_tex_layout(&lin));
    CHECK_EQ(lin.align, 32);
    CHECK_EQ(lin.size, 416ull * 10);
    nvn_tex_desc bad = { .target = 1, .format = 0x7fff, .width = 1, .height = 1 };
    CHECK(!nvn_tex_layout(&bad));
}

TEST(nvn_device_queries_and_syncs)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetInteger, heap, 0, heap + 0x100, 0), RET);
    CHECK_EQ(rl_ld32(t_mem(), heap + 0x100), 231);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetInteger, heap, 0x3a, heap + 0x100, 0), RET);
    CHECK_EQ(rl_ld32(t_mem(), heap + 0x100), 65536);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetTextureHandle, heap, 5, 3, 0), RET);
    CHECK_EQ(c->x[0], 5 | 3ull << 20);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetImageHandle, heap, 7, 0, 0), RET);
    CHECK_EQ(c->x[0], 7);
    uint64_t sync = heap + 0x200;
    CHECK_EQ(call(c, nvnh_nvnSyncInitialize, sync, heap, 0, 0), RET);
    CHECK_EQ(c->x[0], 1);
    CHECK_EQ(call(c, nvnh_nvnSyncWait, sync, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 2);
    nvn_sync_signal(sync);
    CHECK_EQ(call(c, nvnh_nvnSyncWait, sync, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
    CHECK_EQ(call(c, nvnh_nvnSyncFinalize, sync, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnSyncWait, sync, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 3);
}

TEST(nvn_pools_report_their_descriptor_count)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t tex = heap + 0x300, smp = heap + 0x380;
    CHECK_EQ(call(c, nvnh_nvnTexturePoolInitialize, tex, heap, 0x1000, 0x8000), RET);
    CHECK_EQ(c->x[0], 1);
    CHECK_EQ(call(c, nvnh_nvnSamplerPoolInitialize, smp, heap, 0x2000, 2048), RET);
    CHECK_EQ(call(c, nvnh_nvnTexturePoolGetSize, tex, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0x8000);
    CHECK_EQ(call(c, nvnh_nvnSamplerPoolGetSize, smp, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 2048);
    CHECK_EQ(call(c, nvnh_nvnTexturePoolFinalize, tex, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnSamplerPoolFinalize, smp, 0, 0, 0), RET);
}

TEST(nvn_dlss_state_procs_stay_the_drivers_and_work_procs_do_not)
{
    CHECK(nvn_driver_index("nvnDlssNetworkBuilderSetDefaults") >= 0);
    CHECK(nvn_driver_index("nvnDlssParametersSetInputColor") >= 0);
    CHECK(nvn_driver_index("nvnDlssParametersGetPreExposure") >= 0);
    CHECK_EQ(nvn_driver_index("nvnCommandBufferEvaluateDlss"), -1);
    CHECK_EQ(nvn_driver_index("nvnDlssNetworkInitialize"), -1);
    CHECK_EQ(nvn_driver_index("nvnDlssNetworkBuilderGetStorageSize"), -1);
    CHECK_EQ(nvn_driver_index("nvnDlssNetworkBuilderGetOptimalSettings"), -1);
}

TEST(nvn_command_buffer_records_and_submits)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t cb = heap + 0x400, queue = heap + 0x800, handles = heap + 0x900;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferInitialize, cb, heap, 0, 0), RET);
    CHECK_EQ(c->x[0], 1);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferAddCommandMemory, cb, heap, 0, 0x4000), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetCommandMemoryFree, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0x4000);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET);
    c->v[0].d[0] = 0x3ff0000000000000ull;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferClearColor, cb, 0, heap, 0xf), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetStencilRef, cb, 3, 0x80, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferEndRecording, cb, 0, 0, 0), RET);
    uint64_t h = c->x[0];
    const nvn_cmdset *s = nvn_cmdset_of(h);
    CHECK(s != NULL);
    CHECK_EQ(s->n, 2);
    CHECK_EQ(s->cmds[0].op, NVN_CMD_CLEAR_COLOR);
    CHECK_EQ(s->cmds[0].x[3], 0xf);
    CHECK_EQ(s->cmds[0].d[0], 0x3ff0000000000000ull);
    CHECK_EQ(s->cmds[1].op, NVN_CMD_SET_STENCIL_REF);
    CHECK(nvn_cmdset_of(h + 0x100000) == NULL);
    CHECK(nvn_cmdset_of(5) == NULL);
    rl_st64(t_mem(), handles, h);
    CHECK_EQ(call(c, nvnh_nvnQueueSubmitCommands, queue, 1, handles, 0), RL_PENDING);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferFinalize, cb, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RL_PENDING);
}

TEST(nvn_texels_land_in_gobs)
{
    nvn_tex_desc d = { .target = 1, .format = 0x25, .width = 64, .height = 64, .levels = 1 };
    CHECK(nvn_tex_layout(&d));
    static uint8_t storage[64 * 64 * 4], in[64 * 64 * 4], out[64 * 64 * 4];
    for (unsigned i = 0; i < sizeof in; i++)
        in[i] = (uint8_t)(i * 7 + i / 251);
    nvn_region all = { 0, 0, 0, 64, 64, 1 };
    CHECK(nvn_tex_copy(&d, storage, 0, 0, &all, in, true));
    CHECK_EQ(storage[16], in[256 * 1]);
    CHECK_EQ(storage[48], in[256 * 3]);
    CHECK_EQ(storage[64], in[16]);
    CHECK_EQ(storage[64 + 16], in[256 * 1 + 16]);
    CHECK_EQ(storage[128], in[256 * 4]);
    CHECK_EQ(storage[192 + 16 * 3 + 5], in[256 * 7 + 16 + 5]);
    CHECK_EQ(storage[256], in[32]);
    CHECK_EQ(storage[256 + 64 + 128], in[256 * 4 + 48]);
    CHECK_EQ(storage[512 * 8], in[256 * 64 / 4 * 0 + 64 * 4 * 0 + 64]);
    CHECK(nvn_tex_copy(&d, storage, 0, 0, &all, out, false));
    CHECK(memcmp(in, out, sizeof in) == 0);
    nvn_region part = { 8, 4, 0, 16, 8, 1 };
    uint8_t small[16 * 8 * 4];
    CHECK(nvn_tex_copy(&d, storage, 0, 0, &part, small, false));
    CHECK_EQ(small[0], in[(4 * 64 + 8) * 4]);
    CHECK_EQ(small[16 * 4 * 7 + 4 * 15 + 3], in[(11 * 64 + 23) * 4 + 3]);
    nvn_region outside = { 60, 0, 0, 8, 1, 1 };
    CHECK(!nvn_tex_copy(&d, storage, 0, 0, &outside, small, false));
}

/** @brief Where the driver's viewport puts clip-space y, as a memory row: rows counted from the bottom in lower-left mode. */
static float driver_row(uint32_t origin, float rows, const float v[4], float ndc_y)
{
    float scale = origin == NVN_ORIGIN_UPPER_LEFT ? -v[3] / 2 : v[3] / 2;
    float window = ndc_y * scale + v[1] + v[3] / 2;
    return origin == NVN_ORIGIN_UPPER_LEFT ? window : rows - window;
}

/** @brief Where a host viewport (x, y, width, height) puts clip-space y, as a framebuffer row (Vulkan's rule). */
static float host_row(const float vp[4], float ndc_y) { return ndc_y * vp[3] / 2 + vp[1] + vp[3] / 2; }

TEST(nvn_viewport_matches_driver_rows)
{
    static const float views[][4] = { { 0, 0, 1920, 1080 }, { 100, 40, 300, 200 }, { 0, 600, 800, 480 } };
    for (uint32_t origin = 0; origin < 2; origin++)
        for (unsigned i = 0; i < 3; i++)
            for (float y = -1; y <= 1; y += 0.5f) {
                float vp[4];
                nvn_window_viewport(origin, 1080, views[i], vp);
                CHECK_EQ((int64_t)(host_row(vp, y) * 4), (int64_t)(driver_row(origin, 1080, views[i], y) * 4));
            }
    float full[4];
    nvn_window_viewport(NVN_ORIGIN_UPPER_LEFT, 1080, views[0], full);
    CHECK_EQ((int64_t)host_row(full, 1), 0);
    CHECK_EQ((int64_t)host_row(full, -1), 1080);
    CHECK_EQ(nvn_window_top(NVN_ORIGIN_UPPER_LEFT, 1080, 40, 200), 40);
    CHECK_EQ(nvn_window_top(NVN_ORIGIN_LOWER_LEFT, 1080, 40, 200), 840);
}

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

/** One module of three words, stored as a zstd frame with one raw block. */
static const uint32_t table_words[3] = { 0x07230203u, 0x00010600u, 0xdeadbeefu };

static void table_key(char key[65])
{
    for (int i = 0; i < 32; i++)
        snprintf(key + 2 * i, 3, "%02x", 0xa0 + i);
}

/** Writes dir/table.rlspv with one translation, keyed by table_key. */
static bool write_table(const char *dir)
{
    uint8_t frame[4 + 2 + 3 + sizeof table_words] = { 0x28, 0xb5, 0x2f, 0xfd, 0x20, sizeof table_words };
    put_le(frame + 6, (sizeof table_words << 3) | 1, 3);
    memcpy(frame + 9, table_words, sizeof table_words);
    uint8_t table[12 + 48 + sizeof frame + 4];
    memset(table, 0, sizeof table);
    memcpy(table, "RLSPV002", 8);
    put_le(table + 8, 1, 4);
    for (int i = 0; i < 32; i++)
        table[12 + i] = (uint8_t)(0xa0 + i);
    put_le(table + 44, 60, 8);
    put_le(table + 52, sizeof frame, 4);
    put_le(table + 56, 3, 4);
    memcpy(table + 60, frame, sizeof frame);
    char path[PATH_MAX + 16];
    snprintf(path, sizeof path, "%s/table.rlspv", dir);
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(table, 1, sizeof table, f) == sizeof table;
    return fclose(f) == 0 && ok;
}

static bool make_scratch(char dir[PATH_MAX])
{
    const char *base = getenv("TMPDIR");
    snprintf(dir, PATH_MAX, "%s/relift-rlspv-XXXXXX", base && *base ? base : "/tmp");
    return mkdtemp(dir) != NULL;
}

TEST(nvn_reads_compressed_translation_table)
{
    char dir[PATH_MAX];
    CHECK(make_scratch(dir));
    CHECK(write_table(dir));
    setenv("RELIFT_NVN_SHADERS", dir, 1);
    char key[65];
    table_key(key);
    size_t n = 0;
    uint32_t *got = nvn_shader_spirv(key, &n);
    CHECK(got != NULL);
    CHECK_EQ(n, 3);
    CHECK(got && memcmp(got, table_words, sizeof table_words) == 0);
    free(got);
    key[0] = 'b';
    CHECK(nvn_shader_spirv(key, &n) == NULL);
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}

/**
 * The child of nvn_shader_tables_follow_the_executable: it runs from a copy of the test program and says whether
 * the table beside that copy is found (RL_SHADER_EXPECT=found) or not (missing). Without the variable it is a test
 * that passes.
 */
TEST(nvn_shader_tables_child)
{
    const char *want = getenv("RL_SHADER_EXPECT");
    if (!want)
        return;
    CHECK(!getenv("RELIFT_NVN_SHADERS") && !getenv("RELIFT_NVN_TABLES"));
    char key[65];
    table_key(key);
    size_t n = 0;
    uint32_t *got = nvn_shader_spirv(key, &n);
    if (!strcmp(want, "found")) {
        CHECK(got != NULL);
        CHECK_EQ(n, 3);
        CHECK(got && memcmp(got, table_words, sizeof table_words) == 0);
    } else {
        CHECK(got == NULL);
    }
    free(got);
}

static bool copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    bool ok = in && out;
    char buf[1 << 16];
    size_t got;
    while (ok && (got = fread(buf, 1, sizeof buf, in)) > 0)
        ok = fwrite(buf, 1, got, out) == got;
    if (in)
        fclose(in);
    if (out)
        ok = fclose(out) == 0 && ok;
    return ok && chmod(to, 0755) == 0;
}

/** Runs the copy of the test program at exe from the folder cwd and tells whether its child test passed. */
static bool run_copy(const char *exe, const char *cwd, const char *expect)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        unsetenv("RELIFT_NVN_SHADERS");
        unsetenv("RELIFT_NVN_TABLES");
        setenv("RELIFT_TEST_ONE", "nvn_shader_tables_child", 1);
        setenv("RL_SHADER_EXPECT", expect, 1);
        if (chdir(cwd) != 0)
            _exit(125);
        char *argv[] = { (char *)exe, NULL };
        execv(exe, argv);
        _exit(126);
    }
    int st = 0;
    return waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

TEST(nvn_shader_tables_follow_the_executable)
{
    char root[PATH_MAX], built[PATH_MAX + 32], moved[PATH_MAX + 32], elsewhere[PATH_MAX + 32], from[PATH_MAX], exe[PATH_MAX + 64];
    char shaders[PATH_MAX + 64], bare[PATH_MAX + 32];
    CHECK(make_scratch(root));
    CHECK(rl_exe_path(from, sizeof from));
    snprintf(built, sizeof built, "%s/built", root);
    snprintf(moved, sizeof moved, "%s/moved", root);
    snprintf(elsewhere, sizeof elsewhere, "%s/elsewhere", root);
    snprintf(bare, sizeof bare, "%s/bare", root);
    snprintf(shaders, sizeof shaders, "%s/shaders", built);
    CHECK(mkdir(built, 0755) == 0 && mkdir(shaders, 0755) == 0 && mkdir(elsewhere, 0755) == 0 && mkdir(bare, 0755) == 0);
    CHECK(write_table(shaders));
    snprintf(exe, sizeof exe, "%s/game", built);
    CHECK(copy_file(from, exe));
    CHECK(run_copy(exe, elsewhere, "found"));
    CHECK(rename(built, moved) == 0);
    snprintf(exe, sizeof exe, "%s/game", moved);
    {
        struct stat st;
        CHECK(stat(built, &st) != 0);
    }
    CHECK(run_copy(exe, elsewhere, "found"));
    snprintf(exe, sizeof exe, "%s/game", bare);
    CHECK(copy_file(from, exe));
    CHECK(run_copy(exe, moved, "missing"));
    nftw(root, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
}

TEST(nvn_presents_step_the_input_script)
{
    uint64_t f0 = vi_guest_frames();
    CHECK(rl_ports_set_script("1:p1.a"));
    nvn_frame_presented();
    nvn_frame_presented();
    CHECK_EQ(vi_guest_frames(), f0 + 2);
    CHECK(rl_ports_set_script(""));
}

TEST(nvn_storage_class_needs_the_drivers_layout)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x10000);
    rl_nvn_set_layout(NULL);
    CHECK_EQ(call(c, nvnh_nvnTextureBuilderGetStorageClass, heap, 0, 0, 0), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
}

TEST(nvn_device_integers_come_from_the_drivers_table)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    static const int32_t ints[] = { 231, 40, 0, 7 };
    static const uint8_t device[] = { 0, 0, 1, 0 };
    static const struct rl_nvn_layout l = { .nints = 4, .ints = ints, .int_device = device };
    rl_nvn_set_layout(&l);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetInteger, heap, 1, heap + 0x100, 0), RET);
    CHECK_EQ(rl_ld32(t_mem(), heap + 0x100), 40);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetInteger, heap, 2, heap + 0x100, 0), RET);
    CHECK_EQ(rl_ld32(t_mem(), heap + 0x100), 14);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetInteger, heap, 4, heap + 0x100, 0), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    rl_nvn_set_layout(NULL);
}

static void map_req(uint64_t r, uint64_t phys, uint64_t poff, uint64_t voff, uint64_t size, uint32_t cls)
{
    rl_st64(t_mem(), r, phys);
    rl_st64(t_mem(), r + 8, poff);
    rl_st64(t_mem(), r + 16, voff);
    rl_st64(t_mem(), r + 24, size);
    rl_st32(t_mem(), r + 32, cls);
}

TEST(nvn_virtual_pools_have_their_own_addresses_over_the_mapped_pages)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t phys = heap, virt = heap + 0x100, req = heap + 0x400, storage = heap + 0x100000;
    CHECK(nvn_pool_make(phys, storage, 0x80000, 0x18c));
    CHECK(nvn_pool_make(virt, 0, 0x40000, 0x2a1));
    map_req(req, phys, 0x10000, 0, 0x10000, 0x100);
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolMapVirtual, phys, 1, req, 0), RET);
    CHECK_EQ(c->x[0], 0);
    map_req(req, phys, 0x10000, 0, 0x10000, 0x100);
    map_req(req + 0x28, phys, 0x20000, 0x10000, 0x20000, 0x108);
    map_req(req + 0x50, 0, 0, 0x30000, 0x10000, 0);
    uint64_t phys_gpu = nvn_pool_gpu(phys);
    if (phys_gpu == storage) {
        CHECK_EQ(call(c, nvnh_nvnMemoryPoolMapVirtual, virt, 3, req, 0), RL_PENDING);
        CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
        return;
    }
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolMapVirtual, virt, 3, req, 0), RET);
    CHECK_EQ(c->x[0], 1);
    uint64_t size = 0, gpu = nvn_pool_gpu(virt);
    CHECK_EQ(nvn_pool_memory(virt, &size), storage + 0x10000);
    CHECK_EQ(size, 0x40000);
    CHECK(gpu + 0x40000 <= phys_gpu || gpu >= phys_gpu + 0x80000);
    CHECK_EQ(nvn_gpu_guest(gpu + 0x20), storage + 0x10020);
    map_req(req, phys, 0, 0, 0x10000, 0x100);
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolMapVirtual, virt, 1, req, 0), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
}
