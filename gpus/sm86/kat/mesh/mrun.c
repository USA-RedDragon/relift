// mrun <script>: runs a small Vulkan script (mesh draws, compute passes, indirect draws) on one device
// and reads back the color target and buffers, for kat.py. MRUN_DEV picks the device by a part of its
// name (default NVIDIA), MRUN_NV uses VK_NV_mesh_shader, MRUN_TIME prints each command's GPU time.
// Commands between "group" and "endgroup" go into one command buffer, timed as one.
// Blending, when on, is dst = src + 0.5 dst, which depends on the primitive order. "subgroup N" makes
// the compute passes after it run in full subgroups of N invocations.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "line %d: %s = %d\n", __LINE__, #x, r_); exit(1); } } while (0)

static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q; static VkCommandPool cp;
static VkPhysicalDeviceMemoryProperties mp; static VkPipelineCache pcache;
static int W = 64, H = 64, blend = 0, has_mesh = 0, have_target = 0, sgsize = 0;
static PFN_vkCmdDrawMeshTasksEXT draw_mesh; static PFN_vkCmdDrawMeshTasksNV draw_mesh_nv; static int nv;

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags f) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
    fprintf(stderr, "no memory type\n"); exit(1);
}

typedef struct { VkBuffer b; VkDeviceMemory m; void *p; size_t n; VkBuffer db; VkDeviceMemory dm; } Buf;
static Buf bufs[64];
static void begin(void); static void submit(void); static VkCommandBuffer cb;

/* Every buffer lives in device-local memory (db) with a host-visible copy (b) that "sync" moves. */
static void sync(Buf *b, int to_device) {
    begin();
    VkBufferCopy r = { 0, 0, b->n };
    if (to_device) vkCmdCopyBuffer(cb, b->b, b->db, 1, &r); else vkCmdCopyBuffer(cb, b->db, b->b, 1, &r);
    submit();
}

static void mkbuf_host(int id, size_t n) {
    Buf *b = &bufs[id];
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, n,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    CK(vkCreateBuffer(dev, &bi, 0, &b->b));
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(dev, b->b, &r);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, r.size,
        memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CK(vkAllocateMemory(dev, &ai, 0, &b->m));
    CK(vkBindBufferMemory(dev, b->b, b->m, 0));
    CK(vkMapMemory(dev, b->m, 0, n, 0, &b->p));
    memset(b->p, 0, n); b->n = n;
}

static void mkbuf(int id, size_t n) {
    mkbuf_host(id, n);
    Buf *b = &bufs[id];
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, n,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    CK(vkCreateBuffer(dev, &bi, 0, &b->db));
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(dev, b->db, &r);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, r.size, memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CK(vkAllocateMemory(dev, &ai, 0, &b->dm));
    CK(vkBindBufferMemory(dev, b->db, b->dm, 0));
}

static void *rdfile(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(*n + 4); if (fread(b, 1, *n, f) != *n) exit(1); fclose(f); return b;
}

static VkShaderModule shader(const char *p) {
    size_t n; uint32_t *c = rdfile(p, &n);
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, n, c };
    VkShaderModule m; CK(vkCreateShaderModule(dev, &mi, 0, &m)); free(c); return m;
}

typedef struct { int set, binding, buf, ssbo; } Bind;
static Bind binds[64]; static int nbinds;
static VkDescriptorSetLayout dsl[4]; static VkDescriptorSet ds[4]; static VkPipelineLayout lay;
static int layout_done;

static void build_layout(void) {
    if (layout_done) return;
    layout_done = 1;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64 } };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 4, 2, ps };
    VkDescriptorPool pool; CK(vkCreateDescriptorPool(dev, &pi, 0, &pool));
    for (int s = 0; s < 4; s++) {
        VkDescriptorSetLayoutBinding lb[64]; int n = 0;
        for (int i = 0; i < nbinds; i++) if (binds[i].set == s)
            lb[n++] = (VkDescriptorSetLayoutBinding){ binds[i].binding,
                binds[i].ssbo ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, 0 };
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, n, lb };
        CK(vkCreateDescriptorSetLayout(dev, &li, 0, &dsl[s]));
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, pool, 1, &dsl[s] };
        CK(vkAllocateDescriptorSets(dev, &ai, &ds[s]));
        for (int i = 0; i < nbinds; i++) if (binds[i].set == s) {
            VkDescriptorBufferInfo bi = { bufs[binds[i].buf].db, 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds[s], binds[i].binding, 0, 1,
                binds[i].ssbo ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &bi, 0 };
            vkUpdateDescriptorSets(dev, 1, &w, 0, 0);
        }
    }
    VkPushConstantRange pcr = { VK_SHADER_STAGE_ALL, 0, 128 };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 4, dsl, 1, &pcr };
    CK(vkCreatePipelineLayout(dev, &li, 0, &lay));
}

static VkImage img; static VkImageView view; static VkDeviceMemory imem; static Buf rb;
static uint32_t pushc[32];
static VkQueryPool qp; static float ts_period; static int timing; static const char *cur = "";

static int grouped;

static void begin(void) {
    if (grouped) return;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    CK(vkAllocateCommandBuffers(dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CK(vkBeginCommandBuffer(cb, &bi));
    if (timing) {
        vkCmdResetQueryPool(cb, qp, 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
    }
}

static void submit(void) {
    if (grouped) return;
    if (timing) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };
    CK(vkQueueSubmit(q, 1, &si, 0)); CK(vkQueueWaitIdle(q));
    vkFreeCommandBuffers(dev, cp, 1, &cb);
    if (timing) {
        uint64_t t[2];
        CK(vkGetQueryPoolResults(dev, qp, 0, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        printf("time %s %.1f us\n", cur, (t[1] - t[0]) * ts_period / 1000.0);
    }
}

static void barrier(void) {
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, 0, 0, 0);
}

static void img_barrier(VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}

static void make_target(void) {
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT,
        { W, H, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    CK(vkCreateImage(dev, &ii, 0, &img));
    VkMemoryRequirements r; vkGetImageMemoryRequirements(dev, img, &r);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, r.size, memtype(r.memoryTypeBits, 0) };
    CK(vkAllocateMemory(dev, &ai, 0, &imem)); CK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, img, VK_IMAGE_VIEW_TYPE_2D,
        VK_FORMAT_R32G32B32A32_SFLOAT, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CK(vkCreateImageView(dev, &vi, 0, &view));
    begin();
    img_barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkClearColorValue cc = { { 0, 0, 0, 0 } };
    VkImageSubresourceRange sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &sr);
    submit();
}

static VkPipeline graphics(VkShaderStageFlagBits *stg, const char **files, int n, VkPrimitiveTopology topo) {
    build_layout();
    VkPipelineShaderStageCreateInfo st[3];
    for (int i = 0; i < n; i++)
        st[i] = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, stg[i], shader(files[i]), "main" };
    int mesh = stg[n - 1] == VK_SHADER_STAGE_FRAGMENT_BIT && (stg[0] == VK_SHADER_STAGE_MESH_BIT_EXT || stg[0] == VK_SHADER_STAGE_TASK_BIT_EXT);
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, topo };
    VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vs = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vp, 1, &sc };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.lineWidth = 1; rs.cullMode = VK_CULL_MODE_NONE;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { 0 }; cba.colorWriteMask = 0xf;
    if (blend) {
        cba.blendEnable = 1;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &cba,
        { 0.5f, 0.5f, 0.5f, 0.5f } };
    VkFormat cf = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkPipelineRenderingCreateInfo pr = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, 1, &cf };
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pr, 0,
        n, st, mesh ? 0 : &vi, mesh ? 0 : &ia, 0, &vs, &rs, &ms, 0, &cbs, 0, lay };
    VkPipeline pl; CK(vkCreateGraphicsPipelines(dev, pcache, 1, &gi, 0, &pl));
    return pl;
}

static void render_begin(void) {
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, view, VK_IMAGE_LAYOUT_GENERAL,
        0, 0, 0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, { { 0, 0 }, { W, H } }, 1, 0, 1, &ca };
    vkCmdBeginRendering(cb, &ri);
}

static void bind_all(VkPipelineBindPoint bp, VkPipeline pl) {
    vkCmdBindPipeline(cb, bp, pl);
    vkCmdBindDescriptorSets(cb, bp, lay, 0, 4, ds, 0, 0);
    vkCmdPushConstants(cb, lay, VK_SHADER_STAGE_ALL, 0, 128, pushc);
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    const char *want = getenv("MRUN_DEV") ? getenv("MRUN_DEV") : "NVIDIA";
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "mrun", 1, 0, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai };
    VkInstance inst; CK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 8; VkPhysicalDevice pds[8]; CK(vkEnumeratePhysicalDevices(inst, &n, pds));
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        if (strstr(p.deviceName, want)) pd = pds[i];
    }
    if (!pd) { fprintf(stderr, "no device %s\n", want); return 2; }
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, 0, &ne, 0);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep); vkEnumerateDeviceExtensionProperties(pd, 0, &ne, ep);
    int has_mr = 0, has_ucf = 0;
    for (uint32_t i = 0; i < ne; i++) {
        if (!strcmp(ep[i].extensionName, "VK_EXT_mesh_shader")) has_mesh = 1;
        if (!strcmp(ep[i].extensionName, "VK_KHR_shader_maximal_reconvergence")) has_mr = 1;
        if (!strcmp(ep[i].extensionName, "VK_KHR_shader_subgroup_uniform_control_flow")) has_ucf = 1;
    }
    has_mr &= has_ucf;
    if (getenv("MRUN_NO_MESH")) has_mesh = 0;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkPhysicalDeviceMeshShaderFeaturesEXT fm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, 0, 1, 1 };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, has_mesh ? &fm : 0 };
    f12.bufferDeviceAddress = 1; f12.storageBuffer8BitAccess = 1; f12.shaderInt8 = 1;
    f12.uniformAndStorageBuffer8BitAccess = 1; f12.vulkanMemoryModel = 1; f12.vulkanMemoryModelDeviceScope = 1;
    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12 };
    f11.shaderDrawParameters = 1;
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f11 };
    f13.dynamicRendering = 1; f13.shaderDemoteToHelperInvocation = 1; f13.subgroupSizeControl = 1; f13.computeFullSubgroups = 1;
    f13.maintenance4 = 1;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 };
    f2.features.shaderInt64 = 1; f2.features.fragmentStoresAndAtomics = 1; f2.features.vertexPipelineStoresAndAtomics = 1;
    f2.features.geometryShader = getenv("MRUN_NO_GS") ? 0 : 1; f2.features.shaderClipDistance = 1;
    /* Ask only for what the device has (Adreno lacks shaderInt64); a shader that needs more fails to build. */
    VkPhysicalDeviceVulkan12Features h12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features h11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &h12 };
    VkPhysicalDeviceVulkan13Features h13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &h11 };
    VkPhysicalDeviceFeatures2 h2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &h13 };
    vkGetPhysicalDeviceFeatures2(pd, &h2);
    #define HAVE(f, h, m) f.m &= h.m
    HAVE(f12, h12, bufferDeviceAddress); HAVE(f12, h12, storageBuffer8BitAccess); HAVE(f12, h12, shaderInt8);
    HAVE(f12, h12, uniformAndStorageBuffer8BitAccess); HAVE(f12, h12, vulkanMemoryModel);
    HAVE(f12, h12, vulkanMemoryModelDeviceScope); HAVE(f11, h11, shaderDrawParameters);
    HAVE(f13, h13, shaderDemoteToHelperInvocation); HAVE(f13, h13, subgroupSizeControl);
    HAVE(f13, h13, computeFullSubgroups); HAVE(f13, h13, maintenance4);
    HAVE(f2.features, h2.features, shaderInt64); HAVE(f2.features, h2.features, fragmentStoresAndAtomics);
    HAVE(f2.features, h2.features, vertexPipelineStoresAndAtomics); HAVE(f2.features, h2.features, geometryShader);
    HAVE(f2.features, h2.features, shaderClipDistance);
    nv = getenv("MRUN_NV") != 0;
    VkPhysicalDeviceMeshShaderFeaturesNV fnv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV, 0, 1, 1 };
    if (nv) { f12.pNext = &fnv; }
    const char *ext[3]; uint32_t next = 0;
    if (nv) ext[next++] = "VK_NV_mesh_shader";
    else if (has_mesh) ext[next++] = "VK_EXT_mesh_shader";
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR fmr = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR, &f13, 1 };
    if (has_mr) {
        ext[next++] = "VK_KHR_shader_subgroup_uniform_control_flow";
        ext[next++] = "VK_KHR_shader_maximal_reconvergence";
        f2.pNext = &fmr;
    }
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qi, 0, 0, next, ext };
    CK(vkCreateDevice(pd, &dci, 0, &dev));
    vkGetDeviceQueue(dev, 0, 0, &q);
    draw_mesh = (PFN_vkCmdDrawMeshTasksEXT)vkGetDeviceProcAddr(dev, "vkCmdDrawMeshTasksEXT");
    draw_mesh_nv = (PFN_vkCmdDrawMeshTasksNV)vkGetDeviceProcAddr(dev, "vkCmdDrawMeshTasksNV");
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, 0 };
    CK(vkCreateCommandPool(dev, &pci, 0, &cp));
    VkPipelineCacheCreateInfo pcc = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    CK(vkCreatePipelineCache(dev, &pcc, 0, &pcache));
    timing = getenv("MRUN_TIME") != 0;
    if (timing) {
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); ts_period = pp.limits.timestampPeriod;
        VkQueryPoolCreateInfo qi2 = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, 0, 0, VK_QUERY_TYPE_TIMESTAMP, 2 };
        CK(vkCreateQueryPool(dev, &qi2, 0, &qp));
    }
    FILE *sf = fopen(argv[1], "r"); if (!sf) { perror(argv[1]); return 1; }
    char line[1024];
    while (fgets(line, sizeof line, sf)) {
        char a[8][256] = { { 0 } };
        int k = sscanf(line, "%255s %255s %255s %255s %255s %255s %255s %255s", a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        if (k <= 0 || a[0][0] == '#') continue;
        static char curbuf[256]; snprintf(curbuf, sizeof curbuf, "%s %s", a[0], a[1]); cur = curbuf;
        if (!strcmp(a[0], "size")) { W = atoi(a[1]); H = atoi(a[2]); }
        else if (!strcmp(a[0], "buf")) { mkbuf(atoi(a[1]), strtoul(a[2], 0, 0)); sync(&bufs[atoi(a[1])], 1); }
        else if (!strcmp(a[0], "bufi")) {
            size_t sz; void *d = rdfile(a[2], &sz);
            size_t want_n = k > 3 ? strtoul(a[3], 0, 0) : sz;
            mkbuf(atoi(a[1]), want_n > sz ? want_n : sz); memcpy(bufs[atoi(a[1])].p, d, sz); free(d);
            sync(&bufs[atoi(a[1])], 1);
        }
        else if (!strcmp(a[0], "bind")) binds[nbinds++] = (Bind){ atoi(a[1]), atoi(a[2]), atoi(a[3]), !strcmp(a[4], "ssbo") };
        else if (!strcmp(a[0], "push")) { for (int i = 1; i < k; i++) pushc[i - 1] = strtoul(a[i], 0, 0); }
        else if (!strcmp(a[0], "pushf")) { size_t sz; void *d = rdfile(a[1], &sz); memcpy(pushc, d, sz > 128 ? 128 : sz); free(d); }
        else if (!strcmp(a[0], "blend")) blend = !strcmp(a[1], "on");
        else if (!strcmp(a[0], "subgroup")) sgsize = atoi(a[1]);
        else if (!strcmp(a[0], "group")) {
            if (!have_target) { make_target(); have_target = 1; }
            begin(); grouped = 1;
        }
        else if (!strcmp(a[0], "endgroup")) { grouped = 0; submit(); }
        else if (!strcmp(a[0], "comp") || !strcmp(a[0], "compi")) {
            build_layout();
            VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, 0,
                sgsize ? sgsize : 32 };
            int full = sgsize || getenv("MRUN_SG32") != 0;
            VkPipelineShaderStageCreateInfo st = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, full ? &rss : 0,
                full ? VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0, VK_SHADER_STAGE_COMPUTE_BIT, shader(a[1]), "main" };
            VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0, st, lay };
            VkPipeline pl; CK(vkCreateComputePipelines(dev, pcache, 1, &ci, 0, &pl));
            begin(); bind_all(VK_PIPELINE_BIND_POINT_COMPUTE, pl);
            if (!strcmp(a[0], "comp")) vkCmdDispatch(cb, atoi(a[2]), atoi(a[3]), atoi(a[4]));
            else vkCmdDispatchIndirect(cb, bufs[atoi(a[2])].db, strtoul(a[3], 0, 0));
            barrier(); submit();
        }
        else if (!strcmp(a[0], "mesh")) {
            if (!have_target) { make_target(); have_target = 1; }
            VkShaderStageFlagBits stg[3]; const char *fs[3]; int ns = 0;
            if (strcmp(a[1], "-")) { stg[ns] = VK_SHADER_STAGE_TASK_BIT_EXT; fs[ns++] = a[1]; }
            stg[ns] = VK_SHADER_STAGE_MESH_BIT_EXT; fs[ns++] = a[2];
            stg[ns] = VK_SHADER_STAGE_FRAGMENT_BIT; fs[ns++] = a[3];
            VkPipeline pl = graphics(stg, fs, ns, 0);
            begin(); render_begin(); bind_all(VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
            if (nv) draw_mesh_nv(cb, atoi(a[4]), atoi(a[5])); else draw_mesh(cb, atoi(a[4]), atoi(a[5]), atoi(a[6]));
            vkCmdEndRendering(cb); barrier(); submit();
        }
        else if (!strcmp(a[0], "draw") || !strcmp(a[0], "drawi") || !strcmp(a[0], "drawx")) {
            if (!have_target) { make_target(); have_target = 1; }
            VkShaderStageFlagBits stg[2] = { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT };
            const char *fs[2] = { a[1], a[2] };
            VkPrimitiveTopology t = !strcmp(a[3], "tri") ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                : !strcmp(a[3], "line") ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            VkPipeline pl = graphics(stg, fs, 2, t);
            begin(); render_begin(); bind_all(VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
            if (!strcmp(a[0], "draw")) vkCmdDraw(cb, atoi(a[4]), 1, 0, 0);
            else if (!strcmp(a[0], "drawi")) vkCmdDrawIndirect(cb, bufs[atoi(a[4])].db, strtoul(a[5], 0, 0), 1, 16);
            else {
                vkCmdBindIndexBuffer(cb, bufs[atoi(a[6])].db, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexedIndirect(cb, bufs[atoi(a[4])].db, strtoul(a[5], 0, 0), 1, 20);
            }
            vkCmdEndRendering(cb); barrier(); submit();
        }
        else if (!strcmp(a[0], "out")) {
            if (!have_target) { make_target(); have_target = 1; }
            if (!rb.b) { mkbuf_host(63, (size_t)W * H * 16); rb = bufs[63]; }
            begin();
            VkBufferImageCopy bc = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
            vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, rb.b, 1, &bc);
            submit();
            FILE *f = fopen(a[1], "wb"); fwrite(rb.p, 1, (size_t)W * H * 16, f); fclose(f);
        }
        else if (!strcmp(a[0], "dump")) {
            Buf *b = &bufs[atoi(a[1])];
            sync(b, 0);
            size_t len = k > 3 ? strtoul(a[3], 0, 0) : b->n;
            FILE *f = fopen(a[2], "wb"); fwrite(b->p, 1, len, f); fclose(f);
        }
        else if (!strcmp(a[0], "cache")) {
            size_t cs = 0; CK(vkGetPipelineCacheData(dev, pcache, &cs, 0));
            void *cd = malloc(cs); CK(vkGetPipelineCacheData(dev, pcache, &cs, cd));
            FILE *f = fopen(a[1], "wb"); fwrite(cd, 1, cs, f); fclose(f); free(cd);
        }
        else { fprintf(stderr, "unknown: %s\n", a[0]); return 1; }
    }
    return 0;
}
