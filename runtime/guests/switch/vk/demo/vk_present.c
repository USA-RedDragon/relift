/**
 * @file vk_present.c
 * @brief A synthetic Switch program that draws through Vulkan forwarding: a
 * hand-written lifted module whose only import is vkGetInstanceProcAddr. It
 * runs through rl_switch_main with the forwarding hook, so the loader
 * resolves the import, and the guest then creates a vi stray layer, sets its
 * size, creates an instance, a VK_NN_vi_surface surface, a device and a
 * swapchain, and clears and presents RELIFT_DEMO_FRAMES frames (60 by
 * default). The last frame is also copied into mapped memory and checked.
 * Every step prints a PASS or FAIL line through svcOutputDebugString.
 *
 * With a window backend up (VIDEO=sdl3 and a display) the layer shows in a
 * window; otherwise the frames go to headless surfaces. RELIFT_VK_TEST_DEVICE
 * picks a physical device by name. Exits 0 when every step and frame passed,
 * 70 (svcBreak) on a failure, 77 when the host has no Vulkan (and
 * RELIFT_VK_REQUIRE is unset).
 */
#include "hostlib.h"
#include "loader.h"
#include "tc.h"
#include "vkf.h"

#include <stdarg.h>
#include <stdlib.h>

#define BASE 0x8000000ull
#define GOT_GIPA (BASE + 0x2800)
#define HEAP_SIZE (32ull << 20)
#define LAYER_W 960u
#define LAYER_H 540u
#define SKIP 77

static uint8_t g_image[0x3000] __attribute__((aligned(16)));

static rl_ctx *g_c;
static uint8_t *g_mem;
static uint64_t g_brk, g_heap_end;
static uint64_t g_gipa, g_inst;
static int g_failed;

int t_svc_rc;

uint8_t *t_mem(void) { return rl_mem_base(); }

uint32_t t_svc(rl_ctx *c, uint32_t imm, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    c->x[4] = a4;
    c->x[5] = a5;
    t_svc_rc = rl_svc(c, t_mem(), imm);
    return (uint32_t)c->x[0];
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof line)
        n = sizeof line - 1;
    uint64_t a = g_heap_end - 512;
    memcpy(g_mem + a, line, (size_t)n);
    uint64_t sp = g_c->sp;
    t_svc(g_c, SVC_OUTPUT_DEBUG_STRING, a, (uint64_t)n, 0, 0, 0, 0);
    g_c->sp = sp;
}

#define EXPECT(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            say("FAIL " __VA_ARGS__);                                                                                  \
            g_failed++;                                                                                                \
            return -1;                                                                                                 \
        }                                                                                                              \
    } while (0)

#define GP(a) ((void *)(uintptr_t)(a))

static uint64_t galloc(uint64_t n)
{
    g_brk = (g_brk + 15) & ~15ull;
    uint64_t a = g_brk;
    g_brk += n;
    if (g_brk > g_heap_end - 4096)
        abort();
    memset(g_mem + a, 0, n);
    return a;
}

static uint64_t gput(const void *p, uint64_t n)
{
    uint64_t a = galloc(n);
    memcpy(g_mem + a, p, n);
    return a;
}

static uint64_t gstr(const char *s) { return gput(s, strlen(s) + 1); }

static uint64_t g64(uint64_t a) { return rl_ld64(g_mem, a); }

static uint32_t g32(uint64_t a) { return rl_ld32(g_mem, a); }

/** @brief Calls the guest function at fn the way a BLR from lifted code does: x0-x7, the rest on the stack. */
static uint64_t gcall_addr(uint64_t fn, const uint64_t *args, unsigned n)
{
    uint64_t sp0 = g_c->sp, sp = ((sp0 - 8u * (n > 8 ? n - 8 : 0)) & ~15ull) - 64;
    for (unsigned i = 0; i < n; i++) {
        if (i < 8)
            g_c->x[i] = args[i];
        else
            rl_st64(g_mem, sp + 8 * (i - 8), args[i]);
    }
    g_c->sp = sp;
    int rc = rl_indirect(g_c, fn)(g_c, g_mem);
    g_c->sp = sp0;
    if (rc != RL_OK) {
        say("FAIL guest call 0x%llx returned %d", (unsigned long long)fn, rc);
        g_failed++;
    }
    return g_c->x[0];
}

static uint64_t gcall(const char *name, const uint64_t *args, unsigned n)
{
    uint64_t a[2] = { g_inst, gstr(name) };
    uint64_t fn = gcall_addr(g_gipa, a, 2);
    if (!fn) {
        say("FAIL vkGetInstanceProcAddr(%s) is NULL", name);
        g_failed++;
        return (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return gcall_addr(fn, args, n);
}

#define G(name, ...)                                                                                                   \
    ({                                                                                                                 \
        uint64_t g_args_[] = { __VA_ARGS__ };                                                                          \
        gcall(name, g_args_, sizeof g_args_ / sizeof g_args_[0]);                                                      \
    })

#define VKR(x) ((int32_t)(uint32_t)(x))

typedef struct vi_layer_out {
    uint64_t layer;
    uint64_t window;
} vi_layer_out;

/** @brief vi:m, the application display service, the default display, a stray layer and its size. */
static int make_layer(vi_layer_out *out)
{
    uint64_t scratch = galloc(0x1000);
    tc_obj root = tc_open_service(g_c, scratch, "vi:m");
    EXPECT(root.handle, "vi:m did not open");
    tc_obj disp, sys;
    tc_req q = tc_new(2);
    tc_put32(&q, 1);
    EXPECT(tc_sub(g_c, root, q, &disp) == 0, "GetDisplayService failed");
    EXPECT(tc_sub(g_c, disp, tc_new(101), &sys) == 0, "GetSystemDisplayService failed");
    tc_res r = tc_send(g_c, disp, tc_new(1011));
    EXPECT(r.result == 0, "OpenDefaultDisplay: 0x%x", r.result);
    uint64_t display = tc_u64(&r, 0);
    uint64_t parcel = galloc(0x100);
    tc_req s = tc_new(2030);
    tc_put32(&s, 0);
    tc_put32(&s, 0);
    tc_put64(&s, display);
    s.nb = 1;
    s.b_addr[0] = parcel;
    s.b_size[0] = 0x100;
    r = tc_send(g_c, disp, s);
    EXPECT(r.result == 0, "CreateStrayLayer: 0x%x", r.result);
    out->layer = tc_u64(&r, 0);
    out->window = parcel;
    tc_req z = tc_new(2203);
    tc_put64(&z, out->layer);
    tc_put64(&z, LAYER_W);
    tc_put64(&z, LAYER_H);
    r = tc_send(g_c, sys, z);
    EXPECT(r.result == 0, "SetLayerSize: 0x%x", r.result);
    say("PASS vi layer %llu, %ux%u", (unsigned long long)out->layer, LAYER_W, LAYER_H);
    return 0;
}

static void barrier(uint64_t cb, uint64_t image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                    VkAccessFlags dst)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src,
        .dstAccessMask = dst,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = (VkImage)GP(image),
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    G("vkCmdPipelineBarrier", cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0,
      0, 1, gput(&b, sizeof b));
}

typedef struct gpu {
    uint64_t pd, dev, queue, surface, swapchain;
    uint32_t qf, nimages;
    uint64_t images[8];
    VkFormat format;
    VkExtent2D extent;
    uint64_t pool, cmd[8], fence[8], acquired[8], rendered[8];
    uint64_t buf, bufmem, bufmap;
} gpu;

static int pick_device(gpu *g)
{
    uint64_t gn = galloc(4);
    EXPECT(VKR(G("vkEnumeratePhysicalDevices", g_inst, gn, 0)) == VK_SUCCESS, "vkEnumeratePhysicalDevices");
    uint32_t n = g32(gn) < 8 ? g32(gn) : 8;
    rl_st32(g_mem, gn, n);
    uint64_t arr = galloc(8ull * n);
    G("vkEnumeratePhysicalDevices", g_inst, gn, arr);
    const char *want = getenv("RELIFT_VK_TEST_DEVICE");
    for (uint32_t i = 0; i < g32(gn); i++) {
        uint64_t pd = g64(arr + 8ull * i);
        uint64_t gp = galloc(sizeof(VkPhysicalDeviceProperties));
        G("vkGetPhysicalDeviceProperties", pd, gp);
        const char *name = (const char *)g_mem + gp + offsetof(VkPhysicalDeviceProperties, deviceName);
        if (want && !strstr(name, want))
            continue;
        uint64_t gq = galloc(4);
        G("vkGetPhysicalDeviceQueueFamilyProperties", pd, gq, 0);
        uint64_t qa = galloc(g32(gq) * sizeof(VkQueueFamilyProperties));
        G("vkGetPhysicalDeviceQueueFamilyProperties", pd, gq, qa);
        for (uint32_t f = 0; f < g32(gq); f++) {
            if (!(g32(qa + f * sizeof(VkQueueFamilyProperties)) & VK_QUEUE_GRAPHICS_BIT))
                continue;
            uint64_t gs = galloc(4);
            if (VKR(G("vkGetPhysicalDeviceSurfaceSupportKHR", pd, f, g->surface, gs)) != VK_SUCCESS || !g32(gs))
                continue;
            g->pd = pd;
            g->qf = f;
            say("PASS device %s, queue family %u", name, f);
            return 0;
        }
    }
    EXPECT(0, "no physical device can present to the vi surface%s%s", want ? " and matches " : "", want ? want : "");
    return -1;
}

static int make_instance(gpu *g, const vi_layer_out *vl)
{
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = GP(gstr("relift vk_present")),
        .apiVersion = VKF_API_VERSION(1, 1),
    };
    const char *exts[] = { "VK_KHR_surface", "VK_NN_vi_surface" };
    uint64_t names[2] = { gstr(exts[0]), gstr(exts[1]) };
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = GP(gput(&app, sizeof app)),
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = GP(gput(names, sizeof names)),
    };
    uint64_t out = galloc(8);
    g_inst = 0;
    int32_t r = VKR(G("vkCreateInstance", gput(&ci, sizeof ci), 0, out));
    EXPECT(r == VK_SUCCESS, "vkCreateInstance: %d", r);
    g_inst = g64(out);
    VkViSurfaceCreateInfoNN si = { .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, .window = GP(vl->window) };
    r = VKR(G("vkCreateViSurfaceNN", g_inst, gput(&si, sizeof si), 0, out));
    EXPECT(r == VK_SUCCESS, "vkCreateViSurfaceNN: %d", r);
    g->surface = g64(out);
    say("PASS instance and vi surface");
    return 0;
}

static int make_device(gpu *g)
{
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g->qf,
        .queueCount = 1,
        .pQueuePriorities = GP(gput(&prio, 4)),
    };
    uint64_t ext = gstr("VK_KHR_swapchain");
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = GP(gput(&qci, sizeof qci)),
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = GP(gput(&ext, 8)),
    };
    uint64_t out = galloc(8);
    int32_t r = VKR(G("vkCreateDevice", g->pd, gput(&dci, sizeof dci), 0, out));
    EXPECT(r == VK_SUCCESS, "vkCreateDevice: %d", r);
    g->dev = g64(out);
    G("vkGetDeviceQueue", g->dev, g->qf, 0, out);
    g->queue = g64(out);
    return 0;
}

static int make_swapchain(gpu *g)
{
    uint64_t gc = galloc(sizeof(VkSurfaceCapabilitiesKHR));
    int32_t r = VKR(G("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", g->pd, g->surface, gc));
    EXPECT(r == VK_SUCCESS, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR: %d", r);
    VkSurfaceCapabilitiesKHR caps;
    memcpy(&caps, g_mem + gc, sizeof caps);
    EXPECT(caps.currentExtent.width == LAYER_W && caps.currentExtent.height == LAYER_H,
           "surface extent %ux%u is not the layer's", caps.currentExtent.width, caps.currentExtent.height);
    uint64_t gn = galloc(4);
    G("vkGetPhysicalDeviceSurfaceFormatsKHR", g->pd, g->surface, gn, 0);
    uint32_t nf = g32(gn);
    uint64_t gf = galloc(nf * sizeof(VkSurfaceFormatKHR));
    G("vkGetPhysicalDeviceSurfaceFormatsKHR", g->pd, g->surface, gn, gf);
    VkSurfaceFormatKHR fmt;
    memcpy(&fmt, g_mem + gf, sizeof fmt);
    for (uint32_t i = 0; i < nf; i++) {
        VkSurfaceFormatKHR f;
        memcpy(&f, g_mem + gf + i * sizeof f, sizeof f);
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM)
            fmt = f;
    }
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = (VkSurfaceKHR)GP(g->surface),
        .minImageCount = caps.minImageCount + 1,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = caps.currentExtent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    uint64_t out = galloc(8);
    r = VKR(G("vkCreateSwapchainKHR", g->dev, gput(&ci, sizeof ci), 0, out));
    EXPECT(r == VK_SUCCESS, "vkCreateSwapchainKHR: %d", r);
    g->swapchain = g64(out);
    g->format = fmt.format;
    g->extent = caps.currentExtent;
    rl_st32(g_mem, gn, 8);
    uint64_t gi = galloc(64);
    r = VKR(G("vkGetSwapchainImagesKHR", g->dev, g->swapchain, gn, gi));
    EXPECT(r == VK_SUCCESS, "vkGetSwapchainImagesKHR: %d", r);
    g->nimages = g32(gn);
    for (uint32_t i = 0; i < g->nimages; i++)
        g->images[i] = g64(gi + 8ull * i);
    say("PASS swapchain %ux%u, format %d, %u images", g->extent.width, g->extent.height, g->format, g->nimages);
    return 0;
}

static int make_sync(gpu *g)
{
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g->qf,
    };
    uint64_t out = galloc(64);
    EXPECT(VKR(G("vkCreateCommandPool", g->dev, gput(&pci, sizeof pci), 0, out)) == VK_SUCCESS, "vkCreateCommandPool");
    g->pool = g64(out);
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = (VkCommandPool)GP(g->pool),
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = g->nimages,
    };
    EXPECT(VKR(G("vkAllocateCommandBuffers", g->dev, gput(&cai, sizeof cai), out)) == VK_SUCCESS,
           "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    uint64_t gfci = gput(&fci, sizeof fci), gsci = gput(&sci, sizeof sci), h = galloc(8);
    for (uint32_t i = 0; i < g->nimages; i++) {
        g->cmd[i] = g64(out + 8ull * i);
        EXPECT(VKR(G("vkCreateFence", g->dev, gfci, 0, h)) == VK_SUCCESS, "vkCreateFence");
        g->fence[i] = g64(h);
        EXPECT(VKR(G("vkCreateSemaphore", g->dev, gsci, 0, h)) == VK_SUCCESS, "vkCreateSemaphore");
        g->acquired[i] = g64(h);
        EXPECT(VKR(G("vkCreateSemaphore", g->dev, gsci, 0, h)) == VK_SUCCESS, "vkCreateSemaphore");
        g->rendered[i] = g64(h);
    }
    uint64_t size = 4ull * g->extent.width * g->extent.height;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    EXPECT(VKR(G("vkCreateBuffer", g->dev, gput(&bci, sizeof bci), 0, h)) == VK_SUCCESS, "vkCreateBuffer");
    g->buf = g64(h);
    uint64_t greq = galloc(sizeof(VkMemoryRequirements));
    G("vkGetBufferMemoryRequirements", g->dev, g->buf, greq);
    VkMemoryRequirements req;
    memcpy(&req, g_mem + greq, sizeof req);
    uint64_t gmp = galloc(sizeof(VkPhysicalDeviceMemoryProperties));
    G("vkGetPhysicalDeviceMemoryProperties", g->pd, gmp);
    VkPhysicalDeviceMemoryProperties mp;
    memcpy(&mp, g_mem + gmp, sizeof mp);
    uint32_t want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            type = i;
    EXPECT(type != UINT32_MAX, "no host-visible memory type");
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = type };
    EXPECT(VKR(G("vkAllocateMemory", g->dev, gput(&ai, sizeof ai), 0, h)) == VK_SUCCESS, "vkAllocateMemory");
    g->bufmem = g64(h);
    EXPECT(VKR(G("vkBindBufferMemory", g->dev, g->buf, g->bufmem, 0)) == VK_SUCCESS, "vkBindBufferMemory");
    EXPECT(VKR(G("vkMapMemory", g->dev, g->bufmem, 0, VK_WHOLE_SIZE, 0, h)) == VK_SUCCESS, "vkMapMemory");
    g->bufmap = g64(h);
    return 0;
}

static VkClearColorValue color_of(uint32_t frame, uint32_t frames)
{
    if (frame + 1 == frames)
        return (VkClearColorValue){ .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } };
    float t = (float)(frame % 120) / 120.0f;
    return (VkClearColorValue){ .float32 = { t, 0.3f, 1.0f - t, 1.0f } };
}

static int frame(gpu *g, uint32_t n, uint32_t frames)
{
    uint32_t slot = n % g->nimages;
    uint64_t fence = gput(&g->fence[slot], 8);
    EXPECT(VKR(G("vkWaitForFences", g->dev, 1, fence, VK_TRUE, UINT64_MAX)) == VK_SUCCESS, "vkWaitForFences");
    G("vkResetFences", g->dev, 1, fence);
    uint64_t gidx = galloc(4);
    int32_t r = VKR(G("vkAcquireNextImageKHR", g->dev, g->swapchain, UINT64_MAX, g->acquired[slot], 0, gidx));
    EXPECT(r == VK_SUCCESS, "frame %u: vkAcquireNextImageKHR: %d", n, r);
    uint32_t idx = g32(gidx);
    uint64_t cb = g->cmd[slot], img = g->images[idx];
    G("vkResetCommandBuffer", cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    G("vkBeginCommandBuffer", cb, gput(&bi, sizeof bi));
    barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue color = color_of(n, frames);
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    G("vkCmdClearColorImage", cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, gput(&color, sizeof color), 1,
      gput(&range, sizeof range));
    VkImageLayout last = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (n + 1 == frames) {
        barrier(cb, img, last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { g->extent.width, g->extent.height, 1 },
        };
        G("vkCmdCopyImageToBuffer", cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->buf, 1, gput(&copy, sizeof copy));
        last = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    barrier(cb, img, last, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    G("vkEndCommandBuffer", cb);
    uint32_t stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = GP(gput(&g->acquired[slot], 8)),
        .pWaitDstStageMask = GP(gput(&stage, 4)),
        .commandBufferCount = 1,
        .pCommandBuffers = GP(gput(&cb, 8)),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = GP(gput(&g->rendered[idx], 8)),
    };
    r = VKR(G("vkQueueSubmit", g->queue, 1, gput(&si, sizeof si), g->fence[slot]));
    EXPECT(r == VK_SUCCESS, "frame %u: vkQueueSubmit: %d", n, r);
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = GP(gput(&g->rendered[idx], 8)),
        .swapchainCount = 1,
        .pSwapchains = GP(gput(&g->swapchain, 8)),
        .pImageIndices = GP(gput(&idx, 4)),
    };
    r = VKR(G("vkQueuePresentKHR", g->queue, gput(&pi, sizeof pi)));
    EXPECT(r == VK_SUCCESS, "frame %u: vkQueuePresentKHR: %d", n, r);
    return 0;
}

static int check_readback(gpu *g)
{
    EXPECT(VKR(G("vkQueueWaitIdle", g->queue)) == VK_SUCCESS, "vkQueueWaitIdle");
    uint32_t want = g->format == VK_FORMAT_B8G8R8A8_UNORM ? 0xFF4080BFu : 0xFFBF8040u, bad = 0;
    uint32_t px = g->extent.width * g->extent.height;
    for (uint32_t i = 0; i < px; i += 97) {
        uint32_t v = g32(g->bufmap + 4ull * i);
        for (int s = 0; s < 32; s += 8) {
            int d = (int)((v >> s) & 0xFF) - (int)((want >> s) & 0xFF);
            if (d > 1 || d < -1) {
                bad++;
                break;
            }
        }
    }
    EXPECT(!bad, "last frame read back %u bad samples (first 0x%08x, want 0x%08x)", bad, g32(g->bufmap), want);
    say("PASS last frame reads back 0x%08x", g32(g->bufmap));
    return 0;
}

static void teardown(gpu *g)
{
    G("vkDeviceWaitIdle", g->dev);
    for (uint32_t i = 0; i < g->nimages; i++) {
        G("vkDestroyFence", g->dev, g->fence[i], 0);
        G("vkDestroySemaphore", g->dev, g->acquired[i], 0);
        G("vkDestroySemaphore", g->dev, g->rendered[i], 0);
    }
    G("vkUnmapMemory", g->dev, g->bufmem);
    G("vkFreeMemory", g->dev, g->bufmem, 0);
    G("vkDestroyBuffer", g->dev, g->buf, 0);
    G("vkDestroyCommandPool", g->dev, g->pool, 0);
    G("vkDestroySwapchainKHR", g->dev, g->swapchain, 0);
    G("vkDestroyDevice", g->dev, 0);
    G("vkDestroySurfaceKHR", g_inst, g->surface, 0);
    G("vkDestroyInstance", g_inst, 0);
}

static int run(void)
{
    g_gipa = g64(GOT_GIPA);
    EXPECT(g_gipa && g_gipa != 0x1234, "the vkGetInstanceProcAddr import was not resolved (GOT 0x%llx)",
           (unsigned long long)g_gipa);
    say("PASS import vkGetInstanceProcAddr = 0x%llx", (unsigned long long)g_gipa);
    vi_layer_out vl;
    gpu g;
    memset(&g, 0, sizeof g);
    if (make_layer(&vl) || make_instance(&g, &vl) || pick_device(&g) || make_device(&g) || make_swapchain(&g) ||
        make_sync(&g))
        return -1;
    const char *e = getenv("RELIFT_DEMO_FRAMES");
    uint32_t frames = e ? (uint32_t)strtoul(e, NULL, 10) : 60, done = 0;
    if (!frames)
        frames = 60;
    uint64_t t0 = rl_clock_ns();
    for (uint32_t n = 0; n < frames; n++, done++)
        if (frame(&g, n, frames))
            break;
    uint64_t ms = (rl_clock_ns() - t0) / 1000000u;
    EXPECT(done == frames, "presented %u of %u frames", done, frames);
    say("PASS presented %u frames in %llu ms", done, (unsigned long long)ms);
    if (check_readback(&g))
        return -1;
    teardown(&g);
    EXPECT(rl_vk_live_handles() == 0, "%u guest handles still alive", rl_vk_live_handles());
    say("PASS teardown");
    return 0;
}

static int entry(rl_ctx *restrict c, uint8_t *mem)
{
    g_c = c;
    g_mem = mem;
    g_brk = 0;
    if (t_svc(c, SVC_SET_HEAP_SIZE, 0, HEAP_SIZE, 0, 0, 0, 0) == 0) {
        g_brk = c->x[1];
        g_heap_end = g_brk + HEAP_SIZE;
    }
    int rc = g_brk ? run() : -1;
    c->x[0] = c->x[1] = c->x[2] = 0;
    return rl_svc(c, mem, rc == 0 && !g_failed ? 0x07 : 0x26);
}

static const rl_fn_entry g_fns[] = { { BASE, 4, 0, entry } };

static const rl_unit g_unit = {
    .abi_version = RL_ABI_VERSION,
    .ctx_size = sizeof(struct rl_ctx),
    .fn_count = 1,
    .fns = g_fns,
    .name = "vk_present",
};

static const rl_unit *const g_units[] = { &g_unit };

static const rl_switch_module g_module = {
    .name = "main",
    .base = BASE,
    .image = g_image,
    .image_size = sizeof g_image,
    .text_off = 0,
    .text_size = 0x1000,
    .ro_off = 0x1000,
    .ro_size = 0x1000,
    .data_off = 0x2000,
    .data_size = 0x1000,
};

static const rl_switch_program g_program = {
    .modules = &g_module,
    .module_count = 1,
    .entry = BASE,
    .address_space_type = 3,
    .main_prio = 44,
    .main_stack_size = 0x100000,
    .program_id = 0x0100000000017E57ull,
    .units = g_units,
    .unit_count = 1,
};

static void put32(uint32_t off, uint32_t v) { memcpy(g_image + off, &v, 4); }
static void put64(uint32_t off, uint64_t v) { memcpy(g_image + off, &v, 8); }

/** @brief MOD0 at text+4, a dynamic section in data, one undefined symbol and its GLOB_DAT into a GOT slot. */
static void build_image(void)
{
    static const char strtab[] = "\0vkGetInstanceProcAddr\0";
    put32(4, 0x1000);
    memcpy(g_image + 0x1000, "MOD0", 4);
    put32(0x1004, 0x1000);
    put32(0x1100 + 24, 1);
    g_image[0x1100 + 24 + 4] = 0x12;
    memcpy(g_image + 0x1200, strtab, sizeof strtab);
    put64(0x1300, GOT_GIPA - BASE);
    put64(0x1308, (uint64_t)1 << 32 | 1025);
    put64(0x2800, 0x1234);
    const uint64_t dyn[][2] = { { 5, 0x1200 }, { 6, 0x1100 }, { 10, sizeof strtab }, { 11, 24 }, { 7, 0x1300 },
                                { 8, 24 }, { 0, 0 } };
    for (unsigned i = 0; i < sizeof dyn / sizeof dyn[0]; i++) {
        put64(0x2000 + 16 * i, dyn[i][0]);
        put64(0x2008 + 16 * i, dyn[i][1]);
    }
}

int main(int argc, char **argv)
{
    if (!rl_hostlib_vulkan_gipa() && !getenv("RELIFT_VK_REQUIRE")) {
        fprintf(stderr, "vk_present: no host Vulkan loader; skipped\n");
        return SKIP;
    }
    build_image();
    if (rl_switch_add_hook(rl_vk_switch_hook()) != 0)
        return 1;
    return rl_switch_main(&g_program, argc, argv);
}
