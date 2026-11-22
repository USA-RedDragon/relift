/**
 * @file test_upscale_images.c
 * @brief The upscaler stage on image views, as a guest's frame calls it (place 1): every spatial choice runs on images
 * (nearest picks the pixel the mapping names), FSR 2, the FSR 3 upscaler and relift's TAA with depth and motion vectors
 * keep a still flat picture and a still ramp at ratio 1, and all three rebuild a detailed picture from jittered frames
 * at half size closer with the jitter and motion vectors as DLSS takes them than with either one's sign turned.
 */
#include "runner.h"

#include "hostlib.h"

#include <relift/settings.h>
#include <relift/upscale.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<vulkan/vulkan.h>)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

/** @brief A device for the tests and the functions they call. */
static struct {
    bool tried, ok;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkPhysicalDeviceMemoryProperties mem;
    PFN_vkGetDeviceProcAddr gdpa;
#define T_FNS(X)                                                                                                       \
    X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkBindImageMemory) X(vkCreateImageView)     \
    X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkBeginCommandBuffer)   \
    X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage)        \
    X(vkCmdCopyImageToBuffer) X(vkDestroyImageView) X(vkDestroyImage) X(vkFreeMemory) X(vkDestroyBuffer)
#define T_FIELD(n) PFN_##n n;
    T_FNS(T_FIELD)
#undef T_FIELD
} T;

static bool start(void)
{
    if (T.tried)
        return T.ok;
    T.tried = true;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)rl_hostlib_vulkan_gipa();
    if (!gipa)
        return false;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    if (!create || create(&ii, NULL, &T.inst) != VK_SUCCESS)
        return false;
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)gipa(T.inst, "vkEnumeratePhysicalDevices");
    uint32_t n = 1;
    if (en(T.inst, &n, &T.phys) < 0 || !n)
        return false;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(T.inst, "vkGetPhysicalDeviceMemoryProperties"))(T.phys, &T.mem);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueCount = 1,
                                  .pQueuePriorities = &prio };
    VkPhysicalDeviceFeatures feat = { .shaderStorageImageWriteWithoutFormat = VK_TRUE,
                                      .shaderStorageImageExtendedFormats = VK_TRUE };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &q,
                              .pEnabledFeatures = &feat };
    PFN_vkCreateDevice cd = (PFN_vkCreateDevice)gipa(T.inst, "vkCreateDevice");
    if (cd(T.phys, &di, NULL, &T.dev) != VK_SUCCESS)
        return false;
    T.gdpa = (PFN_vkGetDeviceProcAddr)gipa(T.inst, "vkGetDeviceProcAddr");
#define T_LOAD(name) T.name = (PFN_##name)T.gdpa(T.dev, #name);
    T_FNS(T_LOAD)
#undef T_LOAD
    ((PFN_vkGetDeviceQueue)T.gdpa(T.dev, "vkGetDeviceQueue"))(T.dev, 0, 0, &T.queue);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    if (((PFN_vkCreateCommandPool)T.gdpa(T.dev, "vkCreateCommandPool"))(T.dev, &pci, NULL, &T.pool) != VK_SUCCESS)
        return false;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = T.pool,
                                        .commandBufferCount = 1 };
    T.ok = ((PFN_vkAllocateCommandBuffers)T.gdpa(T.dev, "vkAllocateCommandBuffers"))(T.dev, &cai, &T.cb) == VK_SUCCESS;
    if (!T.ok)
        printf("    (no Vulkan device: the image stage is not run)\n");
    return T.ok;
}

static uint32_t mtype(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < T.mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (T.mem.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0;
}

/** @brief An image, its memory and a view of it. */
typedef struct img {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t w, h, bpp;
    VkFormat format;
} img;

static img image(uint32_t w, uint32_t h, VkFormat f, uint32_t bpp)
{
    img m = { .w = w, .h = h, .bpp = bpp, .format = f };
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             .imageType = VK_IMAGE_TYPE_2D,
                             .format = f,
                             .extent = { w, h, 1 },
                             .mipLevels = 1,
                             .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    T.vkCreateImage(T.dev, &ci, NULL, &m.image);
    VkMemoryRequirements rq;
    T.vkGetImageMemoryRequirements(T.dev, m.image, &rq);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = rq.size,
                                .memoryTypeIndex = mtype(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    T.vkAllocateMemory(T.dev, &ai, NULL, &m.memory);
    T.vkBindImageMemory(T.dev, m.image, m.memory, 0);
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 .image = m.image,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = f,
                                 .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    T.vkCreateImageView(T.dev, &vi, NULL, &m.view);
    return m;
}

static void image_free(img *m)
{
    T.vkDestroyImageView(T.dev, m->view, NULL);
    T.vkDestroyImage(T.dev, m->image, NULL);
    T.vkFreeMemory(T.dev, m->memory, NULL);
}

/** @brief A host-visible buffer of n bytes, mapped. */
typedef struct hbuf {
    VkBuffer buf;
    VkDeviceMemory memory;
    void *map;
} hbuf;

static hbuf hbuf_new(size_t n)
{
    hbuf b = { 0 };
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = n,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    T.vkCreateBuffer(T.dev, &bi, NULL, &b.buf);
    VkMemoryRequirements rq;
    T.vkGetBufferMemoryRequirements(T.dev, b.buf, &rq);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = rq.size,
        .memoryTypeIndex =
            mtype(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    T.vkAllocateMemory(T.dev, &ai, NULL, &b.memory);
    T.vkBindBufferMemory(T.dev, b.buf, b.memory, 0);
    T.vkMapMemory(T.dev, b.memory, 0, VK_WHOLE_SIZE, 0, &b.map);
    return b;
}

static void hbuf_free(hbuf *b)
{
    T.vkDestroyBuffer(T.dev, b->buf, NULL);
    T.vkFreeMemory(T.dev, b->memory, NULL);
}

static void begin(void)
{
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    T.vkBeginCommandBuffer(T.cb, &bi);
}

static void barrier_all(void)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    T.vkCmdPipelineBarrier(T.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           NULL, 0, NULL);
}

static void submit(void)
{
    T.vkEndCommandBuffer(T.cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &T.cb };
    T.vkQueueSubmit(T.queue, 1, &si, VK_NULL_HANDLE);
    T.vkQueueWaitIdle(T.queue);
}

/**
 * @brief Moves m to the general layout and fills it with data (m's size times bpp bytes), or leaves it when data is
 * NULL.
 */
static void fill(img *m, const void *data)
{
    size_t n = (size_t)m->w * m->h * m->bpp;
    hbuf b = hbuf_new(n);
    if (data)
        memcpy(b.map, data, n);
    begin();
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = m->image,
                                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    T.vkCmdPipelineBarrier(T.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                           1, &ib);
    if (data) {
        VkBufferImageCopy c = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                .imageExtent = { m->w, m->h, 1 } };
        T.vkCmdCopyBufferToImage(T.cb, b.buf, m->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    barrier_all();
    submit();
    hbuf_free(&b);
}

static void read_back(img *m, void *out)
{
    size_t n = (size_t)m->w * m->h * m->bpp;
    hbuf b = hbuf_new(n);
    begin();
    barrier_all();
    VkBufferImageCopy c = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                            .imageExtent = { m->w, m->h, 1 } };
    T.vkCmdCopyImageToBuffer(T.cb, m->image, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &c);
    barrier_all();
    submit();
    memcpy(out, b.map, n);
    hbuf_free(&b);
}

static rl_post_device device(void)
{
    rl_post_device d = { .instance = (uint64_t)(uintptr_t)T.inst,
                         .physical_device = (uint64_t)(uintptr_t)T.phys,
                         .device = (uint64_t)(uintptr_t)T.dev,
                         .queue = (uint64_t)(uintptr_t)T.queue };
    memcpy(&d.get_device_proc_addr, &T.gdpa, sizeof T.gdpa);
    return d;
}

/** @brief Records in on u and runs it. */
static bool run_stage(rl_upscaler *u, const rl_upscale_inputs *in)
{
    begin();
    barrier_all();
    bool ok = rl_upscaler_record(u, (uint64_t)(uintptr_t)T.cb, in);
    barrier_all();
    submit();
    rl_upscaler_retire(u);
    return ok;
}

static void scene(uint32_t *px, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            int dx = (int)x - (int)w / 2, dy = (int)y - (int)h / 3;
            bool disc = dx * dx + dy * dy < (int)(w * w / 16);
            uint32_t r = disc ? 250 : x * 255 / w, g = disc ? 30 : ((x ^ y) & 4 ? 200 : 40), b = y * 255 / h;
            px[y * w + x] = r | g << 8 | b << 16 | 0xff000000u;
        }
}

TEST(upscale_images_spatial_choices_run_on_images)
{
    if (!start())
        return;
    enum { W = 37, H = 23, OW = 64, OH = 41 };
    static uint32_t src[W * H], img_out[OW * OH];
    scene(src, W, H);
    img in = image(W, H, VK_FORMAT_R8G8B8A8_UNORM, 4), out = image(OW, OH, VK_FORMAT_R8G8B8A8_UNORM, 4);
    fill(&in, src);
    fill(&out, NULL);
    rl_post_device d = device();
    for (uint32_t c = RL_UPSCALE_NEAREST; c <= RL_UPSCALE_LANCZOS; c++) {
        rl_upscaler *u = rl_upscaler_new(&d, c, RL_UPSCALE_IMAGES);
        CHECK(u != NULL);
        if (!u)
            continue;
        rl_upscale_inputs ui = { .color = { .handle = (uint64_t)(uintptr_t)in.view, .width = W, .height = H },
                                 .in = { 0, 0, W, H },
                                 .out = { .handle = (uint64_t)(uintptr_t)out.view, .width = OW, .height = OH },
                                 .out_rect = { 0, 0, OW, OH } };
        CHECK(run_stage(u, &ui));
        read_back(&out, img_out);
        rl_upscale_rect ir = { 0, 0, W, H }, orc = { 0, 0, OW, OH };
        float s[2];
        rl_upscale_map(&ir, &orc, 10, 7, s);
        if (c == RL_UPSCALE_NEAREST)
            CHECK_EQ(img_out[7 * OW + 10], src[(uint32_t)s[1] * W + (uint32_t)s[0]]);
        else
            CHECK(img_out[7 * OW + 10] != 0);
        rl_upscaler_free(u);
    }
    image_free(&in);
    image_free(&out);
}

/**
 * @brief Choice over 16 frames of a still W by H picture src (zero motion, flat depth) into got (W times H words);
 * false when a frame did not record.
 */
static bool temporal_run(uint32_t choice, const uint32_t *src, uint32_t w, uint32_t h, uint32_t *got);

/** @brief Temporal choice over 16 frames of a still picture (temporal_run); the worst channel difference from src. */
static int temporal_still(uint32_t choice, const uint32_t *src, uint32_t w, uint32_t h)
{
    uint32_t *got = calloc((size_t)w * h, 4);
    int worst = 999;
    if (got && temporal_run(choice, src, w, h, got)) {
        worst = 0;
        for (size_t i = 0; i < (size_t)w * h; i++)
            for (int k = 0; k < 3; k++) {
                int dlt = abs((int)((got[i] >> (8 * k)) & 0xff) - (int)((src[i] >> (8 * k)) & 0xff));
                worst = dlt > worst ? dlt : worst;
            }
    }
    free(got);
    return worst;
}

static bool temporal_run(uint32_t choice, const uint32_t *src, uint32_t w, uint32_t h, uint32_t *got)
{
    uint32_t *zero = calloc((size_t)w * h, 4);
    float *depth = malloc((size_t)w * h * 4);
    bool ran = false;
    if (!zero || !depth)
        goto out;
    for (size_t i = 0; i < (size_t)w * h; i++)
        depth[i] = 0.5f;
    img color = image(w, h, VK_FORMAT_R8G8B8A8_UNORM, 4), dst = image(w, h, VK_FORMAT_R8G8B8A8_UNORM, 4);
    img dep = image(w, h, VK_FORMAT_R32_SFLOAT, 4), mv = image(w, h, VK_FORMAT_R16G16_SFLOAT, 4);
    fill(&color, src);
    fill(&dst, NULL);
    fill(&dep, depth);
    fill(&mv, zero);
    rl_post_device d = device();
    rl_upscaler *u = rl_upscaler_new(&d, choice, RL_UPSCALE_IMAGES);
    if (u) {
        rl_upscale_inputs ui = { .color = { .handle = (uint64_t)(uintptr_t)color.view, .width = w, .height = h },
                                 .depth = { .handle = (uint64_t)(uintptr_t)dep.view, .width = w, .height = h },
                                 .motion = { .handle = (uint64_t)(uintptr_t)mv.view, .width = w, .height = h },
                                 .motion_display_size = true,
                                 .motion_scale = { 1.0f, 1.0f },
                                 .pre_exposure = 1.0f,
                                 .in = { 0, 0, w, h },
                                 .out = { .handle = (uint64_t)(uintptr_t)dst.view, .width = w, .height = h },
                                 .out_rect = { 0, 0, w, h } };
        ran = true;
        for (int frame = 0; frame < 16; frame++) {
            ui.reset = frame == 0;
            ran = run_stage(u, &ui) && ran;
        }
        read_back(&dst, got);
        rl_upscaler_free(u);
    }
    image_free(&color);
    image_free(&dst);
    image_free(&dep);
    image_free(&mv);
out:
    free(zero);
    free(depth);
    return ran;
}

/** @brief The worst differences of choice over a still flat picture and a still ramp at ratio 1, printed as name. */
static void still_pictures(uint32_t choice, const char *name, int *flat_worst, int *ramp_worst)
{
    enum { W = 64, H = 48 };
    static uint32_t flat[W * H], ramp[W * H];
    for (uint32_t i = 0; i < W * H; i++) {
        flat[i] = 0xff2050c8u;
        ramp[i] = 0xff000000u | (i % W) * 4 | (i / W) * 5 << 8 | (255 - (i % W) * 2) << 16;
    }
    *flat_worst = temporal_still(choice, flat, W, H);
    *ramp_worst = temporal_still(choice, ramp, W, H);
    printf("    %s 64x48 still, 16 frames: flat worst difference %d, ramp worst difference %d\n", name, *flat_worst,
           *ramp_worst);
}

TEST(upscale_fsr2_keeps_still_pictures_at_ratio_one)
{
    if (!start())
        return;
    int a, b;
    still_pictures(RL_UPSCALE_FSR2, "fsr2", &a, &b);
    CHECK_EQ(a, 0);
    CHECK(b <= 2);
}

TEST(upscale_fsr3_keeps_still_pictures_at_ratio_one)
{
    if (!start())
        return;
    int a, b;
    still_pictures(RL_UPSCALE_FSR3, "fsr3", &a, &b);
    CHECK_EQ(a, 0);
    CHECK(b <= 2);
}

/** @brief The test picture at display pixel position (x, y): detail finer than half the display size holds. */
static float detail(float x, float y)
{
    const float tau = 6.2831853f;
    return 0.5f + 0.4f * sinf(tau * x / 5.3f) * sinf(tau * y / 4.7f);
}

/** @brief The i-th point of the base b Halton sequence, in [0, 1). */
static float halton(uint32_t i, uint32_t b)
{
    float f = 1.0f, r = 0.0f;
    for (; i; i /= b) {
        f /= (float)b;
        r += f * (float)(i % b);
    }
    return r;
}

/**
 * @brief Temporal choice over 48 frames of detail() at W by H render pixels to twice that, the picture moving speed
 * display pixels a frame to the right. Each frame is drawn with the Halton jitter the way DLSS and FSR 2 take it (a
 * render pixel's center shows the picture at the center minus the jitter) and its motion vectors point to where a pixel
 * was the frame before, in render pixels; the stage gets the jitter times jsign and the motion vectors times msign. The
 * mean channel difference from the picture drawn at display size, over the last frame.
 */
static double temporal_moving(uint32_t choice, uint32_t w, uint32_t h, float speed, float jsign, float msign)
{
    uint32_t dw = 2 * w, dh = 2 * h;
    uint32_t *frame = malloc((size_t)w * h * 4), *got = malloc((size_t)dw * dh * 4);
    float *depth = malloc((size_t)w * h * 4);
    uint16_t *motion = malloc((size_t)w * h * 4);
    double err = 999.0;
    if (!frame || !got || !depth || !motion)
        goto out;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        depth[i] = 0.5f;
        motion[2 * i] = (uint16_t)(msign * speed > 0.0f ? 0xb800u : msign * speed < 0.0f ? 0x3800u : 0x0000u);
        motion[2 * i + 1] = 0;
    }
    img color = image(w, h, VK_FORMAT_R8G8B8A8_UNORM, 4), dst = image(dw, dh, VK_FORMAT_R8G8B8A8_UNORM, 4);
    img dep = image(w, h, VK_FORMAT_R32_SFLOAT, 4), mv = image(w, h, VK_FORMAT_R16G16_SFLOAT, 4);
    fill(&dst, NULL);
    fill(&dep, depth);
    fill(&mv, motion);
    rl_post_device d = device();
    rl_upscaler *u = rl_upscaler_new(&d, choice, RL_UPSCALE_IMAGES);
    enum { FRAMES = 48 };
    bool ran = u != NULL;
    float shift = 0.0f;
    for (uint32_t k = 0; ran && k < FRAMES; k++) {
        float jx = halton(k + 1, 2) - 0.5f, jy = halton(k + 1, 3) - 0.5f;
        shift = speed * (float)k;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                float v = detail(2.0f * ((float)x + 0.5f - jx) - shift, 2.0f * ((float)y + 0.5f - jy));
                uint32_t c = (uint32_t)lrintf(v * 255.0f);
                frame[y * w + x] = c | c << 8 | c << 16 | 0xff000000u;
            }
        fill(&color, frame);
        rl_upscale_inputs ui = { .color = { .handle = (uint64_t)(uintptr_t)color.view, .width = w, .height = h },
                                 .depth = { .handle = (uint64_t)(uintptr_t)dep.view, .width = w, .height = h },
                                 .motion = { .handle = (uint64_t)(uintptr_t)mv.view, .width = w, .height = h },
                                 .motion_scale = { 1.0f, 1.0f },
                                 .pre_exposure = 1.0f,
                                 .jitter = { jsign * jx, jsign * jy },
                                 .reset = k == 0,
                                 .in = { 0, 0, w, h },
                                 .out = { .handle = (uint64_t)(uintptr_t)dst.view, .width = dw, .height = dh },
                                 .out_rect = { 0, 0, dw, dh } };
        ran = run_stage(u, &ui);
    }
    if (ran) {
        read_back(&dst, got);
        double sum = 0.0;
        uint32_t n = 0;
        for (uint32_t y = 4; y + 4 < dh; y++)
            for (uint32_t x = 4; x + 4 < dw; x++, n++)
                sum += fabs((double)(got[y * dw + x] & 0xff) -
                            255.0 * (double)detail((float)x + 0.5f - shift, (float)y + 0.5f));
        err = sum / n;
    }
    rl_upscaler_free(u);
    image_free(&color);
    image_free(&dst);
    image_free(&dep);
    image_free(&mv);
out:
    free(frame);
    free(got);
    free(depth);
    free(motion);
    return err;
}

/** @brief Checks choice (printed as name) rebuilds detail() closer with the jitter and motion as DLSS gives them. */
static void takes_dlss_conventions(uint32_t choice, const char *name)
{
    enum { W = 48, H = 32 };
    double still = temporal_moving(choice, W, H, 0.0f, 1.0f, 1.0f);
    double still_flip = temporal_moving(choice, W, H, 0.0f, -1.0f, 1.0f);
    double moving = temporal_moving(choice, W, H, 1.0f, 1.0f, 1.0f);
    double moving_flip = temporal_moving(choice, W, H, 1.0f, 1.0f, -1.0f);
    printf("    %s %ux%u -> %ux%u, mean difference: still %.2f (jitter turned %.2f), moving %.2f (motion turned "
           "%.2f)\n",
           name, W, H, 2 * W, 2 * H, still, still_flip, moving, moving_flip);
    CHECK(still < 0.7 * still_flip);
    CHECK(moving < 0.7 * moving_flip);
}

TEST(upscale_fsr2_takes_the_jitter_and_motion_vectors_as_dlss_gives_them)
{
    if (start())
        takes_dlss_conventions(RL_UPSCALE_FSR2, "fsr2");
}

TEST(upscale_fsr3_takes_the_jitter_and_motion_vectors_as_dlss_gives_them)
{
    if (start())
        takes_dlss_conventions(RL_UPSCALE_FSR3, "fsr3");
}

TEST(upscale_dlss_where_it_cannot_run_runs_its_fallback)
{
    if (!start())
        return;
    int a, b, c, e;
    still_pictures(RL_UPSCALE_DLSS, "dlss", &a, &b);
    still_pictures(RL_UPSCALE_FALLBACK_TEMPORAL, "fallback", &c, &e);
    CHECK_EQ(a, c);
    CHECK_EQ(b, e);
}

TEST(upscale_taa_keeps_still_pictures_at_ratio_one)
{
    if (!start())
        return;
    int a, b;
    still_pictures(RL_UPSCALE_TAA, "taa", &a, &b);
    CHECK_EQ(a, 0);
    CHECK(b <= 2);
}

TEST(upscale_taa_sharpening_changes_a_soft_edge_and_keeps_a_flat_field)
{
    if (!start())
        return;
    enum { W = 64, H = 48 };
    static uint32_t edge[W * H], flat[W * H], plain[W * H], sharp[W * H], flat_sharp[W * H];
    for (uint32_t i = 0; i < W * H; i++) {
        float t = ((float)(i % W) - 28.0f) / 8.0f;
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        uint32_t v = (uint32_t)lrintf(40.0f + 170.0f * t * t * (3.0f - 2.0f * t));
        edge[i] = 0xff000000u | v | v << 8 | v << 16;
        flat[i] = 0xff6080a0u;
    }
    CHECK(rl_settings_set("picture.taa_sharpness", "0") == RL_SETTING_OK);
    CHECK(temporal_run(RL_UPSCALE_TAA, edge, W, H, plain));
    CHECK(rl_settings_set("picture.taa_sharpness", "1") == RL_SETTING_OK);
    CHECK(temporal_run(RL_UPSCALE_TAA, edge, W, H, sharp));
    CHECK(temporal_run(RL_UPSCALE_TAA, flat, W, H, flat_sharp));
    CHECK(rl_settings_set("picture.taa_sharpness", "0.5") == RL_SETTING_OK);
    int edge_diff = 0, flat_diff = 0, plain_diff = 0;
    for (uint32_t i = 0; i < W * H; i++)
        for (int k = 0; k < 3; k++) {
            int a = (int)((plain[i] >> (8 * k)) & 0xff), b = (int)((sharp[i] >> (8 * k)) & 0xff);
            int f = (int)((flat_sharp[i] >> (8 * k)) & 0xff), e = (int)((edge[i] >> (8 * k)) & 0xff);
            edge_diff = abs(a - b) > edge_diff ? abs(a - b) : edge_diff;
            plain_diff = abs(a - e) > plain_diff ? abs(a - e) : plain_diff;
            int g = abs(f - (int)((flat[i] >> (8 * k)) & 0xff));
            flat_diff = g > flat_diff ? g : flat_diff;
        }
    printf("    taa 64x48 soft edge: sharpened differs by up to %d, plain by %d from the input; flat field by %d\n",
           edge_diff, plain_diff, flat_diff);
    CHECK(plain_diff <= 1);
    CHECK(edge_diff >= 4);
    CHECK(flat_diff <= 1);
}

TEST(upscale_taa_takes_the_jitter_and_motion_vectors_as_dlss_gives_them)
{
    if (start())
        takes_dlss_conventions(RL_UPSCALE_TAA, "taa");
}
#endif
