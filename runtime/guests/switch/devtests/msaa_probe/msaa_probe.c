/**
 * @file msaa_probe.c
 * @brief Measures where a Vulkan device's rasterizer puts each sample of 2, 4, 8 and 16 sample render targets, in
 * 1/16 pixel, and prints it with the device's standardSampleLocations and color sample counts as a JSON object
 * (the "ReliftSamplePositions" member of the runtime's device.json).
 *
 * Each sample's x is found by drawing the half plane left of an edge at x = (k + 1/2) / 16, k = 0 to 15, into a 1x1
 * multisample target and reading which samples it covered: the first edge right of a sample covers it, so a sample
 * at k / 16 flips at edge k. y likewise with the half plane above an edge. Positions off the 1/16 grid show as the
 * grid step they fall in.
 *
 * Usage: msaa_probe [driver.so]. Without an argument it loads the system Vulkan loader; with one it loads that
 * Vulkan driver directly (as an installable client driver: vk_icdGetInstanceProcAddr), as relift's Android host
 * loads a Turnip package. RELIFT_VK_DEVICE picks the device whose name contains it. Build: see
 * runtime/guests/switch/devtests/msaa_probe/README.md.
 */
#include "vkg_types.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* edge.vert, by `glslc --target-env=vulkan1.1 -O`: */
// #version 450
// // The half plane left of (axis 0) or above (axis 1) the edge at e pixels of a 1x1 target, as two triangles.
// layout(push_constant) uniform P { float e; uint axis; } p;
// void main()
// {
//     const vec2 c[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
//     vec2 q = c[gl_VertexIndex];
//     float lo = -3.0, hi = 2.0 * p.e - 1.0;
//     vec2 v = p.axis == 0u ? vec2(mix(lo, hi, q.x), mix(-3.0, 3.0, q.y)) : vec2(mix(-3.0, 3.0, q.x), mix(lo, hi, q.y));
//     gl_Position = vec4(v, 0.0, 1.0);
// }
static const uint32_t edge_vert[] = {
    0x07230203, 0x00010300, 0x000d000b, 0x0000005b, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x00000016, 0x0000004f, 0x00040047,
    0x00000016, 0x0000000b, 0x0000002a, 0x00030047, 0x00000021, 0x00000002, 0x00050048, 0x00000021,
    0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000021, 0x00000001, 0x00000023, 0x00000004,
    0x00030047, 0x0000004d, 0x00000002, 0x00050048, 0x0000004d, 0x00000000, 0x0000000b, 0x00000000,
    0x00050048, 0x0000004d, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000004d, 0x00000002,
    0x0000000b, 0x00000003, 0x00050048, 0x0000004d, 0x00000003, 0x0000000b, 0x00000004, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017,
    0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00040015,
    0x0000000a, 0x00000020, 0x00000000, 0x0004002b, 0x0000000a, 0x0000000b, 0x00000006, 0x0004001c,
    0x0000000c, 0x00000007, 0x0000000b, 0x0004002b, 0x00000006, 0x0000000d, 0x00000000, 0x0005002c,
    0x00000007, 0x0000000e, 0x0000000d, 0x0000000d, 0x0004002b, 0x00000006, 0x0000000f, 0x3f800000,
    0x0005002c, 0x00000007, 0x00000010, 0x0000000f, 0x0000000d, 0x0005002c, 0x00000007, 0x00000011,
    0x0000000d, 0x0000000f, 0x0005002c, 0x00000007, 0x00000012, 0x0000000f, 0x0000000f, 0x0009002c,
    0x0000000c, 0x00000013, 0x0000000e, 0x00000010, 0x00000011, 0x00000010, 0x00000012, 0x00000011,
    0x00040015, 0x00000014, 0x00000020, 0x00000001, 0x00040020, 0x00000015, 0x00000001, 0x00000014,
    0x0004003b, 0x00000015, 0x00000016, 0x00000001, 0x00040020, 0x00000018, 0x00000007, 0x0000000c,
    0x0004002b, 0x00000006, 0x0000001e, 0xc0400000, 0x0004002b, 0x00000006, 0x00000020, 0x40000000,
    0x0004001e, 0x00000021, 0x00000006, 0x0000000a, 0x00040020, 0x00000022, 0x00000009, 0x00000021,
    0x0004003b, 0x00000022, 0x00000023, 0x00000009, 0x0004002b, 0x00000014, 0x00000024, 0x00000000,
    0x00040020, 0x00000025, 0x00000009, 0x00000006, 0x0004002b, 0x00000014, 0x0000002b, 0x00000001,
    0x00040020, 0x0000002c, 0x00000009, 0x0000000a, 0x0004002b, 0x0000000a, 0x0000002f, 0x00000000,
    0x00020014, 0x00000030, 0x0004002b, 0x00000006, 0x0000003a, 0x40400000, 0x0004002b, 0x0000000a,
    0x0000003b, 0x00000001, 0x00040017, 0x0000004b, 0x00000006, 0x00000004, 0x0004001c, 0x0000004c,
    0x00000006, 0x0000003b, 0x0006001e, 0x0000004d, 0x0000004b, 0x00000006, 0x0000004c, 0x0000004c,
    0x00040020, 0x0000004e, 0x00000003, 0x0000004d, 0x0004003b, 0x0000004e, 0x0000004f, 0x00000003,
    0x00040020, 0x00000054, 0x00000003, 0x0000004b, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000018, 0x00000019, 0x00000007, 0x0004003d,
    0x00000014, 0x00000017, 0x00000016, 0x0003003e, 0x00000019, 0x00000013, 0x00050041, 0x00000008,
    0x0000001a, 0x00000019, 0x00000017, 0x0004003d, 0x00000007, 0x0000001b, 0x0000001a, 0x00050041,
    0x00000025, 0x00000026, 0x00000023, 0x00000024, 0x0004003d, 0x00000006, 0x00000027, 0x00000026,
    0x00050085, 0x00000006, 0x00000028, 0x00000020, 0x00000027, 0x00050083, 0x00000006, 0x00000029,
    0x00000028, 0x0000000f, 0x00050041, 0x0000002c, 0x0000002d, 0x00000023, 0x0000002b, 0x0004003d,
    0x0000000a, 0x0000002e, 0x0000002d, 0x000500aa, 0x00000030, 0x00000031, 0x0000002e, 0x0000002f,
    0x000300f7, 0x00000034, 0x00000000, 0x000400fa, 0x00000031, 0x00000033, 0x00000040, 0x000200f8,
    0x00000033, 0x00050051, 0x00000006, 0x00000038, 0x0000001b, 0x00000000, 0x0008000c, 0x00000006,
    0x00000039, 0x00000001, 0x0000002e, 0x0000001e, 0x00000029, 0x00000038, 0x00050051, 0x00000006,
    0x0000003d, 0x0000001b, 0x00000001, 0x0008000c, 0x00000006, 0x0000003e, 0x00000001, 0x0000002e,
    0x0000001e, 0x0000003a, 0x0000003d, 0x00050050, 0x00000007, 0x0000003f, 0x00000039, 0x0000003e,
    0x000200f9, 0x00000034, 0x000200f8, 0x00000040, 0x00050051, 0x00000006, 0x00000042, 0x0000001b,
    0x00000000, 0x0008000c, 0x00000006, 0x00000043, 0x00000001, 0x0000002e, 0x0000001e, 0x0000003a,
    0x00000042, 0x00050051, 0x00000006, 0x00000047, 0x0000001b, 0x00000001, 0x0008000c, 0x00000006,
    0x00000048, 0x00000001, 0x0000002e, 0x0000001e, 0x00000029, 0x00000047, 0x00050050, 0x00000007,
    0x00000049, 0x00000043, 0x00000048, 0x000200f9, 0x00000034, 0x000200f8, 0x00000034, 0x000700f5,
    0x00000007, 0x0000005a, 0x0000003f, 0x00000033, 0x00000049, 0x00000040, 0x00050051, 0x00000006,
    0x00000051, 0x0000005a, 0x00000000, 0x00050051, 0x00000006, 0x00000052, 0x0000005a, 0x00000001,
    0x00070050, 0x0000004b, 0x00000053, 0x00000051, 0x00000052, 0x0000000d, 0x0000000f, 0x00050041,
    0x00000054, 0x00000055, 0x0000004f, 0x00000024, 0x0003003e, 0x00000055, 0x00000053, 0x000100fd,
    0x00010038,
};

/* one.frag, by `glslc --target-env=vulkan1.1 -O`: */
// #version 450
// layout(location = 0) out vec4 o;
// void main() { o = vec4(1.0); }
static const uint32_t one_frag[] = {
    0x07230203, 0x00010300, 0x000d000b, 0x0000000c, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009,
    0x00000003, 0x0004002b, 0x00000006, 0x0000000a, 0x3f800000, 0x0007002c, 0x00000007, 0x0000000b,
    0x0000000a, 0x0000000a, 0x0000000a, 0x0000000a, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0003003e, 0x00000009, 0x0000000b, 0x000100fd, 0x00010038,
};

/* mask.comp, by `glslc --target-env=vulkan1.1 -O`: */
// #version 450
// #extension GL_EXT_samplerless_texture_functions : require
// // The mask of the samples of pixel (0, 0) the draw covered.
// layout(local_size_x = 1) in;
// layout(binding = 0) uniform texture2DMS ms;
// layout(binding = 1) buffer Out { uint o; };
// layout(push_constant) uniform P { uint samples; } p;
// void main()
// {
//     uint m = 0u;
//     for (uint s = 0u; s < p.samples; s++)
//         m |= (texelFetch(ms, ivec2(0), int(s)).r > 0.5 ? 1u : 0u) << s;
//     o = m;
// }
static const uint32_t mask_comp[] = {
    0x07230203, 0x00010300, 0x000d000b, 0x0000003c, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0005000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
    0x00000001, 0x00000001, 0x00000001, 0x00030047, 0x00000011, 0x00000002, 0x00050048, 0x00000011,
    0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x0000001e, 0x00000021, 0x00000000, 0x00040047,
    0x0000001e, 0x00000022, 0x00000000, 0x00030047, 0x00000032, 0x00000002, 0x00050048, 0x00000032,
    0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x00000034, 0x00000021, 0x00000001, 0x00040047,
    0x00000034, 0x00000022, 0x00000000, 0x00040047, 0x00000039, 0x0000000b, 0x00000019, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000,
    0x0004002b, 0x00000006, 0x00000009, 0x00000000, 0x0003001e, 0x00000011, 0x00000006, 0x00040020,
    0x00000012, 0x00000009, 0x00000011, 0x0004003b, 0x00000012, 0x00000013, 0x00000009, 0x00040015,
    0x00000014, 0x00000020, 0x00000001, 0x0004002b, 0x00000014, 0x00000015, 0x00000000, 0x00040020,
    0x00000016, 0x00000009, 0x00000006, 0x00020014, 0x00000019, 0x00030016, 0x0000001b, 0x00000020,
    0x00090019, 0x0000001c, 0x0000001b, 0x00000001, 0x00000000, 0x00000000, 0x00000001, 0x00000001,
    0x00000000, 0x00040020, 0x0000001d, 0x00000000, 0x0000001c, 0x0004003b, 0x0000001d, 0x0000001e,
    0x00000000, 0x00040017, 0x00000020, 0x00000014, 0x00000002, 0x0005002c, 0x00000020, 0x00000021,
    0x00000015, 0x00000015, 0x00040017, 0x00000024, 0x0000001b, 0x00000004, 0x0004002b, 0x0000001b,
    0x00000027, 0x3f000000, 0x0004002b, 0x00000006, 0x00000029, 0x00000001, 0x0004002b, 0x00000014,
    0x00000030, 0x00000001, 0x0003001e, 0x00000032, 0x00000006, 0x00040020, 0x00000033, 0x0000000c,
    0x00000032, 0x0004003b, 0x00000033, 0x00000034, 0x0000000c, 0x00040020, 0x00000036, 0x0000000c,
    0x00000006, 0x00040017, 0x00000038, 0x00000006, 0x00000003, 0x0006002c, 0x00000038, 0x00000039,
    0x00000029, 0x00000029, 0x00000029, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x000200f9, 0x0000000b, 0x000200f8, 0x0000000b, 0x000700f5, 0x00000006,
    0x0000003b, 0x00000009, 0x00000005, 0x0000002e, 0x0000000c, 0x000700f5, 0x00000006, 0x0000003a,
    0x00000009, 0x00000005, 0x00000031, 0x0000000c, 0x00050041, 0x00000016, 0x00000017, 0x00000013,
    0x00000015, 0x0004003d, 0x00000006, 0x00000018, 0x00000017, 0x000500b0, 0x00000019, 0x0000001a,
    0x0000003a, 0x00000018, 0x000400f6, 0x0000000d, 0x0000000c, 0x00000000, 0x000400fa, 0x0000001a,
    0x0000000c, 0x0000000d, 0x000200f8, 0x0000000c, 0x0004003d, 0x0000001c, 0x0000001f, 0x0000001e,
    0x0004007c, 0x00000014, 0x00000023, 0x0000003a, 0x0007005f, 0x00000024, 0x00000025, 0x0000001f,
    0x00000021, 0x00000040, 0x00000023, 0x00050051, 0x0000001b, 0x00000026, 0x00000025, 0x00000000,
    0x000500ba, 0x00000019, 0x00000028, 0x00000026, 0x00000027, 0x000600a9, 0x00000006, 0x0000002a,
    0x00000028, 0x00000029, 0x00000009, 0x000500c4, 0x00000006, 0x0000002c, 0x0000002a, 0x0000003a,
    0x000500c5, 0x00000006, 0x0000002e, 0x0000003b, 0x0000002c, 0x00050080, 0x00000006, 0x00000031,
    0x0000003a, 0x00000030, 0x000200f9, 0x0000000b, 0x000200f8, 0x0000000d, 0x00050041, 0x00000036,
    0x00000037, 0x00000034, 0x00000015, 0x0003003e, 0x00000037, 0x0000003b, 0x000100fd, 0x00010038,
};

#define CHECKVK(x)                                                                                                     \
    do {                                                                                                               \
        VkResult r_ = (x);                                                                                             \
        if (r_ != VK_SUCCESS) {                                                                                        \
            fprintf(stderr, "msaa_probe: %s: %d\n", #x, (int)r_);                                                     \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t family;

#define F(name) static PFN_##name name##_;
#define FNS(X)                                                                                                         \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties)        \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceMemoryProperties)                                  \
    X(vkGetPhysicalDeviceImageFormatProperties)
#define DFNS(X)                                                                                                        \
    X(vkGetDeviceQueue) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkBindImageMemory)     \
    X(vkCreateImageView) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory)      \
    X(vkCreateShaderModule) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines)       \
    X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)   \
    X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)                 \
    X(vkCmdPipelineBarrier) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline)                       \
    X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdBindDescriptorSets) X(vkCmdDispatch) X(vkQueueSubmit)                  \
    X(vkQueueWaitIdle) X(vkResetCommandBuffer) X(vkCreateRenderPass) X(vkCreateFramebuffer)
FNS(F)
DFNS(F)
#undef F

static VkShaderModule module_of(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule m;
    CHECKVK(vkCreateShaderModule_(dev, &ci, NULL, &m));
    return m;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties_(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "msaa_probe: no memory type 0x%x\n", (unsigned)want);
    exit(1);
}

static void open_device(const char *icd)
{
    void *lib = icd ? dlopen(icd, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!icd) {
        static const char *const names[] = { "libvulkan.so.1", "libvulkan.so", "libvulkan.dylib", "libMoltenVK.dylib" };
        for (unsigned i = 0; !lib && i < sizeof names / sizeof names[0]; i++)
            lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    }
    if (!lib) {
        fprintf(stderr, "msaa_probe: %s\n", dlerror());
        exit(1);
    }
    if (icd) {
        typedef VkResult (*negotiate_fn)(uint32_t *);
        negotiate_fn neg = (negotiate_fn)dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
        uint32_t v = 5;
        if (neg)
            neg(&v);
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    } else {
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    }
    if (!gipa) {
        fprintf(stderr, "msaa_probe: no vkGetInstanceProcAddr in %s\n", icd ? icd : "the loader");
        exit(1);
    }
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = (1u << 22) | (1u << 12) };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    CHECKVK(create(&ici, NULL, &inst));
#define L(name) name##_ = (PFN_##name)gipa(inst, #name);
    FNS(L)
#undef L
    uint32_t n = 0;
    CHECKVK(vkEnumeratePhysicalDevices_(inst, &n, NULL));
    VkPhysicalDevice *all = calloc(n ? n : 1, sizeof *all);
    CHECKVK(vkEnumeratePhysicalDevices_(inst, &n, all));
    const char *want = getenv("RELIFT_VK_DEVICE");
    for (uint32_t i = 0; i < n && !phys; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties_(all[i], &p);
        if (!want || strstr(p.deviceName, want))
            phys = all[i];
    }
    free(all);
    if (!phys) {
        fprintf(stderr, "msaa_probe: no device\n");
        exit(1);
    }
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties_(phys, &nq, NULL);
    VkQueueFamilyProperties *q = calloc(nq ? nq : 1, sizeof *q);
    vkGetPhysicalDeviceQueueFamilyProperties_(phys, &nq, q);
    for (family = 0; family < nq && !(q[family].queueFlags & VK_QUEUE_GRAPHICS_BIT); family++)
        ;
    free(q);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
                                   .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qi };
    CHECKVK(vkCreateDevice_(phys, &dci, NULL, &dev));
#define D(name) name##_ = (PFN_##name)vkGetDeviceProcAddr_(dev, #name);
    DFNS(D)
#undef D
    vkGetDeviceQueue_(dev, family, 0, &queue);
}

/** @brief One sample count's target, pipelines and readback. */
typedef struct rig {
    VkImage img;
    VkDeviceMemory img_mem, buf_mem;
    VkImageView view;
    VkRenderPass pass;
    VkFramebuffer fb;
    VkPipelineLayout gpl, cpl;
    VkPipeline gpipe, cpipe;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkBuffer buf;
    uint32_t *map;
    VkCommandPool cmd_pool;
    VkCommandBuffer cb;
} rig;

static void rig_up(rig *k, VkSampleCountFlagBits s)
{
    memset(k, 0, sizeof *k);
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                              .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 1, 1, 1 }, .mipLevels = 1,
                              .arrayLayers = 1, .samples = s, .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
    CHECKVK(vkCreateImage_(dev, &ici, NULL, &k->img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements_(dev, k->img, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                 .memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CHECKVK(vkAllocateMemory_(dev, &mai, NULL, &k->img_mem));
    CHECKVK(vkBindImageMemory_(dev, k->img, k->img_mem, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = k->img,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                  .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CHECKVK(vkCreateImageView_(dev, &vci, NULL, &k->view));
    VkAttachmentDescription ad = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = s,
                                   .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                   .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                   .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                   .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                .pColorAttachments = &ar };
    VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
                                .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                   .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd, .dependencyCount = 1,
                                   .pDependencies = &dep };
    CHECKVK(vkCreateRenderPass_(dev, &rpi, NULL, &k->pass));
    VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = k->pass,
                                    .attachmentCount = 1, .pAttachments = &k->view, .width = 1, .height = 1,
                                    .layers = 1 };
    CHECKVK(vkCreateFramebuffer_(dev, &fbi, NULL, &k->fb));
    VkPushConstantRange gpr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &gpr };
    CHECKVK(vkCreatePipelineLayout_(dev, &pli, NULL, &k->gpl));
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = module_of(edge_vert, sizeof edge_vert), .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = module_of(one_frag, sizeof one_frag), .pName = "main" },
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
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = s };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                .attachmentCount = 1, .pAttachments = &ba };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                         .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                         .pViewportState = &vps, .pRasterizationState = &rs,
                                         .pMultisampleState = &ms, .pColorBlendState = &cbs, .layout = k->gpl,
                                         .renderPass = k->pass };
    CHECKVK(vkCreateGraphicsPipelines_(dev, NULL, 1, &gpi, NULL, &k->gpipe));
    VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                                          { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 2, .pBindings = b };
    VkDescriptorSetLayout dsl;
    CHECKVK(vkCreateDescriptorSetLayout_(dev, &li, NULL, &dsl));
    VkPushConstantRange cpr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo cli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                       .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &cpr };
    CHECKVK(vkCreatePipelineLayout_(dev, &cli, NULL, &k->cpl));
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                   .module = module_of(mask_comp, sizeof mask_comp), .pName = "main" },
                                        .layout = k->cpl };
    CHECKVK(vkCreateComputePipelines_(dev, NULL, 1, &cpi, NULL, &k->cpipe));
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
                               .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    CHECKVK(vkCreateBuffer_(dev, &bci, NULL, &k->buf));
    vkGetBufferMemoryRequirements_(dev, k->buf, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECKVK(vkAllocateMemory_(dev, &mai, NULL, &k->buf_mem));
    CHECKVK(vkBindBufferMemory_(dev, k->buf, k->buf_mem, 0));
    CHECKVK(vkMapMemory_(dev, k->buf_mem, 0, 256, 0, (void **)&k->map));
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                       .poolSizeCount = 2, .pPoolSizes = ps };
    CHECKVK(vkCreateDescriptorPool_(dev, &dpi, NULL, &k->pool));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = k->pool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    CHECKVK(vkAllocateDescriptorSets_(dev, &dai, &k->set));
    VkDescriptorImageInfo ii = { NULL, k->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorBufferInfo bi = { k->buf, 0, 256 };
    VkWriteDescriptorSet w[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = k->set, .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = k->set, .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi },
    };
    vkUpdateDescriptorSets_(dev, 2, w, 0, NULL);
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                     .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                     .queueFamilyIndex = family };
    CHECKVK(vkCreateCommandPool_(dev, &cpci, NULL, &k->cmd_pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = k->cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    CHECKVK(vkAllocateCommandBuffers_(dev, &cai, &k->cb));
}

/** @brief The mask of samples the half plane before edge e (pixels) on axis covers. */
static uint32_t covered(rig *k, uint32_t samples, float e, uint32_t axis)
{
    CHECKVK(vkResetCommandBuffer_(k->cb, 0));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECKVK(vkBeginCommandBuffer_(k->cb, &bi));
    VkClearValue clear = { 0 };
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = k->pass,
                                  .framebuffer = k->fb, .renderArea = { { 0, 0 }, { 1, 1 } }, .clearValueCount = 1,
                                  .pClearValues = &clear };
    vkCmdBeginRenderPass_(k->cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline_(k->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, k->gpipe);
    struct {
        float e;
        uint32_t axis;
    } pc = { e, axis };
    vkCmdPushConstants_(k->cb, k->gpl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, &pc);
    vkCmdDraw_(k->cb, 6, 1, 0, 0);
    vkCmdEndRenderPass_(k->cb);
    vkCmdBindPipeline_(k->cb, VK_PIPELINE_BIND_POINT_COMPUTE, k->cpipe);
    vkCmdBindDescriptorSets_(k->cb, VK_PIPELINE_BIND_POINT_COMPUTE, k->cpl, 0, 1, &k->set, 0, NULL);
    vkCmdPushConstants_(k->cb, k->cpl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &samples);
    vkCmdDispatch_(k->cb, 1, 1, 1);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier_(k->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL,
                          0, NULL);
    CHECKVK(vkEndCommandBuffer_(k->cb));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &k->cb };
    CHECKVK(vkQueueSubmit_(queue, 1, &si, NULL));
    CHECKVK(vkQueueWaitIdle_(queue));
    return *k->map;
}

/** @brief Vulkan's standard sample locations, in 1/16 pixel (the Vulkan specification's table). */
static const uint8_t standard[4][16][2] = {
    { { 12, 12 }, { 4, 4 } },
    { { 6, 2 }, { 14, 6 }, { 2, 10 }, { 10, 14 } },
    { { 9, 5 }, { 7, 11 }, { 13, 9 }, { 5, 3 }, { 3, 13 }, { 1, 7 }, { 11, 15 }, { 15, 1 } },
    { { 9, 9 }, { 7, 5 }, { 5, 10 }, { 12, 7 }, { 3, 6 }, { 10, 13 }, { 13, 11 }, { 11, 3 }, { 6, 14 }, { 8, 1 },
      { 4, 2 }, { 2, 12 }, { 0, 8 }, { 15, 4 }, { 14, 15 }, { 1, 0 } },
};

int main(int argc, char **argv)
{
    open_device(argc > 1 ? argv[1] : NULL);
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties_(phys, &p);
    VkImageFormatProperties ifp;
    CHECKVK(vkGetPhysicalDeviceImageFormatProperties_(phys, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                                      VK_IMAGE_TILING_OPTIMAL,
                                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                      0, &ifp));
    printf("{ \"ReliftSamplePositions\": { \"deviceName\": \"%s\", \"driverVersion\": %u, "
           "\"standardSampleLocations\": %s, \"framebufferColorSampleCounts\": %u",
           p.deviceName, p.driverVersion, p.limits.standardSampleLocations ? "true" : "false",
           (unsigned)p.limits.framebufferColorSampleCounts);
    bool all_standard = true;
    for (uint32_t c = 0; c < 4; c++) {
        uint32_t n = 2u << c;
        if (!(ifp.sampleCounts & n))
            continue;
        rig k;
        rig_up(&k, (VkSampleCountFlagBits)n);
        uint8_t at[16][2];
        memset(at, 0xff, sizeof at);
        for (uint32_t axis = 0; axis < 2; axis++)
            for (int32_t e = 0; e <= 16; e++) {
                uint32_t m = covered(&k, n, ((float)e + 0.5f) / 16.0f, axis);
                for (uint32_t s = 0; s < n; s++)
                    if ((m >> s & 1) && at[s][axis] == 0xff)
                        at[s][axis] = (uint8_t)e;
            }
        printf(", \"%u\": [", n);
        for (uint32_t s = 0; s < n; s++) {
            printf("%s[%u, %u]", s ? ", " : "", at[s][0], at[s][1]);
            all_standard &= at[s][0] == standard[c][s][0] && at[s][1] == standard[c][s][1];
        }
        printf("]");
    }
    printf(", \"measuredStandard\": %s } }\n", all_standard ? "true" : "false");
    return 0;
}
