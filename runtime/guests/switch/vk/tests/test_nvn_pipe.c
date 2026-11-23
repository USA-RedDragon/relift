/**
 * @file test_nvn_pipe.c
 * @brief Pipelines of the NVN2 layer: specialization constants are laid out
 * in id order, libraries are made once per stage and reused by every pipeline
 * that links them, and the persistent cache is read back only when its header
 * matches the device, the driver and the shader table.
 */
#include "nvn_pipe.h"
#include "nvn_module.h"
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "runner.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** @brief A vertex program: a vec4 position at location 0, passed to location 0 as a color. */
static const uint32_t vert_spv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000001b, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0008000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00000011, 0x00000015,
    0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060005,
    0x0000000b, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x0000000b, 0x00000000,
    0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000000b, 0x00000001, 0x505f6c67, 0x746e696f,
    0x657a6953, 0x00000000, 0x00070006, 0x0000000b, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369,
    0x0065636e, 0x00070006, 0x0000000b, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e,
    0x00030005, 0x0000000d, 0x00000000, 0x00030005, 0x00000011, 0x00736f70, 0x00030005, 0x00000015,
    0x006c6f63, 0x00030047, 0x0000000b, 0x00000002, 0x00050048, 0x0000000b, 0x00000000, 0x0000000b,
    0x00000000, 0x00050048, 0x0000000b, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000000b,
    0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000000b, 0x00000003, 0x0000000b, 0x00000004,
    0x00040047, 0x00000011, 0x0000001e, 0x00000000, 0x00040047, 0x00000015, 0x0000001e, 0x00000000,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040015, 0x00000008, 0x00000020, 0x00000000,
    0x0004002b, 0x00000008, 0x00000009, 0x00000001, 0x0004001c, 0x0000000a, 0x00000006, 0x00000009,
    0x0006001e, 0x0000000b, 0x00000007, 0x00000006, 0x0000000a, 0x0000000a, 0x00040020, 0x0000000c,
    0x00000003, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000003, 0x00040015, 0x0000000e,
    0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f, 0x00000000, 0x00040020, 0x00000010,
    0x00000001, 0x00000007, 0x0004003b, 0x00000010, 0x00000011, 0x00000001, 0x00040020, 0x00000013,
    0x00000003, 0x00000007, 0x0004003b, 0x00000013, 0x00000015, 0x00000003, 0x0004002b, 0x00000006,
    0x00000017, 0x3f000000, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x0004003d, 0x00000007, 0x00000012, 0x00000011, 0x00050041, 0x00000013, 0x00000014,
    0x0000000d, 0x0000000f, 0x0003003e, 0x00000014, 0x00000012, 0x0004003d, 0x00000007, 0x00000016,
    0x00000011, 0x0005008e, 0x00000007, 0x00000018, 0x00000016, 0x00000017, 0x00070050, 0x00000007,
    0x00000019, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00050081, 0x00000007, 0x0000001a,
    0x00000018, 0x00000019, 0x0003003e, 0x00000015, 0x0000001a, 0x000100fd, 0x00010038,
};

/** @brief A fragment program writing the color it reads at location 0. */
static const uint32_t frag_spv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000000d, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000b, 0x00030010,
    0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00040005, 0x00000009, 0x6374756f, 0x00000000, 0x00030005, 0x0000000b, 0x006c6f63,
    0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000b, 0x0000001e, 0x00000000,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007,
    0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x00040020, 0x0000000a, 0x00000001, 0x00000007,
    0x0004003b, 0x0000000a, 0x0000000b, 0x00000001, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x00000007, 0x0000000c, 0x0000000b, 0x0003003e,
    0x00000009, 0x0000000c, 0x000100fd, 0x00010038,
};

/** @brief A geometry program passing each triangle through with its location 0 color, into layer 1. */
static const uint32_t geom_spv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x00000033, 0x00000000, 0x00020011, 0x00000002, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x000a000f, 0x00000003, 0x00000004, 0x6e69616d, 0x00000000, 0x0000001a, 0x0000001f, 0x00000026,
    0x00000029, 0x0000002e, 0x00030010, 0x00000004, 0x00000016, 0x00040010, 0x00000004, 0x00000000,
    0x00000001, 0x00030010, 0x00000004, 0x0000001d, 0x00040010, 0x00000004, 0x0000001a, 0x00000003,
    0x00030047, 0x00000018, 0x00000002, 0x00050048, 0x00000018, 0x00000000, 0x0000000b, 0x00000000,
    0x00050048, 0x00000018, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x00000018, 0x00000002,
    0x0000000b, 0x00000003, 0x00050048, 0x00000018, 0x00000003, 0x0000000b, 0x00000004, 0x00030047,
    0x0000001b, 0x00000002, 0x00050048, 0x0000001b, 0x00000000, 0x0000000b, 0x00000000, 0x00050048,
    0x0000001b, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000001b, 0x00000002, 0x0000000b,
    0x00000003, 0x00050048, 0x0000001b, 0x00000003, 0x0000000b, 0x00000004, 0x00040047, 0x00000026,
    0x0000001e, 0x00000000, 0x00040047, 0x00000029, 0x0000001e, 0x00000000, 0x00040047, 0x0000002e,
    0x0000000b, 0x00000009, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015,
    0x00000006, 0x00000020, 0x00000001, 0x0004002b, 0x00000006, 0x00000009, 0x00000000, 0x0004002b,
    0x00000006, 0x00000010, 0x00000003, 0x00020014, 0x00000011, 0x00030016, 0x00000013, 0x00000020,
    0x00040017, 0x00000014, 0x00000013, 0x00000004, 0x00040015, 0x00000015, 0x00000020, 0x00000000,
    0x0004002b, 0x00000015, 0x00000016, 0x00000001, 0x0004001c, 0x00000017, 0x00000013, 0x00000016,
    0x0006001e, 0x00000018, 0x00000014, 0x00000013, 0x00000017, 0x00000017, 0x00040020, 0x00000019,
    0x00000003, 0x00000018, 0x0004003b, 0x00000019, 0x0000001a, 0x00000003, 0x0006001e, 0x0000001b,
    0x00000014, 0x00000013, 0x00000017, 0x00000017, 0x0004002b, 0x00000015, 0x0000001c, 0x00000003,
    0x0004001c, 0x0000001d, 0x0000001b, 0x0000001c, 0x00040020, 0x0000001e, 0x00000001, 0x0000001d,
    0x0004003b, 0x0000001e, 0x0000001f, 0x00000001, 0x00040020, 0x00000021, 0x00000001, 0x00000014,
    0x00040020, 0x00000024, 0x00000003, 0x00000014, 0x0004003b, 0x00000024, 0x00000026, 0x00000003,
    0x0004001c, 0x00000027, 0x00000014, 0x0000001c, 0x00040020, 0x00000028, 0x00000001, 0x00000027,
    0x0004003b, 0x00000028, 0x00000029, 0x00000001, 0x00040020, 0x0000002d, 0x00000003, 0x00000006,
    0x0004003b, 0x0000002d, 0x0000002e, 0x00000003, 0x0004002b, 0x00000006, 0x0000002f, 0x00000001,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x000200f9,
    0x0000000a, 0x000200f8, 0x0000000a, 0x000700f5, 0x00000006, 0x00000032, 0x00000009, 0x00000005,
    0x00000031, 0x0000000b, 0x000500b1, 0x00000011, 0x00000012, 0x00000032, 0x00000010, 0x000400f6,
    0x0000000c, 0x0000000b, 0x00000000, 0x000400fa, 0x00000012, 0x0000000b, 0x0000000c, 0x000200f8,
    0x0000000b, 0x00060041, 0x00000021, 0x00000022, 0x0000001f, 0x00000032, 0x00000009, 0x0004003d,
    0x00000014, 0x00000023, 0x00000022, 0x00050041, 0x00000024, 0x00000025, 0x0000001a, 0x00000009,
    0x0003003e, 0x00000025, 0x00000023, 0x00050041, 0x00000021, 0x0000002b, 0x00000029, 0x00000032,
    0x0004003d, 0x00000014, 0x0000002c, 0x0000002b, 0x0003003e, 0x00000026, 0x0000002c, 0x0003003e,
    0x0000002e, 0x0000002f, 0x000100da, 0x00050080, 0x00000006, 0x00000031, 0x00000032, 0x0000002f,
    0x000200f9, 0x0000000a, 0x000200f8, 0x0000000c, 0x000100db, 0x000100fd, 0x00010038,
};

static VkShaderModule make_module(const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = bytes,
                                    .pCode = code };
    VkShaderModule m = NULL;
    if (VK(vkCreateShaderModule)(g.dev, &ci, NULL, &m) != VK_SUCCESS)
        return NULL;
    return m;
}

/** @brief A key that draws triangles of one vec4 attribute to one render target of format fmt. */
static void make_key(nvn_pipe_key *k, VkShaderModule vs, VkShaderModule fs, VkFormat fmt, uint32_t stride)
{
    memset(k, 0, sizeof *k);
    k->mods[0] = vs;
    k->mods[1] = fs;
    k->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    k->nattr = 1;
    k->nbind = 1;
    k->attr[0] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
    k->bind[0] = (VkVertexInputBindingDescription){ 0, stride, VK_VERTEX_INPUT_RATE_VERTEX };
    k->ncolor = 1;
    k->color[0] = fmt;
    k->depth = VK_FORMAT_UNDEFINED;
    k->stencil = VK_FORMAT_UNDEFINED;
    k->blend[0].colorWriteMask = 0xf;
    k->polygon = VK_POLYGON_MODE_FILL;
    k->cull = VK_CULL_MODE_NONE;
    k->front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    k->depth_func = VK_COMPARE_OP_ALWAYS;
}

TEST(nvn_spec_sort_orders_by_id_and_repacks_the_data)
{
    VkSpecializationMapEntry map[4] = { { 9, 0, 4 }, { 2, 4, 4 }, { 9, 8, 4 }, { 5, 12, 4 } };
    uint32_t vals[4] = { 90, 20, 91, 50 };
    uint32_t n = 4;
    nvn_spec_sort(&n, map, vals);
    CHECK_EQ(n, 3u);
    CHECK_EQ(map[0].constantID, 2u);
    CHECK_EQ(map[1].constantID, 5u);
    CHECK_EQ(map[2].constantID, 9u);
    CHECK_EQ(vals[0], 20u);
    CHECK_EQ(vals[1], 50u);
    CHECK_EQ(vals[2], 90u);
    for (uint32_t i = 0; i < n; i++)
        CHECK_EQ(map[i].offset, 4 * i);
}

TEST(nvn_pipe_libraries_are_made_once_and_linked_by_every_pipeline)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    CHECK(nvn_draw_layouts());
    VkShaderModule vs = make_module(vert_spv, sizeof vert_spv), fs = make_module(frag_spv, sizeof frag_spv);
    CHECK(vs && fs);
    nvn_pipe_key a, b;
    make_key(&a, vs, fs, VK_FORMAT_R8G8B8A8_UNORM, 16);
    make_key(&b, vs, fs, VK_FORMAT_B8G8R8A8_UNORM, 32);
    b.blend[0].blendEnable = VK_TRUE;
    b.blend[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    b.blend[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    b.blend[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    b.blend[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    b.blend[0].colorBlendOp = b.blend[0].alphaBlendOp = VK_BLEND_OP_ADD;

    uint64_t made0, ns0, late0, lns0, made1, ns1, late1, lns1;
    nvn_pipe_stats(&made0, &ns0, &late0, &lns0);
    bool libs = nvn_pipe_uses_libraries();
    if (libs) {
        CHECK(nvn_pipe_warm_vertex(&a));
        CHECK(nvn_pipe_warm_fragment(&a));
        nvn_pipe_stats(&made1, &ns1, &late1, &lns1);
        CHECK_EQ(made1 - made0, 2u);
        made0 = made1;
    }
    VkPipeline pa = nvn_pipe_graphics(&a);
    CHECK(pa != NULL);
    nvn_pipe_stats(&made1, &ns1, &late1, &lns1);
    CHECK_EQ(made1 - made0, libs ? 3u : 1u);
    made0 = made1;
    CHECK(nvn_pipe_graphics(&a) == pa);
    nvn_pipe_stats(&made1, &ns1, &late1, &lns1);
    CHECK_EQ(made1 - made0, 0u);
    VkPipeline pb = nvn_pipe_graphics(&b);
    CHECK(pb != NULL && pb != pa);
    nvn_pipe_stats(&made1, &ns1, &late1, &lns1);
    CHECK_EQ(made1 - made0, libs ? 3u : 1u);
    VK(vkDestroyShaderModule)(g.dev, vs, NULL);
    VK(vkDestroyShaderModule)(g.dev, fs, NULL);
}

TEST(nvn_pipe_a_pipeline_with_a_geometry_program_is_made_whole_once)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    if (!g.geometry_shader) {
        printf("  skipped: the host device has no geometry shaders\n");
        return;
    }
    CHECK(nvn_draw_layouts());
    VkShaderModule vs = make_module(vert_spv, sizeof vert_spv), fs = make_module(frag_spv, sizeof frag_spv),
                   gs = make_module(geom_spv, sizeof geom_spv);
    CHECK(vs && fs && gs);
    nvn_pipe_key k;
    make_key(&k, vs, fs, VK_FORMAT_R8G8B8A8_UNORM, 16);
    k.mods[2] = gs;
    uint64_t made0, ns0, late0, lns0, made1, ns1, late1, lns1;
    nvn_pipe_stats(&made0, &ns0, &late0, &lns0);
    VkPipeline p = nvn_pipe_graphics(&k);
    CHECK(p != NULL);
    nvn_pipe_stats(&made1, &ns1, &late1, &lns1);
    CHECK_EQ(made1 - made0, 1u);
    CHECK(nvn_pipe_graphics(&k) == p);
    nvn_pipe_stats(&made0, &ns0, &late0, &lns0);
    CHECK_EQ(made0, made1);
    VK(vkDestroyShaderModule)(g.dev, vs, NULL);
    VK(vkDestroyShaderModule)(g.dev, fs, NULL);
    VK(vkDestroyShaderModule)(g.dev, gs, NULL);
}

/** @brief Overwrites the byte at offset in the file at path with its complement. */
static void flip_byte(const char *path, long offset)
{
    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL);
    if (!f)
        return;
    int c = 0;
    CHECK_EQ(fseek(f, offset, SEEK_SET), 0);
    c = fgetc(f);
    CHECK_EQ(fseek(f, offset, SEEK_SET), 0);
    fputc(c ^ 0xff, f);
    fclose(f);
}

TEST(nvn_pipe_cache_is_read_back_and_discarded_on_a_changed_header_or_body)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    char dir[] = "nvn_pipe_cache_test_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[300];
    snprintf(path, sizeof path, "%s/pipeline-cache.bin", dir);
    setenv("RELIFT_PIPELINE_CACHE", dir, 1);
    nvn_pipe_init();
    CHECK_EQ(nvn_pipe_cache_bytes(), 0u);
    CHECK(nvn_draw_layouts());
    VkShaderModule vs = make_module(vert_spv, sizeof vert_spv), fs = make_module(frag_spv, sizeof frag_spv);
    CHECK(vs && fs);
    nvn_pipe_key k;
    make_key(&k, vs, fs, VK_FORMAT_R8G8B8A8_UNORM, 16);
    CHECK(nvn_pipe_graphics(&k) != NULL);
    nvn_pipe_save();
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f) {
        char magic[8];
        CHECK_EQ(fread(magic, 1, 8, f), 8u);
        CHECK(!memcmp(magic, "RLPLC001", 8));
        fclose(f);
    }
    uint64_t written = nvn_pipe_cache_bytes();
    CHECK(written > 0);
    nvn_pipe_init();
    CHECK_EQ(nvn_pipe_cache_bytes(), written);
    flip_byte(path, 8 + 64 + 3);
    nvn_pipe_init();
    CHECK_EQ(nvn_pipe_cache_bytes(), 0u);
    flip_byte(path, 8 + 64 + 3);
    nvn_pipe_init();
    CHECK_EQ(nvn_pipe_cache_bytes(), written);
    flip_byte(path, 200);
    nvn_pipe_init();
    CHECK_EQ(nvn_pipe_cache_bytes(), 0u);
    unsetenv("RELIFT_PIPELINE_CACHE");
    remove(path);
    rmdir(dir);
    VK(vkDestroyShaderModule)(g.dev, vs, NULL);
    VK(vkDestroyShaderModule)(g.dev, fs, NULL);
}

TEST(nvn_pipe_without_libraries_makes_whole_pipelines_and_warms_no_stage)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    char dir[] = "nvn_pipe_mono_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    setenv("RELIFT_PIPELINE_MODE", "monolithic", 1);
    setenv("RELIFT_PIPELINE_CACHE", dir, 1);
    nvn_pipe_init();
    CHECK(!nvn_pipe_uses_libraries());
    CHECK(nvn_draw_layouts());
    VkShaderModule vs = make_module(vert_spv, sizeof vert_spv), fs = make_module(frag_spv, sizeof frag_spv);
    CHECK(vs && fs);
    nvn_pipe_key k;
    make_key(&k, vs, fs, VK_FORMAT_R8G8B8A8_UNORM, 16);
    CHECK(!nvn_pipe_warm_vertex(&k));
    CHECK(!nvn_pipe_warm_fragment(&k));
    uint64_t made0, ns, late, lns, made1;
    nvn_pipe_stats(&made0, &ns, &late, &lns);
    CHECK(nvn_pipe_graphics(&k) != NULL);
    nvn_pipe_stats(&made1, &ns, &late, &lns);
    CHECK_EQ(made1 - made0, 1u);
    unsetenv("RELIFT_PIPELINE_MODE");
    unsetenv("RELIFT_PIPELINE_CACHE");
    nvn_pipe_init();
    char path[300];
    snprintf(path, sizeof path, "%s/pipeline-cache.bin", dir);
    remove(path);
    rmdir(dir);
    VK(vkDestroyShaderModule)(g.dev, vs, NULL);
    VK(vkDestroyShaderModule)(g.dev, fs, NULL);
}

TEST(nvn_pipe_records_what_the_game_made_and_reads_it_back_on_the_same_device)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    char dir[] = "nvn_pipe_keys_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    setenv("RELIFT_PIPELINE_MODE", "monolithic", 1);
    setenv("RELIFT_PIPELINE_CACHE", dir, 1);
    nvn_pipe_init();
    CHECK(nvn_draw_layouts());
    VkShaderModule vs = make_module(vert_spv, sizeof vert_spv), fs = make_module(frag_spv, sizeof frag_spv);
    CHECK(vs && fs);
    nvn_pipe_key k;
    make_key(&k, vs, fs, VK_FORMAT_R8G8B8A8_UNORM, 16);
    k.mods[0] = k.mods[1] = NULL;
    const char *keys[5] = { "vertex-program", "fragment-program", NULL, NULL, NULL };
    nvn_pipe_record_graphics(keys, &k);
    nvn_pipe_record_graphics(keys, &k);
    VkSpecializationMapEntry map[1] = { { 3, 0, 4 } };
    uint32_t vals[1] = { 7 };
    nvn_pipe_record_compute("compute-program", 1, map, vals);
    uint32_t wg[3] = { 8, 4, 1 };
    CHECK(nvn_pipe_record_program("compute-program", NVN_STAGE_COMPUTE, wg));
    CHECK(!nvn_pipe_record_program("compute-program", NVN_STAGE_COMPUTE, wg));
    CHECK(!nvn_pipe_record_program("vertex-program", 0, wg));
    nvn_pipe_save();
    nvn_pipe_init();
    CHECK_EQ(nvn_pipe_recorded(), 3u);
    const nvn_pipe_rec *a = nvn_pipe_recorded_at(0), *b = nvn_pipe_recorded_at(1), *c = nvn_pipe_recorded_at(2);
    CHECK(c && c->kind == NVN_REC_PROGRAM && c->k.spec[0][0] == 8 && c->k.topology == NVN_STAGE_COMPUTE);
    CHECK(a && b);
    if (a && b) {
        CHECK_EQ(a->kind, NVN_REC_GRAPHICS);
        CHECK(!strcmp(a->keys[0], "vertex-program") && !strcmp(a->keys[1], "fragment-program"));
        CHECK_EQ(a->k.nattr, 1u);
        CHECK(a->k.mods[0] == NULL && a->k.mods[1] == NULL);
        CHECK_EQ(b->kind, NVN_REC_COMPUTE);
        CHECK(!strcmp(b->keys[5], "compute-program"));
        CHECK_EQ(b->k.spec[0][0], 7u);
    }
    char path[300];
    snprintf(path, sizeof path, "%s/pipeline-keys.bin", dir);
    flip_byte(path, 8 + 3);
    nvn_pipe_init();
    CHECK_EQ(nvn_pipe_recorded(), 0u);
    unsetenv("RELIFT_PIPELINE_MODE");
    unsetenv("RELIFT_PIPELINE_CACHE");
    nvn_pipe_init();
    remove(path);
    snprintf(path, sizeof path, "%s/pipeline-cache.bin", dir);
    remove(path);
    rmdir(dir);
    VK(vkDestroyShaderModule)(g.dev, vs, NULL);
    VK(vkDestroyShaderModule)(g.dev, fs, NULL);
}

TEST(nvn_pipelines_compiled_in_play_are_saved_two_seconds_later)
{
    const uint64_t s = 1000000000ull;
    CHECK(!nvn_pipe_save_due(100 * s, 99 * s, 0, false));
    CHECK(!nvn_pipe_save_due(100 * s, 99 * s, 0, true));
    CHECK(nvn_pipe_save_due(200 * s, 100 * s, 0, true));
    CHECK(!nvn_pipe_save_due(101 * s, 100 * s, 100 * s, true));
    CHECK(nvn_pipe_save_due(103 * s, 100 * s, 100 * s, true));
    CHECK(!nvn_pipe_save_due(103 * s, 100 * s, 100 * s, false));
}

TEST(nvn_the_warm_up_makes_the_libraries_a_first_draw_links)
{
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_gfx_init(&st);
    static nvn_module m;
    memset(&m, 0, sizeof m);
    m.m = (VkShaderModule)(uintptr_t)0x1234;
    m.alpha_test = true;
    nvn_pipe_key k;
    nvn_warm_key(&k, &m, 1u);
    CHECK_EQ(k.sample_mask, st.sample_mask & 1u);
    CHECK_EQ(k.viewport_swizzle, st.viewport_swizzle[0]);
    CHECK_EQ(k.viewport_swizzle, NVN_VIEWPORT_SWIZZLE_IDENTITY);
    CHECK_EQ(k.depth_clamp, st.depth_clamp);
    CHECK_EQ(k.nspec[1u], 1u);
    CHECK_EQ(k.spec_map[1u][0].constantID, ALPHA_FUNC_SPEC);
    CHECK_EQ(k.spec[1u][0], NVN_ALPHA_ALWAYS);
    m.alpha_test = false;
    nvn_warm_key(&k, &m, 0u);
    CHECK_EQ(k.nspec[0u], 0u);
    CHECK(k.mods[0u] == m.m);
}

static nvn_pipe_key g_other_key;
static bool g_other_ok;

static void *make_other(void *arg)
{
    (void)arg;
    g_other_ok = nvn_pipe_warm_fragment(&g_other_key);
    return NULL;
}

TEST(nvn_pipe_a_draw_makes_its_pipeline_while_a_warm_up_thread_makes_another)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    CHECK(nvn_draw_layouts());
    VkShaderModule vs = make_module(vert_spv, sizeof vert_spv), fs = make_module(frag_spv, sizeof frag_spv);
    CHECK(vs && fs);
    nvn_pipe_key a;
    make_key(&a, vs, fs, VK_FORMAT_R8G8B8A8_UNORM, 24);
    make_key(&g_other_key, vs, fs, VK_FORMAT_R16G16B16A16_SFLOAT, 40);
    g_other_key.sample_mask = 0;
    pthread_t t;
    CHECK_EQ(pthread_create(&t, NULL, make_other, NULL), 0);
    CHECK(nvn_pipe_graphics(&a) != NULL);
    pthread_join(t, NULL);
    CHECK(g_other_ok || !nvn_pipe_uses_libraries());
    VK(vkDestroyShaderModule)(g.dev, vs, NULL);
    VK(vkDestroyShaderModule)(g.dev, fs, NULL);
}

TEST(nvn_a_draw_without_its_exact_library_reads_texture_kinds_at_run_time)
{
    VkSpecializationMapEntry map[3] = { { 0x100, 0, 4 }, { NVN_FETCH_KIND_SPEC, 4, 4 }, { NVN_FETCH_KIND_SPEC + 1, 8, 4 } };
    uint32_t vals[3] = { 2, 1, NVN_KIND_AT_RUN_TIME };
    CHECK(nvn_spec_at_run_time(3, map, vals));
    CHECK_EQ(vals[0], 2u);
    CHECK_EQ(vals[1], NVN_KIND_AT_RUN_TIME);
    CHECK_EQ(vals[2], NVN_KIND_AT_RUN_TIME);
    CHECK(!nvn_spec_at_run_time(3, map, vals));
}
