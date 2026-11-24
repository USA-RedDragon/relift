// xrun <capture vs> <gs or -> <stride> <count> <draw vs> <draw fs> <out dir>: captures <count> points of the capture
// program (a vertex program, and a geometry program unless "-") into transform feedback buffer 0, <stride> bytes a
// vertex, with the rasterizer discarding; writes the buffer's bytes (capture.bin) and the byte count the counter
// buffer holds (count.txt). Then draws the captured vertices (vec4 at byte 0 of each, as attribute 0) as points into a
// 64x64 RGBA32F target with vkCmdDrawIndirectByteCountEXT, the host's DRAW_AUTO, and writes it (image.raw), for
// kat.py. XRUN_DEV picks the device by a part of its name (default NVIDIA); XRUN_CACHE=<file> writes the pipeline
// cache, from which kat.py carves the driver's code. XRUN_MEM=<record bytes>:<slots> runs a memory path translation
// instead: no transform feedback, a zeroed scratch of that many records whose device address goes to the push
// constants at byte 24 (and a written-page bitmap's at byte 16), written to scratch.bin; no draw follows. XRUN_TOPO
// (points, lines, triangles, lines-adjacency, triangles-adjacency; default points) is the capture draw's primitive.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "line %d: %s = %d\n", __LINE__, #x, r_); exit(1); } } while (0)

static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q; static VkCommandPool cp;
static VkPhysicalDeviceMemoryProperties mp; static VkPipelineCache pcache;
static PFN_vkCmdBindTransformFeedbackBuffersEXT bind_xfb; static PFN_vkCmdBeginTransformFeedbackEXT begin_xfb;
static PFN_vkCmdEndTransformFeedbackEXT end_xfb; static PFN_vkCmdDrawIndirectByteCountEXT draw_auto;
enum { W = 64, H = 64 };

static int mem_rec, mem_slots;

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags f) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
    fprintf(stderr, "no memory type\n"); exit(1);
}

typedef struct { VkBuffer b; VkDeviceMemory m; void *p; } Buf;
static Buf mkbuf(size_t n, VkBufferUsageFlags u) {
    Buf b;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, n, u | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkMemoryAllocateFlagsInfo fl = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0,
        (u & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0 };
    CK(vkCreateBuffer(dev, &bi, 0, &b.b));
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(dev, b.b, &r);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, r.size,
        memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CK(vkAllocateMemory(dev, &ai, 0, &b.m));
    CK(vkBindBufferMemory(dev, b.b, b.m, 0));
    CK(vkMapMemory(dev, b.m, 0, n, 0, &b.p));
    memset(b.p, 0, n);
    return b;
}

static void *rdfile(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(*n + 4); if (fread(b, 1, *n, f) != *n) exit(1); fclose(f); return b;
}

static void wrfile(const char *dir, const char *name, const void *p, size_t n) {
    char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb"); if (!f || fwrite(p, 1, n, f) != n) { perror(path); exit(1); } fclose(f);
}

static VkShaderModule shader(const char *p) {
    size_t n; uint32_t *c = rdfile(p, &n);
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, n, c };
    VkShaderModule m; CK(vkCreateShaderModule(dev, &mi, 0, &m)); free(c); return m;
}

static VkPipelineLayout layout(void) {
    VkPushConstantRange pr = { VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128 };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 0, 0, 1, &pr };
    VkPipelineLayout l; CK(vkCreatePipelineLayout(dev, &li, 0, &l)); return l;
}

/* A point list pipeline: with stride 0 no vertex input and the rasterizer discarding, else attribute 0 a vec4 at byte
   0 of binding 0, drawn into the RGBA32F target. */
static VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

static VkPipeline pipeline(VkPipelineLayout l, VkShaderModule vs, VkShaderModule gs, VkShaderModule fs, uint32_t stride) {
    VkPipelineShaderStageCreateInfo st[3]; uint32_t ns = 0;
    st[ns++] = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", 0 };
    if (gs) st[ns++] = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_GEOMETRY_BIT, gs, "main", 0 };
    if (fs) st[ns++] = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", 0 };
    VkVertexInputBindingDescription vb = { 0, stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, 0, 0,
        stride ? 1u : 0u, &vb, stride ? 1u : 0u, &va };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0,
        fs ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : topo, 0 };
    VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vp, 1, &sc };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, 0, 0, 0,
        fs ? VK_FALSE : VK_TRUE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1 };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState ba = { 0 }; ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, fs ? 1u : 0u, &ba };
    VkFormat fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, fs ? 1u : 0u, &fmt };
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, ns, st, &vi, &ia, 0, &vps, &rs, &ms, 0, &cb, 0, l };
    VkPipeline p; CK(vkCreateGraphicsPipelines(dev, pcache, 1, &pi, 0, &p)); return p;
}

static VkCommandBuffer begin(void) {
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CK(vkBeginCommandBuffer(cb, &bi)); return cb;
}

static void submit(VkCommandBuffer cb) {
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };
    CK(vkQueueSubmit(q, 1, &si, 0)); CK(vkQueueWaitIdle(q));
    vkFreeCommandBuffers(dev, cp, 1, &cb);
}

int main(int argc, char **argv) {
    if (argc != 8) { fprintf(stderr, "usage: xrun <capture vs> <gs|-> <stride> <count> <draw vs> <draw fs> <out dir>\n"); return 1; }
    uint32_t stride = (uint32_t)atoi(argv[3]), count = (uint32_t)atoi(argv[4]);
    const char *want = getenv("XRUN_DEV") ? getenv("XRUN_DEV") : "NVIDIA";
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "xrun", 1, 0, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai };
    VkInstance inst; CK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 8; VkPhysicalDevice pds[8]; CK(vkEnumeratePhysicalDevices(inst, &n, pds));
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        if (strstr(p.deviceName, want)) pd = pds[i];
    }
    if (!pd) { fprintf(stderr, "no device %s\n", want); return 2; }
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    float prio = 1;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkPhysicalDeviceTransformFeedbackFeaturesEXT fx = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, 0, 1, 0 };
    const char *t = getenv("XRUN_TOPO") ? getenv("XRUN_TOPO") : "points";
    if (!strcmp(t, "lines")) topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    else if (!strcmp(t, "triangles")) topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    else if (!strcmp(t, "lines-adjacency")) topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    else if (!strcmp(t, "triangles-adjacency")) topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    else if (strcmp(t, "points")) { fprintf(stderr, "XRUN_TOPO %s\n", t); return 1; }
    if (getenv("XRUN_MEM")) sscanf(getenv("XRUN_MEM"), "%d:%d", &mem_rec, &mem_slots);
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &fx };
    f12.bufferDeviceAddress = 1;
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12 };
    f13.dynamicRendering = 1;
    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f13 };
    f11.shaderDrawParameters = 1;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11 };
    f2.features.geometryShader = 1; f2.features.shaderInt64 = 1; f2.features.vertexPipelineStoresAndAtomics = 1;
    const char *ext[] = { "VK_EXT_transform_feedback" };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qi, 0, 0, 1, ext };
    CK(vkCreateDevice(pd, &dci, 0, &dev));
    vkGetDeviceQueue(dev, 0, 0, &q);
    bind_xfb = (PFN_vkCmdBindTransformFeedbackBuffersEXT)vkGetDeviceProcAddr(dev, "vkCmdBindTransformFeedbackBuffersEXT");
    begin_xfb = (PFN_vkCmdBeginTransformFeedbackEXT)vkGetDeviceProcAddr(dev, "vkCmdBeginTransformFeedbackEXT");
    end_xfb = (PFN_vkCmdEndTransformFeedbackEXT)vkGetDeviceProcAddr(dev, "vkCmdEndTransformFeedbackEXT");
    draw_auto = (PFN_vkCmdDrawIndirectByteCountEXT)vkGetDeviceProcAddr(dev, "vkCmdDrawIndirectByteCountEXT");
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    CK(vkCreateCommandPool(dev, &pci, 0, &cp));
    VkPipelineCacheCreateInfo pcc = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    CK(vkCreatePipelineCache(dev, &pcc, 0, &pcache));

    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT,
        { W, H, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    VkImage img; CK(vkCreateImage(dev, &ii, 0, &img));
    VkMemoryRequirements ir; vkGetImageMemoryRequirements(dev, img, &ir);
    VkMemoryAllocateInfo iai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, ir.size, memtype(ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory im; CK(vkAllocateMemory(dev, &iai, 0, &im)); CK(vkBindImageMemory(dev, img, im, 0));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, img, VK_IMAGE_VIEW_TYPE_2D, ii.format,
        { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view; CK(vkCreateImageView(dev, &vci, 0, &view));

    Buf xb = mkbuf((size_t)stride * count, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    Buf cnt = mkbuf(4, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    Buf rb = mkbuf((size_t)W * H * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkPipelineLayout l = layout();
    VkPipeline cap = pipeline(l, shader(argv[1]), strcmp(argv[2], "-") ? shader(argv[2]) : 0, 0, 0);
    VkPipeline drw = pipeline(l, shader(argv[5]), 0, shader(argv[6]), stride);

    VkCommandBuffer cb = begin();
    VkImageMemoryBarrier to_att = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, ~0u, ~0u, img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, 0, 0, 0, 1, &to_att);
    VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        0, 0, 0, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo rinfo = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, { { 0, 0 }, { W, H } }, 1, 0, 1, &att };
    uint8_t push[128] = { 0 };
    float one = 1.0f; memcpy(push, &one, 4);
    VkRenderingInfo cinfo = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, { { 0, 0 }, { W, H } }, 1, 0, 0, 0 };
    vkCmdBeginRendering(cb, &cinfo);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cap);
    vkCmdPushConstants(cb, l, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof push, push);
    VkDeviceSize zero = 0, size = (VkDeviceSize)stride * count;
    Buf sc = { 0 };
    if (mem_rec) {
        sc = mkbuf((size_t)mem_rec * mem_slots, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        VkBufferDeviceAddressInfo bai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, sc.b };
        uint64_t a = vkGetBufferDeviceAddress(dev, &bai);
        memcpy(push + 24, &a, 8);
        /* A translation for a host that mirrors guest memory marks the pages it stores to in a bitmap (1 << 20
           bits) whose address is at byte 16. */
        Buf pages = mkbuf(1u << 17, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        VkBufferDeviceAddressInfo pai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, pages.b };
        uint64_t pa = vkGetBufferDeviceAddress(dev, &pai);
        memcpy(push + 16, &pa, 8);
        vkCmdPushConstants(cb, l, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof push, push);
        vkCmdDraw(cb, count, 1, 0, 0);
        vkCmdEndRendering(cb);
        submit(cb);
        wrfile(argv[7], "scratch.bin", sc.p, (size_t)mem_rec * mem_slots);
        return 0;
    }
    bind_xfb(cb, 0, 1, &xb.b, &zero, &size);
    begin_xfb(cb, 0, 0, 0, 0);
    vkCmdDraw(cb, count, 1, 0, 0);
    end_xfb(cb, 0, 1, &cnt.b, &zero);
    vkCmdEndRendering(cb);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0,
        VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT,
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, 0, 0, 0);
    vkCmdBeginRendering(cb, &rinfo);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, drw);
    vkCmdBindVertexBuffers(cb, 0, 1, &xb.b, &zero);
    draw_auto(cb, 1, 0, cnt.b, 0, 0, stride);
    vkCmdEndRendering(cb);
    VkImageMemoryBarrier to_src = to_att;
    to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &to_src);
    VkBufferImageCopy bic = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &bic);
    submit(cb);

    wrfile(argv[7], "capture.bin", xb.p, (size_t)stride * count);
    wrfile(argv[7], "image.raw", rb.p, (size_t)W * H * 16);
    char text[32]; int tn = snprintf(text, sizeof text, "%u\n", *(uint32_t *)cnt.p);
    wrfile(argv[7], "count.txt", text, (size_t)tn);
    if (getenv("XRUN_CACHE")) {
        size_t cs = 0; CK(vkGetPipelineCacheData(dev, pcache, &cs, 0));
        void *cd = malloc(cs); CK(vkGetPipelineCacheData(dev, pcache, &cs, cd));
        FILE *f = fopen(getenv("XRUN_CACHE"), "wb"); if (!f || fwrite(cd, 1, cs, f) != cs) { perror("cache"); return 1; } fclose(f);
    }
    return 0;
}
