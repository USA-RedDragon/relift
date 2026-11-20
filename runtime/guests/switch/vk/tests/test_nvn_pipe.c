/**
 * @file test_nvn_pipe.c
 * @brief Pipelines of the NVN2 layer: specialization constants are laid out
 * in id order, libraries are made once per stage and reused by every pipeline
 * that links them, and the persistent cache is read back only when its header
 * matches the device, the driver and the shader table.
 */
#include "nvn_pipe.h"
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "runner.h"

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
