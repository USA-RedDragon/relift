/**
 * @file test_nvn_texset.c
 * @brief The texture set's arrays hold a null descriptor in every element nothing was bound to: a compute program
 * that reads a sampled image element and writes a storage image element no texture was ever bound to runs to the
 * end, reads zeros and stores nothing (VK_EXT_robustness2 nullDescriptor).
 */
#include "hos.h"
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "nvn_watch.h"
#include "runner.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/**
 * @brief Reads the handle in element 1 of an input buffer (its id, as a program that reads handles out of memory does),
 * fetches texel (1, 0) of the sampled 2D float texture of that id (set 0, binding 2) and writes the packed texel and
 * the id to an output buffer (set 1, bindings 0 and 1).
 */
static const uint32_t read_handle_spv[] = {
    0x07230203, 0x00010500, 0x0008000b, 0x00000033, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x000014b5, 0x00020011, 0x000014b6, 0x00020011, 0x000014bb, 0x0006000b, 0x00000001, 0x4c534c47,
    0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0008000f, 0x00000005,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x0000001d, 0x0000002a, 0x00060010, 0x00000004,
    0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x00080004,
    0x455f4c47, 0x6e5f5458, 0x6e756e6f, 0x726f6669, 0x75715f6d, 0x66696c61, 0x00726569, 0x000b0004,
    0x455f4c47, 0x735f5458, 0x6c706d61, 0x656c7265, 0x745f7373, 0x75747865, 0x665f6572, 0x74636e75,
    0x736e6f69, 0x00000000, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005, 0x00000008,
    0x00006469, 0x00030005, 0x0000000b, 0x00006e49, 0x00050006, 0x0000000b, 0x00000000, 0x646e6168,
    0x0000656c, 0x00030005, 0x0000000d, 0x00000000, 0x00030005, 0x00000019, 0x00000063, 0x00030005,
    0x0000001d, 0x00786574, 0x00030005, 0x00000028, 0x0074754f, 0x00040006, 0x00000028, 0x00000000,
    0x0000006f, 0x00030005, 0x0000002a, 0x00000000, 0x00040047, 0x0000000a, 0x00000006, 0x00000004,
    0x00030047, 0x0000000b, 0x00000002, 0x00040048, 0x0000000b, 0x00000000, 0x00000018, 0x00050048,
    0x0000000b, 0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x0000000d, 0x00000018, 0x00040047,
    0x0000000d, 0x00000021, 0x00000001, 0x00040047, 0x0000000d, 0x00000022, 0x00000001, 0x00040047,
    0x0000001d, 0x00000021, 0x00000002, 0x00040047, 0x0000001d, 0x00000022, 0x00000000, 0x00030047,
    0x0000001f, 0x000014b4, 0x00030047, 0x00000021, 0x000014b4, 0x00030047, 0x00000022, 0x000014b4,
    0x00040047, 0x00000027, 0x00000006, 0x00000004, 0x00030047, 0x00000028, 0x00000002, 0x00050048,
    0x00000028, 0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x0000002a, 0x00000021, 0x00000000,
    0x00040047, 0x0000002a, 0x00000022, 0x00000001, 0x00040047, 0x00000032, 0x0000000b, 0x00000019,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020,
    0x00000000, 0x00040020, 0x00000007, 0x00000007, 0x00000006, 0x0004002b, 0x00000006, 0x00000009,
    0x00000002, 0x0004001c, 0x0000000a, 0x00000006, 0x00000009, 0x0003001e, 0x0000000b, 0x0000000a,
    0x00040020, 0x0000000c, 0x0000000c, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x0000000c,
    0x00040015, 0x0000000e, 0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f, 0x00000000,
    0x0004002b, 0x0000000e, 0x00000010, 0x00000001, 0x00040020, 0x00000011, 0x0000000c, 0x00000006,
    0x0004002b, 0x00000006, 0x00000014, 0x000fffff, 0x00030016, 0x00000016, 0x00000020, 0x00040017,
    0x00000017, 0x00000016, 0x00000004, 0x00040020, 0x00000018, 0x00000007, 0x00000017, 0x00090019,
    0x0000001a, 0x00000016, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
    0x0003001d, 0x0000001b, 0x0000001a, 0x00040020, 0x0000001c, 0x00000000, 0x0000001b, 0x0004003b,
    0x0000001c, 0x0000001d, 0x00000000, 0x00040020, 0x00000020, 0x00000000, 0x0000001a, 0x00040017,
    0x00000023, 0x0000000e, 0x00000002, 0x0005002c, 0x00000023, 0x00000024, 0x00000010, 0x0000000f,
    0x0004002b, 0x00000006, 0x00000026, 0x00000004, 0x0004001c, 0x00000027, 0x00000006, 0x00000026,
    0x0003001e, 0x00000028, 0x00000027, 0x00040020, 0x00000029, 0x0000000c, 0x00000028, 0x0004003b,
    0x00000029, 0x0000002a, 0x0000000c, 0x00040017, 0x00000030, 0x00000006, 0x00000003, 0x0004002b,
    0x00000006, 0x00000031, 0x00000001, 0x0006002c, 0x00000030, 0x00000032, 0x00000031, 0x00000031,
    0x00000031, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0004003b, 0x00000007, 0x00000008, 0x00000007, 0x0004003b, 0x00000018, 0x00000019, 0x00000007,
    0x00060041, 0x00000011, 0x00000012, 0x0000000d, 0x0000000f, 0x00000010, 0x0004003d, 0x00000006,
    0x00000013, 0x00000012, 0x000500c7, 0x00000006, 0x00000015, 0x00000013, 0x00000014, 0x0003003e,
    0x00000008, 0x00000015, 0x0004003d, 0x00000006, 0x0000001e, 0x00000008, 0x00040053, 0x00000006,
    0x0000001f, 0x0000001e, 0x00050041, 0x00000020, 0x00000021, 0x0000001d, 0x0000001f, 0x0004003d,
    0x0000001a, 0x00000022, 0x00000021, 0x0007005f, 0x00000017, 0x00000025, 0x00000022, 0x00000024,
    0x00000002, 0x0000000f, 0x0003003e, 0x00000019, 0x00000025, 0x0004003d, 0x00000017, 0x0000002b,
    0x00000019, 0x0006000c, 0x00000006, 0x0000002c, 0x00000001, 0x00000037, 0x0000002b, 0x00060041,
    0x00000011, 0x0000002d, 0x0000002a, 0x0000000f, 0x0000000f, 0x0003003e, 0x0000002d, 0x0000002c,
    0x0004003d, 0x00000006, 0x0000002e, 0x00000008, 0x00060041, 0x00000011, 0x0000002f, 0x0000002a,
    0x0000000f, 0x00000010, 0x0003003e, 0x0000002f, 0x0000002e, 0x000100fd, 0x00010038,
};

/** @brief Runs read_handle_spv with handle as the handle in memory; out[0] gets the packed texel, out[1] the id. */
static void run_read_handle(uint32_t handle, uint32_t out[2])
{
    VkDescriptorSetLayout tex = nvn_texset_layout();
    VkDescriptorSet tex_set = nvn_texset();
    VkDescriptorSetLayoutBinding ob[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                                           { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo oli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 2,
                                            .pBindings = ob };
    VkDescriptorSetLayout out_layout;
    CHECK_EQ(VK(vkCreateDescriptorSetLayout)(g.dev, &oli, NULL, &out_layout), VK_SUCCESS);
    VkDescriptorSetLayout layouts[2] = { tex, out_layout };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 2,
                                       .pSetLayouts = layouts };
    VkPipelineLayout pl;
    CHECK_EQ(VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &pl), VK_SUCCESS);
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = sizeof read_handle_spv,
                                     .pCode = read_handle_spv };
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
                               .size = 64,
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
    CHECK_EQ(VK(vkMapMemory)(g.dev, mem, 0, 64, 0, (void **)&map), VK_SUCCESS);
    memset(map, 0xff, 64);
    map[8] = 0;
    map[9] = handle;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
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
    VkDescriptorBufferInfo bi[2] = { { buf, 0, 32 }, { buf, 32, 32 } };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = out_set,
                               .descriptorCount = 2,
                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               .pBufferInfo = bi };
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
    out[0] = map[0];
    out[1] = map[1];
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

TEST(nvn_texset_a_handle_read_out_of_memory_samples_the_texture_registered_under_its_id)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    CHECK(nvn_texset_layout());
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    static rl_ctx c;
    static uint8_t mem[0x10000];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    static replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    const uint64_t pool = 0x100, tex_a = 0x200, tex_b = 0x300, storage_a = 0x4000, storage_b = 0x5000;
    CHECK(nvn_pool_make(pool, 0, 0x10000, 0x2a1));
    nvn_tex_desc d = { .target = 1, .format = 37, .width = 4, .height = 4, .depth = 1, .levels = 1, .flags = 1u << 4,
                       .stride = 32, .swizzle = { 2, 3, 4, 5 }, .pool = pool, .offset = storage_a };
    CHECK(nvn_tex_layout(&d));
    CHECK(nvn_texture_record(tex_a, &d, NULL));
    d.offset = storage_b;
    CHECK(nvn_texture_record(tex_b, &d, NULL));
    for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++) {
            uint8_t *a = mem + storage_a + y * 32 + x * 4, *b = mem + storage_b + y * 32 + x * 4;
            a[0] = (uint8_t)(0x10 + x);
            a[1] = 0x22;
            a[2] = 0x33;
            a[3] = 0x44;
            b[0] = 0xaa;
            b[1] = (uint8_t)(0x50 + x);
            b[2] = 0xcc;
            b[3] = 0xdd;
        }
    c.x[1] = 5;
    c.x[2] = tex_a;
    c.x[3] = 0;
    nvnh_nvnTexturePoolRegisterTexture(&c, mem);
    c.x[1] = 4097;
    c.x[2] = tex_b;
    nvnh_nvnTexturePoolRegisterTexture(&c, mem);
    CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
    CHECK(nvn_vk_flush(&r));
    uint32_t out[2] = { 0, 0 };
    run_read_handle(5u | 7u << 20, out);
    CHECK_EQ(out[1], 5u);
    CHECK_EQ(out[0], 0x443322 << 8 | 0x11);
    run_read_handle(4097u, out);
    CHECK_EQ(out[1], 4097u);
    CHECK_EQ(out[0], 0xddccu << 16 | 0x51u << 8 | 0xaau);
    CHECK(nvn_texset_pinned() > 4097u);
}

TEST(nvn_one_dimensional_textures_get_host_images_of_their_own_dimension)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    static rl_ctx c;
    static uint8_t mem[0x10000];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    static replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    const uint64_t pool = 0x100, tex = 0x200, array = 0x300;
    CHECK(nvn_pool_make(pool, 0, 0x10000, 0x2a1));
    nvn_tex_desc d = { .target = 0, .format = 37, .width = 4, .height = 1, .depth = 1, .levels = 1, .pool = pool,
                       .offset = 0x4000 };
    CHECK(nvn_tex_layout(&d));
    CHECK(nvn_texture_record(tex, &d, NULL));
    vimg *v = nvn_vk_image(&r, tex);
    CHECK(v != NULL);
    CHECK(v && v->view != NULL);
    nvn_tex_desc a = { .target = 3, .format = 37, .width = 4, .height = 1, .depth = 3, .levels = 1, .pool = pool,
                       .offset = 0x8000 };
    CHECK(nvn_tex_layout(&a));
    CHECK(nvn_texture_record(array, &a, NULL));
    vimg *w = nvn_vk_image(&r, array);
    CHECK(w != NULL);
    CHECK(w && w->view != NULL);
    CHECK(nvn_vk_flush(&r));
}

TEST(nvn_texset_a_view_of_another_block_footprint_is_not_made_resident)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    static rl_ctx c;
    static uint8_t mem[0x10000];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    static replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    const uint64_t pool = 0x100, tex = 0x200, rgba = 0x300;
    CHECK(nvn_pool_make(pool, 0, 0x10000, 0x2a1));
    nvn_tex_desc d = { .target = 1, .format = 75, .width = 16, .height = 16, .depth = 1, .levels = 1, .pool = pool,
                       .offset = 0x4000 };
    CHECK(nvn_tex_layout(&d));
    CHECK(nvn_texture_record(tex, &d, NULL));
    nvn_tex_desc p = { .target = 1, .format = 37, .width = 4, .height = 4, .depth = 1, .levels = 1, .pool = pool,
                       .offset = 0x8000 };
    CHECK(nvn_tex_layout(&p));
    CHECK(nvn_texture_record(rgba, &p, NULL));
    vimg *bc5 = nvn_vk_image(&r, tex), *plain = nvn_vk_image(&r, rgba);
    CHECK(bc5 != NULL && plain != NULL);
    nvn_view nv;
    memset(&nv, 0, sizeof nv);
    CHECK(nvn_texset_view_fits(bc5, &nv));
    nv.has_format = true;
    nv.format = 75;
    CHECK(nvn_texset_view_fits(bc5, &nv));
    nv.format = 123;
    CHECK(!nvn_texset_view_fits(bc5, &nv));
    nv.format = 37;
    CHECK(nvn_texset_view_fits(plain, &nv));
    CHECK(nvn_vk_flush(&r));
}

TEST(nvn_texset_an_image_handle_binds_the_texture_registered_under_its_id)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    CHECK(nvn_texset_layout());
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    static rl_ctx c;
    static uint8_t mem[0x10000];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    static replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    const uint64_t pool = 0x100, tex = 0x200;
    CHECK(nvn_pool_make(pool, 0, 0x10000, 0x2a1));
    nvn_tex_desc d = { .target = 1, .format = 37, .width = 4, .height = 4, .depth = 1, .levels = 1, .flags = 1u << 4,
                       .stride = 32, .pool = pool, .offset = 0x4000 };
    CHECK(nvn_tex_layout(&d));
    CHECK(nvn_texture_record(tex, &d, NULL));
    c.x[1] = 9;
    c.x[2] = tex;
    c.x[3] = 0;
    nvnh_nvnTexturePoolRegisterTexture(&c, mem);
    uint32_t slot = UINT32_MAX, kind = 0, dim = 0;
    CHECK_EQ(nvn_texset_image(&r, 9u, &slot, &kind, &dim), RL_OK);
    CHECK(slot != UINT32_MAX);
    uint32_t again = UINT32_MAX;
    CHECK_EQ(nvn_texset_image(&r, 9u, &again, &kind, &dim), RL_OK);
    CHECK_EQ(again, slot);
    CHECK(nvn_vk_flush(&r));
}

TEST(nvn_texset_a_texture_whose_pool_is_gone_reads_zeros)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes) || !g.null_descriptor)
        return;
    CHECK(nvn_texset_layout());
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    static rl_ctx c;
    static uint8_t mem[0x10000];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    static replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    const uint64_t pool = 0x180, tex = 0x280;
    CHECK(nvn_pool_make(pool, 0, 0x10000, 0x2a1));
    nvn_tex_desc d = { .target = 1, .format = 37, .width = 4, .height = 4, .depth = 1, .levels = 1, .flags = 1u << 4,
                       .stride = 32, .pool = pool, .offset = 0x4000 };
    CHECK(nvn_tex_layout(&d));
    CHECK(nvn_texture_record(tex, &d, NULL));
    c.x[1] = 21;
    c.x[2] = tex;
    c.x[3] = 0;
    nvnh_nvnTexturePoolRegisterTexture(&c, mem);
    nvn_obj_free(nvn_obj_get(pool, NVN_POOL));
    CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
    uint32_t kind = 9, dim = 9, slot = UINT32_MAX;
    CHECK_EQ(nvn_texset_bind(&r, 21u, &kind, &dim, &slot), RL_OK);
    CHECK(slot != UINT32_MAX);
    CHECK_EQ(kind, 0);
    CHECK(nvn_vk_flush(&r));
}

TEST(nvn_an_image_takes_every_newer_write_of_an_alias_even_after_a_quiet_lookup)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    static rl_ctx c;
    static uint8_t mem[0x10000];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    static replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    const uint64_t pool = 0x100, a = 0x200, b = 0x300;
    CHECK(nvn_pool_make(pool, 0, 0x10000, 0x2a1));
    nvn_tex_desc d = { .target = 1, .format = 37, .width = 4, .height = 4, .depth = 1, .levels = 1, .pool = pool,
                       .offset = 0x4000 };
    CHECK(nvn_tex_layout(&d));
    nvn_tex_desc e = d;
    e.height = 2;
    CHECK(nvn_texture_record(a, &d, NULL));
    CHECK(nvn_texture_record(b, &e, NULL));
    vimg *va = nvn_vk_image(&r, a), *vb = nvn_vk_image(&r, b);
    CHECK(va != NULL && vb != NULL && va != vb);
    CHECK(nvn_vk_image(&r, b) == vb);
    uint64_t quiet = vb->written;
    CHECK(nvn_vk_image(&r, b) == vb);
    CHECK_EQ(vb->written, quiet);
    nvn_vk_written(va);
    CHECK(nvn_vk_image(&r, b) == vb);
    CHECK_EQ(vb->written, va->written);
    nvn_vk_written(va);
    CHECK(nvn_vk_image(&r, b) == vb);
    CHECK_EQ(vb->written, va->written);
    CHECK(nvn_vk_flush(&r));
}

TEST(nvn_a_texture_finds_its_host_image_among_many_by_storage_and_shape)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    static rl_ctx c;
    static uint8_t mem[0x10000];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    static replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    const uint64_t pool = 0x100;
    CHECK(nvn_pool_make(pool, 0, 0x10000, 0x2a1));
    vimg *first[24];
    for (uint32_t i = 0; i < 24; i++) {
        nvn_tex_desc d = { .target = 1, .format = 37, .width = 4, .height = 4, .depth = 1, .levels = 1, .pool = pool,
                           .offset = 0x1000 + 0x200 * (i / 2) };
        if (i % 2)
            d.width = 2;
        CHECK(nvn_tex_layout(&d));
        CHECK(nvn_texture_record(0x1000 + i, &d, NULL));
        first[i] = nvn_vk_image(&r, 0x1000 + i);
        CHECK(first[i] != NULL);
    }
    for (uint32_t i = 0; i < 24; i++) {
        CHECK(nvn_vk_image(&r, 0x1000 + i) == first[i]);
        if (i % 2)
            CHECK(first[i] != first[i - 1]);
    }
    CHECK(nvn_vk_flush(&r));
}

TEST(nvn_texset_a_view_of_a_cube_counts_its_six_faces)
{
    vimg cube;
    memset(&cube, 0, sizeof cube);
    cube.d.target = NVN_TARGET_CUBEMAP;
    cube.d.depth = 1;
    cube.d.levels = 1;
    CHECK_EQ(nvn_texset_view_layers(&cube, NULL), 6u);
    nvn_view nv;
    memset(&nv, 0, sizeof nv);
    nv.has_layers = true;
    nv.min_layer = 2;
    CHECK_EQ(nvn_texset_view_layers(&cube, &nv), 4u);
    nv.num_layers = 1;
    CHECK_EQ(nvn_texset_view_layers(&cube, &nv), 1u);
    nv.num_layers = 0;
    nv.min_layer = 7;
    CHECK_EQ(nvn_texset_view_layers(&cube, &nv), 0u);
    vimg arr = cube;
    arr.d.target = 4;
    arr.d.depth = 12;
    CHECK_EQ(nvn_texset_view_layers(&arr, NULL), 12u);
}

TEST(nvn_a_barrier_over_a_whole_volume_names_its_remaining_layers)
{
    vimg vol;
    memset(&vol, 0, sizeof vol);
    vol.d.target = NVN_TARGET_3D;
    vol.d.depth = 16;
    vol.d.levels = 3;
    vol.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageSubresourceRange rr = nvn_vk_barrier_range(&vol);
    CHECK_EQ(rr.layerCount, VK_REMAINING_ARRAY_LAYERS);
    CHECK_EQ(rr.levelCount, VK_REMAINING_MIP_LEVELS);
    CHECK_EQ(rr.baseArrayLayer, 0u);
    CHECK_EQ(nvn_vk_image_range(&vol).layerCount, 1u);
}

TEST(nvn_an_integer_texture_reads_back_bit_for_bit)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    static rl_ctx c;
    static uint8_t mem[0x10000];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    static replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    const uint64_t pool = 0x1a0, tex = 0x2a0;
    for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++) {
            uint32_t word = 0x80000000u | y << 16 | x * 0x1111u;
            memcpy(mem + 0x4000 + y * 32 + x * 4, &word, 4);
        }
    CHECK(nvn_pool_make(pool, 0, 0x10000, 0x2a1));
    nvn_tex_desc d = { .target = 1, .format = 11, .width = 4, .height = 4, .depth = 1, .levels = 1, .flags = 1u << 4,
                       .stride = 32, .pool = pool, .offset = 0x4000 };
    CHECK(nvn_tex_layout(&d));
    CHECK(nvn_texture_record(tex, &d, NULL));
    vimg *v = nvn_vk_image(&r, tex);
    CHECK(v != NULL);
    if (!v)
        return;
    CHECK_EQ(v->format, VK_FORMAT_R32_UINT);
    uint8_t *px = nvn_vk_read_rgba8(&r, v);
    CHECK(px != NULL);
    for (uint32_t y = 0; px && y < 4; y++)
        CHECK(!memcmp(px + y * 16, mem + 0x4000 + y * 32, 16));
    free(px);
    CHECK(nvn_vk_flush(&r));
}

TEST(nvn_an_image_of_a_removed_pool_is_freed_after_its_last_batch)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    CHECK(nvn_texset_layout());
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    static rl_ctx c;
    static uint8_t mem[0x10000];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    static replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    const uint64_t pool = 0x1c0, tex = 0x2c0;
    CHECK(nvn_pool_make(pool, 0, 0x10000, 0x2a1));
    nvn_tex_desc d = { .target = 1, .format = 37, .width = 4, .height = 4, .depth = 1, .levels = 1, .flags = 1u << 4,
                       .stride = 32, .pool = pool, .offset = 0x4000 };
    CHECK(nvn_tex_layout(&d));
    CHECK(nvn_texture_record(tex, &d, NULL));
    c.x[1] = 23;
    c.x[2] = tex;
    c.x[3] = 0;
    nvnh_nvnTexturePoolRegisterTexture(&c, mem);
    CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
    uint32_t kind = 9, dim = 9, slot = UINT32_MAX;
    CHECK_EQ(nvn_texset_bind(&r, 23u, &kind, &dim, &slot), RL_OK);
    CHECK(nvn_vk_begin(&r));
    nvn_obj_free(nvn_obj_get(pool, NVN_POOL));
    nvn_vk_retire_images();
    CHECK_EQ(nvn_vk_retired_count(), 1u);
    nvn_vk_retire_images();
    CHECK_EQ(nvn_vk_retired_count(), 1u);
    CHECK(nvn_vk_flush(&r));
    nvn_vk_retire_images();
    CHECK_EQ(nvn_vk_retired_count(), 0u);
    CHECK_EQ(nvn_texset_bind(&r, 23u, &kind, &dim, &slot), RL_OK);
    CHECK(nvn_vk_flush(&r));
}

TEST(nvn_texset_a_synced_texture_whose_pool_goes_reads_zeros_through_a_handle_read_out_of_memory)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes) || !g.null_descriptor)
        return;
    CHECK(nvn_texset_layout());
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    static rl_ctx c;
    static uint8_t mem[0x10000];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    static replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    const uint64_t pool = 0x380, tex = 0x480, storage = 0x6000;
    CHECK(nvn_pool_make(pool, 0, 0x10000, 0x2a1));
    nvn_tex_desc d = { .target = 1, .format = 37, .width = 4, .height = 4, .depth = 1, .levels = 1, .flags = 1u << 4,
                       .stride = 32, .swizzle = { 2, 3, 4, 5 }, .pool = pool, .offset = storage };
    CHECK(nvn_tex_layout(&d));
    CHECK(nvn_texture_record(tex, &d, NULL));
    for (uint32_t i = 0; i < 4 * 32; i += 4) {
        mem[storage + i] = 0x61;
        mem[storage + i + 1] = 0x62;
        mem[storage + i + 2] = 0x63;
        mem[storage + i + 3] = 0x64;
    }
    c.x[1] = 33;
    c.x[2] = tex;
    c.x[3] = 0;
    nvnh_nvnTexturePoolRegisterTexture(&c, mem);
    CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
    CHECK(nvn_vk_flush(&r));
    uint32_t out[2] = { 0, 0 };
    run_read_handle(33u, out);
    CHECK_EQ(out[1], 33u);
    CHECK_EQ(out[0], 0x64636261u);
    nvn_obj_free(nvn_obj_get(pool, NVN_POOL));
    g.presents++;
    CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
    CHECK(nvn_vk_flush(&r));
    run_read_handle(33u, out);
    CHECK_EQ(out[1], 33u);
    CHECK_EQ(out[0], 0u);
}

TEST(nvn_texset_handles_a_bank_names_are_bound_without_making_every_registered_texture_resident)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    CHECK(nvn_texset_layout());
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    enum { COUNT = 5000, FIRST = 200, SPAN = 0x80 };
    static rl_ctx c;
    static uint8_t mem[0x10000 + COUNT * SPAN];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    static replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    const uint64_t pool = 0x500, tex = 0x10000;
    CHECK(nvn_pool_make(pool, 0, sizeof mem, 0x2a1));
    CHECK(nvn_texset_pinned() > FIRST + COUNT);
    for (uint32_t i = 0; i < COUNT; i++) {
        nvn_tex_desc d = { .target = 1, .format = 37, .width = 4, .height = 4, .depth = 1, .levels = 1,
                           .flags = 1u << 4, .stride = 32, .swizzle = { 2, 3, 4, 5 }, .pool = pool,
                           .offset = 0x10000 + (uint64_t)i * SPAN };
        CHECK(nvn_tex_layout(&d));
        CHECK(nvn_texture_record(tex + 0x100 * (uint64_t)i, &d, NULL));
        uint8_t *p = mem + d.offset;
        for (uint32_t k = 0; k < SPAN; k += 4) {
            p[k] = (uint8_t)i;
            p[k + 1] = (uint8_t)(i >> 8);
            p[k + 2] = 0x5a;
            p[k + 3] = 0xa5;
        }
        c.x[1] = FIRST + i;
        c.x[2] = tex + 0x100 * (uint64_t)i;
        c.x[3] = 0;
        nvnh_nvnTexturePoolRegisterTexture(&c, mem);
    }
    uint32_t images = g.nimages;
    uint32_t bank[16];
    for (uint32_t k = 0; k < 16; k++)
        bank[k] = 0xffffffffu;
    bank[4] = FIRST + 4321u;
    bank[9] = FIRST + 17u;
    CHECK_EQ(nvn_texset_bind_words(&r, (const uint8_t *)bank, sizeof bank, 8, true), RL_OK);
    CHECK_EQ(nvn_texset_bind_words(&r, (const uint8_t *)bank, sizeof bank, 0, false), RL_OK);
    CHECK(nvn_vk_flush(&r));
    CHECK(g.nimages - images <= 2u);
    uint32_t out[2] = { 0, 0 };
    run_read_handle(FIRST + 4321u, out);
    CHECK_EQ(out[1], FIRST + 4321u);
    CHECK_EQ(out[0], 0xa55a0000u | (4321u >> 8) << 8 | (4321u & 0xff));
    run_read_handle(FIRST + 17u, out);
    CHECK_EQ(out[0], 0xa55a0000u | 17u);
}

/** @brief Registers count 4x4 RGBA8 textures under ids first.. over the pool, texel words (i, 0x5a, 0xa5). */
static void register_many(replay *r, uint8_t *mem, uint64_t pool, uint64_t tex, uint32_t first, uint32_t count,
                          uint64_t at, uint32_t span)
{
    for (uint32_t i = 0; i < count; i++) {
        nvn_tex_desc d = { .target = 1, .format = 37, .width = 4, .height = 4, .depth = 1, .levels = 1,
                           .flags = 1u << 4, .stride = 32, .swizzle = { 2, 3, 4, 5 }, .pool = pool,
                           .offset = at + (uint64_t)i * span };
        CHECK(nvn_tex_layout(&d));
        CHECK(nvn_texture_record(tex + 0x100 * (uint64_t)i, &d, NULL));
        uint8_t *p = mem + d.offset;
        for (uint32_t k = 0; k < span; k += 4) {
            p[k] = (uint8_t)i;
            p[k + 1] = (uint8_t)(i >> 8);
            p[k + 2] = 0x5a;
            p[k + 3] = 0xa5;
        }
        r->c->x[1] = first + i;
        r->c->x[2] = tex + 0x100 * (uint64_t)i;
        r->c->x[3] = 0;
        nvnh_nvnTexturePoolRegisterTexture(r->c, mem);
    }
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

TEST(nvn_texset_every_registered_texture_reads_its_texels_and_an_unchanged_frame_costs_little)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    CHECK(nvn_texset_layout());
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    enum { COUNT = 5000, FIRST = 6000, SPAN = 0x80 };
    static rl_ctx c;
    static uint8_t mem[0x10000 + COUNT * SPAN];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    static replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    const uint64_t pool = 0x600;
    CHECK(nvn_pool_make(pool, 0, sizeof mem, 0x2a1));
    CHECK(nvn_texset_pinned() > FIRST + COUNT);
    register_many(&r, mem, pool, 0x20000000, FIRST, COUNT, 0x10000, SPAN);
    CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
    CHECK(nvn_vk_flush(&r));
    CHECK(g.nimages >= COUNT);
    uint32_t out[2] = { 0, 0 };
    run_read_handle(FIRST + 4711u, out);
    CHECK_EQ(out[1], FIRST + 4711u);
    CHECK_EQ(out[0], 0xa55a0000u | (4711u >> 8) << 8 | (4711u & 0xff));
    double t0 = now_ms();
    for (int f = 0; f < 10; f++) {
        g.presents++;
        CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
    }
    double per = (now_ms() - t0) / 10;
    printf("  an unchanged frame over %u registered textures: %.3f ms\n", COUNT, per);
    CHECK(per < 2.0);
    for (uint32_t k = 0; k < SPAN; k += 4)
        mem[0x10000 + 4711u * SPAN + k] = 0x77;
    nvn_watch_touch(0x10000 + 4711u * SPAN, SPAN);
    g.presents++;
    CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
    CHECK(nvn_vk_flush(&r));
    run_read_handle(FIRST + 4711u, out);
    CHECK_EQ(out[0] & 0xffu, 0x77u);
    printf("  images %u for %u registered textures\n", g.nimages, COUNT);
}
