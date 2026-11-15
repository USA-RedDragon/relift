/**
 * @file test_vk.c
 * @brief The Vulkan forwarding layer driven the way lifted guest code drives
 * it: guest structs built in a synthetic guest memory image, arguments in
 * the guest context, thunks reached through vkGetInstanceProcAddr and the
 * dispatch table, run against the host's real Vulkan driver (lavapipe in
 * CI). The layer shows one physical device; RELIFT_VK_DEVICE picks it by a
 * part of its name. With no host Vulkan the tests are skipped, unless
 * RELIFT_VK_REQUIRE is set (CI sets it where lavapipe is installed).
 */
#include "a64.h"
#include "hostlib.h"
#include "runner.h"
#include "vi.h"
#include "vkf.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define THUNKS 0x08000000ull
#define HEAP 0x10000000ull
#define HEAP_SIZE (32ull << 20)
#define STACK 0x20000000ull
#define STACK_SIZE (1ull << 20)
#define MAPWIN 0x40000000ull
#define MAPWIN_SIZE (1ull << 30)
#define CALLBACK_FN 0x09000000ull

__attribute__((constructor(200))) static void skip_without_vulkan(void)
{
    if (rl_hostlib_vulkan_gipa() || getenv("RELIFT_VK_REQUIRE"))
        return;
    fprintf(stderr, "relift runtime tests: no host Vulkan loader; Vulkan forwarding tests skipped\n");
    _exit(0);
}

static const rl_guest_desc vk_guest = {
    .name = "vk-test",
    .page_size = 0x1000,
    .address_bits = 36,
    .tick_hz = 19200000,
};

static rl_vm g_vm;
static rl_ctx *g_c;
static uint8_t *g_mem;
static uint64_t g_brk;
static uint64_t g_inst;

#define GP(a) ((void *)(uintptr_t)(a))

static uint64_t galloc(uint64_t n)
{
    g_brk = (g_brk + 15) & ~15ull;
    uint64_t a = g_brk;
    g_brk += n;
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

static uint64_t gstrv(const char *const *s, unsigned n)
{
    uint64_t v[8];
    for (unsigned i = 0; i < n; i++)
        v[i] = gstr(s[i]);
    return gput(v, 8 * n);
}

static uint64_t g64(uint64_t a) { return rl_ld64(g_mem, a); }

static uint32_t g32(uint64_t a) { return rl_ld32(g_mem, a); }

static int g_rc;

/** @brief Calls the guest address fn with args the way a BLR from lifted code does; returns x0. */
static uint64_t gcall_addr(uint64_t fn, const uint64_t *args, unsigned n)
{
    uint64_t sp = STACK + STACK_SIZE - 4096;
    for (unsigned i = 0; i < n; i++) {
        if (i < 8)
            g_c->x[i] = args[i];
        else
            rl_st64(g_mem, sp + 8 * (i - 8), args[i]);
    }
    g_c->sp = sp;
    g_c->x[19] = 0x1919191919191919ull;
    g_rc = rl_indirect(g_c, fn)(g_c, g_mem);
    return g_c->x[0];
}

static uint64_t gipa(uint64_t inst, const char *name)
{
    uint64_t a[2] = { inst, gstr(name) };
    uint64_t sp = STACK + STACK_SIZE - 4096;
    g_c->x[0] = a[0];
    g_c->x[1] = a[1];
    g_c->sp = sp;
    rl_fn f = rl_vk_import("vkGetInstanceProcAddr");
    g_rc = f(g_c, g_mem);
    return g_c->x[0];
}

static uint64_t gcall(const char *name, const uint64_t *args, unsigned n)
{
    uint64_t fn = gipa(g_inst, name);
    if (!fn) {
        fprintf(stderr, "no guest address for %s\n", name);
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

static int boot(const rl_vk_surface_provider *surfaces)
{
    if (rl_core_init(&vk_guest, rl_a64_cpu()) != 0)
        return -1;
    if (rl_vm_init(&g_vm, 36, 0) != 0)
        return -1;
    rl_set_mem_base(g_vm.base);
    rl_fault_install(&g_vm);
    g_mem = g_vm.base;
    if (rl_vm_set(&g_vm, HEAP, HEAP_SIZE, RL_VM_RW, 1, RL_VM_FRESH) || rl_vm_set(&g_vm, STACK, STACK_SIZE, RL_VM_RW, 2, RL_VM_FRESH))
        return -1;
    g_brk = HEAP;
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return -1;
    rl_thread_bind(t);
    g_c = t->ctx;
    rl_vk_config cfg = {
        .thunk_base = THUNKS,
        .vm = &g_vm,
        .map_base = MAPWIN,
        .map_size = MAPWIN_SIZE,
        .map_tag = 3,
        .surfaces = surfaces,
    };
    if (rl_vk_init(&cfg) != 0) {
        fprintf(stderr, "no host Vulkan: install a Vulkan loader and driver (mesa lavapipe works headless)\n");
        return -1;
    }
    const rl_unit *u = rl_vk_unit();
    if (rl_units_register(&u, 1) != 0)
        return -1;
    rl_dispatch_view(g_c);
    return 0;
}

static uint64_t make_instance(const char *const *exts, unsigned n_exts, uint64_t pnext)
{
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = GP(gstr("relift-vk-test")),
        .applicationVersion = 1,
        .pEngineName = GP(gstr("synthetic guest")),
        .apiVersion = VKF_API_VERSION(1, 1),
    };
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = GP(pnext),
        .pApplicationInfo = GP(gput(&app, sizeof app)),
        .enabledExtensionCount = n_exts,
        .ppEnabledExtensionNames = n_exts ? GP(gstrv(exts, n_exts)) : NULL,
    };
    uint64_t out = galloc(8);
    g_inst = 0;
    int32_t r = VKR(G("vkCreateInstance", gput(&ci, sizeof ci), 0, out));
    if (r != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance: %d\n", r);
        return 0;
    }
    g_inst = g64(out);
    return g_inst;
}

static uint32_t physical_devices(uint64_t *out, uint32_t cap)
{
    uint64_t gn = galloc(4);
    if (VKR(G("vkEnumeratePhysicalDevices", g_inst, gn, 0)) != VK_SUCCESS)
        return 0;
    uint32_t n = g32(gn);
    if (n > cap)
        n = cap;
    rl_st32(g_mem, gn, n);
    uint64_t arr = galloc(8ull * n);
    int32_t r = VKR(G("vkEnumeratePhysicalDevices", g_inst, gn, arr));
    if (r != VK_SUCCESS && r != VK_INCOMPLETE)
        return 0;
    n = g32(gn);
    for (uint32_t i = 0; i < n; i++)
        out[i] = g64(arr + 8 * i);
    return n;
}

static bool device_wanted(uint64_t pd, char *name)
{
    uint64_t gp = galloc(sizeof(VkPhysicalDeviceProperties));
    G("vkGetPhysicalDeviceProperties", pd, gp);
    memcpy(name, g_mem + gp + offsetof(VkPhysicalDeviceProperties, deviceName), 256);
    const char *want = getenv(RL_VK_DEVICE_ENV);
    return !want || strstr(name, want);
}

static uint32_t queue_family(uint64_t pd, uint32_t flags)
{
    uint64_t gn = galloc(4);
    G("vkGetPhysicalDeviceQueueFamilyProperties", pd, gn, 0);
    uint32_t n = g32(gn);
    uint64_t arr = galloc(n * sizeof(VkQueueFamilyProperties));
    G("vkGetPhysicalDeviceQueueFamilyProperties", pd, gn, arr);
    for (uint32_t i = 0; i < g32(gn); i++)
        if ((g32(arr + i * sizeof(VkQueueFamilyProperties)) & flags) == flags)
            return i;
    return UINT32_MAX;
}

static uint64_t make_device(uint64_t pd, uint32_t qf, const char *const *exts, unsigned n_exts)
{
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qf,
        .queueCount = 1,
        .pQueuePriorities = GP(gput(&prio, 4)),
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = GP(gput(&qci, sizeof qci)),
        .enabledExtensionCount = n_exts,
        .ppEnabledExtensionNames = n_exts ? GP(gstrv(exts, n_exts)) : NULL,
    };
    uint64_t out = galloc(8);
    int32_t r = VKR(G("vkCreateDevice", pd, gput(&dci, sizeof dci), 0, out));
    if (r != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDevice: %d\n", r);
        return 0;
    }
    return g64(out);
}

static uint32_t memory_type(uint64_t pd, uint32_t bits, uint32_t want)
{
    uint64_t gm = galloc(sizeof(VkPhysicalDeviceMemoryProperties));
    G("vkGetPhysicalDeviceMemoryProperties", pd, gm);
    VkPhysicalDeviceMemoryProperties mp;
    memcpy(&mp, g_mem + gm, sizeof mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

TEST(vk_thunk_table)
{
    CHECK_EQ(boot(NULL), 0);
    uint64_t ci = rl_vk_guest_addr("vkCreateInstance");
    CHECK(ci >= THUNKS && ci < THUNKS + rl_vk_thunk_bytes());
    CHECK(rl_vk_import("vkGetInstanceProcAddr") != NULL);
    CHECK(rl_vk_import("vkNotACommand") == NULL);
    CHECK_EQ(gipa(0, "vkCreateInstance"), ci);
    CHECK_EQ(g_rc, RL_OK);
    CHECK_EQ(gipa(0, "vkCmdDraw"), 0);
    CHECK_EQ(gipa(0, "vkNotACommand"), 0);
    CHECK_EQ(rl_vk_guest_addr("vkGetPhysicalDeviceProperties2KHR"), rl_vk_guest_addr("vkGetPhysicalDeviceProperties2"));
    CHECK_EQ(rl_vk_guest_addr("vkMapMemory2KHR"), 0);
    CHECK(rl_dispatch_find(ci) != NULL);
}

TEST(vk_instance_and_physical_devices)
{
    CHECK_EQ(boot(NULL), 0);
    uint64_t gn = galloc(4);
    CHECK_EQ(VKR(G("vkEnumerateInstanceExtensionProperties", 0, gn, 0)), VK_SUCCESS);
    CHECK(g32(gn) <= vkg_instance_ext_count);
    uint64_t inst = make_instance(NULL, 0, 0);
    CHECK(inst != 0);
    CHECK_EQ(inst >> 56, 0x56);
    CHECK(vkf_obj_of(inst, VKG_KIND_VkInstance) != NULL);
    CHECK((uint64_t)(uintptr_t)vkf_obj_of(inst, VKG_KIND_VkInstance)->host != inst);
    CHECK(gipa(inst, "vkCmdDraw") != 0);
    CHECK_EQ(gipa(inst, "vkCreateViSurfaceNN"), 0);
    uint64_t pds[8];
    uint32_t n = physical_devices(pds, 8);
    CHECK(n > 0);
    uint64_t again[8];
    CHECK_EQ(physical_devices(again, 8), n);
    CHECK_EQ(again[0], pds[0]);
    for (uint32_t i = 0; i < n; i++) {
        char name[256];
        device_wanted(pds[i], name);
        CHECK(name[0] != 0);
        VkPhysicalDeviceVulkan12Properties v12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
        uint64_t g12 = gput(&v12, sizeof v12);
        VkPhysicalDeviceIDProperties idp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .pNext = GP(g12) };
        uint64_t gid = gput(&idp, sizeof idp);
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = GP(gid) };
        uint64_t gp2 = gput(&p2, sizeof p2);
        G("vkGetPhysicalDeviceProperties2KHR", pds[i], gp2);
        CHECK_EQ(g64(gp2 + 8), gid);
        CHECK_EQ(g64(gid + 8), g12);
        CHECK(!strcmp((const char *)g_mem + gp2 + 16 + offsetof(VkPhysicalDeviceProperties, deviceName), name));
        CHECK(g32(g12 + offsetof(VkPhysicalDeviceVulkan12Properties, driverID)) != 0);
        CHECK(g_mem[g12 + offsetof(VkPhysicalDeviceVulkan12Properties, driverName)] != 0);
        uint8_t zero[VK_UUID_SIZE] = { 0 };
        CHECK(memcmp(g_mem + gid + offsetof(VkPhysicalDeviceIDProperties, deviceUUID), zero, VK_UUID_SIZE) != 0);
        fprintf(stderr, "  device %u: %s, driver %s\n", i, name, (const char *)g_mem + g12 + offsetof(VkPhysicalDeviceVulkan12Properties, driverName));
        uint64_t en = galloc(4);
        CHECK_EQ(VKR(G("vkEnumerateDeviceExtensionProperties", pds[i], 0, en, 0)), VK_SUCCESS);
        CHECK(g32(en) <= vkg_device_ext_count);
    }
    G("vkDestroyInstance", inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
}

/** @brief Records an image-layout barrier through vkCmdPipelineBarrier (ten arguments: two go on the guest stack). */
static void barrier(uint64_t cb, uint64_t image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst)
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
    G("vkCmdPipelineBarrier", cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1,
      gput(&b, sizeof b));
}

#define W 64
#define H 64

typedef struct mapped_buf {
    uint64_t buf;
    uint64_t mem;
    uint64_t ptr;
} mapped_buf;

static int make_mapped(uint64_t pd, uint64_t dev, uint64_t size, uint32_t usage, mapped_buf *out)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    uint64_t gb = galloc(8);
    if (VKR(G("vkCreateBuffer", dev, gput(&bci, sizeof bci), 0, gb)) != VK_SUCCESS)
        return -1;
    out->buf = g64(gb);
    uint64_t greq = galloc(sizeof(VkMemoryRequirements));
    G("vkGetBufferMemoryRequirements", dev, out->buf, greq);
    VkMemoryRequirements req;
    memcpy(&req, g_mem + greq, sizeof req);
    uint32_t t = memory_type(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t == UINT32_MAX)
        return -1;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = t };
    uint64_t gm = galloc(8);
    if (VKR(G("vkAllocateMemory", dev, gput(&mai, sizeof mai), 0, gm)) != VK_SUCCESS)
        return -1;
    out->mem = g64(gm);
    if (VKR(G("vkBindBufferMemory", dev, out->buf, out->mem, 0)) != VK_SUCCESS)
        return -1;
    uint64_t gptr = galloc(8);
    if (VKR(G("vkMapMemory", dev, out->mem, 0, VK_WHOLE_SIZE, 0, gptr)) != VK_SUCCESS)
        return -1;
    out->ptr = g64(gptr);
    return 0;
}

/** @brief Clears an image on the GPU, copies it and a guest-written buffer into a mapped buffer, and checks both through guest memory. */
static void clear_and_read_back(int expect_mode)
{
    CHECK_EQ(boot(NULL), 0);
    CHECK(make_instance(NULL, 0, 0) != 0);
    uint64_t pds[8];
    uint32_t n = physical_devices(pds, 8);
    CHECK(n > 0);
    unsigned ran = 0;
    for (uint32_t i = 0; i < n; i++) {
        char name[256];
        if (!device_wanted(pds[i], name))
            continue;
        uint64_t pd = pds[i];
        uint32_t qf = queue_family(pd, VK_QUEUE_GRAPHICS_BIT);
        CHECK(qf != UINT32_MAX);
        uint64_t dev = make_device(pd, qf, NULL, 0);
        CHECK(dev != 0);
        int mode = rl_vk_device_map_mode(dev);
        fprintf(stderr, "  %s: maps by %s\n", name, mode == RL_VK_MAP_PLACED ? "placed" : mode == RL_VK_MAP_IMPORT ? "import" : "shadow");
        if (expect_mode != RL_VK_MAP_AUTO && mode != expect_mode) {
            fprintf(stderr, "  %s: cannot map by the forced mode; skipped\n", name);
            G("vkDestroyDevice", dev, 0);
            continue;
        }
        uint64_t gq = galloc(8);
        G("vkGetDeviceQueue", dev, qf, 0, gq);
        uint64_t queue = g64(gq);
        CHECK(vkf_obj_of(queue, VKG_KIND_VkQueue) != NULL);
        G("vkGetDeviceQueue", dev, qf, 0, gq);
        CHECK_EQ(g64(gq), queue);

        mapped_buf rb, up;
        CHECK_EQ(make_mapped(pd, dev, 2 * W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb), 0);
        CHECK_EQ(make_mapped(pd, dev, W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &up), 0);
        CHECK(rb.ptr >= MAPWIN && rb.ptr + 2 * W * H * 4 <= MAPWIN + MAPWIN_SIZE);
        CHECK(up.ptr >= MAPWIN && up.ptr + W * H * 4 <= MAPWIN + MAPWIN_SIZE);
        memset(g_mem + rb.ptr, 0xEE, 2 * W * H * 4);
        for (uint32_t p = 0; p < W * H; p++)
            rl_st32(g_mem, up.ptr + 4 * p, 0xA5000000u | p);

        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { W, H, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        uint64_t gi = galloc(8);
        CHECK_EQ(VKR(G("vkCreateImage", dev, gput(&ici, sizeof ici), 0, gi)), VK_SUCCESS);
        uint64_t image = g64(gi);
        VkMemoryRequirements2 mr2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
        uint64_t gmr = gput(&mr2, sizeof mr2);
        VkImageMemoryRequirementsInfo2 iri = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = (VkImage)GP(image) };
        G("vkGetImageMemoryRequirements2", dev, gput(&iri, sizeof iri), gmr);
        VkMemoryRequirements req;
        memcpy(&req, g_mem + gmr + offsetof(VkMemoryRequirements2, memoryRequirements), sizeof req);
        CHECK(req.size >= W * H * 4);
        uint32_t t = memory_type(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (t == UINT32_MAX)
            t = memory_type(pd, req.memoryTypeBits, 0);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = t };
        uint64_t gim = galloc(8);
        CHECK_EQ(VKR(G("vkAllocateMemory", dev, gput(&mai, sizeof mai), 0, gim)), VK_SUCCESS);
        CHECK_EQ(VKR(G("vkBindImageMemory", dev, image, g64(gim), 0)), VK_SUCCESS);

        VkCommandPoolCreateInfo pci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = qf,
        };
        uint64_t gpool = galloc(8);
        CHECK_EQ(VKR(G("vkCreateCommandPool", dev, gput(&pci, sizeof pci), 0, gpool)), VK_SUCCESS);
        uint64_t pool = g64(gpool);
        VkCommandBufferAllocateInfo cai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = (VkCommandPool)GP(pool),
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        uint64_t gcb = galloc(8);
        CHECK_EQ(VKR(G("vkAllocateCommandBuffers", dev, gput(&cai, sizeof cai), gcb)), VK_SUCCESS);
        uint64_t cb = g64(gcb);
        CHECK(vkf_obj_of(cb, VKG_KIND_VkCommandBuffer) != NULL);

        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        CHECK_EQ(VKR(G("vkBeginCommandBuffer", cb, gput(&bi, sizeof bi))), VK_SUCCESS);
        barrier(cb, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue color = { .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } };
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        G("vkCmdClearColorImage", cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, gput(&color, sizeof color), 1, gput(&range, sizeof range));
        barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy bic = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { W, H, 1 },
        };
        G("vkCmdCopyImageToBuffer", cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, gput(&bic, sizeof bic));
        VkBufferCopy bc = { .srcOffset = 0, .dstOffset = W * H * 4, .size = W * H * 4 };
        G("vkCmdCopyBuffer", cb, up.buf, rb.buf, 1, gput(&bc, sizeof bc));
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        G("vkCmdPipelineBarrier", cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, gput(&mb, sizeof mb), 0, 0, 0, 0);
        CHECK_EQ(VKR(G("vkEndCommandBuffer", cb)), VK_SUCCESS);

        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        uint64_t gf = galloc(8);
        CHECK_EQ(VKR(G("vkCreateFence", dev, gput(&fci, sizeof fci), 0, gf)), VK_SUCCESS);
        uint64_t fence = g64(gf);
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = GP(gput(&cb, 8)),
        };
        CHECK_EQ(VKR(G("vkQueueSubmit", queue, 1, gput(&si, sizeof si), fence)), VK_SUCCESS);
        CHECK_EQ(VKR(G("vkWaitForFences", dev, 1, gput(&fence, 8), VK_TRUE, UINT64_MAX)), VK_SUCCESS);

        unsigned bad = 0;
        for (uint32_t p = 0; p < W * H; p++) {
            uint32_t v = rl_ld32(g_mem, rb.ptr + 4 * p);
            uint8_t r = v & 0xFF, gch = (v >> 8) & 0xFF, b = (v >> 16) & 0xFF, a = v >> 24;
            if (r < 63 || r > 65 || gch < 127 || gch > 129 || b < 190 || b > 192 || a != 255)
                bad++;
            if (rl_ld32(g_mem, rb.ptr + W * H * 4 + 4 * p) != (0xA5000000u | p))
                bad++;
        }
        fprintf(stderr, "  %s: first pixel 0x%08x, copied word 0x%08x, %u bad\n", name, rl_ld32(g_mem, rb.ptr),
                rl_ld32(g_mem, rb.ptr + W * H * 4 + 4), bad);
        CHECK_EQ(bad, 0);

        G("vkUnmapMemory", dev, rb.mem);
        uint64_t gptr = galloc(8);
        CHECK_EQ(VKR(G("vkMapMemory", dev, rb.mem, 16, VK_WHOLE_SIZE, 0, gptr)), VK_SUCCESS);
        CHECK_EQ(rl_ld32(g_mem, g64(gptr) + W * H * 4 - 16 + 4), 0xA5000001u);

        G("vkDestroyFence", dev, fence, 0);
        G("vkFreeCommandBuffers", dev, pool, 1, gcb);
        CHECK(vkf_obj_of(cb, VKG_KIND_VkCommandBuffer) == NULL);
        G("vkDestroyCommandPool", dev, pool, 0);
        G("vkDestroyImage", dev, image, 0);
        G("vkFreeMemory", dev, g64(gim), 0);
        G("vkDestroyBuffer", dev, rb.buf, 0);
        G("vkFreeMemory", dev, rb.mem, 0);
        G("vkDestroyBuffer", dev, up.buf, 0);
        G("vkFreeMemory", dev, up.mem, 0);
        G("vkDestroyDevice", dev, 0);
        CHECK(vkf_obj_of(dev, VKG_KIND_VkDevice) == NULL);
        CHECK(vkf_obj_of(queue, VKG_KIND_VkQueue) == NULL);
        ran++;
    }
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
    CHECK(ran > 0 || expect_mode != RL_VK_MAP_AUTO);
}

TEST(vk_clear_and_read_back)
{
    clear_and_read_back(RL_VK_MAP_AUTO);
}

TEST(vk_clear_and_read_back_placed)
{
    setenv(RL_VK_MAP_ENV, "placed", 1);
    clear_and_read_back(RL_VK_MAP_PLACED);
}

TEST(vk_clear_and_read_back_import)
{
    setenv(RL_VK_MAP_ENV, "import", 1);
    clear_and_read_back(RL_VK_MAP_IMPORT);
}

TEST(vk_clear_and_read_back_shadow)
{
    setenv(RL_VK_MAP_ENV, "shadow", 1);
    clear_and_read_back(RL_VK_MAP_SHADOW);
}

TEST(vk_mapped_memory_is_guest_memory)
{
    setenv(RL_VK_MAP_ENV, "shadow", 1);
    CHECK_EQ(boot(NULL), 0);
    CHECK(make_instance(NULL, 0, 0) != 0);
    uint64_t pds[8];
    CHECK(physical_devices(pds, 8) > 0);
    uint32_t qf = queue_family(pds[0], VK_QUEUE_TRANSFER_BIT);
    if (qf == UINT32_MAX)
        qf = queue_family(pds[0], VK_QUEUE_GRAPHICS_BIT);
    uint64_t dev = make_device(pds[0], qf, NULL, 0);
    CHECK(dev != 0);
    mapped_buf b;
    CHECK_EQ(make_mapped(pds[0], dev, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b), 0);
    t_probe p = t_write(b.ptr, 7);
    CHECK(!p.faulted);
    G("vkUnmapMemory", dev, b.mem);
    p = t_write(b.ptr, 7);
    CHECK(p.faulted);
    CHECK_EQ(p.addr, b.ptr);
    G("vkDestroyBuffer", dev, b.buf, 0);
    G("vkFreeMemory", dev, b.mem, 0);
    G("vkDestroyDevice", dev, 0);
    G("vkDestroyInstance", g_inst, 0);
}

/** @brief In import mode a dedicated buffer allocation is still backed by guest pages: the host sees guest writes with no sync. */
TEST(vk_import_backs_dedicated_buffer_memory)
{
    setenv(RL_VK_MAP_ENV, "import", 1);
    CHECK_EQ(boot(NULL), 0);
    CHECK(make_instance(NULL, 0, 0) != 0);
    uint64_t pds[8];
    CHECK(physical_devices(pds, 8) > 0);
    uint32_t qf = queue_family(pds[0], VK_QUEUE_GRAPHICS_BIT);
    uint64_t dev = make_device(pds[0], qf, NULL, 0);
    CHECK(dev != 0);
    if (rl_vk_device_map_mode(dev) != RL_VK_MAP_IMPORT) {
        fprintf(stderr, "  skipped: the device does not import host pointers\n");
        G("vkDestroyDevice", dev, 0);
        G("vkDestroyInstance", g_inst, 0);
        return;
    }
    enum { SIZE = 1 << 16 };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = SIZE, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT };
    uint64_t gb = galloc(8);
    CHECK_EQ(VKR(G("vkCreateBuffer", dev, gput(&bci, sizeof bci), 0, gb)), VK_SUCCESS);
    uint64_t buf = g64(gb);
    uint64_t greq = galloc(sizeof(VkMemoryRequirements));
    G("vkGetBufferMemoryRequirements", dev, buf, greq);
    VkMemoryRequirements req;
    memcpy(&req, g_mem + greq, sizeof req);
    uint32_t t = memory_type(pds[0], req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(t != UINT32_MAX);
    VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = (VkBuffer)(uintptr_t)buf };
    uint64_t gded = gput(&ded, sizeof ded);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = GP(gded), .allocationSize = req.size, .memoryTypeIndex = t };
    uint64_t gm = galloc(8);
    CHECK_EQ(VKR(G("vkAllocateMemory", dev, gput(&mai, sizeof mai), 0, gm)), VK_SUCCESS);
    uint64_t mem = g64(gm);
    CHECK_EQ(VKR(G("vkBindBufferMemory", dev, buf, mem, 0)), VK_SUCCESS);
    uint64_t gptr = galloc(8);
    CHECK_EQ(VKR(G("vkMapMemory", dev, mem, 0, VK_WHOLE_SIZE, 0, gptr)), VK_SUCCESS);
    uint64_t ptr = g64(gptr);
    memset(g_mem + ptr, 0x5a, SIZE);

    vkf_obj *o = vkf_obj_of(dev, VKG_KIND_VkDevice);
    CHECK(o != NULL);
    void *host = NULL;
    CHECK_EQ(VKF_FN(o->fns, vkMapMemory)(o->host, (VkDeviceMemory)(uintptr_t)mem, 0, VK_WHOLE_SIZE, 0, &host), VK_SUCCESS);
    CHECK(host == g_mem + ptr);
    CHECK_EQ(((uint8_t *)host)[SIZE - 1], 0x5a);
    VKF_FN(o->fns, vkUnmapMemory)(o->host, (VkDeviceMemory)(uintptr_t)mem);

    G("vkUnmapMemory", dev, mem);
    G("vkDestroyBuffer", dev, buf, 0);
    G("vkFreeMemory", dev, mem, 0);
    G("vkDestroyDevice", dev, 0);
    G("vkDestroyInstance", g_inst, 0);
}

/**
 * @brief A shadow allocation keeps the guest's writes made while a submit is in
 * flight: the device fills one half, the guest rewrites the other half between
 * the submit and the fence check, and both survive the wait and a second submit.
 */
TEST(vk_shadow_keeps_guest_writes_across_waits)
{
    setenv(RL_VK_MAP_ENV, "shadow", 1);
    CHECK_EQ(boot(NULL), 0);
    CHECK(make_instance(NULL, 0, 0) != 0);
    uint64_t pds[8];
    CHECK(physical_devices(pds, 8) > 0);
    uint64_t pd = pds[0];
    uint32_t qf = queue_family(pd, VK_QUEUE_GRAPHICS_BIT);
    CHECK(qf != UINT32_MAX);
    uint64_t dev = make_device(pd, qf, NULL, 0);
    CHECK(dev != 0);
    CHECK_EQ(rl_vk_device_map_mode(dev), RL_VK_MAP_SHADOW);
    uint64_t gq = galloc(8);
    G("vkGetDeviceQueue", dev, qf, 0, gq);
    uint64_t queue = g64(gq);

    enum { HALF = 4096 };
    mapped_buf b;
    CHECK_EQ(make_mapped(pd, dev, 2 * HALF, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b), 0);
    memset(g_mem + b.ptr, 0x11, 2 * HALF);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    uint64_t gpool = galloc(8);
    CHECK_EQ(VKR(G("vkCreateCommandPool", dev, gput(&pci, sizeof pci), 0, gpool)), VK_SUCCESS);
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = (VkCommandPool)GP(g64(gpool)),
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 2,
    };
    uint64_t gcb = galloc(16);
    CHECK_EQ(VKR(G("vkAllocateCommandBuffers", dev, gput(&cai, sizeof cai), gcb)), VK_SUCCESS);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    for (unsigned i = 0; i < 2; i++) {
        uint64_t cb = g64(gcb + 8 * i);
        CHECK_EQ(VKR(G("vkBeginCommandBuffer", cb, gput(&bi, sizeof bi))), VK_SUCCESS);
        if (i == 0)
            G("vkCmdFillBuffer", cb, b.buf, 0, HALF, 0x22222222u);
        G("vkCmdPipelineBarrier", cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, gput(&mb, sizeof mb), 0, 0, 0, 0);
        CHECK_EQ(VKR(G("vkEndCommandBuffer", cb)), VK_SUCCESS);
    }
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    uint64_t gf = galloc(8);
    CHECK_EQ(VKR(G("vkCreateFence", dev, gput(&fci, sizeof fci), 0, gf)), VK_SUCCESS);
    uint64_t fence = g64(gf);
    uint64_t gfence = gput(&fence, 8);

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = GP(gcb) };
    CHECK_EQ(VKR(G("vkQueueSubmit", queue, 1, gput(&si, sizeof si), fence)), VK_SUCCESS);
    memset(g_mem + b.ptr + HALF, 0x33, HALF);
    G("vkGetFenceStatus", dev, fence);
    CHECK_EQ(VKR(G("vkWaitForFences", dev, 1, gfence, VK_TRUE, UINT64_MAX)), VK_SUCCESS);
    memset(g_mem + b.ptr + HALF, 0x44, 16);
    CHECK_EQ(VKR(G("vkResetFences", dev, 1, gfence)), VK_SUCCESS);
    si.pCommandBuffers = GP(gcb + 8);
    CHECK_EQ(VKR(G("vkQueueSubmit", queue, 1, gput(&si, sizeof si), fence)), VK_SUCCESS);
    CHECK_EQ(VKR(G("vkWaitForFences", dev, 1, gfence, VK_TRUE, UINT64_MAX)), VK_SUCCESS);

    unsigned device_bad = 0, guest_bad = 0;
    for (uint32_t i = 0; i < HALF; i++) {
        if (g_mem[b.ptr + i] != 0x22)
            device_bad++;
        if (g_mem[b.ptr + HALF + i] != (i < 16 ? 0x44 : 0x33))
            guest_bad++;
    }
    fprintf(stderr, "  shadow: %u device bytes and %u guest bytes wrong\n", device_bad, guest_bad);
    CHECK_EQ(device_bad, 0);
    CHECK_EQ(guest_bad, 0);

    G("vkDestroyFence", dev, fence, 0);
    G("vkDestroyCommandPool", dev, g64(gpool), 0);
    G("vkDestroyBuffer", dev, b.buf, 0);
    G("vkFreeMemory", dev, b.mem, 0);
    G("vkDestroyDevice", dev, 0);
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
}

static uint64_t g_cb_calls, g_cb_user, g_cb_sev, g_cb_x19;
static char g_cb_msg[256];

static int guest_messenger(rl_ctx *restrict c, uint8_t *mem)
{
    g_cb_calls++;
    g_cb_sev = c->x[0];
    g_cb_user = c->x[3];
    g_cb_x19 = c->x[19];
    uint64_t data = c->x[2];
    uint64_t msg = rl_ld64(mem, data + offsetof(VkDebugUtilsMessengerCallbackDataEXT, pMessage));
    snprintf(g_cb_msg, sizeof g_cb_msg, "%s", (const char *)mem + msg);
    c->x[19] = 0xDEAD;
    c->x[0] = VK_FALSE;
    return RL_OK;
}

static const rl_fn_entry cb_entries[] = { { CALLBACK_FN, 4, 0, guest_messenger } };
static const rl_unit cb_unit = {
    .abi_version = RL_ABI_VERSION,
    .ctx_size = sizeof(rl_ctx),
    .fn_count = 1,
    .fns = cb_entries,
    .name = "test-callbacks",
};

TEST(vk_debug_messenger_calls_guest_code)
{
    CHECK_EQ(boot(NULL), 0);
    const rl_unit *u = &cb_unit;
    CHECK_EQ(rl_units_register(&u, 1), 0);
    rl_dispatch_view(g_c);
    const char *exts[] = { "VK_EXT_debug_utils" };
    CHECK(make_instance(exts, 1, 0) != 0);
    VkDebugUtilsMessengerCreateInfoEXT mci = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
        .pfnUserCallback = (PFN_vkDebugUtilsMessengerCallbackEXT)GP(CALLBACK_FN),
        .pUserData = GP(0x1234567),
    };
    uint64_t gm = galloc(8);
    CHECK_EQ(VKR(G("vkCreateDebugUtilsMessengerEXT", g_inst, gput(&mci, sizeof mci), 0, gm)), VK_SUCCESS);
    VkDebugUtilsMessengerCallbackDataEXT d = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT,
        .pMessageIdName = GP(gstr("relift")),
        .pMessage = GP(gstr("hello from the synthetic guest")),
    };
    uint64_t before = g_cb_calls;
    G("vkSubmitDebugUtilsMessageEXT", g_inst, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
      gput(&d, sizeof d));
    CHECK_EQ(g_rc, RL_OK);
    CHECK_EQ(g_cb_calls, before + 1);
    CHECK_EQ(g_cb_sev, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT);
    CHECK_EQ(g_cb_user, 0x1234567);
    CHECK(!strcmp(g_cb_msg, "hello from the synthetic guest"));
    CHECK_EQ(g_cb_x19, 0x1919191919191919ull);
    CHECK_EQ(g_c->x[19], 0x1919191919191919ull);
    G("vkDestroyDebugUtilsMessengerEXT", g_inst, g64(gm), 0);
    G("vkSubmitDebugUtilsMessageEXT", g_inst, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
      gput(&d, sizeof d));
    CHECK_EQ(g_cb_calls, before + 1);
    G("vkDestroyInstance", g_inst, 0);
}

static uint32_t g_host_w = 64, g_host_h = 64;
static uint64_t g_host_gen;

static int32_t resizable_create(void *user, void *host_instance, void *gipa, uint64_t guest_window, uint64_t *surface)
{
    (void)user;
    const rl_vk_surface_provider *h = rl_vk_headless_surfaces();
    return h->create(h->user, host_instance, gipa, guest_window, surface);
}

static void resizable_extent(void *user, uint64_t surface, uint32_t guest[2], uint32_t host[2], uint64_t *generation)
{
    (void)user;
    (void)surface;
    guest[0] = 64;
    guest[1] = 64;
    host[0] = g_host_w;
    host[1] = g_host_h;
    *generation = g_host_gen;
}

static rl_vk_surface_provider g_resizable = { .create = resizable_create, .extent = resizable_extent };

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

TEST(vk_vi_surface_presents_headless)
{
    g_resizable.instance_extensions = rl_vk_headless_surfaces()->instance_extensions;
    g_resizable.presents = rl_vk_headless_surfaces()->presents;
    g_host_w = g_host_h = 64;
    g_host_gen = 0;
    CHECK_EQ(boot(&g_resizable), 0);
    uint64_t gn = galloc(4);
    CHECK_EQ(VKR(G("vkEnumerateInstanceExtensionProperties", 0, gn, 0)), VK_SUCCESS);
    uint32_t n = g32(gn);
    uint64_t props = galloc(n * sizeof(VkExtensionProperties));
    CHECK_EQ(VKR(G("vkEnumerateInstanceExtensionProperties", 0, gn, props)), VK_SUCCESS);
    bool vi = false;
    for (uint32_t i = 0; i < n; i++)
        vi = vi || !strcmp((const char *)g_mem + props + i * sizeof(VkExtensionProperties), "VK_NN_vi_surface");
    CHECK(vi);
    const char *exts[] = { "VK_KHR_surface", "VK_NN_vi_surface" };
    CHECK(make_instance(exts, 2, 0) != 0);
    CHECK(gipa(g_inst, "vkCreateViSurfaceNN") != 0);
    VkViSurfaceCreateInfoNN sci = { .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, .window = GP(galloc(64)) };
    uint64_t gs = galloc(8);
    CHECK_EQ(VKR(G("vkCreateViSurfaceNN", g_inst, gput(&sci, sizeof sci), 0, gs)), VK_SUCCESS);
    uint64_t surface = g64(gs);
    CHECK(surface != 0);
    uint64_t pds[8];
    uint32_t np = physical_devices(pds, 8);
    unsigned presented = 0;
    for (uint32_t i = 0; i < np && !presented; i++) {
        char name[256];
        if (!device_wanted(pds[i], name))
            continue;
        uint32_t qf = queue_family(pds[i], VK_QUEUE_GRAPHICS_BIT);
        uint64_t gsup = galloc(4);
        if (qf == UINT32_MAX || VKR(G("vkGetPhysicalDeviceSurfaceSupportKHR", pds[i], qf, surface, gsup)) != VK_SUCCESS || !g32(gsup))
            continue;
        uint64_t gcaps = galloc(sizeof(VkSurfaceCapabilitiesKHR));
        CHECK_EQ(VKR(G("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", pds[i], surface, gcaps)), VK_SUCCESS);
        VkSurfaceCapabilitiesKHR caps;
        memcpy(&caps, g_mem + gcaps, sizeof caps);
        uint64_t gfn = galloc(4);
        CHECK_EQ(VKR(G("vkGetPhysicalDeviceSurfaceFormatsKHR", pds[i], surface, gfn, 0)), VK_SUCCESS);
        CHECK(g32(gfn) > 0);
        uint64_t gfmts = galloc(g32(gfn) * sizeof(VkSurfaceFormatKHR));
        CHECK(VKR(G("vkGetPhysicalDeviceSurfaceFormatsKHR", pds[i], surface, gfn, gfmts)) >= 0);
        VkSurfaceFormatKHR fmt;
        memcpy(&fmt, g_mem + gfmts, sizeof fmt);
        CHECK_EQ(g32(gfn), 2);
        CHECK_EQ(fmt.format, VK_FORMAT_R8G8B8A8_SRGB);
        CHECK_EQ(g32(gfmts + 8), VK_FORMAT_R8G8B8A8_UNORM);
        rl_st32(g_mem, gfn, 1);
        CHECK_EQ(VKR(G("vkGetPhysicalDeviceSurfaceFormatsKHR", pds[i], surface, gfn, gfmts)), VK_INCOMPLETE);
        CHECK_EQ(g32(gfn), 1);
        uint64_t gmn = galloc(4), gmodes = galloc(16);
        CHECK_EQ(VKR(G("vkGetPhysicalDeviceSurfacePresentModesKHR", pds[i], surface, gmn, 0)), VK_SUCCESS);
        CHECK_EQ(g32(gmn), 1);
        rl_st32(g_mem, gmn, 4);
        CHECK_EQ(VKR(G("vkGetPhysicalDeviceSurfacePresentModesKHR", pds[i], surface, gmn, gmodes)), VK_SUCCESS);
        CHECK_EQ(g32(gmn), 1);
        CHECK_EQ(g32(gmodes), VK_PRESENT_MODE_FIFO_KHR);
        rl_st32(g_mem, gmn, 0);
        CHECK_EQ(VKR(G("vkGetPhysicalDeviceSurfacePresentModesKHR", pds[i], surface, gmn, gmodes)), VK_INCOMPLETE);
        const char *dexts[] = { "VK_KHR_swapchain" };
        uint64_t dev = make_device(pds[i], qf, dexts, 1);
        CHECK(dev != 0);
        VkSwapchainCreateInfoKHR swci = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = (VkSurfaceKHR)GP(surface),
            .minImageCount = caps.minImageCount,
            .imageFormat = fmt.format,
            .imageColorSpace = fmt.colorSpace,
            .imageExtent = { 64, 64 },
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_FIFO_KHR,
            .clipped = VK_TRUE,
        };
        uint64_t gsw = galloc(8);
        CHECK_EQ(VKR(G("vkCreateSwapchainKHR", dev, gput(&swci, sizeof swci), 0, gsw)), VK_SUCCESS);
        uint64_t sw = g64(gsw);
        uint64_t gin = galloc(4);
        CHECK_EQ(VKR(G("vkGetSwapchainImagesKHR", dev, sw, gin, 0)), VK_SUCCESS);
        CHECK(g32(gin) >= 1);
        uint64_t gimgs = galloc(8ull * g32(gin));
        CHECK_EQ(VKR(G("vkGetSwapchainImagesKHR", dev, sw, gin, gimgs)), VK_SUCCESS);
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        uint64_t gf = galloc(8);
        CHECK_EQ(VKR(G("vkCreateFence", dev, gput(&fci, sizeof fci), 0, gf)), VK_SUCCESS);
        uint64_t fence = g64(gf);
        uint64_t gidx = galloc(4);
        int32_t ar = VKR(G("vkAcquireNextImageKHR", dev, sw, UINT64_MAX, 0, fence, gidx));
        CHECK(ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR);
        CHECK_EQ(VKR(G("vkWaitForFences", dev, 1, gput(&fence, 8), VK_TRUE, UINT64_MAX)), VK_SUCCESS);
        uint32_t idx = g32(gidx);
        uint64_t image = g64(gimgs + 8ull * idx);
        uint64_t gq = galloc(8);
        G("vkGetDeviceQueue", dev, qf, 0, gq);
        uint64_t queue = g64(gq);
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
        uint64_t gpool = galloc(8);
        CHECK_EQ(VKR(G("vkCreateCommandPool", dev, gput(&pci, sizeof pci), 0, gpool)), VK_SUCCESS);
        VkCommandBufferAllocateInfo cai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = (VkCommandPool)GP(g64(gpool)),
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        uint64_t gcb = galloc(8);
        CHECK_EQ(VKR(G("vkAllocateCommandBuffers", dev, gput(&cai, sizeof cai), gcb)), VK_SUCCESS);
        uint64_t cb = g64(gcb);
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CHECK_EQ(VKR(G("vkBeginCommandBuffer", cb, gput(&bi, sizeof bi))), VK_SUCCESS);
        barrier(cb, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue color = { .float32 = { 0.0f, 0.5f, 1.0f, 1.0f } };
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        G("vkCmdClearColorImage", cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, gput(&color, sizeof color), 1, gput(&range, sizeof range));
        barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        CHECK_EQ(VKR(G("vkEndCommandBuffer", cb)), VK_SUCCESS);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = GP(gput(&cb, 8)) };
        CHECK_EQ(VKR(G("vkQueueSubmit", queue, 1, gput(&si, sizeof si), 0)), VK_SUCCESS);
        CHECK_EQ(VKR(G("vkQueueWaitIdle", queue)), VK_SUCCESS);
        uint64_t gres = galloc(4);
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1,
            .pSwapchains = GP(gput(&sw, 8)),
            .pImageIndices = GP(gput(&idx, 4)),
            .pResults = GP(gres),
        };
        int32_t pr = VKR(G("vkQueuePresentKHR", queue, gput(&pi, sizeof pi)));
        CHECK(pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR);
        CHECK(VKR(g32(gres)) == VK_SUCCESS || VKR(g32(gres)) == VK_SUBOPTIMAL_KHR);
        fprintf(stderr, "  %s: presented image %u of %u (%dx%d swapchain, format %d)\n", name, idx, g32(gin), 64, 64, fmt.format);
        uint32_t hw = 0, hh = 0, made = 0;
        CHECK(rl_vk_swapchain_host(sw, &hw, &hh, &made));
        CHECK_EQ(hw, 64);
        CHECK_EQ(hh, 64);
        CHECK_EQ(made, 1);
        g_host_w = 200;
        g_host_h = 90;
        g_host_gen++;
        CHECK_EQ(VKR(G("vkWaitForFences", dev, 1, gput(&fence, 8), VK_TRUE, UINT64_MAX)), VK_SUCCESS);
        CHECK_EQ(VKR(G("vkResetFences", dev, 1, gput(&fence, 8))), VK_SUCCESS);
        CHECK_EQ(VKR(G("vkAcquireNextImageKHR", dev, sw, UINT64_MAX, 0, fence, gidx)), VK_SUCCESS);
        CHECK_EQ(VKR(G("vkWaitForFences", dev, 1, gput(&fence, 8), VK_TRUE, UINT64_MAX)), VK_SUCCESS);
        CHECK(rl_vk_swapchain_host(sw, &hw, &hh, &made));
        CHECK_EQ(hw, 200);
        CHECK_EQ(hh, 90);
        CHECK_EQ(made, 2);
        idx = g32(gidx);
        image = g64(gimgs + 8ull * idx);
        CHECK_EQ(VKR(G("vkResetCommandBuffer", cb, 0)), VK_SUCCESS);
        CHECK_EQ(VKR(G("vkBeginCommandBuffer", cb, gput(&bi, sizeof bi))), VK_SUCCESS);
        barrier(cb, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0);
        CHECK_EQ(VKR(G("vkEndCommandBuffer", cb)), VK_SUCCESS);
        CHECK_EQ(VKR(G("vkQueueSubmit", queue, 1, gput(&si, sizeof si), 0)), VK_SUCCESS);
        pi.pImageIndices = GP(gput(&idx, 4));
        int64_t before = mono_ns();
        uint64_t frames = vi_guest_frames();
        CHECK_EQ(VKR(G("vkQueuePresentKHR", queue, gput(&pi, sizeof pi))), VK_SUCCESS);
        CHECK_EQ(vi_guest_frames(), frames + 1);
        CHECK_EQ(VKR(G("vkResetFences", dev, 1, gput(&fence, 8))), VK_SUCCESS);
        CHECK_EQ(VKR(G("vkAcquireNextImageKHR", dev, sw, 0, 0, fence, gidx)), VK_NOT_READY);
        CHECK_EQ(VKR(G("vkAcquireNextImageKHR", dev, sw, UINT64_MAX, 0, fence, gidx)), VK_SUCCESS);
        CHECK(mono_ns() >= vi_vsync_after(before));
        CHECK(g32(gidx) != idx);
        CHECK_EQ(VKR(G("vkWaitForFences", dev, 1, gput(&fence, 8), VK_TRUE, UINT64_MAX)), VK_SUCCESS);
        CHECK_EQ(VKR(G("vkDeviceWaitIdle", dev)), VK_SUCCESS);
        G("vkDestroyCommandPool", dev, g64(gpool), 0);
        G("vkDestroyFence", dev, fence, 0);
        G("vkDestroySwapchainKHR", dev, sw, 0);
        G("vkDestroyDevice", dev, 0);
        presented++;
    }
    G("vkDestroySurfaceKHR", g_inst, surface, 0);
    G("vkDestroyInstance", g_inst, 0);
    CHECK(presented > 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
}

TEST(vk_bad_handles_and_unknown_structs)
{
    CHECK_EQ(boot(NULL), 0);
    CHECK(make_instance(NULL, 0, 0) != 0);
    CHECK_EQ(VKR(G("vkEnumeratePhysicalDevices", 0x1234, galloc(4), 0)), VK_ERROR_UNKNOWN);
    CHECK_EQ(VKR(G("vkCmdDraw", g_inst, 3, 1, 0, 0)), VK_ERROR_UNKNOWN);
    uint64_t pds[8];
    CHECK(physical_devices(pds, 8) > 0);
    uint64_t dev = make_device(pds[0], queue_family(pds[0], 0), NULL, 0);
    CHECK(dev != 0);
    uint64_t unknown[4] = { 0x7FFF0000u, 0, 0, 0 };
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = GP(gput(unknown, sizeof unknown)),
        .size = 256,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    uint64_t gb = galloc(8);
    CHECK_EQ(VKR(G("vkCreateBuffer", dev, gput(&bci, sizeof bci), 0, gb)), VK_SUCCESS);
    CHECK(g64(gb) != 0);
    G("vkDestroyBuffer", dev, g64(gb), 0);
    uint64_t gdpa = gipa(g_inst, "vkGetDeviceProcAddr");
    CHECK(gdpa != 0);
    uint64_t args[2] = { dev, gstr("vkCmdDraw") };
    CHECK_EQ(gcall_addr(gdpa, args, 2), rl_vk_guest_addr("vkCmdDraw"));
    args[1] = gstr("vkCreateInstance");
    CHECK_EQ(gcall_addr(gdpa, args, 2), 0);
    G("vkDestroyDevice", dev, 0);
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
}

static void check_switch_family(const VkQueueFamilyProperties *p)
{
    CHECK_EQ(p->queueFlags, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT);
    CHECK_EQ(p->queueCount, 16);
    CHECK_EQ(p->timestampValidBits, 64);
    CHECK_EQ(p->minImageTransferGranularity.width, 1);
    CHECK_EQ(p->minImageTransferGranularity.height, 1);
    CHECK_EQ(p->minImageTransferGranularity.depth, 1);
}

TEST(vk_queue_families_are_the_switch_layout)
{
    CHECK_EQ(boot(NULL), 0);
    CHECK(make_instance(NULL, 0, 0) != 0);
    uint64_t pds[8];
    uint32_t n = physical_devices(pds, 8);
    CHECK(n > 0);
    for (uint32_t d = 0; d < n; d++) {
        char name[256];
        if (!device_wanted(pds[d], name))
            continue;
        uint64_t gn = galloc(4);
        G("vkGetPhysicalDeviceQueueFamilyProperties", pds[d], gn, 0);
        CHECK_EQ(g32(gn), 1);
        uint64_t arr = galloc(4 * sizeof(VkQueueFamilyProperties));
        rl_st32(g_mem, gn, 4);
        G("vkGetPhysicalDeviceQueueFamilyProperties", pds[d], gn, arr);
        CHECK_EQ(g32(gn), 1);
        VkQueueFamilyProperties p;
        memcpy(&p, g_mem + arr, sizeof p);
        check_switch_family(&p);
        VkQueueFamilyProperties2 p2[2] = {
            { .sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2 },
            { .sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2 },
        };
        uint64_t g2 = gput(p2, sizeof p2);
        rl_st32(g_mem, gn, 2);
        G("vkGetPhysicalDeviceQueueFamilyProperties2KHR", pds[d], gn, g2);
        CHECK_EQ(g32(gn), 1);
        memcpy(&p, g_mem + g2 + offsetof(VkQueueFamilyProperties2, queueFamilyProperties), sizeof p);
        check_switch_family(&p);
        float prio[16];
        for (unsigned i = 0; i < 16; i++)
            prio[i] = 1.0f;
        VkDeviceQueueCreateInfo qci = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 16,
            .pQueuePriorities = GP(gput(prio, sizeof prio)),
        };
        VkDeviceCreateInfo dci = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = GP(gput(&qci, sizeof qci)),
        };
        uint64_t out = galloc(8);
        CHECK_EQ(VKR(G("vkCreateDevice", pds[d], gput(&dci, sizeof dci), 0, out)), VK_SUCCESS);
        uint64_t dev = g64(out);
        uint64_t gq = galloc(8);
        for (unsigned i = 0; i < 16; i++) {
            rl_st64(g_mem, gq, 0);
            G("vkGetDeviceQueue", dev, 0, i, gq);
            CHECK(g64(gq) != 0);
            CHECK_EQ(VKR(G("vkQueueSubmit", g64(gq), 0, 0, 0)), VK_SUCCESS);
            CHECK_EQ(VKR(G("vkQueueWaitIdle", g64(gq))), VK_SUCCESS);
        }
        VkDeviceQueueInfo2 qi2 = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, .queueIndex = 15 };
        rl_st64(g_mem, gq, 0);
        G("vkGetDeviceQueue2", dev, gput(&qi2, sizeof qi2), gq);
        CHECK(g64(gq) != 0);
        rl_st64(g_mem, gq, 1);
        G("vkGetDeviceQueue", dev, 0, 16, gq);
        CHECK_EQ(g64(gq), 0);
        CHECK_EQ(VKR(G("vkDeviceWaitIdle", dev)), VK_SUCCESS);
        G("vkDestroyDevice", dev, 0);
        qci.queueFamilyIndex = 1;
        qci.queueCount = 1;
        dci.pQueueCreateInfos = GP(gput(&qci, sizeof qci));
        CHECK_EQ(VKR(G("vkCreateDevice", pds[d], gput(&dci, sizeof dci), 0, out)), VK_ERROR_INITIALIZATION_FAILED);
    }
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
}

TEST(vk_one_physical_device_that_presents_headless)
{
    CHECK_EQ(boot(rl_vk_headless_surfaces()), 0);
    const char *exts[] = { "VK_KHR_surface", "VK_NN_vi_surface" };
    CHECK(make_instance(exts, 2, 0) != 0);
    uint64_t pds[8];
    CHECK_EQ(physical_devices(pds, 8), 1);
    uint64_t gn = galloc(4);
    CHECK_EQ(VKR(G("vkEnumeratePhysicalDeviceGroups", g_inst, gn, 0)), VK_SUCCESS);
    CHECK_EQ(g32(gn), 1);
    VkPhysicalDeviceGroupProperties grp[2] = {
        { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES },
        { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES },
    };
    uint64_t gg = gput(grp, sizeof grp);
    rl_st32(g_mem, gn, 2);
    CHECK_EQ(VKR(G("vkEnumeratePhysicalDeviceGroupsKHR", g_inst, gn, gg)), VK_SUCCESS);
    CHECK_EQ(g32(gn), 1);
    CHECK_EQ(g32(gg + offsetof(VkPhysicalDeviceGroupProperties, physicalDeviceCount)), 1);
    CHECK_EQ(g64(gg + offsetof(VkPhysicalDeviceGroupProperties, physicalDevices)), pds[0]);
    rl_st32(g_mem, gn, 0);
    CHECK_EQ(VKR(G("vkEnumeratePhysicalDevices", g_inst, gn, galloc(8))), VK_INCOMPLETE);
    VkViSurfaceCreateInfoNN sci = { .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, .window = GP(galloc(64)) };
    uint64_t gs = galloc(8);
    CHECK_EQ(VKR(G("vkCreateViSurfaceNN", g_inst, gput(&sci, sizeof sci), 0, gs)), VK_SUCCESS);
    uint64_t gsup = galloc(4);
    CHECK_EQ(VKR(G("vkGetPhysicalDeviceSurfaceSupportKHR", pds[0], 0, g64(gs), gsup)), VK_SUCCESS);
    CHECK_EQ(g32(gsup), VK_TRUE);
    uint64_t gcaps = galloc(sizeof(VkSurfaceCapabilitiesKHR));
    CHECK_EQ(VKR(G("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", pds[0], g64(gs), gcaps)), VK_SUCCESS);
    G("vkDestroySurfaceKHR", g_inst, g64(gs), 0);
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
}
