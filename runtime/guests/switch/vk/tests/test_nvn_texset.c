/**
 * @file test_nvn_texset.c
 * @brief The texture set's arrays hold a null descriptor in every element nothing was bound to: a compute program
 * that reads a sampled image element and writes a storage image element no texture was ever bound to runs to the
 * end, reads zeros and stores nothing (VK_EXT_robustness2 nullDescriptor).
 */
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "runner.h"

#include <string.h>

/** @brief Binds sampled image 3D element 5 and storage image 2D element 7 (set 0, bindings 3 and 35) and writes the fetched texel to a buffer. */
static const uint32_t never_bound_spv[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x00000047, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x00000038, 0x00020011, 0x000014b5, 0x00020011, 0x000014b6, 0x00020011, 0x000014bb, 0x00020011,
    0x000014bd, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x0008000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d,
    0x0000001b, 0x0000002a, 0x00060010, 0x00000004, 0x00000011, 0x00000001, 0x00000001, 0x00000001,
    0x00040047, 0x0000000d, 0x00000021, 0x00000003, 0x00040047, 0x0000000d, 0x00000022, 0x00000000,
    0x00030047, 0x00000012, 0x000014b4, 0x00030047, 0x00000013, 0x000014b4, 0x00030047, 0x0000001b,
    0x00000019, 0x00040047, 0x0000001b, 0x00000021, 0x00000023, 0x00040047, 0x0000001b, 0x00000022,
    0x00000000, 0x00030047, 0x0000001f, 0x000014b4, 0x00030047, 0x00000020, 0x000014b4, 0x00040047,
    0x00000027, 0x00000006, 0x00000004, 0x00030047, 0x00000028, 0x00000002, 0x00050048, 0x00000028,
    0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x0000002a, 0x00000021, 0x00000000, 0x00040047,
    0x0000002a, 0x00000022, 0x00000001, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00090019,
    0x0000000a, 0x00000006, 0x00000002, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
    0x0003001d, 0x0000000b, 0x0000000a, 0x00040020, 0x0000000c, 0x00000000, 0x0000000b, 0x0004003b,
    0x0000000c, 0x0000000d, 0x00000000, 0x00040015, 0x0000000e, 0x00000020, 0x00000001, 0x0004002b,
    0x0000000e, 0x0000000f, 0x00000005, 0x00040020, 0x00000011, 0x00000000, 0x0000000a, 0x00040017,
    0x00000014, 0x0000000e, 0x00000003, 0x0004002b, 0x0000000e, 0x00000015, 0x00000000, 0x0006002c,
    0x00000014, 0x00000016, 0x00000015, 0x00000015, 0x00000015, 0x00090019, 0x00000018, 0x00000006,
    0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000002, 0x00000000, 0x0003001d, 0x00000019,
    0x00000018, 0x00040020, 0x0000001a, 0x00000000, 0x00000019, 0x0004003b, 0x0000001a, 0x0000001b,
    0x00000000, 0x0004002b, 0x0000000e, 0x0000001c, 0x00000007, 0x00040020, 0x0000001e, 0x00000000,
    0x00000018, 0x00040017, 0x00000021, 0x0000000e, 0x00000002, 0x0005002c, 0x00000021, 0x00000022,
    0x00000015, 0x00000015, 0x0004002b, 0x00000006, 0x00000023, 0x3f800000, 0x0007002c, 0x00000007,
    0x00000024, 0x00000023, 0x00000023, 0x00000023, 0x00000023, 0x00040015, 0x00000025, 0x00000020,
    0x00000000, 0x0004002b, 0x00000025, 0x00000026, 0x00000008, 0x0004001c, 0x00000027, 0x00000025,
    0x00000026, 0x0003001e, 0x00000028, 0x00000027, 0x00040020, 0x00000029, 0x0000000c, 0x00000028,
    0x0004003b, 0x00000029, 0x0000002a, 0x0000000c, 0x00040020, 0x00000030, 0x0000000c, 0x00000025,
    0x0004002b, 0x0000000e, 0x00000032, 0x00000001, 0x0004002b, 0x0000000e, 0x00000038, 0x00000002,
    0x0004002b, 0x0000000e, 0x0000003e, 0x00000003, 0x0004002b, 0x0000000e, 0x00000044, 0x00000004,
    0x0004002b, 0x00000025, 0x00000045, 0x600dc0de, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x00050041, 0x00000011, 0x00000012, 0x0000000d, 0x0000000f,
    0x0004003d, 0x0000000a, 0x00000013, 0x00000012, 0x0007005f, 0x00000007, 0x00000017, 0x00000013,
    0x00000016, 0x00000002, 0x00000015, 0x00050041, 0x0000001e, 0x0000001f, 0x0000001b, 0x0000001c,
    0x0004003d, 0x00000018, 0x00000020, 0x0000001f, 0x00040063, 0x00000020, 0x00000022, 0x00000024,
    0x00050051, 0x00000006, 0x0000002e, 0x00000017, 0x00000000, 0x0004007c, 0x00000025, 0x0000002f,
    0x0000002e, 0x00060041, 0x00000030, 0x00000031, 0x0000002a, 0x00000015, 0x00000015, 0x0003003e,
    0x00000031, 0x0000002f, 0x00050051, 0x00000006, 0x00000035, 0x00000017, 0x00000001, 0x0004007c,
    0x00000025, 0x00000036, 0x00000035, 0x00060041, 0x00000030, 0x00000037, 0x0000002a, 0x00000015,
    0x00000032, 0x0003003e, 0x00000037, 0x00000036, 0x00050051, 0x00000006, 0x0000003b, 0x00000017,
    0x00000002, 0x0004007c, 0x00000025, 0x0000003c, 0x0000003b, 0x00060041, 0x00000030, 0x0000003d,
    0x0000002a, 0x00000015, 0x00000038, 0x0003003e, 0x0000003d, 0x0000003c, 0x00050051, 0x00000006,
    0x00000041, 0x00000017, 0x00000003, 0x0004007c, 0x00000025, 0x00000042, 0x00000041, 0x00060041,
    0x00000030, 0x00000043, 0x0000002a, 0x00000015, 0x0000003e, 0x0003003e, 0x00000043, 0x00000042,
    0x00060041, 0x00000030, 0x00000046, 0x0000002a, 0x00000015, 0x00000044, 0x0003003e, 0x00000046,
    0x00000045, 0x000100fd, 0x00010038
};

TEST(nvn_texset_elements_nothing_was_bound_to_read_zeros)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes) || !g.null_descriptor)
        return;
    VkDescriptorSetLayout tex = nvn_texset_layout();
    VkDescriptorSet tex_set = nvn_texset();
    CHECK(tex && tex_set);

    VkDescriptorSetLayoutBinding ob = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo oli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 1,
                                            .pBindings = &ob };
    VkDescriptorSetLayout out_layout;
    CHECK_EQ(VK(vkCreateDescriptorSetLayout)(g.dev, &oli, NULL, &out_layout), VK_SUCCESS);
    VkDescriptorSetLayout layouts[2] = { tex, out_layout };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 2,
                                       .pSetLayouts = layouts };
    VkPipelineLayout pl;
    CHECK_EQ(VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &pl), VK_SUCCESS);
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = sizeof never_bound_spv,
                                     .pCode = never_bound_spv };
    VkShaderModule sm;
    CHECK_EQ(VK(vkCreateShaderModule)(g.dev, &smi, NULL, &sm), VK_SUCCESS);
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                   .module = sm,
                                                   .pName = "main" },
                                        .layout = pl };
    VkPipeline pipe;
    CHECK_EQ(VK(vkCreateComputePipelines)(g.dev, NULL, 1, &cpi, NULL, &pipe), VK_SUCCESS);

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = 32,
                               .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    VkBuffer buf;
    CHECK_EQ(VK(vkCreateBuffer)(g.dev, &bci, NULL, &buf), VK_SUCCESS);
    VkMemoryRequirements req;
    VK(vkGetBufferMemoryRequirements)(g.dev, buf, &req);
    int32_t type = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(type >= 0);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)type };
    VkDeviceMemory mem;
    CHECK_EQ(VK(vkAllocateMemory)(g.dev, &mai, NULL, &mem), VK_SUCCESS);
    CHECK_EQ(VK(vkBindBufferMemory)(g.dev, buf, mem, 0), VK_SUCCESS);
    uint32_t *map;
    CHECK_EQ(VK(vkMapMemory)(g.dev, mem, 0, 32, 0, (void **)&map), VK_SUCCESS);
    memset(map, 0xff, 32);

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1,
                                       .poolSizeCount = 1,
                                       .pPoolSizes = &ps };
    VkDescriptorPool pool;
    CHECK_EQ(VK(vkCreateDescriptorPool)(g.dev, &dpi, NULL, &pool), VK_SUCCESS);
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = pool,
                                        .descriptorSetCount = 1,
                                        .pSetLayouts = &out_layout };
    VkDescriptorSet out_set;
    CHECK_EQ(VK(vkAllocateDescriptorSets)(g.dev, &dai, &out_set), VK_SUCCESS);
    VkDescriptorBufferInfo bi = { buf, 0, 32 };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = out_set,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               .pBufferInfo = &bi };
    VK(vkUpdateDescriptorSets)(g.dev, 1, &w, 0, NULL);

    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = g.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK_EQ(VK(vkAllocateCommandBuffers)(g.dev, &cai, &cb), VK_SUCCESS);
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer)(cb, &cbi);
    VK(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    VkDescriptorSet sets[2] = { tex_set, out_set };
    VK(vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 2, sets, 0, NULL);
    VK(vkCmdDispatch)(cb, 1, 1, 1);
    VK(vkEndCommandBuffer)(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK(vkResetFences)(g.dev, 1, &g.fence);
    CHECK_EQ(VK(vkQueueSubmit)(g.queue, 1, &si, g.fence), VK_SUCCESS);
    CHECK_EQ(VK(vkWaitForFences)(g.dev, 1, &g.fence, VK_TRUE, 10000000000ull), VK_SUCCESS);

    CHECK_EQ(map[4], 0x600dc0deu);
    for (uint32_t i = 0; i < 4; i++)
        CHECK_EQ(map[i], 0u);

    VK(vkFreeCommandBuffers)(g.dev, g.pool, 1, &cb);
    VK(vkDestroyDescriptorPool)(g.dev, pool, NULL);
    VK(vkUnmapMemory)(g.dev, mem);
    VK(vkFreeMemory)(g.dev, mem, NULL);
    VK(vkDestroyBuffer)(g.dev, buf, NULL);
    VK(vkDestroyPipeline)(g.dev, pipe, NULL);
    VK(vkDestroyShaderModule)(g.dev, sm, NULL);
    VK(vkDestroyPipelineLayout)(g.dev, pl, NULL);
    VK(vkDestroyDescriptorSetLayout)(g.dev, out_layout, NULL);
}

TEST(nvn_texset_a_sampled_handle_nothing_registered_binds_a_slot_of_null_descriptors)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes) || !g.null_descriptor)
        return;
    CHECK(nvn_texset_layout());
    static replay r;
    memset(&r, 0, sizeof r);
    uint32_t kind = 9, dim = 9, slot = UINT32_MAX, again = UINT32_MAX;
    CHECK_EQ(nvn_texset_bind(&r, 0x30000u | 11u, &kind, &dim, &slot), RL_OK);
    CHECK(slot != UINT32_MAX);
    CHECK_EQ(kind, 0);
    CHECK(dim != 9u);
    CHECK_EQ(nvn_texset_bind(&r, 0x30000u | 11u, &kind, &dim, &again), RL_OK);
    CHECK_EQ(again, slot);
    uint32_t other;
    CHECK_EQ(nvn_texset_bind(&r, 12u, &kind, &dim, &other), RL_OK);
    CHECK(other != slot);
}
