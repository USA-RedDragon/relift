/**
 * @file msaa_cells.c
 * @brief Shows where an NVIDIA GPU stores each sample of a multisample target: draws sample s of every pixel as the
 * value s + 1 (a color target shaded per sample, or a stencil target drawn once per sample with that sample's mask)
 * into an 8x8 target, then copies a single-sample image bound to the same memory, gw by gh times the size, and
 * prints its values: each gw by gh block is one pixel's grid of samples. On the RTX 3090 the 2, 4 and 8 sample grids
 * are Mesa NVK's cells (src/nouveau/nil/image.rs sa_offset), for color and stencil alike; its only 16-sample target
 * is a stencil one. Vulkan leaves the contents of an aliasing image undefined, so this measures the hardware, and
 * says nothing on other vendors.
 *
 * Usage: msaa_cells <samples> [s8]. Build: see README.md.
 */
#include "vkg_types.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* av.vert, af.frag and afm.frag below, by `glslc --target-env=vulkan1.3 -O`. */
// #version 450
// layout(location = 0) flat out uint inst;
// void main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); inst = uint(gl_InstanceIndex); }

// #version 450
// layout(location = 0) out uvec4 o;
// void main() { o = uvec4(uint(gl_SampleID) + 1u, 0u, 0u, 0u); }

// #version 450
// layout(location = 0) flat in uint inst;
// void main() { gl_SampleMask[0] = int(1u << inst); }
static const uint32_t vert[] = { 0x07230203, 0x00010600, 0x000d000b, 0x00000031, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0009000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x0000001d, 0x0000002c, 0x0000002d, 0x00040047, 0x0000000c, 0x0000000b, 0x0000002a, 0x00030047, 0x0000001b, 0x00000002, 0x00050048, 0x0000001b, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000001b, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000001b, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000001b, 0x00000003, 0x0000000b, 0x00000004, 0x00030047, 0x0000002c, 0x0000000e, 0x00040047, 0x0000002c, 0x0000001e, 0x00000000, 0x00040047, 0x0000002d, 0x0000000b, 0x0000002b, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040015, 0x0000000a, 0x00000020, 0x00000001, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x0000000a, 0x0000000e, 0x00000001, 0x0004002b, 0x0000000a, 0x00000010, 0x00000002, 0x00040017, 0x00000017, 0x00000006, 0x00000004, 0x00040015, 0x00000018, 0x00000020, 0x00000000, 0x0004002b, 0x00000018, 0x00000019, 0x00000001, 0x0004001c, 0x0000001a, 0x00000006, 0x00000019, 0x0006001e, 0x0000001b, 0x00000017, 0x00000006, 0x0000001a, 0x0000001a, 0x00040020, 0x0000001c, 0x00000003, 0x0000001b, 0x0004003b, 0x0000001c, 0x0000001d, 0x00000003, 0x0004002b, 0x0000000a, 0x0000001e, 0x00000000, 0x0004002b, 0x00000006, 0x00000020, 0x40000000, 0x0004002b, 0x00000006, 0x00000022, 0x3f800000, 0x0004002b, 0x00000006, 0x00000025, 0x00000000, 0x00040020, 0x00000029, 0x00000003, 0x00000017, 0x00040020, 0x0000002b, 0x00000003, 0x00000018, 0x0004003b, 0x0000002b, 0x0000002c, 0x00000003, 0x0004003b, 0x0000000b, 0x0000002d, 0x00000001, 0x0005002c, 0x00000007, 0x00000030, 0x00000022, 0x00000022, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c, 0x000500c4, 0x0000000a, 0x0000000f, 0x0000000d, 0x0000000e, 0x000500c7, 0x0000000a, 0x00000011, 0x0000000f, 0x00000010, 0x0004006f, 0x00000006, 0x00000012, 0x00000011, 0x000500c7, 0x0000000a, 0x00000014, 0x0000000d, 0x00000010, 0x0004006f, 0x00000006, 0x00000015, 0x00000014, 0x00050050, 0x00000007, 0x00000016, 0x00000012, 0x00000015, 0x0005008e, 0x00000007, 0x00000021, 0x00000016, 0x00000020, 0x00050083, 0x00000007, 0x00000024, 0x00000021, 0x00000030, 0x00050051, 0x00000006, 0x00000026, 0x00000024, 0x00000000, 0x00050051, 0x00000006, 0x00000027, 0x00000024, 0x00000001, 0x00070050, 0x00000017, 0x00000028, 0x00000026, 0x00000027, 0x00000025, 0x00000022, 0x00050041, 0x00000029, 0x0000002a, 0x0000001d, 0x0000001e, 0x0003003e, 0x0000002a, 0x00000028, 0x0004003d, 0x0000000a, 0x0000002e, 0x0000002d, 0x0004007c, 0x00000018, 0x0000002f, 0x0000002e, 0x0003003e, 0x0000002c, 0x0000002f, 0x000100fd, 0x00010038 };
static const uint32_t frag[] = { 0x07230203, 0x00010600, 0x000d000b, 0x00000013, 0x00000000, 0x00020011, 0x00000001, 0x00020011, 0x00000023, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000c, 0x00030010, 0x00000004, 0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000c, 0x0000000b, 0x00000012, 0x00030047, 0x0000000c, 0x0000000e, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x00040015, 0x0000000a, 0x00000020, 0x00000001, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x00000006, 0x0000000f, 0x00000001, 0x0004002b, 0x00000006, 0x00000011, 0x00000000, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c, 0x0004007c, 0x00000006, 0x0000000e, 0x0000000d, 0x00050080, 0x00000006, 0x00000010, 0x0000000e, 0x0000000f, 0x00070050, 0x00000007, 0x00000012, 0x00000010, 0x00000011, 0x00000011, 0x00000011, 0x0003003e, 0x00000009, 0x00000012, 0x000100fd, 0x00010038 };
static const uint32_t frag_mask[] = { 0x07230203, 0x00010600, 0x000d000b, 0x00000014, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000b, 0x0000000e, 0x00030010, 0x00000004, 0x00000007, 0x00040047, 0x0000000b, 0x0000000b, 0x00000014, 0x00030047, 0x0000000e, 0x0000000e, 0x00040047, 0x0000000e, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000001, 0x00040015, 0x00000007, 0x00000020, 0x00000000, 0x0004002b, 0x00000007, 0x00000008, 0x00000001, 0x0004001c, 0x00000009, 0x00000006, 0x00000008, 0x00040020, 0x0000000a, 0x00000003, 0x00000009, 0x0004003b, 0x0000000a, 0x0000000b, 0x00000003, 0x0004002b, 0x00000006, 0x0000000c, 0x00000000, 0x00040020, 0x0000000d, 0x00000001, 0x00000007, 0x0004003b, 0x0000000d, 0x0000000e, 0x00000001, 0x00040020, 0x00000012, 0x00000003, 0x00000006, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x00000007, 0x0000000f, 0x0000000e, 0x000500c4, 0x00000007, 0x00000010, 0x00000008, 0x0000000f, 0x0004007c, 0x00000006, 0x00000011, 0x00000010, 0x00050041, 0x00000012, 0x00000013, 0x0000000b, 0x0000000c, 0x0003003e, 0x00000013, 0x00000011, 0x000100fd, 0x00010038 };

#define C(x)                                                                                                           \
    do {                                                                                                               \
        VkResult r_ = (x);                                                                                             \
        if (r_) {                                                                                                      \
            fprintf(stderr, "%s: %d\n", #x, r_);                                                                       \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static VkDevice dev;
#define G(n) PFN_##n n##_ = (PFN_##n)gipa(inst, #n)

int main(int argc, char **argv)
{
    uint32_t n = argc > 1 ? (uint32_t)atoi(argv[1]) : 8, gw = n >= 2 ? 2 : 1, gh = n >= 4 ? 2 : 1;
    if (n >= 8)
        gw = 4;
    if (n >= 16)
        gh = 4;
    VkFormat fmt = argc > 2 && !strcmp(argv[2], "s8") ? VK_FORMAT_S8_UINT : VK_FORMAT_R8G8B8A8_UINT;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib)
        return 1;
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst;
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = (1u << 22) | (3u << 12) };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    C(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst));
    G(vkEnumeratePhysicalDevices);
    G(vkCreateDevice);
    G(vkGetDeviceProcAddr);
    G(vkGetPhysicalDeviceMemoryProperties);
    G(vkGetPhysicalDeviceProperties);
    uint32_t np = 8;
    VkPhysicalDevice ps[8];
    C(vkEnumeratePhysicalDevices_(inst, &np, ps));
    VkPhysicalDevice phys = NULL;
    for (uint32_t i = 0; i < np; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties_(ps[i], &p);
        if (strstr(p.deviceName, "NVIDIA"))
            phys = ps[i];
    }
    float prio = 1;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                   .pQueuePriorities = &prio };
    VkPhysicalDeviceFeatures pf = { .sampleRateShading = VK_TRUE };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qi, .pEnabledFeatures = &pf };
    C(vkCreateDevice_(phys, &dci, NULL, &dev));
#define D(nm) PFN_##nm nm##_ = (PFN_##nm)vkGetDeviceProcAddr_(dev, #nm)
    D(vkGetDeviceQueue);
    D(vkCreateImage);
    D(vkGetImageMemoryRequirements);
    D(vkAllocateMemory);
    D(vkBindImageMemory);
    D(vkCreateImageView);
    D(vkCreateBuffer);
    D(vkGetBufferMemoryRequirements);
    D(vkBindBufferMemory);
    D(vkMapMemory);
    D(vkCreateShaderModule);
    D(vkCreatePipelineLayout);
    D(vkCreateGraphicsPipelines);
    D(vkCreateCommandPool);
    D(vkAllocateCommandBuffers);
    D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer);
    D(vkCmdPipelineBarrier);
    D(vkCmdBeginRendering);
    D(vkCmdEndRendering);
    D(vkCmdBindPipeline);
    D(vkCmdDraw);
    D(vkCmdCopyImageToBuffer);
    D(vkQueueSubmit);
    D(vkQueueWaitIdle);
    D(vkCmdSetStencilReference);
    VkQueue q;
    vkGetDeviceQueue_(dev, 0, 0, &q);
    const uint32_t W = 8, H = 8;
    int stencil = fmt == VK_FORMAT_S8_UINT;
    VkImageCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_ALIAS_BIT,
                             .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { W, H, 1 }, .mipLevels = 1,
                             .arrayLayers = 1, .samples = (VkSampleCountFlagBits)n, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = stencil ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                              : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
    VkImage ms, ss;
    C(vkCreateImage_(dev, &mi, NULL, &ms));
    VkImageCreateInfo si = mi;
    si.samples = VK_SAMPLE_COUNT_1_BIT;
    si.extent = (VkExtent3D){ W * gw, H * gh, 1 };
    si.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    C(vkCreateImage_(dev, &si, NULL, &ss));
    VkMemoryRequirements mr, sr;
    vkGetImageMemoryRequirements_(dev, ms, &mr);
    vkGetImageMemoryRequirements_(dev, ss, &sr);
    printf("ms %llu bytes types 0x%x, ss %llu bytes types 0x%x\n", (unsigned long long)mr.size, mr.memoryTypeBits,
           (unsigned long long)sr.size, sr.memoryTypeBits);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties_(phys, &mp);
    uint32_t t = 0;
    while (!((mr.memoryTypeBits & sr.memoryTypeBits) >> t & 1))
        t++;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size > sr.size ? mr.size : sr.size, .memoryTypeIndex = t };
    VkDeviceMemory mem;
    C(vkAllocateMemory_(dev, &mai, NULL, &mem));
    C(vkBindImageMemory_(dev, ms, mem, 0));
    C(vkBindImageMemory_(dev, ss, mem, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = ms,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                  .subresourceRange = { stencil ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                                : VK_IMAGE_ASPECT_COLOR_BIT,
                                                        0, 1, 0, 1 } };
    VkImageView view;
    C(vkCreateImageView_(dev, &vci, NULL, &view));
    VkShaderModuleCreateInfo vs = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vert,
                                    .pCode = vert };
    VkShaderModuleCreateInfo fs = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = stencil ? sizeof frag_mask : sizeof frag,
                                    .pCode = stencil ? frag_mask : frag };
    VkShaderModule vm, fm;
    C(vkCreateShaderModule_(dev, &vs, NULL, &vm));
    C(vkCreateShaderModule_(dev, &fs, NULL, &fm));
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl;
    C(vkCreatePipelineLayout_(dev, &pli, NULL, &pl));
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vm, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fm, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, (float)W, (float)H, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                              .viewportCount = 1, .pViewports = &vp, .scissorCount = 1,
                                              .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo msi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                 .rasterizationSamples = (VkSampleCountFlagBits)n,
                                                 .sampleShadingEnable = !stencil, .minSampleShading = 1 };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                .attachmentCount = stencil ? 0 : 1, .pAttachments = &ba };
    VkStencilOpState so = { .failOp = VK_STENCIL_OP_REPLACE, .passOp = VK_STENCIL_OP_REPLACE,
                            .depthFailOp = VK_STENCIL_OP_REPLACE, .compareOp = VK_COMPARE_OP_ALWAYS,
                            .compareMask = 0xff, .writeMask = 0xff, .reference = 0 };
    VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                  .stencilTestEnable = VK_TRUE, .front = so, .back = so };
    VkDynamicState dyn = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 1, .pDynamicStates = &dyn };
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                         .colorAttachmentCount = stencil ? 0 : 1, .pColorAttachmentFormats = &fmt,
                                         .stencilAttachmentFormat = stencil ? fmt : VK_FORMAT_UNDEFINED };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                         .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                         .pInputAssemblyState = &ia, .pViewportState = &vps,
                                         .pRasterizationState = &rs, .pMultisampleState = &msi,
                                         .pDepthStencilState = stencil ? &dss : NULL, .pColorBlendState = &cbs,
                                         .pDynamicState = stencil ? &ds : NULL, .layout = pl };
    VkPipeline pipe;
    C(vkCreateGraphicsPipelines_(dev, NULL, 1, &gpi, NULL, &pipe));
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)W * gw * H * gh * 4,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buf;
    C(vkCreateBuffer_(dev, &bci, NULL, &buf));
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements_(dev, buf, &br);
    uint32_t bt = 0;
    while (!((br.memoryTypeBits >> bt & 1) && (mp.memoryTypes[bt].propertyFlags & 6) == 6))
        bt++;
    VkMemoryAllocateInfo bai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = br.size,
                                 .memoryTypeIndex = bt };
    VkDeviceMemory bm;
    C(vkAllocateMemory_(dev, &bai, NULL, &bm));
    C(vkBindBufferMemory_(dev, buf, bm, 0));
    uint8_t *map;
    C(vkMapMemory_(dev, bm, 0, br.size, 0, (void **)&map));
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool cp;
    C(vkCreateCommandPool_(dev, &cpi, NULL, &cp));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
                                        .commandBufferCount = 1 };
    VkCommandBuffer cb;
    C(vkAllocateCommandBuffers_(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    C(vkBeginCommandBuffer_(cb, &cbi));
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = 0x1ffff,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = ~0u, .dstQueueFamilyIndex = ~0u, .image = ms,
                                .subresourceRange = vci.subresourceRange };
    vkCmdPipelineBarrier_(cb, 0x10000, 0x10000, 0, 0, NULL, 0, NULL, 1, &ib);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
                                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo rinf = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = sc, .layerCount = 1,
                             .colorAttachmentCount = stencil ? 0 : 1, .pColorAttachments = &ca,
                             .pStencilAttachment = stencil ? &ca : NULL };
    vkCmdBeginRendering_(cb, &rinf);
    vkCmdBindPipeline_(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    if (stencil) {
        /* one draw per sample, its sample mask that sample only (the shader writes gl_SampleMask = 1 << ref) */
        for (uint32_t s = 0; s < n; s++) {
            vkCmdSetStencilReference_(cb, VK_STENCIL_FACE_FRONT_AND_BACK, s + 1);
            vkCmdDraw_(cb, 3, 1, 0, s);
        }
    } else {
        vkCmdDraw_(cb, 3, 1, 0, 0);
    }
    vkCmdEndRendering_(cb);
    VkImageMemoryBarrier ib2 = ib;
    ib2.image = ss;
    ib2.subresourceRange.aspectMask = vci.subresourceRange.aspectMask;
    vkCmdPipelineBarrier_(cb, 0x10000, 0x10000, 0, 0, NULL, 0, NULL, 1, &ib2);
    VkBufferImageCopy bic = { .imageSubresource = { vci.subresourceRange.aspectMask, 0, 0, 1 },
                              .imageExtent = { W * gw, H * gh, 1 } };
    vkCmdCopyImageToBuffer_(cb, ss, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bic);
    C(vkEndCommandBuffer_(cb));
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    C(vkQueueSubmit_(q, 1, &sub, NULL));
    C(vkQueueWaitIdle_(q));
    uint32_t bpp = stencil ? 1 : 4;
    for (uint32_t y = 0; y < H * gh; y++) {
        for (uint32_t x = 0; x < W * gw; x++)
            printf("%3u", map[(y * W * gw + x) * bpp]);
        printf("\n");
    }
    return 0;
}
