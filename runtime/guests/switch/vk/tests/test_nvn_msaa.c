/**
 * @file test_nvn_msaa.c
 * @brief Multisample textures get host images of their own sample count, which draw into them at that count and
 * bind only as multisample textures, where a program reads each sample it was drawn with; their samples move
 * between the image and the guest's storage in NVK's grid cells, both ways, Downsample averages the driver's
 * taps, programmable sample locations land on NVN's 1/16 pixel points, and NVIDIA's raster samples give coverage to
 * color and coverage modulation as their hardware states say.
 */
#include "hos.h"
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief A replay over 64 KiB of guest memory, all zeros, with a pool over all of it. */
typedef struct rig {
    rl_ctx c;
    uint8_t mem[0x10000];
    nvn_gfx st;
    replay r;
} rig;

/** @brief Whether the host makes RGBA8 render targets of n samples (lavapipe has no 2). */
static bool host_samples(uint32_t n)
{
    VkImageFormatProperties ifp;
    return VK(vkGetPhysicalDeviceImageFormatProperties)(g.phys, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                                         VK_IMAGE_TILING_OPTIMAL,
                                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                             VK_IMAGE_USAGE_SAMPLED_BIT,
                                                         0, &ifp) == VK_SUCCESS &&
           (ifp.sampleCounts & n);
}

static bool rig_up(rig *g_, uint64_t pool)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return false;
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    memset(g_, 0, sizeof *g_);
    g_->r.c = &g_->c;
    g_->r.mem = g_->mem;
    g_->r.st = &g_->st;
    return nvn_pool_make(pool, 0, 0x10000, 0x2a1);
}

TEST(nvn_a_multisample_texture_gets_a_host_image_of_its_sample_count)
{
    static rig k;
    const uint64_t pool = 0x110, tex = 0x210, arr = 0x310;
    if (!rig_up(&k, pool))
        return;
    if (!host_samples(8)) {
        fprintf(stderr, "nvn_a_multisample_texture_gets_a_host_image_of_its_sample_count: the host device has no 8-sample color targets, skipped\n");
        return;
    }
    nvn_tex_desc d = { .target = NVN_TARGET_2D_MULTISAMPLE, .format = 37, .width = 16, .height = 8, .depth = 1,
                       .levels = 1, .samples = 8, .pool = pool, .offset = 0x1000 };
    CHECK(nvn_tex_layout(&d));
    CHECK(nvn_vk_can_image(&d));
    CHECK(nvn_texture_record(tex, &d, NULL));
    vimg *v = nvn_vk_image(&k.r, tex);
    CHECK(v != NULL);
    if (!v)
        return;
    CHECK_EQ(nvn_vk_sample_bits(&v->d), VK_SAMPLE_COUNT_8_BIT);
    CHECK(!v->storage_ok);
    CHECK_EQ(nvn_vk_image_range(v).layerCount, 1u);
    nvn_tex_desc a = d;
    a.target = NVN_TARGET_2D_MULTISAMPLE_ARRAY;
    a.depth = 3;
    a.samples = 4;
    a.offset = 0x8000;
    CHECK(nvn_tex_layout(&a));
    CHECK(nvn_texture_record(arr, &a, NULL));
    vimg *w = nvn_vk_image(&k.r, arr);
    CHECK(w != NULL);
    CHECK(w && nvn_vk_image_range(w).layerCount == 3u);
    CHECK(nvn_vk_flush(&k.r));
}

TEST(nvn_multisample_textures_need_a_multisample_target_and_one_level)
{
    nvn_tex_desc d = { .target = NVN_TARGET_2D_MULTISAMPLE, .format = 37, .width = 16, .height = 8, .depth = 1,
                       .levels = 1, .samples = 4 };
    CHECK(nvn_vk_can_image(&d));
    d.samples = 3;
    CHECK(!nvn_vk_can_image(&d));
    d.samples = 4;
    d.levels = 2;
    CHECK(!nvn_vk_can_image(&d));
    d.levels = 1;
    d.target = 1;
    CHECK(!nvn_vk_can_image(&d));
    d.samples = 1;
    CHECK(nvn_vk_can_image(&d));
}

TEST(nvn_render_targets_report_their_common_sample_count)
{
    vimg a = { .d = { .samples = 4 } }, b = { .d = { .samples = 4 } }, one = { .d = { .samples = 0 } };
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    uint32_t n = 99;
    CHECK(nvn_vk_target_samples(&st, &n));
    CHECK_EQ(n, 0u);
    st.ncolor = 2;
    st.color[1].v = &a;
    st.depth.v = &b;
    CHECK(nvn_vk_target_samples(&st, &n));
    CHECK_EQ(n, 4u);
    st.color[0].v = &one;
    CHECK(!nvn_vk_target_samples(&st, &n));
}

/**
 * @brief fill.vert, by `glslc --target-env=vulkan1.3 -O`:
 *
 * #version 450
 * void main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }
 */
static const uint32_t fill_vert_spv[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x0000002c, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x0000001d, 0x00040047,
    0x0000000c, 0x0000000b, 0x0000002a, 0x00030047, 0x0000001b, 0x00000002, 0x00050048, 0x0000001b,
    0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000001b, 0x00000001, 0x0000000b, 0x00000001,
    0x00050048, 0x0000001b, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000001b, 0x00000003,
    0x0000000b, 0x00000004, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040015, 0x0000000a,
    0x00000020, 0x00000001, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b,
    0x0000000c, 0x00000001, 0x0004002b, 0x0000000a, 0x0000000e, 0x00000001, 0x0004002b, 0x0000000a,
    0x00000010, 0x00000002, 0x00040017, 0x00000017, 0x00000006, 0x00000004, 0x00040015, 0x00000018,
    0x00000020, 0x00000000, 0x0004002b, 0x00000018, 0x00000019, 0x00000001, 0x0004001c, 0x0000001a,
    0x00000006, 0x00000019, 0x0006001e, 0x0000001b, 0x00000017, 0x00000006, 0x0000001a, 0x0000001a,
    0x00040020, 0x0000001c, 0x00000003, 0x0000001b, 0x0004003b, 0x0000001c, 0x0000001d, 0x00000003,
    0x0004002b, 0x0000000a, 0x0000001e, 0x00000000, 0x0004002b, 0x00000006, 0x00000020, 0x40000000,
    0x0004002b, 0x00000006, 0x00000022, 0x3f800000, 0x0004002b, 0x00000006, 0x00000025, 0x00000000,
    0x00040020, 0x00000029, 0x00000003, 0x00000017, 0x0005002c, 0x00000007, 0x0000002b, 0x00000022,
    0x00000022, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c, 0x000500c4, 0x0000000a, 0x0000000f, 0x0000000d,
    0x0000000e, 0x000500c7, 0x0000000a, 0x00000011, 0x0000000f, 0x00000010, 0x0004006f, 0x00000006,
    0x00000012, 0x00000011, 0x000500c7, 0x0000000a, 0x00000014, 0x0000000d, 0x00000010, 0x0004006f,
    0x00000006, 0x00000015, 0x00000014, 0x00050050, 0x00000007, 0x00000016, 0x00000012, 0x00000015,
    0x0005008e, 0x00000007, 0x00000021, 0x00000016, 0x00000020, 0x00050083, 0x00000007, 0x00000024,
    0x00000021, 0x0000002b, 0x00050051, 0x00000006, 0x00000026, 0x00000024, 0x00000000, 0x00050051,
    0x00000006, 0x00000027, 0x00000024, 0x00000001, 0x00070050, 0x00000017, 0x00000028, 0x00000026,
    0x00000027, 0x00000025, 0x00000022, 0x00050041, 0x00000029, 0x0000002a, 0x0000001d, 0x0000001e,
    0x0003003e, 0x0000002a, 0x00000028, 0x000100fd, 0x00010038,
};

/**
 * @brief fill.frag, by `glslc --target-env=vulkan1.3 -O`:
 *
 * #version 450
 * layout(location = 0) out vec4 o;
 * void main() { o = vec4(float(gl_SampleID + 1) * 16.0 / 255.0, float(7 - gl_SampleID) * 8.0 / 255.0, 0.0, 1.0); }
 */
static const uint32_t fill_frag_spv[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x00000022, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x00000023, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009,
    0x0000000c, 0x00030010, 0x00000004, 0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000,
    0x00040047, 0x0000000c, 0x0000000b, 0x00000012, 0x00030047, 0x0000000c, 0x0000000e, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017,
    0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b,
    0x00000008, 0x00000009, 0x00000003, 0x00040015, 0x0000000a, 0x00000020, 0x00000001, 0x00040020,
    0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b,
    0x0000000a, 0x0000000e, 0x00000001, 0x0004002b, 0x0000000a, 0x00000015, 0x00000007, 0x0004002b,
    0x00000006, 0x0000001c, 0x00000000, 0x0004002b, 0x00000006, 0x0000001d, 0x3f800000, 0x0004002b,
    0x00000006, 0x00000020, 0x3d808081, 0x0004002b, 0x00000006, 0x00000021, 0x3d008081, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a,
    0x0000000d, 0x0000000c, 0x00050080, 0x0000000a, 0x0000000f, 0x0000000d, 0x0000000e, 0x0004006f,
    0x00000006, 0x00000010, 0x0000000f, 0x00050085, 0x00000006, 0x00000014, 0x00000010, 0x00000020,
    0x00050082, 0x0000000a, 0x00000017, 0x00000015, 0x0000000d, 0x0004006f, 0x00000006, 0x00000018,
    0x00000017, 0x00050085, 0x00000006, 0x0000001b, 0x00000018, 0x00000021, 0x00070050, 0x00000007,
    0x0000001e, 0x00000014, 0x0000001b, 0x0000001c, 0x0000001d, 0x0003003e, 0x00000009, 0x0000001e,
    0x000100fd, 0x00010038,
};

/**
 * @brief fetch.comp, by `glslc --target-env=vulkan1.3 -O`:
 *
 * #version 450
 * #extension GL_EXT_nonuniform_qualifier : require
 * #extension GL_EXT_samplerless_texture_functions : require
 * layout(local_size_x = 8) in;
 * layout(set = 0, binding = 8) uniform texture2DMS tex[];
 * layout(set = 1, binding = 0) buffer Out { uint o[]; };
 * layout(push_constant) uniform P { uint id; } p;
 * void main() { uint s = gl_LocalInvocationID.x; o[s] = packUnorm4x8(texelFetch(tex[p.id], ivec2(1, 2), int(s))); }
 */
static const uint32_t fetch_comp_spv[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x00000030, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x000014b6, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x0009000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000b,
    0x00000013, 0x0000001b, 0x0000001e, 0x00060010, 0x00000004, 0x00000011, 0x00000008, 0x00000001,
    0x00000001, 0x00040047, 0x0000000b, 0x0000000b, 0x0000001b, 0x00040047, 0x00000010, 0x00000006,
    0x00000004, 0x00030047, 0x00000011, 0x00000002, 0x00050048, 0x00000011, 0x00000000, 0x00000023,
    0x00000000, 0x00040047, 0x00000013, 0x00000021, 0x00000000, 0x00040047, 0x00000013, 0x00000022,
    0x00000001, 0x00040047, 0x0000001b, 0x00000021, 0x00000008, 0x00040047, 0x0000001b, 0x00000022,
    0x00000000, 0x00030047, 0x0000001c, 0x00000002, 0x00050048, 0x0000001c, 0x00000000, 0x00000023,
    0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006,
    0x00000020, 0x00000000, 0x00040017, 0x00000009, 0x00000006, 0x00000003, 0x00040020, 0x0000000a,
    0x00000001, 0x00000009, 0x0004003b, 0x0000000a, 0x0000000b, 0x00000001, 0x0004002b, 0x00000006,
    0x0000000c, 0x00000000, 0x00040020, 0x0000000d, 0x00000001, 0x00000006, 0x0003001d, 0x00000010,
    0x00000006, 0x0003001e, 0x00000011, 0x00000010, 0x00040020, 0x00000012, 0x0000000c, 0x00000011,
    0x0004003b, 0x00000012, 0x00000013, 0x0000000c, 0x00040015, 0x00000014, 0x00000020, 0x00000001,
    0x0004002b, 0x00000014, 0x00000015, 0x00000000, 0x00030016, 0x00000017, 0x00000020, 0x00090019,
    0x00000018, 0x00000017, 0x00000001, 0x00000000, 0x00000000, 0x00000001, 0x00000001, 0x00000000,
    0x0003001d, 0x00000019, 0x00000018, 0x00040020, 0x0000001a, 0x00000000, 0x00000019, 0x0004003b,
    0x0000001a, 0x0000001b, 0x00000000, 0x0003001e, 0x0000001c, 0x00000006, 0x00040020, 0x0000001d,
    0x00000009, 0x0000001c, 0x0004003b, 0x0000001d, 0x0000001e, 0x00000009, 0x00040020, 0x0000001f,
    0x00000009, 0x00000006, 0x00040020, 0x00000022, 0x00000000, 0x00000018, 0x00040017, 0x00000025,
    0x00000014, 0x00000002, 0x0004002b, 0x00000014, 0x00000026, 0x00000001, 0x0004002b, 0x00000014,
    0x00000027, 0x00000002, 0x0005002c, 0x00000025, 0x00000028, 0x00000026, 0x00000027, 0x00040017,
    0x0000002b, 0x00000017, 0x00000004, 0x00040020, 0x0000002e, 0x0000000c, 0x00000006, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x00050041, 0x0000000d,
    0x0000000e, 0x0000000b, 0x0000000c, 0x0004003d, 0x00000006, 0x0000000f, 0x0000000e, 0x00050041,
    0x0000001f, 0x00000020, 0x0000001e, 0x00000015, 0x0004003d, 0x00000006, 0x00000021, 0x00000020,
    0x00050041, 0x00000022, 0x00000023, 0x0000001b, 0x00000021, 0x0004003d, 0x00000018, 0x00000024,
    0x00000023, 0x0004007c, 0x00000014, 0x0000002a, 0x0000000f, 0x0007005f, 0x0000002b, 0x0000002c,
    0x00000024, 0x00000028, 0x00000040, 0x0000002a, 0x0006000c, 0x00000006, 0x0000002d, 0x00000001,
    0x00000037, 0x0000002c, 0x00060041, 0x0000002e, 0x0000002f, 0x00000013, 0x00000015, 0x0000000f,
    0x0003003e, 0x0000002f, 0x0000002d, 0x000100fd, 0x00010038,
};

/** @brief A host-visible buffer of size bytes, mapped at *map. */
static bool host_buffer(VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem, uint32_t **map)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size,
                               .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    if (VK(vkCreateBuffer)(g.dev, &bci, NULL, buf) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    VK(vkGetBufferMemoryRequirements)(g.dev, *buf, &req);
    int32_t type = nvn_vk_mem_type(req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)type };
    return type >= 0 && VK(vkAllocateMemory)(g.dev, &mai, NULL, mem) == VK_SUCCESS &&
           VK(vkBindBufferMemory)(g.dev, *buf, *mem, 0) == VK_SUCCESS &&
           VK(vkMapMemory)(g.dev, *mem, 0, size, 0, (void **)map) == VK_SUCCESS;
}

static VkShaderModule module_of(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = size,
                                     .pCode = code };
    VkShaderModule m = NULL;
    VK(vkCreateShaderModule)(g.dev, &smi, NULL, &m);
    return m;
}

/** @brief Draws fill_frag_spv over all of v at 8 samples, each sample shaded on its own (gl_SampleID). */
static bool fill_samples(VkCommandBuffer cb, const vimg *v, VkPipeline *pipe, VkPipelineLayout *pl)
{
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    if (VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, pl) != VK_SUCCESS)
        return false;
    VkShaderModule vs = module_of(fill_vert_spv, sizeof fill_vert_spv), fs = module_of(fill_frag_spv, sizeof fill_frag_spv);
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, (float)v->d.width, (float)v->d.height, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { v->d.width, v->d.height } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                              .viewportCount = 1, .pViewports = &vp, .scissorCount = 1,
                                              .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_8_BIT };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                .attachmentCount = 1, .pAttachments = &ba };
    VkFormat f = v->format;
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                         .colorAttachmentCount = 1, .pColorAttachmentFormats = &f };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                         .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
                                         .pInputAssemblyState = &ia, .pViewportState = &vps,
                                         .pRasterizationState = &rs, .pMultisampleState = &ms,
                                         .pColorBlendState = &cbs, .layout = *pl };
    bool ok = VK(vkCreateGraphicsPipelines)(g.dev, NULL, 1, &gpi, NULL, pipe) == VK_SUCCESS;
    VK(vkDestroyShaderModule)(g.dev, vs, NULL);
    VK(vkDestroyShaderModule)(g.dev, fs, NULL);
    if (!ok)
        return false;
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v->view,
                                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo rinfo = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = sc, .layerCount = 1,
                              .colorAttachmentCount = 1, .pColorAttachments = &ca };
    VK(vkCmdBeginRendering)(cb, &rinfo);
    VK(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe);
    VK(vkCmdDraw)(cb, 3, 1, 0, 0);
    VK(vkCmdEndRendering)(cb);
    return true;
}

TEST(nvn_texset_a_multisample_texture_reads_each_sample_it_was_drawn_with)
{
    static rig k;
    const uint64_t pool = 0x120, tex = 0x220;
    if (!rig_up(&k, pool))
        return;
    if (!host_samples(8)) {
        fprintf(stderr, "nvn_texset_a_multisample_texture_reads_each_sample_it_was_drawn_with: the host device has no 8-sample color targets, skipped\n");
        return;
    }
    CHECK(nvn_texset_layout());
    nvn_tex_desc d = { .target = NVN_TARGET_2D_MULTISAMPLE, .format = 37, .width = 4, .height = 4, .depth = 1,
                       .levels = 1, .samples = 8, .swizzle = { 2, 3, 4, 5 }, .pool = pool, .offset = 0x2000 };
    CHECK(nvn_tex_layout(&d));
    CHECK(nvn_texture_record(tex, &d, NULL));
    k.c.x[1] = 9;
    k.c.x[2] = tex;
    k.c.x[3] = 0;
    nvnh_nvnTexturePoolRegisterTexture(&k.c, k.mem);
    CHECK_EQ(nvn_texset_sync_registered(&k.r), RL_OK);
    vimg *v = nvn_vk_image(&k.r, tex);
    CHECK(v != NULL);
    CHECK(nvn_vk_flush(&k.r));
    if (!v)
        return;
    VkBuffer buf;
    VkDeviceMemory mem;
    uint32_t *map;
    CHECK(host_buffer(64, &buf, &mem, &map));
    memset(map, 0, 64);
    VkDescriptorSetLayoutBinding ob = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo oli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 1, .pBindings = &ob };
    VkDescriptorSetLayout out_layout;
    CHECK_EQ(VK(vkCreateDescriptorSetLayout)(g.dev, &oli, NULL, &out_layout), VK_SUCCESS);
    VkDescriptorSetLayout layouts[2] = { nvn_texset_layout(), out_layout };
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2,
                                       .pSetLayouts = layouts, .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &pr };
    VkPipelineLayout cpl;
    CHECK_EQ(VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &cpl), VK_SUCCESS);
    VkShaderModule cs = module_of(fetch_comp_spv, sizeof fetch_comp_spv);
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs,
                                                   .pName = "main" },
                                        .layout = cpl };
    VkPipeline cpipe;
    CHECK_EQ(VK(vkCreateComputePipelines)(g.dev, NULL, 1, &cpi, NULL, &cpipe), VK_SUCCESS);
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                       .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dpool;
    CHECK_EQ(VK(vkCreateDescriptorPool)(g.dev, &dpi, NULL, &dpool), VK_SUCCESS);
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = dpool, .descriptorSetCount = 1,
                                        .pSetLayouts = &out_layout };
    VkDescriptorSet out_set;
    CHECK_EQ(VK(vkAllocateDescriptorSets)(g.dev, &dai, &out_set), VK_SUCCESS);
    VkDescriptorBufferInfo bi = { buf, 0, 64 };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = out_set,
                               .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               .pBufferInfo = &bi };
    VK(vkUpdateDescriptorSets)(g.dev, 1, &w, 0, NULL);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = g.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK_EQ(VK(vkAllocateCommandBuffers)(g.dev, &cai, &cb), VK_SUCCESS);
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer)(cb, &cbi);
    VkPipeline gpipe = NULL;
    VkPipelineLayout gpl = NULL;
    CHECK(fill_samples(cb, v, &gpipe, &gpl));
    nvn_vk_barrier(cb);
    VK(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
    VkDescriptorSet sets[2] = { nvn_texset(), out_set };
    VK(vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cpl, 0, 2, sets, 0, NULL);
    uint32_t id = 9;
    VK(vkCmdPushConstants)(cb, cpl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &id);
    VK(vkCmdDispatch)(cb, 1, 1, 1);
    VK(vkEndCommandBuffer)(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK(vkResetFences)(g.dev, 1, &g.fence);
    CHECK_EQ(VK(vkQueueSubmit)(g.queue, 1, &si, g.fence), VK_SUCCESS);
    CHECK_EQ(VK(vkWaitForFences)(g.dev, 1, &g.fence, VK_TRUE, 10000000000ull), VK_SUCCESS);
    for (uint32_t s = 0; s < 8; s++)
        CHECK_EQ(map[s], 0xff000000u | (7u - s) * 8u << 8 | (s + 1u) * 16u);
    VK(vkFreeCommandBuffers)(g.dev, g.pool, 1, &cb);
    VK(vkDestroyPipeline)(g.dev, gpipe, NULL);
    VK(vkDestroyPipelineLayout)(g.dev, gpl, NULL);
    VK(vkDestroyPipeline)(g.dev, cpipe, NULL);
    VK(vkDestroyShaderModule)(g.dev, cs, NULL);
    VK(vkDestroyPipelineLayout)(g.dev, cpl, NULL);
    VK(vkDestroyDescriptorPool)(g.dev, dpool, NULL);
    VK(vkDestroyDescriptorSetLayout)(g.dev, out_layout, NULL);
    VK(vkUnmapMemory)(g.dev, mem);
    VK(vkFreeMemory)(g.dev, mem, NULL);
    VK(vkDestroyBuffer)(g.dev, buf, NULL);
}

/**
 * @brief fetchall.comp, by `glslc --target-env=vulkan1.3 -O`: every sample of every pixel of texture id, one
 * workgroup a pixel:
 *
 * #version 450
 * #extension GL_EXT_nonuniform_qualifier : require
 * #extension GL_EXT_samplerless_texture_functions : require
 * layout(local_size_x = 1) in;
 * layout(set = 0, binding = 8) uniform texture2DMS tex[];
 * layout(set = 1, binding = 0) buffer Out { uint o[]; };
 * layout(push_constant) uniform P { uint id, width, samples; } p;
 * void main()
 * {
 *     uvec2 q = gl_WorkGroupID.xy;
 *     for (uint s = 0u; s < p.samples; s++)
 *         o[(q.y * p.width + q.x) * p.samples + s] = packUnorm4x8(texelFetch(tex[p.id], ivec2(q), int(s)));
 * }
 */
static const uint32_t fetchall_comp_spv[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x00000050, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x000014b6, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x0009000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000c,
    0x0000001a, 0x00000025, 0x0000003a, 0x00060010, 0x00000004, 0x00000011, 0x00000001, 0x00000001,
    0x00000001, 0x00040047, 0x0000000c, 0x0000000b, 0x0000001a, 0x00030047, 0x00000018, 0x00000002,
    0x00050048, 0x00000018, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000018, 0x00000001,
    0x00000023, 0x00000004, 0x00050048, 0x00000018, 0x00000002, 0x00000023, 0x00000008, 0x00040047,
    0x00000022, 0x00000006, 0x00000004, 0x00030047, 0x00000023, 0x00000002, 0x00050048, 0x00000023,
    0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x00000025, 0x00000021, 0x00000000, 0x00040047,
    0x00000025, 0x00000022, 0x00000001, 0x00040047, 0x0000003a, 0x00000021, 0x00000008, 0x00040047,
    0x0000003a, 0x00000022, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040017, 0x00000007, 0x00000006, 0x00000002,
    0x00040017, 0x0000000a, 0x00000006, 0x00000003, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a,
    0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x00000006, 0x00000011, 0x00000000,
    0x0005001e, 0x00000018, 0x00000006, 0x00000006, 0x00000006, 0x00040020, 0x00000019, 0x00000009,
    0x00000018, 0x0004003b, 0x00000019, 0x0000001a, 0x00000009, 0x00040015, 0x0000001b, 0x00000020,
    0x00000001, 0x0004002b, 0x0000001b, 0x0000001c, 0x00000002, 0x00040020, 0x0000001d, 0x00000009,
    0x00000006, 0x00020014, 0x00000020, 0x0003001d, 0x00000022, 0x00000006, 0x0003001e, 0x00000023,
    0x00000022, 0x00040020, 0x00000024, 0x0000000c, 0x00000023, 0x0004003b, 0x00000024, 0x00000025,
    0x0000000c, 0x0004002b, 0x0000001b, 0x00000026, 0x00000000, 0x0004002b, 0x0000001b, 0x0000002a,
    0x00000001, 0x00030016, 0x00000036, 0x00000020, 0x00090019, 0x00000037, 0x00000036, 0x00000001,
    0x00000000, 0x00000000, 0x00000001, 0x00000001, 0x00000000, 0x0003001d, 0x00000038, 0x00000037,
    0x00040020, 0x00000039, 0x00000000, 0x00000038, 0x0004003b, 0x00000039, 0x0000003a, 0x00000000,
    0x00040020, 0x0000003d, 0x00000000, 0x00000037, 0x00040017, 0x00000041, 0x0000001b, 0x00000002,
    0x00040017, 0x00000045, 0x00000036, 0x00000004, 0x00040020, 0x00000048, 0x0000000c, 0x00000006,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d,
    0x0000000a, 0x0000000d, 0x0000000c, 0x0007004f, 0x00000007, 0x0000000e, 0x0000000d, 0x0000000d,
    0x00000000, 0x00000001, 0x000200f9, 0x00000012, 0x000200f8, 0x00000012, 0x000700f5, 0x00000006,
    0x0000004f, 0x00000011, 0x00000005, 0x0000004b, 0x00000013, 0x00050041, 0x0000001d, 0x0000001e,
    0x0000001a, 0x0000001c, 0x0004003d, 0x00000006, 0x0000001f, 0x0000001e, 0x000500b0, 0x00000020,
    0x00000021, 0x0000004f, 0x0000001f, 0x000400f6, 0x00000014, 0x00000013, 0x00000000, 0x000400fa,
    0x00000021, 0x00000013, 0x00000014, 0x000200f8, 0x00000013, 0x00050051, 0x00000006, 0x00000029,
    0x0000000d, 0x00000001, 0x00050041, 0x0000001d, 0x0000002b, 0x0000001a, 0x0000002a, 0x0004003d,
    0x00000006, 0x0000002c, 0x0000002b, 0x00050084, 0x00000006, 0x0000002d, 0x00000029, 0x0000002c,
    0x00050051, 0x00000006, 0x0000002f, 0x0000000d, 0x00000000, 0x00050080, 0x00000006, 0x00000030,
    0x0000002d, 0x0000002f, 0x00050084, 0x00000006, 0x00000033, 0x00000030, 0x0000001f, 0x00050080,
    0x00000006, 0x00000035, 0x00000033, 0x0000004f, 0x00050041, 0x0000001d, 0x0000003b, 0x0000001a,
    0x00000026, 0x0004003d, 0x00000006, 0x0000003c, 0x0000003b, 0x00050041, 0x0000003d, 0x0000003e,
    0x0000003a, 0x0000003c, 0x0004003d, 0x00000037, 0x0000003f, 0x0000003e, 0x0004007c, 0x00000041,
    0x00000042, 0x0000000e, 0x0004007c, 0x0000001b, 0x00000044, 0x0000004f, 0x0007005f, 0x00000045,
    0x00000046, 0x0000003f, 0x00000042, 0x00000040, 0x00000044, 0x0006000c, 0x00000006, 0x00000047,
    0x00000001, 0x00000037, 0x00000046, 0x00060041, 0x00000048, 0x00000049, 0x00000025, 0x00000026,
    0x00000035, 0x0003003e, 0x00000049, 0x00000047, 0x00050080, 0x00000006, 0x0000004b, 0x0000004f,
    0x0000002a, 0x000200f9, 0x00000012, 0x000200f8, 0x00000014, 0x000100fd, 0x00010038,
};

/**
 * @brief The cell (cx | cy << 2) of sample s of 2, 4 and 8 samples (NVK's, src/nouveau/nil/image.rs sa_offset) and
 * of 16 (the RTX 3090's, measured by msaa_cells).
 */
static uint32_t nvk_cell(uint32_t samples, uint32_t s)
{
    static const uint8_t eight[8] = { 2, 5, 7, 1, 4, 0, 6, 3 };
    static const uint8_t sixteen[16] = { 10, 5, 9, 6, 4, 14, 11, 3, 13, 2, 1, 12, 8, 7, 15, 0 };
    return samples == 2 ? 1u - s : samples == 4 ? (s & 1u) | (s >> 1) << 2 : samples == 8 ? eight[s] : sixteen[s];
}

TEST(nvn_multisample_cells_are_nvks_and_the_3090s)
{
    static const uint32_t counts[4] = { 2, 4, 8, 16 };
    for (uint32_t c = 0; c < 4; c++) {
        uint32_t gw, gh;
        uint64_t cells;
        CHECK(nvn_msaa_cells(counts[c], &gw, &gh, &cells));
        uint32_t seen = 0;
        for (uint32_t s = 0; s < counts[c]; s++) {
            uint32_t cell = (uint32_t)(cells >> (4 * s) & 15u);
            CHECK_EQ(cell, nvk_cell(counts[c], s));
            CHECK((cell & 3u) < gw && (cell >> 2) < gh);
            seen |= 1u << cell;
        }
        CHECK_EQ((uint32_t)__builtin_popcount(seen), counts[c]);
    }
    uint32_t gw, gh;
    uint64_t cells;
    CHECK(!nvn_msaa_cells(32, &gw, &gh, &cells));
}

/** @brief Runs fetchall_comp_spv over texture id (w by h pixels of samples samples) into out. */
static void fetch_all(uint32_t id, uint32_t w, uint32_t h, uint32_t samples, uint32_t *out)
{
    VkBuffer buf;
    VkDeviceMemory mem;
    uint32_t *map;
    VkDeviceSize size = (VkDeviceSize)w * h * samples * 4;
    CHECK(host_buffer(size, &buf, &mem, &map));
    memset(map, 0, size);
    VkDescriptorSetLayoutBinding ob = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo oli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 1, .pBindings = &ob };
    VkDescriptorSetLayout out_layout;
    CHECK_EQ(VK(vkCreateDescriptorSetLayout)(g.dev, &oli, NULL, &out_layout), VK_SUCCESS);
    VkDescriptorSetLayout layouts[2] = { nvn_texset_layout(), out_layout };
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 12 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2,
                                       .pSetLayouts = layouts, .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &pr };
    VkPipelineLayout pl;
    CHECK_EQ(VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &pl), VK_SUCCESS);
    VkShaderModule cs = module_of(fetchall_comp_spv, sizeof fetchall_comp_spv);
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs,
                                                   .pName = "main" },
                                        .layout = pl };
    VkPipeline pipe;
    CHECK_EQ(VK(vkCreateComputePipelines)(g.dev, NULL, 1, &cpi, NULL, &pipe), VK_SUCCESS);
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                       .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dpool;
    CHECK_EQ(VK(vkCreateDescriptorPool)(g.dev, &dpi, NULL, &dpool), VK_SUCCESS);
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = dpool, .descriptorSetCount = 1,
                                        .pSetLayouts = &out_layout };
    VkDescriptorSet out_set;
    CHECK_EQ(VK(vkAllocateDescriptorSets)(g.dev, &dai, &out_set), VK_SUCCESS);
    VkDescriptorBufferInfo bi = { buf, 0, size };
    VkWriteDescriptorSet wd = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = out_set,
                                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                .pBufferInfo = &bi };
    VK(vkUpdateDescriptorSets)(g.dev, 1, &wd, 0, NULL);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = g.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK_EQ(VK(vkAllocateCommandBuffers)(g.dev, &cai, &cb), VK_SUCCESS);
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer)(cb, &cbi);
    VK(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    VkDescriptorSet sets[2] = { nvn_texset(), out_set };
    VK(vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 2, sets, 0, NULL);
    uint32_t pc[3] = { id, w, samples };
    VK(vkCmdPushConstants)(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    VK(vkCmdDispatch)(cb, w, h, 1);
    VK(vkEndCommandBuffer)(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK(vkResetFences)(g.dev, 1, &g.fence);
    CHECK_EQ(VK(vkQueueSubmit)(g.queue, 1, &si, g.fence), VK_SUCCESS);
    CHECK_EQ(VK(vkWaitForFences)(g.dev, 1, &g.fence, VK_TRUE, 10000000000ull), VK_SUCCESS);
    memcpy(out, map, size);
    VK(vkFreeCommandBuffers)(g.dev, g.pool, 1, &cb);
    VK(vkDestroyPipeline)(g.dev, pipe, NULL);
    VK(vkDestroyShaderModule)(g.dev, cs, NULL);
    VK(vkDestroyPipelineLayout)(g.dev, pl, NULL);
    VK(vkDestroyDescriptorPool)(g.dev, dpool, NULL);
    VK(vkDestroyDescriptorSetLayout)(g.dev, out_layout, NULL);
    VK(vkUnmapMemory)(g.dev, mem);
    VK(vkFreeMemory)(g.dev, mem, NULL);
    VK(vkDestroyBuffer)(g.dev, buf, NULL);
}

/** @brief The word the test puts in the guest texel (x, y) of a twin layout. */
static uint32_t twin_word(uint32_t x, uint32_t y, uint32_t samples) { return 0x80000000u | samples << 24 | y << 12 | x; }

TEST(nvn_multisample_texels_move_between_guest_storage_and_each_sample_both_ways)
{
    static const uint32_t counts[3] = { 2, 4, 8 };
    for (uint32_t c = 0; c < 3; c++) {
        static rig k;
        const uint64_t pool = 0x130, tex = 0x230;
        if (!rig_up(&k, pool))
            return;
        CHECK(nvn_texset_layout());
        uint32_t n = counts[c], gw, gh, w = 5, h = 3;
        uint64_t cells;
        if (!host_samples(n))
            continue;
        CHECK(nvn_msaa_cells(n, &gw, &gh, &cells));
        nvn_tex_desc d = { .target = NVN_TARGET_2D_MULTISAMPLE, .format = 37, .width = w, .height = h, .depth = 1,
                           .levels = 1, .samples = n, .swizzle = { 2, 3, 4, 5 }, .pool = pool, .offset = 0x4000 };
        CHECK(nvn_tex_layout(&d));
        nvn_tex_desc t = d;
        t.target = 1;
        t.samples = 0;
        t.width = w * gw;
        t.height = h * gh;
        CHECK(nvn_tex_layout(&t));
        CHECK_EQ(t.size, d.size);
        uint32_t packed[5 * 4 * 3 * 2];
        for (uint32_t y = 0; y < t.height; y++)
            for (uint32_t x = 0; x < t.width; x++)
                packed[y * t.width + x] = twin_word(x, y, n);
        nvn_region rg = { 0, 0, 0, (int32_t)t.width, (int32_t)t.height, 1 };
        CHECK(nvn_tex_copy(&t, k.mem + d.offset, 0, 0, &rg, (uint8_t *)packed, true));
        CHECK(nvn_texture_record(tex, &d, NULL));
        k.c.x[1] = 11;
        k.c.x[2] = tex;
        k.c.x[3] = 0;
        nvnh_nvnTexturePoolRegisterTexture(&k.c, k.mem);
        CHECK_EQ(nvn_texset_sync_registered(&k.r), RL_OK);
        vimg *v = nvn_vk_image(&k.r, tex);
        CHECK(v != NULL);
        CHECK(nvn_vk_flush(&k.r));
        if (!v)
            return;
        uint32_t got[5 * 3 * 8];
        fetch_all(11, w, h, n, got);
        uint32_t bad = 0;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                for (uint32_t s = 0; s < n; s++) {
                    uint32_t cell = nvk_cell(n, s);
                    bad += got[(y * w + x) * n + s] != twin_word(x * gw + (cell & 3u), y * gh + (cell >> 2), n);
                }
        CHECK_EQ(bad, 0u);
        memset(k.mem + d.offset, 0, d.size);
        CHECK(nvn_msaa_write_back(&k.r, v));
        CHECK(nvn_vk_flush(&k.r));
        uint32_t back[5 * 4 * 3 * 2];
        memset(back, 0, sizeof back);
        CHECK(nvn_tex_copy(&t, k.mem + d.offset, 0, 0, &rg, (uint8_t *)back, false));
        CHECK(!memcmp(back, packed, (size_t)t.width * t.height * 4));
    }
}

TEST(nvn_a_drawn_sample_is_stored_in_its_grid_cell)
{
    static rig k;
    const uint64_t pool = 0x140, tex = 0x240;
    if (!rig_up(&k, pool))
        return;
    if (!host_samples(8)) {
        fprintf(stderr, "nvn_a_drawn_sample_is_stored_in_its_grid_cell: the host device has no 8-sample color targets, skipped\n");
        return;
    }
    nvn_tex_desc d = { .target = NVN_TARGET_2D_MULTISAMPLE, .format = 37, .width = 4, .height = 4, .depth = 1,
                       .levels = 1, .samples = 8, .pool = pool, .offset = 0x2000 };
    CHECK(nvn_tex_layout(&d));
    CHECK(nvn_texture_record(tex, &d, NULL));
    vimg *v = nvn_vk_image(&k.r, tex);
    CHECK(v != NULL);
    CHECK(nvn_vk_flush(&k.r));
    if (!v)
        return;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = g.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK_EQ(VK(vkAllocateCommandBuffers)(g.dev, &cai, &cb), VK_SUCCESS);
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer)(cb, &cbi);
    VkPipeline gpipe = NULL;
    VkPipelineLayout gpl = NULL;
    CHECK(fill_samples(cb, v, &gpipe, &gpl));
    nvn_vk_barrier(cb);
    VK(vkEndCommandBuffer)(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK(vkResetFences)(g.dev, 1, &g.fence);
    CHECK_EQ(VK(vkQueueSubmit)(g.queue, 1, &si, g.fence), VK_SUCCESS);
    CHECK_EQ(VK(vkWaitForFences)(g.dev, 1, &g.fence, VK_TRUE, 10000000000ull), VK_SUCCESS);
    CHECK(nvn_msaa_write_back(&k.r, v));
    CHECK(nvn_vk_flush(&k.r));
    nvn_tex_desc t = d;
    t.target = 1;
    t.samples = 0;
    t.width = 16;
    t.height = 8;
    CHECK(nvn_tex_layout(&t));
    uint32_t px[16 * 8];
    nvn_region rg = { 0, 0, 0, 16, 8, 1 };
    CHECK(nvn_tex_copy(&t, k.mem + d.offset, 0, 0, &rg, (uint8_t *)px, false));
    for (uint32_t s = 0; s < 8; s++) {
        uint32_t cell = nvk_cell(8, s);
        CHECK_EQ(px[(4 + (cell >> 2)) * 16 + 8 + (cell & 3u)], 0xff000000u | (7u - s) * 8u << 8 | (s + 1u) * 16u);
    }
    VK(vkFreeCommandBuffers)(g.dev, g.pool, 1, &cb);
    VK(vkDestroyPipeline)(g.dev, gpipe, NULL);
    VK(vkDestroyPipelineLayout)(g.dev, gpl, NULL);
}

TEST(nvn_downsample_averages_the_drivers_taps_of_each_sample_count)
{
    static const uint32_t counts[3] = { 2, 4, 8 };
    for (uint32_t c = 0; c < 3; c++) {
        static rig k;
        const uint64_t pool = 0x150, src = 0x250, dst = 0x350;
        if (!rig_up(&k, pool))
            return;
        uint32_t n = counts[c], gw, gh, taps, ntaps, w = 4, h = 2;
        uint64_t cells;
        if (!host_samples(n))
            continue;
        CHECK(nvn_msaa_cells(n, &gw, &gh, &cells));
        CHECK(nvn_msaa_downsample_taps(n, &taps, &ntaps));
        nvn_tex_desc d = { .target = NVN_TARGET_2D_MULTISAMPLE, .format = 37, .width = w, .height = h, .depth = 1,
                           .levels = 1, .samples = n, .pool = pool, .offset = 0x4000 };
        CHECK(nvn_tex_layout(&d));
        nvn_tex_desc t = d;
        t.target = 1;
        t.samples = 0;
        t.width = w * gw;
        t.height = h * gh;
        CHECK(nvn_tex_layout(&t));
        /* sample s of pixel (x, y): red 8 * (s + 1), green 4 * (x + 1) * (s + 1), blue 16 * y, alpha 255 - 4 * s */
        uint8_t texels[4 * 2 * 8 * 4];
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                for (uint32_t s = 0; s < n; s++) {
                    uint32_t cell = nvk_cell(n, s), tx = x * gw + (cell & 3u), ty = y * gh + (cell >> 2);
                    uint8_t *p = texels + (ty * t.width + tx) * 4;
                    p[0] = (uint8_t)(8 * (s + 1));
                    p[1] = (uint8_t)(4 * (x + 1) * (s + 1));
                    p[2] = (uint8_t)(16 * y);
                    p[3] = (uint8_t)(255 - 4 * s);
                }
        nvn_region rg = { 0, 0, 0, (int32_t)t.width, (int32_t)t.height, 1 };
        CHECK(nvn_tex_copy(&t, k.mem + d.offset, 0, 0, &rg, texels, true));
        nvn_tex_desc o = { .target = 1, .format = 37, .width = w, .height = h, .depth = 1, .levels = 1, .pool = pool,
                           .offset = 0xc000 };
        CHECK(nvn_tex_layout(&o));
        CHECK(nvn_texture_record(src, &d, NULL));
        CHECK(nvn_texture_record(dst, &o, NULL));
        vimg *sv = nvn_vk_image(&k.r, src), *dv = nvn_vk_image(&k.r, dst);
        CHECK(sv != NULL && dv != NULL);
        if (!sv || !dv)
            return;
        VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = dv->image,
                                      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = dv->format,
                                      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VkImageView target;
        CHECK_EQ(VK(vkCreateImageView)(g.dev, &vci, NULL, &target), VK_SUCCESS);
        const char *why = "";
        CHECK(nvn_msaa_resolve(&k.r, sv, target, dv->format, w, h, &why));
        nvn_vk_written(dv);
        uint8_t *px = nvn_vk_read_rgba8(&k.r, dv);
        CHECK(px != NULL);
        uint32_t bad = 0;
        for (uint32_t y = 0; px && y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t sum[4] = { 0, 0, 0, 0 };
                for (uint32_t i = 0; i < ntaps; i++) {
                    uint32_t s = taps >> (4 * i) & 15u;
                    sum[0] += 8 * (s + 1);
                    sum[1] += 4 * (x + 1) * (s + 1);
                    sum[2] += 16 * y;
                    sum[3] += 255 - 4 * s;
                }
                for (uint32_t ch = 0; ch < 4; ch++)
                    bad += px[(y * w + x) * 4 + ch] != sum[ch] / ntaps;
            }
        CHECK_EQ(bad, 0u);
        free(px);
        VK(vkDestroyImageView)(g.dev, target, NULL);
    }
}

TEST(nvn_downsample_taps_are_the_middle_of_the_grid)
{
    uint32_t taps, n;
    CHECK(nvn_msaa_downsample_taps(2, &taps, &n));
    CHECK_EQ(n, 2u);
    CHECK(nvn_msaa_downsample_taps(4, &taps, &n));
    CHECK_EQ(n, 4u);
    CHECK(nvn_msaa_downsample_taps(8, &taps, &n));
    CHECK_EQ(n, 4u);
    uint32_t cells = 0;
    for (uint32_t i = 0; i < 4; i++)
        cells |= 1u << nvk_cell(8, taps >> (4 * i) & 15u);
    CHECK_EQ(cells, 1u << 1 | 1u << 2 | 1u << 5 | 1u << 6);
    CHECK(nvn_msaa_downsample_taps(16, &taps, &n));
    CHECK_EQ(n, 4u);
    cells = 0;
    for (uint32_t i = 0; i < 4; i++)
        cells |= 1u << nvk_cell(16, taps >> (4 * i) & 15u);
    CHECK_EQ(cells, 1u << 5 | 1u << 6 | 1u << 9 | 1u << 10);
    CHECK(!nvn_msaa_downsample_taps(32, &taps, &n));
}

/**
 * @brief edge.vert, by `glslc --target-env=vulkan1.3 -O`:
 *
 * #version 450
 * // The half plane left of (axis 0) or above (axis 1) the edge at e pixels of a 1x1 target, as two triangles.
 * layout(push_constant) uniform P { float e; uint axis; } p;
 * void main()
 * {
 *     const vec2 c[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
 *     vec2 q = c[gl_VertexIndex];
 *     float lo = -3.0, hi = 2.0 * p.e - 1.0;
 *     vec2 v = p.axis == 0u ? vec2(mix(lo, hi, q.x), mix(-3.0, 3.0, q.y)) : vec2(mix(-3.0, 3.0, q.x), mix(lo, hi, q.y));
 *     gl_Position = vec4(v, 0.0, 1.0);
 * }
 */
static const uint32_t edge_vert_spv[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x0000005b, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0008000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x00000016, 0x00000023, 0x0000004f,
    0x00040047, 0x00000016, 0x0000000b, 0x0000002a, 0x00030047, 0x00000019, 0x00000018, 0x00030047,
    0x00000021, 0x00000002, 0x00050048, 0x00000021, 0x00000000, 0x00000023, 0x00000000, 0x00050048,
    0x00000021, 0x00000001, 0x00000023, 0x00000004, 0x00030047, 0x0000004d, 0x00000002, 0x00050048,
    0x0000004d, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000004d, 0x00000001, 0x0000000b,
    0x00000001, 0x00050048, 0x0000004d, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000004d,
    0x00000003, 0x0000000b, 0x00000004, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020,
    0x00000008, 0x00000007, 0x00000007, 0x00040015, 0x0000000a, 0x00000020, 0x00000000, 0x0004002b,
    0x0000000a, 0x0000000b, 0x00000006, 0x0004001c, 0x0000000c, 0x00000007, 0x0000000b, 0x0004002b,
    0x00000006, 0x0000000d, 0x00000000, 0x0005002c, 0x00000007, 0x0000000e, 0x0000000d, 0x0000000d,
    0x0004002b, 0x00000006, 0x0000000f, 0x3f800000, 0x0005002c, 0x00000007, 0x00000010, 0x0000000f,
    0x0000000d, 0x0005002c, 0x00000007, 0x00000011, 0x0000000d, 0x0000000f, 0x0005002c, 0x00000007,
    0x00000012, 0x0000000f, 0x0000000f, 0x0009002c, 0x0000000c, 0x00000013, 0x0000000e, 0x00000010,
    0x00000011, 0x00000010, 0x00000012, 0x00000011, 0x00040015, 0x00000014, 0x00000020, 0x00000001,
    0x00040020, 0x00000015, 0x00000001, 0x00000014, 0x0004003b, 0x00000015, 0x00000016, 0x00000001,
    0x00040020, 0x00000018, 0x00000007, 0x0000000c, 0x0004002b, 0x00000006, 0x0000001e, 0xc0400000,
    0x0004002b, 0x00000006, 0x00000020, 0x40000000, 0x0004001e, 0x00000021, 0x00000006, 0x0000000a,
    0x00040020, 0x00000022, 0x00000009, 0x00000021, 0x0004003b, 0x00000022, 0x00000023, 0x00000009,
    0x0004002b, 0x00000014, 0x00000024, 0x00000000, 0x00040020, 0x00000025, 0x00000009, 0x00000006,
    0x0004002b, 0x00000014, 0x0000002b, 0x00000001, 0x00040020, 0x0000002c, 0x00000009, 0x0000000a,
    0x0004002b, 0x0000000a, 0x0000002f, 0x00000000, 0x00020014, 0x00000030, 0x0004002b, 0x00000006,
    0x0000003a, 0x40400000, 0x0004002b, 0x0000000a, 0x0000003b, 0x00000001, 0x00040017, 0x0000004b,
    0x00000006, 0x00000004, 0x0004001c, 0x0000004c, 0x00000006, 0x0000003b, 0x0006001e, 0x0000004d,
    0x0000004b, 0x00000006, 0x0000004c, 0x0000004c, 0x00040020, 0x0000004e, 0x00000003, 0x0000004d,
    0x0004003b, 0x0000004e, 0x0000004f, 0x00000003, 0x00040020, 0x00000054, 0x00000003, 0x0000004b,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0005003b,
    0x00000018, 0x00000019, 0x00000007, 0x00000013, 0x0004003d, 0x00000014, 0x00000017, 0x00000016,
    0x00050041, 0x00000008, 0x0000001a, 0x00000019, 0x00000017, 0x0004003d, 0x00000007, 0x0000001b,
    0x0000001a, 0x00050041, 0x00000025, 0x00000026, 0x00000023, 0x00000024, 0x0004003d, 0x00000006,
    0x00000027, 0x00000026, 0x00050085, 0x00000006, 0x00000028, 0x00000020, 0x00000027, 0x00050083,
    0x00000006, 0x00000029, 0x00000028, 0x0000000f, 0x00050041, 0x0000002c, 0x0000002d, 0x00000023,
    0x0000002b, 0x0004003d, 0x0000000a, 0x0000002e, 0x0000002d, 0x000500aa, 0x00000030, 0x00000031,
    0x0000002e, 0x0000002f, 0x000300f7, 0x00000034, 0x00000000, 0x000400fa, 0x00000031, 0x00000033,
    0x00000040, 0x000200f8, 0x00000033, 0x00050051, 0x00000006, 0x00000038, 0x0000001b, 0x00000000,
    0x0008000c, 0x00000006, 0x00000039, 0x00000001, 0x0000002e, 0x0000001e, 0x00000029, 0x00000038,
    0x00050051, 0x00000006, 0x0000003d, 0x0000001b, 0x00000001, 0x0008000c, 0x00000006, 0x0000003e,
    0x00000001, 0x0000002e, 0x0000001e, 0x0000003a, 0x0000003d, 0x00050050, 0x00000007, 0x0000003f,
    0x00000039, 0x0000003e, 0x000200f9, 0x00000034, 0x000200f8, 0x00000040, 0x00050051, 0x00000006,
    0x00000042, 0x0000001b, 0x00000000, 0x0008000c, 0x00000006, 0x00000043, 0x00000001, 0x0000002e,
    0x0000001e, 0x0000003a, 0x00000042, 0x00050051, 0x00000006, 0x00000047, 0x0000001b, 0x00000001,
    0x0008000c, 0x00000006, 0x00000048, 0x00000001, 0x0000002e, 0x0000001e, 0x00000029, 0x00000047,
    0x00050050, 0x00000007, 0x00000049, 0x00000043, 0x00000048, 0x000200f9, 0x00000034, 0x000200f8,
    0x00000034, 0x000700f5, 0x00000007, 0x0000005a, 0x0000003f, 0x00000033, 0x00000049, 0x00000040,
    0x00050051, 0x00000006, 0x00000051, 0x0000005a, 0x00000000, 0x00050051, 0x00000006, 0x00000052,
    0x0000005a, 0x00000001, 0x00070050, 0x0000004b, 0x00000053, 0x00000051, 0x00000052, 0x0000000d,
    0x0000000f, 0x00050041, 0x00000054, 0x00000055, 0x0000004f, 0x00000024, 0x0003003e, 0x00000055,
    0x00000053, 0x000100fd, 0x00010038,
};

/**
 * @brief mask.comp, by `glslc --target-env=vulkan1.3 -O`:
 *
 * #version 450
 * #extension GL_EXT_samplerless_texture_functions : require
 * // The mask of the samples of pixel (0, 0) the draw covered.
 * layout(local_size_x = 1) in;
 * layout(binding = 0) uniform texture2DMS ms;
 * layout(binding = 1) buffer Out { uint o; };
 * layout(push_constant) uniform P { uint samples; } p;
 * void main()
 * {
 *     uint m = 0u;
 *     for (uint s = 0u; s < p.samples; s++)
 *         m |= (texelFetch(ms, ivec2(0), int(s)).r > 0.5 ? 1u : 0u) << s;
 *     o = m;
 * }
 */
static const uint32_t mask_comp_spv[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x0000003c, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0008000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x00000013, 0x0000001e, 0x00000034,
    0x00060010, 0x00000004, 0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x00030047, 0x00000011,
    0x00000002, 0x00050048, 0x00000011, 0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x0000001e,
    0x00000021, 0x00000000, 0x00040047, 0x0000001e, 0x00000022, 0x00000000, 0x00030047, 0x00000032,
    0x00000002, 0x00050048, 0x00000032, 0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x00000034,
    0x00000021, 0x00000001, 0x00040047, 0x00000034, 0x00000022, 0x00000000, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x0004002b,
    0x00000006, 0x00000009, 0x00000000, 0x0003001e, 0x00000011, 0x00000006, 0x00040020, 0x00000012,
    0x00000009, 0x00000011, 0x0004003b, 0x00000012, 0x00000013, 0x00000009, 0x00040015, 0x00000014,
    0x00000020, 0x00000001, 0x0004002b, 0x00000014, 0x00000015, 0x00000000, 0x00040020, 0x00000016,
    0x00000009, 0x00000006, 0x00020014, 0x00000019, 0x00030016, 0x0000001b, 0x00000020, 0x00090019,
    0x0000001c, 0x0000001b, 0x00000001, 0x00000000, 0x00000000, 0x00000001, 0x00000001, 0x00000000,
    0x00040020, 0x0000001d, 0x00000000, 0x0000001c, 0x0004003b, 0x0000001d, 0x0000001e, 0x00000000,
    0x00040017, 0x00000020, 0x00000014, 0x00000002, 0x0005002c, 0x00000020, 0x00000021, 0x00000015,
    0x00000015, 0x00040017, 0x00000024, 0x0000001b, 0x00000004, 0x0004002b, 0x0000001b, 0x00000027,
    0x3f000000, 0x0004002b, 0x00000006, 0x00000029, 0x00000001, 0x0004002b, 0x00000014, 0x00000030,
    0x00000001, 0x0003001e, 0x00000032, 0x00000006, 0x00040020, 0x00000033, 0x0000000c, 0x00000032,
    0x0004003b, 0x00000033, 0x00000034, 0x0000000c, 0x00040020, 0x00000036, 0x0000000c, 0x00000006,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x000200f9,
    0x0000000b, 0x000200f8, 0x0000000b, 0x000700f5, 0x00000006, 0x0000003b, 0x00000009, 0x00000005,
    0x0000002e, 0x0000000c, 0x000700f5, 0x00000006, 0x0000003a, 0x00000009, 0x00000005, 0x00000031,
    0x0000000c, 0x00050041, 0x00000016, 0x00000017, 0x00000013, 0x00000015, 0x0004003d, 0x00000006,
    0x00000018, 0x00000017, 0x000500b0, 0x00000019, 0x0000001a, 0x0000003a, 0x00000018, 0x000400f6,
    0x0000000d, 0x0000000c, 0x00000000, 0x000400fa, 0x0000001a, 0x0000000c, 0x0000000d, 0x000200f8,
    0x0000000c, 0x0004003d, 0x0000001c, 0x0000001f, 0x0000001e, 0x0004007c, 0x00000014, 0x00000023,
    0x0000003a, 0x0007005f, 0x00000024, 0x00000025, 0x0000001f, 0x00000021, 0x00000040, 0x00000023,
    0x00050051, 0x0000001b, 0x00000026, 0x00000025, 0x00000000, 0x000500ba, 0x00000019, 0x00000028,
    0x00000026, 0x00000027, 0x000600a9, 0x00000006, 0x0000002a, 0x00000028, 0x00000029, 0x00000009,
    0x000500c4, 0x00000006, 0x0000002c, 0x0000002a, 0x0000003a, 0x000500c5, 0x00000006, 0x0000002e,
    0x0000003b, 0x0000002c, 0x00050080, 0x00000006, 0x00000031, 0x0000003a, 0x00000030, 0x000200f9,
    0x0000000b, 0x000200f8, 0x0000000d, 0x00050041, 0x00000036, 0x00000037, 0x00000034, 0x00000015,
    0x0003003e, 0x00000037, 0x0000003b, 0x000100fd, 0x00010038,
};

/**
 * @brief one.frag, by `glslc --target-env=vulkan1.3 -O`:
 *
 * #version 450
 * layout(location = 0) out vec4 o;
 * void main() { o = vec4(1.0); }
 */
static const uint32_t one_frag_spv[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x0000000c, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009,
    0x00000003, 0x0004002b, 0x00000006, 0x0000000a, 0x3f800000, 0x0007002c, 0x00000007, 0x0000000b,
    0x0000000a, 0x0000000a, 0x0000000a, 0x0000000a, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0003003e, 0x00000009, 0x0000000b, 0x000100fd, 0x00010038,
};

/** @brief A 1x1 target of n samples with a pipeline drawing the half plane before an edge at locs, and a readback. */
typedef struct loc_rig {
    VkImage img;
    VkDeviceMemory img_mem, buf_mem;
    VkImageView view;
    VkBuffer buf;
    uint32_t *map;
    VkPipelineLayout gpl, cpl;
    VkPipeline gpipe, cpipe;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet set;
} loc_rig;

static void loc_rig_up(loc_rig *k, uint32_t n, const VkSampleLocationEXT *locs)
{
    memset(k, 0, sizeof *k);
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 1, 1, 1 },
                              .mipLevels = 1, .arrayLayers = 1, .samples = (VkSampleCountFlagBits)n,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
    CHECK_EQ(VK(vkCreateImage)(g.dev, &ici, NULL, &k->img), VK_SUCCESS);
    VkMemoryRequirements req;
    VK(vkGetImageMemoryRequirements)(g.dev, k->img, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)nvn_vk_mem_type(req.memoryTypeBits,
                                                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CHECK_EQ(VK(vkAllocateMemory)(g.dev, &mai, NULL, &k->img_mem), VK_SUCCESS);
    CHECK_EQ(VK(vkBindImageMemory)(g.dev, k->img, k->img_mem, 0), VK_SUCCESS);
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = k->img,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                  .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CHECK_EQ(VK(vkCreateImageView)(g.dev, &vci, NULL, &k->view), VK_SUCCESS);
    CHECK(host_buffer(256, &k->buf, &k->buf_mem, &k->map));
    VkPushConstantRange gpr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &gpr };
    CHECK_EQ(VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &k->gpl), VK_SUCCESS);
    VkShaderModule vs = module_of(edge_vert_spv, sizeof edge_vert_spv), fs = module_of(one_frag_spv, sizeof one_frag_spv);
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, 1, 1, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { 1, 1 } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                              .viewportCount = 1, .pViewports = &vp, .scissorCount = 1,
                                              .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1 };
    VkPipelineSampleLocationsStateCreateInfoEXT sl = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT,
        .sampleLocationsEnable = VK_TRUE,
        .sampleLocationsInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
                                 .sampleLocationsPerPixel = (VkSampleCountFlagBits)n,
                                 .sampleLocationGridSize = { 1, 1 }, .sampleLocationsCount = n,
                                 .pSampleLocations = locs },
    };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .pNext = &sl, .rasterizationSamples = (VkSampleCountFlagBits)n };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                .attachmentCount = 1, .pAttachments = &ba };
    VkFormat f = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                         .colorAttachmentCount = 1, .pColorAttachmentFormats = &f };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                         .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                         .pInputAssemblyState = &ia, .pViewportState = &vps,
                                         .pRasterizationState = &rs, .pMultisampleState = &ms,
                                         .pColorBlendState = &cbs, .layout = k->gpl };
    CHECK_EQ(VK(vkCreateGraphicsPipelines)(g.dev, NULL, 1, &gpi, NULL, &k->gpipe), VK_SUCCESS);
    VK(vkDestroyShaderModule)(g.dev, vs, NULL);
    VK(vkDestroyShaderModule)(g.dev, fs, NULL);
    VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                                          { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 2, .pBindings = b };
    CHECK_EQ(VK(vkCreateDescriptorSetLayout)(g.dev, &li, NULL, &k->dsl), VK_SUCCESS);
    VkPushConstantRange cpr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo cli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                       .pSetLayouts = &k->dsl, .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &cpr };
    CHECK_EQ(VK(vkCreatePipelineLayout)(g.dev, &cli, NULL, &k->cpl), VK_SUCCESS);
    VkShaderModule cs = module_of(mask_comp_spv, sizeof mask_comp_spv);
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs,
                                                   .pName = "main" },
                                        .layout = k->cpl };
    CHECK_EQ(VK(vkCreateComputePipelines)(g.dev, NULL, 1, &cpi, NULL, &k->cpipe), VK_SUCCESS);
    VK(vkDestroyShaderModule)(g.dev, cs, NULL);
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                       .poolSizeCount = 2, .pPoolSizes = ps };
    CHECK_EQ(VK(vkCreateDescriptorPool)(g.dev, &dpi, NULL, &k->pool), VK_SUCCESS);
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = k->pool, .descriptorSetCount = 1, .pSetLayouts = &k->dsl };
    CHECK_EQ(VK(vkAllocateDescriptorSets)(g.dev, &dai, &k->set), VK_SUCCESS);
    VkDescriptorImageInfo ii = { NULL, k->view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorBufferInfo bi = { k->buf, 0, 256 };
    VkWriteDescriptorSet w[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = k->set, .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = k->set, .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi },
    };
    VK(vkUpdateDescriptorSets)(g.dev, 2, w, 0, NULL);
}

static void loc_rig_down(loc_rig *k)
{
    VK(vkDestroyDescriptorPool)(g.dev, k->pool, NULL);
    VK(vkDestroyPipeline)(g.dev, k->cpipe, NULL);
    VK(vkDestroyPipeline)(g.dev, k->gpipe, NULL);
    VK(vkDestroyPipelineLayout)(g.dev, k->cpl, NULL);
    VK(vkDestroyPipelineLayout)(g.dev, k->gpl, NULL);
    VK(vkDestroyDescriptorSetLayout)(g.dev, k->dsl, NULL);
    VK(vkDestroyImageView)(g.dev, k->view, NULL);
    VK(vkDestroyImage)(g.dev, k->img, NULL);
    VK(vkFreeMemory)(g.dev, k->img_mem, NULL);
    VK(vkUnmapMemory)(g.dev, k->buf_mem);
    VK(vkDestroyBuffer)(g.dev, k->buf, NULL);
    VK(vkFreeMemory)(g.dev, k->buf_mem, NULL);
}

/** @brief The samples the half plane before edge e (pixels) on axis covers, into *mask. */
static void edge_mask(loc_rig *k, uint32_t n, float e, uint32_t axis, uint32_t *mask)
{
    *mask = 0;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = g.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK_EQ(VK(vkAllocateCommandBuffers)(g.dev, &cai, &cb), VK_SUCCESS);
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer)(cb, &cbi);
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = k->img,
                                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                             NULL, 1, &ib);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = k->view,
                                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { 1, 1 } },
                           .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    VK(vkCmdBeginRendering)(cb, &ri);
    VK(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, k->gpipe);
    struct {
        float e;
        uint32_t axis;
    } pc = { e, axis };
    VK(vkCmdPushConstants)(cb, k->gpl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, &pc);
    VK(vkCmdDraw)(cb, 6, 1, 0, 0);
    VK(vkCmdEndRendering)(cb);
    nvn_vk_barrier(cb);
    VK(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, k->cpipe);
    VK(vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, k->cpl, 0, 1, &k->set, 0, NULL);
    VK(vkCmdPushConstants)(cb, k->cpl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &n);
    VK(vkCmdDispatch)(cb, 1, 1, 1);
    nvn_vk_barrier(cb);
    VK(vkEndCommandBuffer)(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK(vkResetFences)(g.dev, 1, &g.fence);
    CHECK_EQ(VK(vkQueueSubmit)(g.queue, 1, &si, g.fence), VK_SUCCESS);
    CHECK_EQ(VK(vkWaitForFences)(g.dev, 1, &g.fence, VK_TRUE, 10000000000ull), VK_SUCCESS);
    VK(vkFreeCommandBuffers)(g.dev, g.pool, 1, &cb);
    *mask = *k->map;
}

TEST(nvn_programmable_sample_locations_land_on_nvns_sixteenths)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    if (!g.sample_locations || !g.sample_location_counts) {
        fprintf(stderr, "nvn_programmable_sample_locations_land_on_nvns_sixteenths: no exact sample locations, skipped\n");
        return;
    }
    VkImageFormatProperties ifp;
    CHECK_EQ(VK(vkGetPhysicalDeviceImageFormatProperties)(g.phys, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                                          VK_IMAGE_TILING_OPTIMAL,
                                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                              VK_IMAGE_USAGE_SAMPLED_BIT,
                                                          0, &ifp),
             VK_SUCCESS);
    uint32_t seed = 0x2468ace1u;
    for (uint32_t n = 2; n <= 16; n *= 2) {
        if (!(g.sample_location_counts & ifp.sampleCounts & n))
            continue;
        for (uint32_t round = 0; round < 3; round++) {
            uint8_t nib[16];
            for (uint32_t s = 0; s < n; s++) {
                seed = seed * 1103515245u + 12345u;
                nib[s] = (uint8_t)(seed >> 16);
            }
            VkSampleLocationEXT locs[16];
            nvn_vk_sample_locations(nib, n, locs);
            static loc_rig k;
            loc_rig_up(&k, n, locs);
            uint8_t at[16][2];
            memset(at, 0xff, sizeof at);
            for (uint32_t axis = 0; axis < 2; axis++)
                for (uint32_t e = 0; e <= 16; e++) {
                    uint32_t m;
                    edge_mask(&k, n, ((float)e + 0.5f) / 16.0f, axis, &m);
                    for (uint32_t s = 0; s < n; s++)
                        if ((m >> s & 1) && at[s][axis] == 0xff)
                            at[s][axis] = (uint8_t)e;
                }
            for (uint32_t s = 0; s < n; s++) {
                CHECK_EQ(at[s][0], nib[s] & 15u);
                CHECK_EQ(at[s][1], nib[s] >> 4);
            }
            loc_rig_down(&k);
        }
    }
}

/**
 * @brief two.frag, by `glslc --target-env=vulkan1.3 -O`:
 *
 * #version 450
 * layout(location = 0) out uvec4 c;
 * layout(location = 1) out vec4 m;
 * void main() { c = uvec4(0u); m = vec4(1.0); }
 */
static const uint32_t two_frag_spv[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x00000012, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000f, 0x00030010,
    0x00000004, 0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000f,
    0x0000001e, 0x00000001, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015,
    0x00000006, 0x00000020, 0x00000000, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020,
    0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x0004002b,
    0x00000006, 0x0000000a, 0x00000000, 0x0007002c, 0x00000007, 0x0000000b, 0x0000000a, 0x0000000a,
    0x0000000a, 0x0000000a, 0x00030016, 0x0000000c, 0x00000020, 0x00040017, 0x0000000d, 0x0000000c,
    0x00000004, 0x00040020, 0x0000000e, 0x00000003, 0x0000000d, 0x0004003b, 0x0000000e, 0x0000000f,
    0x00000003, 0x0004002b, 0x0000000c, 0x00000010, 0x3f800000, 0x0007002c, 0x0000000d, 0x00000011,
    0x00000010, 0x00000010, 0x00000010, 0x00000010, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0003003e, 0x00000009, 0x0000000b, 0x0003003e, 0x0000000f,
    0x00000011, 0x000100fd, 0x00010038,
};

/** @brief A 1x1 single-sample image of format f for a mixed-samples draw, read back through buf. */
static void tir_image(VkFormat f, VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                              .extent = { 1, 1, 1 }, .mipLevels = 1, .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    CHECK_EQ(VK(vkCreateImage)(g.dev, &ici, NULL, img), VK_SUCCESS);
    VkMemoryRequirements req;
    VK(vkGetImageMemoryRequirements)(g.dev, *img, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)nvn_vk_mem_type(req.memoryTypeBits,
                                                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CHECK_EQ(VK(vkAllocateMemory)(g.dev, &mai, NULL, mem), VK_SUCCESS);
    CHECK_EQ(VK(vkBindImageMemory)(g.dev, *img, *mem, 0), VK_SUCCESS);
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                  .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CHECK_EQ(VK(vkCreateImageView)(g.dev, &vci, NULL, view), VK_SUCCESS);
}

/**
 * @brief Draws the half plane left of an edge at e pixels with raster samples n into two single-sample targets: an
 * R32_UINT one that coverage to color writes, and an RGBA8 one of white modulated by coverage (RGBA). Out: the mask
 * and the modulated red byte.
 */
static void tir_draw(uint32_t n, float e, uint32_t *mask, uint32_t *red)
{
    VkImage img[2];
    VkDeviceMemory mem[2];
    VkImageView view[2];
    VkFormat fmt[2] = { VK_FORMAT_R32_UINT, VK_FORMAT_R8G8B8A8_UNORM };
    for (uint32_t i = 0; i < 2; i++)
        tir_image(fmt[i], &img[i], &mem[i], &view[i]);
    VkBuffer buf;
    VkDeviceMemory bmem;
    uint32_t *map;
    CHECK(host_buffer(256, &buf, &bmem, &map));
    VkPushConstantRange gpr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &gpr };
    VkPipelineLayout pl;
    CHECK_EQ(VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &pl), VK_SUCCESS);
    VkShaderModule vs = module_of(edge_vert_spv, sizeof edge_vert_spv), fs = module_of(two_frag_spv, sizeof two_frag_spv);
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, 1, 1, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { 1, 1 } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                              .viewportCount = 1, .pViewports = &vp, .scissorCount = 1,
                                              .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1 };
    /* the same structures the layer chains (nvn_pipe.c), by their Vulkan values */
    struct {
        VkStructureType sType;
        const void *pNext;
        VkFlags flags;
        VkBool32 enable;
        uint32_t location;
    } c2c = { (VkStructureType)1000149000, NULL, 0, VK_TRUE, 0 };
    struct {
        VkStructureType sType;
        const void *pNext;
        VkFlags flags;
        int mode;
        VkBool32 table;
        uint32_t count;
        const float *values;
    } cm = { (VkStructureType)1000152000, &c2c, 0, 3, VK_FALSE, 0, NULL };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .pNext = &cm, .rasterizationSamples = (VkSampleCountFlagBits)n };
    VkPipelineColorBlendAttachmentState ba[2] = { { .colorWriteMask = 0xf }, { .colorWriteMask = 0xf } };
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                .attachmentCount = 2, .pAttachments = ba };
    VkSampleCountFlagBits one[2] = { VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_1_BIT };
    struct {
        VkStructureType sType;
        const void *pNext;
        uint32_t count;
        const VkSampleCountFlagBits *colors;
        VkSampleCountFlagBits depth;
    } counts = { (VkStructureType)1000044008, NULL, 2, one, (VkSampleCountFlagBits)n };
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .pNext = &counts,
                                         .colorAttachmentCount = 2, .pColorAttachmentFormats = fmt };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                         .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                         .pInputAssemblyState = &ia, .pViewportState = &vps,
                                         .pRasterizationState = &rs, .pMultisampleState = &ms,
                                         .pColorBlendState = &cbs, .layout = pl };
    VkPipeline pipe;
    CHECK_EQ(VK(vkCreateGraphicsPipelines)(g.dev, NULL, 1, &gpi, NULL, &pipe), VK_SUCCESS);
    VK(vkDestroyShaderModule)(g.dev, vs, NULL);
    VK(vkDestroyShaderModule)(g.dev, fs, NULL);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = g.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK_EQ(VK(vkAllocateCommandBuffers)(g.dev, &cai, &cb), VK_SUCCESS);
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer)(cb, &cbi);
    VkImageMemoryBarrier ib[2];
    for (uint32_t i = 0; i < 2; i++)
        ib[i] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img[i],
                                        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                             NULL, 2, ib);
    VkRenderingAttachmentInfo ca[2];
    for (uint32_t i = 0; i < 2; i++)
        ca[i] = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view[i],
                                             .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                             .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                             .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo rinf = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = sc, .layerCount = 1,
                             .colorAttachmentCount = 2, .pColorAttachments = ca };
    VK(vkCmdBeginRendering)(cb, &rinf);
    VK(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    struct {
        float e;
        uint32_t axis;
    } pc = { e, 0 };
    VK(vkCmdPushConstants)(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, &pc);
    VK(vkCmdDraw)(cb, 6, 1, 0, 0);
    VK(vkCmdEndRendering)(cb);
    nvn_vk_barrier(cb);
    for (uint32_t i = 0; i < 2; i++) {
        VkBufferImageCopy bic = { .bufferOffset = 16 * i, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                  .imageExtent = { 1, 1, 1 } };
        VK(vkCmdCopyImageToBuffer)(cb, img[i], VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bic);
    }
    nvn_vk_barrier(cb);
    VK(vkEndCommandBuffer)(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK(vkResetFences)(g.dev, 1, &g.fence);
    CHECK_EQ(VK(vkQueueSubmit)(g.queue, 1, &si, g.fence), VK_SUCCESS);
    CHECK_EQ(VK(vkWaitForFences)(g.dev, 1, &g.fence, VK_TRUE, 10000000000ull), VK_SUCCESS);
    *mask = map[0];
    *red = map[4] & 0xffu;
    VK(vkFreeCommandBuffers)(g.dev, g.pool, 1, &cb);
    VK(vkDestroyPipeline)(g.dev, pipe, NULL);
    VK(vkDestroyPipelineLayout)(g.dev, pl, NULL);
    for (uint32_t i = 0; i < 2; i++) {
        VK(vkDestroyImageView)(g.dev, view[i], NULL);
        VK(vkDestroyImage)(g.dev, img[i], NULL);
        VK(vkFreeMemory)(g.dev, mem[i], NULL);
    }
    VK(vkUnmapMemory)(g.dev, bmem);
    VK(vkDestroyBuffer)(g.dev, buf, NULL);
    VK(vkFreeMemory)(g.dev, bmem, NULL);
}

TEST(nvn_raster_samples_give_coverage_to_color_and_modulation_by_standard_positions)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    if (!g.mixed_samples || !g.coverage_to_color) {
        fprintf(stderr, "nvn_raster_samples_give_coverage_to_color_and_modulation: no NVIDIA mixed samples, skipped\n");
        return;
    }
    static const uint8_t standard[3][8] = { { 12, 4 }, { 6, 14, 2, 10 }, { 9, 7, 13, 5, 3, 1, 11, 15 } };
    /*
     * The RTX 3090's RGBA8 red byte of white modulated by k of n covered raster samples (measured by this test,
     * RELIFT_TIR_TABLE): the nearest byte to 255 k / n, ties down (127.5 is 127). The console's TIR modulation is the
     * same hardware state (SET_TIR_MODULATION).
     */
    static const uint8_t modulated[3][9] = { { 0, 127, 255 },
                                             { 0, 64, 127, 191, 255 },
                                             { 0, 32, 64, 96, 127, 159, 191, 223, 255 } };
    const char *path = getenv("RELIFT_TIR_TABLE");
    FILE *out = path ? fopen(path, "w") : NULL;
    for (uint32_t c = 0; c < 3; c++) {
        uint32_t n = 2u << c;
        for (uint32_t e = 0; e <= 16; e++) {
            uint32_t mask, red, want = 0;
            tir_draw(n, ((float)e + 0.5f) / 16.0f, &mask, &red);
            for (uint32_t s = 0; s < n; s++)
                want |= (uint32_t)(standard[c][s] <= e) << s;
            CHECK_EQ(mask, want);
            uint32_t k = (uint32_t)__builtin_popcount(want);
            CHECK_EQ(red, modulated[c][k]);
            if (out)
                fprintf(out, "%u %u\t%x %u\n", n, e, mask, red);
        }
    }
    if (out)
        fclose(out);
}
