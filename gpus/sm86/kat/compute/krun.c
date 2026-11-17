/* krun spec [device-substring]: runs one compute dispatch of a SPIR-V module laid out as the sm86
 * translator lays out its resources, and writes the output buffers to files.
 * Spec lines:
 *   spv <path>                     the module
 *   local X Y Z                    workgroup size (specialization constants 0..2)
 *   groups X Y Z                   dispatch size
 *   shared <bytes>                 shared memory size (specialization constant 3)
 *   localmem <bytes>               local memory size (specialization constant 4)
 *   spec <id> <value>              any other specialization constant
 *   buf <path>                     storage buffer holding the file's bytes
 *   out <path> <bytes>             zeroed storage buffer written to path afterwards
 *   outf <path> <bytes> <init>     the same, starting with the bytes of file init
 *   bank <n> <path> | size <bytes> constant bank n (set 0 binding n-3, or set 1 binding n)
 *   put <n> <off> addr <k>         64-bit address of buffer k at byte off of bank n
 *   put <n> <off> u32 <v>          32-bit value at byte off of bank n
 *   push <off> addr <k> | u32 <v>  the same in the push constants
 *   segments <shift>               buffer k is element k of set 3 binding 0 and "addr k" is k << shift
 *   image <type> <w> <h> <d> <levels> <fmt>
 *                                  type 1d 2d 3d cube 1darr 2darr cubearr; fmt f32 u32 u8 r32u;
 *                                  texel (x, y, z, level + 100)
 *   view <binding> <index>         a view of the last image at set 2 binding, array element index
 *   sampler <index> <filter> <norm> <cmp>
 *                                  filter 0 nearest 1 linear; cmp 0 off, else VkCompareOp + 1
 *   dump <path>                    the last image's first level, written afterwards
 * The device is the first whose name holds the given text. KRUN_CACHE=<path> writes the pipeline
 * cache there. Exits with 77 when the module needs VK_KHR_shader_fma and the device lacks it.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)
#define MAXB 16
#define NBIND 104
#define COUNT 4

static VkDevice dev;
static VkPhysicalDevice pd;
static VkQueue q;
static VkCommandPool cp;

static void *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(*n ? *n : 1);
    if (*n && fread(b, 1, *n, f) != *n) exit(1);
    fclose(f);
    return b;
}

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    exit(2);
}

typedef struct { VkBuffer b; VkDeviceMemory m; void *map; size_t n; VkDeviceAddress addr; const char *out; } Buf;

static Buf mkbuf(size_t n, VkBufferUsageFlags u) {
    Buf r = {0};
    r.n = n;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, n, u | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE };
    CK(vkCreateBuffer(dev, &bi, 0, &r.b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, r.b, &mr);
    VkMemoryAllocateFlagsInfo fi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi, mr.size, memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CK(vkAllocateMemory(dev, &ai, 0, &r.m));
    CK(vkBindBufferMemory(dev, r.b, r.m, 0));
    CK(vkMapMemory(dev, r.m, 0, n, 0, &r.map));
    memset(r.map, 0, n);
    VkBufferDeviceAddressInfo da = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, r.b };
    r.addr = vkGetBufferDeviceAddress(dev, &da);
    return r;
}

static VkCommandBuffer begin(void) {
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cbi, &cb));
    VkCommandBufferBeginInfo bgi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CK(vkBeginCommandBuffer(cb, &bgi));
    return cb;
}

static void submit(VkCommandBuffer cb) {
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo sub = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };
    CK(vkQueueSubmit(q, 1, &sub, 0));
    CK(vkQueueWaitIdle(q));
}

typedef struct { VkImage im; VkFormat fmt; int type, w, h, d, levels, layers; } Img;

static const char *TYPES[] = {"1d", "2d", "3d", "cube", "1darr", "2darr", "cubearr"};

static Img mkimage(int type, int w, int h, int d, int levels, const char *fs) {
    Img r = {0};
    r.type = type; r.w = w; r.h = (type == 0 || type == 4) ? 1 : h; r.levels = levels;
    int is_u8 = !strcmp(fs, "u8"), is_u32 = !strcmp(fs, "u32"), is_r32 = !strcmp(fs, "r32u");
    r.fmt = is_r32 ? VK_FORMAT_R32_UINT : is_u8 ? VK_FORMAT_R8G8B8A8_UNORM : is_u32 ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT;
    r.d = type == 2 ? d : 1;
    r.layers = type == 3 ? 6 : (type == 4 || type == 5 || type == 6) ? d : 1;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | ((type == 3 || type == 6) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0);
    ii.imageType = (type == 0 || type == 4) ? VK_IMAGE_TYPE_1D : type == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format = r.fmt;
    ii.extent = (VkExtent3D){ w, r.h, r.d };
    ii.mipLevels = levels; ii.arrayLayers = r.layers;
    ii.samples = 1; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CK(vkCreateImage(dev, &ii, 0, &r.im));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, r.im, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory m; CK(vkAllocateMemory(dev, &ai, 0, &m));
    CK(vkBindImageMemory(dev, r.im, m, 0));
    int bpp = (is_u8 || is_r32) ? 4 : 16;
    VkCommandBuffer cb = begin();
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0, r.im, { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, r.layers } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b);
    Buf stage[32];
    for (int l = 0; l < levels; l++) {
        int lw = w >> l; if (lw < 1) lw = 1;
        int lh = r.h >> l; if (lh < 1) lh = 1;
        int ld = r.d >> l; if (ld < 1) ld = 1;
        int nz = type == 2 ? ld : r.layers;
        stage[l] = mkbuf((size_t)lw * lh * nz * bpp, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        unsigned char *buf = stage[l].map;
        for (int z = 0; z < nz; z++) for (int y = 0; y < lh; y++) for (int x = 0; x < lw; x++) {
            size_t o = (size_t)z * lh * lw + (size_t)y * lw + x;
            if (is_r32) { ((unsigned *)buf)[o] = x + 16 * y + 256 * z; }
            else if (is_u8) { unsigned char *p = buf + o * 4; p[0] = x * 16; p[1] = y * 16; p[2] = z * 16; p[3] = l * 16 + 8; }
            else if (is_u32) { unsigned *p = (unsigned *)(buf + o * 16); p[0] = x; p[1] = y; p[2] = z; p[3] = l + 100; }
            else { float *p = (float *)(buf + o * 16); p[0] = x; p[1] = y; p[2] = z; p[3] = l + 100; }
        }
        VkBufferImageCopy c = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, type == 2 ? 1 : r.layers }, {0, 0, 0}, { lw, lh, type == 2 ? ld : 1 } };
        vkCmdCopyBufferToImage(cb, stage[l].b, r.im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
    submit(cb);
    return r;
}

static VkFormat kind_format(VkFormat f, int kind) {
    if (f == VK_FORMAT_R32_UINT) return kind == 1 ? VK_FORMAT_R32_SINT : kind == 0 ? VK_FORMAT_R32_SFLOAT : f;
    if (f == VK_FORMAT_R8G8B8A8_UNORM) return kind == 1 ? VK_FORMAT_R8G8B8A8_SINT : kind == 2 ? VK_FORMAT_R8G8B8A8_UINT : f;
    if (kind == 0) return VK_FORMAT_R32G32B32A32_SFLOAT;
    return kind == 1 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
}

static VkImageView mkview(Img *im, int binding) {
    int kind, dim, storage = binding >= 32;
    if (!storage) { kind = (binding - 1) / 10; dim = (binding - 1) % 10; }
    else { kind = (binding - 32) / 8; dim = (binding - 32) % 8; }
    VkImageViewType vt;
    int layers = 1;
    if (!storage) {
        static const VkImageViewType T[] = { VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE,
            VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY };
        vt = T[dim];
        layers = (dim == 3) ? 6 : (dim == 4 || dim == 5 || dim == 6) ? im->layers : 1;
    } else {
        static const VkImageViewType T[] = { VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D,
            VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_3D };
        vt = T[dim];
        layers = (dim == 2 || dim == 4) ? im->layers : 1;
        if (kind > 2) kind = 2;
    }
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, im->im, vt, kind_format(im->fmt, kind) };
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, storage ? 1 : im->levels, 0, layers };
    VkImageView v; CK(vkCreateImageView(dev, &vi, 0, &v));
    return v;
}

static unsigned char push[128];

static int has_ext(const char *name) {
    uint32_t en = 0; vkEnumerateDeviceExtensionProperties(pd, 0, &en, 0);
    VkExtensionProperties *ex = malloc(en * sizeof *ex); vkEnumerateDeviceExtensionProperties(pd, 0, &en, ex);
    int r = 0;
    for (uint32_t k = 0; k < en; k++) r |= !strcmp(ex[k].extensionName, name);
    free(ex);
    return r;
}

static int contains(const void *b, size_t n, const char *s) {
    size_t m = strlen(s);
    for (size_t i = 0; i + m <= n; i++) if (!memcmp((const char *)b + i, s, m)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "krun spec [device-substring]\n"); return 1; }
    const char *want = argc > 2 ? argv[2] : "";
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "krun", 1, 0, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst; CK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 8; VkPhysicalDevice pds[8]; CK(vkEnumeratePhysicalDevices(inst, &n, pds));
    pd = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        if (strstr(p.deviceName, want)) { pd = pds[i]; fprintf(stderr, "device %s\n", p.deviceName); break; }
    }
    if (!pd) { fprintf(stderr, "no device\n"); return 1; }

    size_t sn; char *spec = slurp(argv[1], &sn);
    spec = realloc(spec, sn + 1); spec[sn] = 0;
    const char *spv = 0;
    for (char *l = spec; l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : 0) {
        char a[512];
        if (sscanf(l, "spv %511s", a) == 1) spv = strdup(a);
    }
    if (!spv) { fprintf(stderr, "no spv line\n"); return 1; }
    size_t cn; void *code = slurp(spv, &cn);
    int fma = has_ext("VK_KHR_shader_fma");
    if (contains(code, cn, "SPV_KHR_fma") && !fma) { fprintf(stderr, "needs VK_KHR_shader_fma\n"); return 77; }

    VkPhysicalDeviceVulkan12Features s12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 s2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &s12 };
    vkGetPhysicalDeviceFeatures2(pd, &s2);
    float prio = 1;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.bufferDeviceAddress = 1;
    f12.descriptorIndexing = 1;
    f12.runtimeDescriptorArray = 1;
    f12.shaderSampledImageArrayNonUniformIndexing = 1;
    f12.shaderStorageImageArrayNonUniformIndexing = s12.shaderStorageImageArrayNonUniformIndexing;
    f12.shaderStorageBufferArrayNonUniformIndexing = 1;
    f12.descriptorBindingPartiallyBound = 1;
    f12.shaderBufferInt64Atomics = s12.shaderBufferInt64Atomics;
    f12.shaderSharedInt64Atomics = s12.shaderSharedInt64Atomics;
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12 };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 };
    f2.features.shaderInt64 = s2.features.shaderInt64;
    f2.features.shaderStorageImageWriteWithoutFormat = s2.features.shaderStorageImageWriteWithoutFormat;
    f2.features.shaderStorageImageReadWithoutFormat = s2.features.shaderStorageImageReadWithoutFormat;
    f2.features.shaderImageGatherExtended = s2.features.shaderImageGatherExtended;
    f2.features.imageCubeArray = s2.features.imageCubeArray;
    f2.features.shaderResourceMinLod = s2.features.shaderResourceMinLod;
    const char *exts[3]; uint32_t nexts = 0;
    VkPhysicalDeviceShaderClockFeaturesKHR fck = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR };
    fck.shaderSubgroupClock = 1; fck.shaderDeviceClock = 1;
    VkPhysicalDeviceShaderFmaFeaturesKHR ffma = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FMA_FEATURES_KHR, 0, 0, 1, 0 };
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR fmr = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR, 0, 1 };
    if (has_ext("VK_KHR_shader_clock")) { exts[nexts++] = "VK_KHR_shader_clock"; fck.pNext = f12.pNext; f12.pNext = &fck; }
    if (fma) { exts[nexts++] = "VK_KHR_shader_fma"; ffma.pNext = f12.pNext; f12.pNext = &ffma; }
    if (has_ext("VK_KHR_shader_maximal_reconvergence")) { exts[nexts++] = "VK_KHR_shader_maximal_reconvergence"; fmr.pNext = f12.pNext; f12.pNext = &fmr; }
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qi, 0, 0, nexts, exts };
    CK(vkCreateDevice(pd, &dci, 0, &dev));
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, 0 };
    CK(vkCreateCommandPool(dev, &cpci, 0, &cp));

    Buf bufs[MAXB]; int nb = 0;
    Buf banks[18]; int have[18] = {0};
    uint32_t sdata[64] = {1, 1, 1, 4096, 1024}; VkSpecializationMapEntry me[64]; uint32_t nspec = 5;
    for (int k = 0; k < 5; k++) me[k] = (VkSpecializationMapEntry){ k, 4 * k, 4 };
    uint32_t groups[3] = {1, 1, 1};
    int segshift = -1;
    Img imgs[8]; int ni = 0;
    VkWriteDescriptorSet writes[64]; VkDescriptorImageInfo infos[64]; int nw = 0;
    char *save = 0; const char *dump = 0;
    for (char *l = strtok_r(spec, "\n", &save); l; l = strtok_r(0, "\n", &save)) {
        char a[512], b[512], c[64]; unsigned long x, y, z, u, t;
        if (sscanf(l, "dump %511s", a) == 1) dump = strdup(a);
        else if (sscanf(l, "local %lu %lu %lu", &x, &y, &z) == 3) { sdata[0] = x; sdata[1] = y; sdata[2] = z; }
        else if (sscanf(l, "groups %lu %lu %lu", &x, &y, &z) == 3) { groups[0] = x; groups[1] = y; groups[2] = z; }
        else if (sscanf(l, "shared %lu", &x) == 1) sdata[3] = x / 4;
        else if (sscanf(l, "localmem %lu", &x) == 1) sdata[4] = x / 4;
        else if (sscanf(l, "segments %lu", &x) == 1) segshift = (int)x;
        else if (sscanf(l, "spec %lu %lu", &x, &y) == 2) { me[nspec] = (VkSpecializationMapEntry){ x, 4 * nspec, 4 }; sdata[nspec++] = y; }
        else if (sscanf(l, "image %63s %lu %lu %lu %lu %511s", c, &x, &y, &z, &u, a) == 6) {
            int ty = -1; for (int k = 0; k < 7; k++) if (!strcmp(c, TYPES[k])) ty = k;
            imgs[ni++] = mkimage(ty, x, y, z, u, a);
        } else if (sscanf(l, "view %lu %lu", &x, &y) == 2) {
            infos[nw] = (VkDescriptorImageInfo){ 0, mkview(&imgs[ni - 1], x), VK_IMAGE_LAYOUT_GENERAL };
            writes[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, 0, x, y, 1, x >= 32 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &infos[nw], 0, 0 };
            nw++;
        } else if (sscanf(l, "sampler %lu %lu %lu %lu", &x, &y, &z, &t) == 4) {
            VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            si.magFilter = si.minFilter = y ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            si.mipmapMode = y ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.maxLod = z ? 16 : 0; si.unnormalizedCoordinates = !z;
            si.compareEnable = t != 0; si.compareOp = t ? t - 1 : 0;
            VkSampler s; CK(vkCreateSampler(dev, &si, 0, &s));
            infos[nw] = (VkDescriptorImageInfo){ s, 0, 0 };
            writes[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, 0, 0, x, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &infos[nw], 0, 0 };
            nw++;
        } else if (sscanf(l, "buf %511s", a) == 1) {
            size_t fn; void *d = slurp(a, &fn);
            bufs[nb] = mkbuf(fn, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            memcpy(bufs[nb].map, d, fn); free(d); nb++;
        } else if (sscanf(l, "outf %511s %lu %511s", a, &x, b) == 3) {
            size_t fn; void *d = slurp(b, &fn);
            bufs[nb] = mkbuf(x, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            memcpy(bufs[nb].map, d, fn < x ? fn : x); free(d);
            bufs[nb].out = strdup(a); nb++;
        } else if (sscanf(l, "out %511s %lu", a, &x) == 2) {
            bufs[nb] = mkbuf(x, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            bufs[nb].out = strdup(a); nb++;
        } else if (sscanf(l, "bank %lu size %lu", &x, &y) == 2) {
            banks[x] = mkbuf(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); have[x] = 1;
        } else if (sscanf(l, "bank %lu %511s", &x, a) == 2) {
            size_t fn; void *d = slurp(a, &fn);
            banks[x] = mkbuf(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); have[x] = 1;
            memcpy(banks[x].map, d, fn < 65536 ? fn : 65536); free(d);
        } else if (sscanf(l, "push %li %63s %li", (long *)&y, c, (long *)&z) == 3) {
            if (!strcmp(c, "addr")) memcpy(push + y, &bufs[z].addr, 8);
            else { uint32_t v = z; memcpy(push + y, &v, 4); }
        } else if (sscanf(l, "put %lu %li %63s %li", &x, (long *)&y, c, (long *)&z) == 4) {
            if (!have[x]) { banks[x] = mkbuf(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); have[x] = 1; }
            if (!strcmp(c, "addr")) {
                uint64_t ad = segshift >= 0 ? (uint64_t)z << segshift : bufs[z].addr;
                memcpy((char *)banks[x].map + y, &ad, 8);
            } else { uint32_t v = z; memcpy((char *)banks[x].map + y, &v, 4); }
        }
    }
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, cn, code };
    VkShaderModule sm; CK(vkCreateShaderModule(dev, &smi, 0, &sm));

    VkDescriptorSetLayout sl[4];
    VkDescriptorSetLayoutBinding lb[2][18]; uint32_t ln[2] = {0, 0};
    for (int k = 0; k < 18; k++) if (have[k]) {
        int set = k >= 3 ? 0 : 1; uint32_t bind = k >= 3 ? k - 3 : k;
        VkDescriptorSetLayoutBinding e = { bind, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, 0 };
        lb[set][ln[set]++] = e;
    }
    for (int s = 0; s < 2; s++) {
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, ln[s], lb[s] };
        CK(vkCreateDescriptorSetLayout(dev, &li, 0, &sl[s]));
    }
    VkDescriptorSetLayoutBinding tb[NBIND]; VkDescriptorBindingFlags tf[NBIND]; uint32_t nt = 0;
    for (int k = 0; k < NBIND; k++) {
        if (k == 31) continue;
        VkDescriptorType ty = k == 0 ? VK_DESCRIPTOR_TYPE_SAMPLER : k < 32 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if (k >= 32 && (k - 32) % 8 == 1) continue;
        if (k >= 32 && (k - 32) % 8 >= 6) continue;
        if (k >= 1 && k < 32 && (k - 1) % 10 >= 7) continue;
        tb[nt] = (VkDescriptorSetLayoutBinding){ k, ty, COUNT, VK_SHADER_STAGE_ALL, 0 };
        tf[nt++] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, 0, nt, tf };
    VkDescriptorSetLayoutCreateInfo tli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi, 0, nt, tb };
    CK(vkCreateDescriptorSetLayout(dev, &tli, 0, &sl[2]));
    {
        VkDescriptorSetLayoutBinding e[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nb ? nb : 1, VK_SHADER_STAGE_ALL, 0 },
                                              { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, 0 } };
        VkDescriptorBindingFlags ef[2] = { VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0 };
        VkDescriptorSetLayoutBindingFlagsCreateInfo efi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, 0, 2, ef };
        VkDescriptorSetLayoutCreateInfo l3 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &efi, 0, 2, e };
        CK(vkCreateDescriptorSetLayout(dev, &l3, 0, &sl[3]));
    }
    Buf locks = mkbuf(16384, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkPushConstantRange pr = { VK_SHADER_STAGE_ALL, 0, 128 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 4, sl, 1, &pr };
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, 0, &pl));
    VkSpecializationInfo si = { nspec, me, 4 * nspec, sdata };
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0,
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", &si }, pl, 0, 0 };
    const char *cpath = getenv("KRUN_CACHE");
    VkPipelineCache pc = 0;
    if (cpath) { VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO }; CK(vkCreatePipelineCache(dev, &pci, 0, &pc)); }
    VkPipeline pipe; CK(vkCreateComputePipelines(dev, pc, 1, &cpi, 0, &pipe));
    if (cpath) { size_t cs = 0; CK(vkGetPipelineCacheData(dev, pc, &cs, 0)); void *cd = malloc(cs); CK(vkGetPipelineCacheData(dev, pc, &cs, cd)); FILE *cf = fopen(cpath, "wb"); fwrite(cd, 1, cs, cf); fclose(cf); }

    VkDescriptorPoolSize ps[5] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 36 }, { VK_DESCRIPTOR_TYPE_SAMPLER, COUNT },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 32 * COUNT }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 80 * COUNT },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAXB + 1 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 4, 5, ps };
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, 0, &dp));
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 4, sl };
    VkDescriptorSet ds[4]; CK(vkAllocateDescriptorSets(dev, &dai, ds));
    for (int k = 0; k < 18; k++) if (have[k]) {
        VkDescriptorBufferInfo bi = { banks[k].b, 0, 65536 };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds[k >= 3 ? 0 : 1], k >= 3 ? k - 3 : k, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &bi, 0 };
        vkUpdateDescriptorSets(dev, 1, &w, 0, 0);
    }
    for (int k = 0; k < nw; k++) writes[k].dstSet = ds[2];
    vkUpdateDescriptorSets(dev, nw, writes, 0, 0);
    {
        VkDescriptorBufferInfo bi = { locks.b, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds[3], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &bi, 0 };
        vkUpdateDescriptorSets(dev, 1, &w, 0, 0);
    }
    for (int k = 0; k < nb; k++) {
        VkDescriptorBufferInfo bi = { bufs[k].b, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds[3], 0, k, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &bi, 0 };
        vkUpdateDescriptorSets(dev, 1, &w, 0, 0);
    }
    VkCommandBuffer cb = begin();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 4, ds, 0, 0);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_ALL, 0, 128, push);
    vkCmdDispatch(cb, groups[0], groups[1], groups[2]);
    Buf db = {0};
    if (dump && ni) {
        Img *im = &imgs[ni - 1];
        int bpp = (im->fmt == VK_FORMAT_R8G8B8A8_UNORM || im->fmt == VK_FORMAT_R32_UINT) ? 4 : 16;
        int nz = im->type == 2 ? im->d : im->layers;
        db = mkbuf((size_t)im->w * im->h * nz * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        VkBufferImageCopy c = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, im->type == 2 ? 1 : im->layers }, {0, 0, 0}, { im->w, im->h, im->type == 2 ? im->d : 1 } };
        vkCmdCopyImageToBuffer(cb, im->im, VK_IMAGE_LAYOUT_GENERAL, db.b, 1, &c);
    }
    submit(cb);
    if (dump && ni) { FILE *f = fopen(dump, "wb"); fwrite(db.map, 1, db.n, f); fclose(f); }
    for (int k = 0; k < nb; k++) if (bufs[k].out) {
        FILE *f = fopen(bufs[k].out, "wb");
        fwrite(bufs[k].map, 1, bufs[k].n, f); fclose(f);
    }
    return 0;
}
