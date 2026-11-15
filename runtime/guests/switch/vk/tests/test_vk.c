/**
 * @file test_vk.c
 * @brief The Vulkan forwarding layer driven the way lifted guest code drives
 * it: guest structs built in a synthetic guest memory image, arguments in
 * the guest context, thunks reached through vkGetInstanceProcAddr and the
 * dispatch table, run against the host's real Vulkan driver (lavapipe in
 * CI). The layer shows one physical device; RELIFT_VK_DEVICE picks it by a
 * part of its name, and RELIFT_VK_LIST_DEVICES=1 prints the host's device
 * names (one per line) instead of running, for make test to run the tests
 * on each. With no host Vulkan the tests are skipped, unless
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

__attribute__((constructor(201))) static void list_devices(void)
{
    if (!getenv("RELIFT_VK_LIST_DEVICES"))
        return;
    void *g = rl_hostlib_vulkan_gipa();
    PFN_vkGetInstanceProcAddr gipa;
    memcpy(&gipa, &g, sizeof gipa);
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties props =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!create || !props)
        _exit(0);
    uint32_t n = 0;
    props(NULL, &n, NULL);
    VkExtensionProperties *ext = calloc(n ? n : 1, sizeof *ext);
    props(NULL, &n, ext);
    const char *portability = "VK_KHR_portability_enumeration";
    bool have = false;
    for (uint32_t i = 0; i < n; i++)
        have = have || !strcmp(ext[i].extensionName, portability);
    free(ext);
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags = have ? 1u : 0u,
        .enabledExtensionCount = have ? 1u : 0u,
        .ppEnabledExtensionNames = &portability,
    };
    VkInstance inst = NULL;
    if (create(&ci, NULL, &inst) < 0)
        _exit(0);
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties =
        (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    VkPhysicalDevice pd[16];
    uint32_t count = 16;
    if (enumerate(inst, &count, pd) >= 0)
        for (uint32_t i = 0; i < count; i++) {
            VkPhysicalDeviceProperties p;
            properties(pd[i], &p);
            printf("%s\n", p.deviceName);
        }
    fflush(stdout);
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

static int boot_with(const rl_vk_surface_provider *surfaces, void *gipa)
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
        .gipa = gipa,
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

static int boot(const rl_vk_surface_provider *surfaces) { return boot_with(surfaces, NULL); }

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

static void device_name(uint64_t pd, char *name)
{
    uint64_t gp = galloc(sizeof(VkPhysicalDeviceProperties));
    G("vkGetPhysicalDeviceProperties", pd, gp);
    memcpy(name, g_mem + gp + offsetof(VkPhysicalDeviceProperties, deviceName), 256);
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
        device_name(pds[i], name);
        CHECK(name[0] != 0);
        VkPhysicalDeviceVulkan12Properties v12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
        uint64_t g12 = gput(&v12, sizeof v12);
        VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = GP(g12) };
        uint64_t gdrv = gput(&drv, sizeof drv);
        VkPhysicalDeviceIDProperties idp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .pNext = GP(gdrv) };
        uint64_t gid = gput(&idp, sizeof idp);
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = GP(gid) };
        uint64_t gp2 = gput(&p2, sizeof p2);
        G("vkGetPhysicalDeviceProperties2KHR", pds[i], gp2);
        CHECK_EQ(g64(gp2 + 8), gid);
        CHECK_EQ(g64(gid + 8), gdrv);
        CHECK_EQ(g64(gdrv + 8), g12);
        CHECK(!strcmp((const char *)g_mem + gp2 + 16 + offsetof(VkPhysicalDeviceProperties, deviceName), name));
        CHECK_EQ(g32(gdrv + offsetof(VkPhysicalDeviceDriverProperties, driverID)), VK_DRIVER_ID_NVIDIA_PROPRIETARY);
        CHECK(!strcmp((const char *)g_mem + gdrv + offsetof(VkPhysicalDeviceDriverProperties, driverName), "NVIDIA"));
        CHECK_EQ(g32(g12 + offsetof(VkPhysicalDeviceVulkan12Properties, driverID)), 0);
        uint8_t zero[VK_UUID_SIZE] = { 0 };
        CHECK(memcmp(g_mem + gid + offsetof(VkPhysicalDeviceIDProperties, deviceUUID), zero, VK_UUID_SIZE) != 0);
        fprintf(stderr, "  device %u: %s\n", i, name);
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
        device_name(pds[i], name);
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
    g_resizable.offscreen = rl_vk_headless_surfaces()->offscreen;
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
        device_name(pds[i], name);
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

/* A fake host driver: the real one, with the Switch's limits changed as a test sets. */
static void *(*g_real_gipa)(void *, const char *);
static PFN_vkGetPhysicalDeviceProperties g_real_props;
static PFN_vkGetPhysicalDeviceProperties2 g_real_props2;
static PFN_vkGetPhysicalDeviceFeatures g_real_feats;
static PFN_vkGetPhysicalDeviceFeatures2 g_real_feats2;
static void (*g_tweak)(VkPhysicalDeviceLimits *);
static int g_drop_feature = -1;

static void fake_limits(VkPhysicalDeviceLimits *l)
{
    float period = l->timestampPeriod;
    *l = vkf_switch.props.limits;
    l->timestampPeriod = period;
    if (g_tweak)
        g_tweak(l);
}

static void fake_props(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p)
{
    g_real_props(pd, p);
    fake_limits(&p->limits);
}

static void fake_props2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
    g_real_props2(pd, p);
    fake_limits(&p->properties.limits);
}

static void drop(VkPhysicalDeviceFeatures *f)
{
    if (g_drop_feature >= 0)
        ((VkBool32 *)f)[g_drop_feature] = VK_FALSE;
}

static void fake_feats(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f)
{
    g_real_feats(pd, f);
    drop(f);
}

static void fake_feats2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    g_real_feats2(pd, f);
    drop(&f->features);
}

static void *fake_gipa(void *inst, const char *name)
{
    void *r = g_real_gipa(inst, name);
    if (!r || !inst)
        return r;
    void (*f)(void) = NULL;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties"))
        memcpy(&g_real_props, &r, sizeof r), f = (void (*)(void))fake_props;
    else if (!strcmp(name, "vkGetPhysicalDeviceProperties2") || !strcmp(name, "vkGetPhysicalDeviceProperties2KHR"))
        memcpy(&g_real_props2, &r, sizeof r), f = (void (*)(void))fake_props2;
    else if (!strcmp(name, "vkGetPhysicalDeviceFeatures"))
        memcpy(&g_real_feats, &r, sizeof r), f = (void (*)(void))fake_feats;
    else if (!strcmp(name, "vkGetPhysicalDeviceFeatures2") || !strcmp(name, "vkGetPhysicalDeviceFeatures2KHR"))
        memcpy(&g_real_feats2, &r, sizeof r), f = (void (*)(void))fake_feats2;
    if (f)
        memcpy(&r, &f, sizeof r);
    return r;
}

static uint64_t boot_fake(void (*tweak)(VkPhysicalDeviceLimits *), int drop_feature)
{
    void *real = rl_hostlib_vulkan_gipa();
    memcpy(&g_real_gipa, &real, sizeof real);
    g_tweak = tweak;
    g_drop_feature = drop_feature;
    void *(*fg)(void *, const char *) = fake_gipa;
    void *gipa;
    memcpy(&gipa, &fg, sizeof gipa);
    if (boot_with(NULL, gipa) != 0 || !make_instance(NULL, 0, 0))
        return 0;
    uint64_t pd = 0;
    return physical_devices(&pd, 1) ? pd : 0;
}

static int32_t create_device(uint64_t pd, uint64_t features, uint64_t *dev)
{
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1,
        .pQueuePriorities = GP(gput(&prio, 4)),
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = GP(gput(&qci, sizeof qci)),
        .pEnabledFeatures = GP(features),
    };
    uint64_t out = galloc(8);
    rl_st64(g_mem, out, 0);
    int32_t r = VKR(G("vkCreateDevice", pd, gput(&dci, sizeof dci), 0, out));
    *dev = g64(out);
    return r;
}

static void finish(uint64_t dev)
{
    if (dev)
        G("vkDestroyDevice", dev, 0);
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
    g_tweak = NULL;
    g_drop_feature = -1;
}

static VkPhysicalDeviceLimits guest_limits(uint64_t pd)
{
    uint64_t gp = galloc(sizeof(VkPhysicalDeviceProperties));
    G("vkGetPhysicalDeviceProperties", pd, gp);
    VkPhysicalDeviceLimits l;
    memcpy(&l, g_mem + gp + offsetof(VkPhysicalDeviceProperties, limits), sizeof l);
    return l;
}

static bool limits_are_the_switch(const VkPhysicalDeviceLimits *l)
{
    VkPhysicalDeviceLimits a = *l, b = vkf_switch.props.limits;
    a.timestampPeriod = b.timestampPeriod = 0;
    return !memcmp(&a, &b, sizeof a);
}

TEST(vk_guest_sees_the_switch_driver)
{
    uint64_t pd = boot_fake(NULL, -1);
    CHECK(pd != 0);
    uint64_t gp = galloc(sizeof(VkPhysicalDeviceProperties));
    G("vkGetPhysicalDeviceProperties", pd, gp);
    VkPhysicalDeviceProperties p;
    memcpy(&p, g_mem + gp, sizeof p);
    CHECK(!strcmp(p.deviceName, vkf_switch.props.deviceName));
    CHECK_EQ(p.apiVersion, vkf_switch.props.apiVersion);
    CHECK_EQ(p.driverVersion, vkf_switch.props.driverVersion);
    CHECK_EQ(p.vendorID, vkf_switch.props.vendorID);
    CHECK_EQ(p.deviceID, vkf_switch.props.deviceID);
    CHECK(limits_are_the_switch(&p.limits));
    uint64_t chain = 0;
    for (uint32_t i = vkf_switch.props_ext_count; i-- > 0;) {
        uint64_t g = galloc(vkf_switch.props_ext[i].size);
        rl_st32(g_mem, g, (uint32_t)vkf_switch.props_ext[i].stype);
        rl_st64(g_mem, g + 8, chain);
        chain = g;
    }
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = GP(chain) };
    uint64_t g2 = gput(&p2, sizeof p2);
    G("vkGetPhysicalDeviceProperties2", pd, g2);
    CHECK(!memcmp(g_mem + g2 + offsetof(VkPhysicalDeviceProperties2, properties), &p, sizeof p));
    uint64_t g = chain;
    for (uint32_t i = 0; i < vkf_switch.props_ext_count; i++, g = g64(g + 8)) {
        const vkf_switch_struct *s = &vkf_switch.props_ext[i];
        CHECK(!memcmp(g_mem + g + 16, (const uint8_t *)s->data + 16, s->size - 16));
    }
    uint64_t gm = galloc(sizeof(VkPhysicalDeviceMemoryProperties));
    G("vkGetPhysicalDeviceMemoryProperties", pd, gm);
    CHECK(!memcmp(g_mem + gm, &vkf_switch.memory, sizeof vkf_switch.memory));
    uint64_t gf = galloc(sizeof(VkPhysicalDeviceFeatures));
    G("vkGetPhysicalDeviceFeatures", pd, gf);
    const VkBool32 *gb = (const VkBool32 *)(g_mem + gf), *sb = (const VkBool32 *)&vkf_switch.features;
    for (unsigned i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++)
        CHECK(!gb[i] || sb[i]);
    uint64_t gn = galloc(4);
    CHECK_EQ(VKR(G("vkEnumerateDeviceExtensionProperties", pd, 0, gn, 0)), VK_SUCCESS);
    uint32_t n = g32(gn);
    uint64_t ge = galloc(n * sizeof(VkExtensionProperties));
    CHECK_EQ(VKR(G("vkEnumerateDeviceExtensionProperties", pd, 0, gn, ge)), VK_SUCCESS);
    for (uint32_t i = 0; i < n; i++) {
        const VkExtensionProperties *e = (const VkExtensionProperties *)(g_mem + ge) + i;
        const VkExtensionProperties *s = vkf_switch_ext(vkf_switch.device_exts, vkf_switch.device_ext_count, e->extensionName);
        CHECK(s != NULL);
        CHECK_EQ(e->specVersion, s->specVersion);
    }
    uint64_t gfp = galloc(sizeof(VkFormatProperties));
    for (uint32_t f = 0; f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; f++) {
        G("vkGetPhysicalDeviceFormatProperties", pd, f, gfp);
        VkFormatProperties s = vkf_switch_format_props((VkFormat)f);
        CHECK_EQ(g32(gfp) & ~s.linearTilingFeatures, 0);
        CHECK_EQ(g32(gfp + 4) & ~s.optimalTilingFeatures, 0);
        CHECK_EQ(g32(gfp + 8) & ~s.bufferFeatures, 0);
    }
    uint64_t gv = galloc(4);
    CHECK_EQ(VKR(G("vkEnumerateInstanceVersion", gv)), VK_SUCCESS);
    CHECK_EQ(g32(gv), vkf_switch.instance_version);
    uint64_t dev;
    CHECK_EQ(create_device(pd, 0, &dev), VK_SUCCESS);
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    };
    uint64_t gbuf = galloc(8);
    CHECK_EQ(VKR(G("vkCreateBuffer", dev, gput(&bci, sizeof bci), 0, gbuf)), VK_SUCCESS);
    uint64_t greq = galloc(sizeof(VkMemoryRequirements));
    G("vkGetBufferMemoryRequirements", dev, g64(gbuf), greq);
    uint32_t bits = g32(greq + offsetof(VkMemoryRequirements, memoryTypeBits));
    CHECK(bits != 0);
    CHECK_EQ(bits >> vkf_switch.memory.memoryTypeCount, 0);
    uint32_t type = memory_type(pd, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    CHECK(type != UINT32_MAX);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 4096, .memoryTypeIndex = type };
    uint64_t gmem = galloc(8);
    CHECK_EQ(VKR(G("vkAllocateMemory", dev, gput(&ai, sizeof ai), 0, gmem)), VK_SUCCESS);
    CHECK_EQ(VKR(G("vkBindBufferMemory", dev, g64(gbuf), g64(gmem), 0)), VK_SUCCESS);
    G("vkDestroyBuffer", dev, g64(gbuf), 0);
    G("vkFreeMemory", dev, g64(gmem), 0);
    finish(dev);
}

static void large_host(VkPhysicalDeviceLimits *l)
{
    l->maxBoundDescriptorSets = 64;
    l->maxPerStageDescriptorSamplers = 1u << 20;
    l->maxPushConstantsSize = 1024;
    l->maxImageDimension2D = 1u << 16;
    l->maxVertexInputAttributes = 64;
    l->minUniformBufferOffsetAlignment = 1;
}

TEST(vk_a_larger_host_shows_the_switch_limits)
{
    uint64_t pd = boot_fake(large_host, -1);
    CHECK(pd != 0);
    VkPhysicalDeviceLimits l = guest_limits(pd);
    CHECK(limits_are_the_switch(&l));
    uint64_t dev;
    CHECK_EQ(create_device(pd, 0, &dev), VK_SUCCESS);
    finish(dev);
}

static void small_descriptors(VkPhysicalDeviceLimits *l)
{
    l->maxBoundDescriptorSets = 2;
    l->maxPerStageDescriptorSamplers = 3;
    l->maxPushConstantsSize = 64;
}

static uint64_t ubo_pool(uint64_t dev, VkDescriptorPoolCreateFlags flags)
{
    VkDescriptorPoolSize size = { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 4 };
    VkDescriptorPoolCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = flags,
        .maxSets = 4,
        .poolSizeCount = 1,
        .pPoolSizes = GP(gput(&size, sizeof size)),
    };
    uint64_t out = galloc(8);
    rl_st64(g_mem, out, 0);
    return VKR(G("vkCreateDescriptorPool", dev, gput(&ci, sizeof ci), 0, out)) == VK_SUCCESS ? g64(out) : 0;
}

static uint64_t ubo_sets(uint64_t dev, uint64_t pool, uint64_t layout, uint32_t n)
{
    uint64_t layouts = galloc(8u * n), sets = galloc(8u * n);
    for (uint32_t i = 0; i < n; i++) {
        rl_st64(g_mem, layouts + 8u * i, layout);
        rl_st64(g_mem, sets + 8u * i, 0);
    }
    VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = (VkDescriptorPool)(uintptr_t)pool,
        .descriptorSetCount = n,
        .pSetLayouts = GP(layouts),
    };
    if (VKR(G("vkAllocateDescriptorSets", dev, gput(&ai, sizeof ai), sets)) != VK_SUCCESS)
        return 0;
    for (uint32_t i = 0; i < n; i++)
        if (!g64(sets + 8u * i))
            return 0;
    return sets;
}

TEST(vk_descriptor_pools_hold_more_than_their_counts)
{
    CHECK_EQ(boot(NULL), 0);
    CHECK(make_instance(NULL, 0, 0) != 0);
    uint64_t pds[8];
    CHECK(physical_devices(pds, 8) > 0);
    uint64_t dev = make_device(pds[0], queue_family(pds[0], 0), NULL, 0);
    CHECK(dev != 0);
    VkDescriptorSetLayoutBinding b = {
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_ALL,
    };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = GP(gput(&b, sizeof b)),
    };
    uint64_t gl = galloc(8);
    CHECK_EQ(VKR(G("vkCreateDescriptorSetLayout", dev, gput(&lci, sizeof lci), 0, gl)), VK_SUCCESS);
    uint64_t layout = g64(gl);
    uint64_t pool = ubo_pool(dev, 0);
    CHECK(pool != 0);
    CHECK(ubo_sets(dev, pool, layout, 3) != 0);
    CHECK(ubo_sets(dev, pool, layout, 64) != 0);
    CHECK(ubo_sets(dev, pool, layout, 2) != 0);
    CHECK_EQ(VKR(G("vkResetDescriptorPool", dev, pool, 0)), VK_SUCCESS);
    CHECK(ubo_sets(dev, pool, layout, 40) != 0);
    G("vkDestroyDescriptorPool", dev, pool, 0);
    uint64_t fpool = ubo_pool(dev, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
    CHECK(fpool != 0);
    uint64_t sets = ubo_sets(dev, fpool, layout, 20);
    CHECK(sets != 0);
    CHECK_EQ(VKR(G("vkFreeDescriptorSets", dev, fpool, 20, sets)), VK_SUCCESS);
    sets = ubo_sets(dev, fpool, layout, 20);
    CHECK(sets != 0);
    CHECK_EQ(VKR(G("vkFreeDescriptorSets", dev, fpool, 1, sets + 8u * 19)), VK_SUCCESS);
    G("vkDestroyDescriptorPool", dev, fpool, 0);
    CHECK(ubo_sets(dev, ubo_pool(dev, 0), layout, 9) != 0);
    G("vkDestroyDescriptorSetLayout", dev, layout, 0);
    G("vkDestroyDevice", dev, 0);
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
}

static int32_t set_layout(uint64_t dev, uint32_t samplers, uint64_t *out)
{
    VkDescriptorSetLayoutBinding b = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = samplers,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = GP(gput(&b, sizeof b)),
    };
    uint64_t g = galloc(8);
    rl_st64(g_mem, g, 0);
    int32_t r = VKR(G("vkCreateDescriptorSetLayout", dev, gput(&ci, sizeof ci), 0, g));
    *out = g64(g);
    return r;
}

static int32_t pipeline_layout(uint64_t dev, const uint64_t *sets, uint32_t n, uint32_t push)
{
    VkPushConstantRange pr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .size = push };
    VkPipelineLayoutCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = n,
        .pSetLayouts = GP(gput(sets, 8ull * n)),
        .pushConstantRangeCount = push ? 1 : 0,
        .pPushConstantRanges = GP(gput(&pr, sizeof pr)),
    };
    uint64_t g = galloc(8);
    rl_st64(g_mem, g, 0);
    int32_t r = VKR(G("vkCreatePipelineLayout", dev, gput(&ci, sizeof ci), 0, g));
    if (r == VK_SUCCESS)
        G("vkDestroyPipelineLayout", dev, g64(g), 0);
    return r;
}

TEST(vk_a_smaller_host_fails_only_the_calls_past_its_limits)
{
    const VkPhysicalDeviceLimits *s = &vkf_switch.props.limits;
    CHECK(s->maxBoundDescriptorSets > 2 && s->maxPushConstantsSize > 64);
    uint64_t pd = boot_fake(small_descriptors, -1);
    CHECK(pd != 0);
    VkPhysicalDeviceLimits l = guest_limits(pd);
    CHECK(limits_are_the_switch(&l));
    uint64_t dev;
    CHECK_EQ(create_device(pd, 0, &dev), VK_SUCCESS);
    uint64_t small;
    CHECK_EQ(set_layout(dev, 2, &small), VK_SUCCESS);
    uint64_t sets[3] = { small, small, small };
    CHECK_EQ(pipeline_layout(dev, sets, 1, 64), VK_SUCCESS);
    CHECK_EQ(pipeline_layout(dev, sets, 1, 128), VK_ERROR_INITIALIZATION_FAILED);
    CHECK_EQ(pipeline_layout(dev, sets, 3, 0), VK_ERROR_INITIALIZATION_FAILED);
    G("vkDestroyDescriptorSetLayout", dev, small, 0);
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { l.maxImageDimension2D * 2, 1, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    uint64_t gi = galloc(8);
    CHECK(VKR(G("vkCreateImage", dev, gput(&ici, sizeof ici), 0, gi)) < 0);
    VkFramebufferCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .width = l.maxFramebufferWidth * 2,
        .height = 1,
        .layers = 1,
    };
    uint64_t gf = galloc(8);
    CHECK(VKR(G("vkCreateFramebuffer", dev, gput(&fci, sizeof fci), 0, gf)) < 0);
    finish(dev);
}

TEST(vk_a_smaller_host_forwards_descriptor_counts_past_its_limits)
{
    CHECK(vkf_switch.props.limits.maxPerStageDescriptorSamplers > 3);
    uint64_t pd = boot_fake(small_descriptors, -1);
    CHECK(pd != 0);
    uint64_t dev;
    CHECK_EQ(create_device(pd, 0, &dev), VK_SUCCESS);
    uint64_t big, small;
    CHECK_EQ(set_layout(dev, 4, &big), VK_SUCCESS);
    CHECK(big != 0);
    CHECK_EQ(set_layout(dev, 2, &small), VK_SUCCESS);
    uint64_t two[2] = { small, small };
    CHECK_EQ(pipeline_layout(dev, two, 2, 0), VK_SUCCESS);
    rl_cfg()->strict = true;
    uint64_t refused;
    CHECK_EQ(set_layout(dev, 4, &refused), VK_ERROR_INITIALIZATION_FAILED);
    CHECK_EQ(refused, 0);
    CHECK_EQ(pipeline_layout(dev, two, 2, 0), VK_ERROR_INITIALIZATION_FAILED);
    uint64_t one;
    CHECK_EQ(set_layout(dev, 1, &one), VK_SUCCESS);
    uint64_t fits[2] = { one, small };
    CHECK_EQ(pipeline_layout(dev, fits, 2, 0), VK_SUCCESS);
    rl_cfg()->strict = false;
    G("vkDestroyDescriptorSetLayout", dev, one, 0);
    G("vkDestroyDescriptorSetLayout", dev, small, 0);
    G("vkDestroyDescriptorSetLayout", dev, big, 0);
    finish(dev);
}

static void few_attributes(VkPhysicalDeviceLimits *l) { l->maxVertexInputAttributes = vkf_switch.props.limits.maxVertexInputAttributes - 1; }

TEST(vk_a_host_short_of_an_unchecked_limit_shows_its_own)
{
    uint64_t pd = boot_fake(few_attributes, -1);
    CHECK(pd != 0);
    VkPhysicalDeviceLimits l = guest_limits(pd);
    CHECK_EQ(l.maxVertexInputAttributes, vkf_switch.props.limits.maxVertexInputAttributes - 1);
    l.maxVertexInputAttributes = vkf_switch.props.limits.maxVertexInputAttributes;
    CHECK(limits_are_the_switch(&l));
    uint64_t dev;
    CHECK_EQ(create_device(pd, 0, &dev), VK_SUCCESS);
    G("vkDestroyDevice", dev, 0);
    rl_cfg()->strict = true;
    CHECK_EQ(create_device(pd, 0, &dev), VK_ERROR_INITIALIZATION_FAILED);
    rl_cfg()->strict = false;
    CHECK_EQ(dev, 0);
    finish(0);
}

TEST(vk_a_feature_the_host_lacks_is_hidden_and_refused)
{
    const VkBool32 *sb = (const VkBool32 *)&vkf_switch.features;
    int pick = -1;
    for (int i = 0; i < (int)(sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32)) && pick < 0; i++)
        if (sb[i])
            pick = i;
    CHECK(pick >= 0);
    uint64_t pd = boot_fake(NULL, pick);
    CHECK(pd != 0);
    uint64_t gf = galloc(sizeof(VkPhysicalDeviceFeatures));
    G("vkGetPhysicalDeviceFeatures", pd, gf);
    CHECK_EQ(g32(gf + 4ull * (uint64_t)pick), VK_FALSE);
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    uint64_t g2 = gput(&f2, sizeof f2);
    G("vkGetPhysicalDeviceFeatures2", pd, g2);
    CHECK_EQ(g32(g2 + offsetof(VkPhysicalDeviceFeatures2, features) + 4ull * (uint64_t)pick), VK_FALSE);
    VkPhysicalDeviceFeatures want = { 0 };
    ((VkBool32 *)&want)[pick] = VK_TRUE;
    uint64_t dev;
    CHECK_EQ(create_device(pd, gput(&want, sizeof want), &dev), VK_ERROR_FEATURE_NOT_PRESENT);
    finish(0);
    uint64_t gl = galloc(8);
    CHECK_EQ(VKR(G("vkCreateDescriptorSetLayout", dev, gput(&lci, sizeof lci), 0, gl)), VK_SUCCESS);
    uint64_t layout = g64(gl);
    uint64_t pool = ubo_pool(dev, 0);
    CHECK(pool != 0);
    CHECK(ubo_sets(dev, pool, layout, 3) != 0);
    CHECK(ubo_sets(dev, pool, layout, 64) != 0);
    CHECK(ubo_sets(dev, pool, layout, 2) != 0);
    CHECK_EQ(VKR(G("vkResetDescriptorPool", dev, pool, 0)), VK_SUCCESS);
    CHECK(ubo_sets(dev, pool, layout, 40) != 0);
    G("vkDestroyDescriptorPool", dev, pool, 0);
    uint64_t fpool = ubo_pool(dev, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
    CHECK(fpool != 0);
    uint64_t sets = ubo_sets(dev, fpool, layout, 20);
    CHECK(sets != 0);
    CHECK_EQ(VKR(G("vkFreeDescriptorSets", dev, fpool, 20, sets)), VK_SUCCESS);
    sets = ubo_sets(dev, fpool, layout, 20);
    CHECK(sets != 0);
    CHECK_EQ(VKR(G("vkFreeDescriptorSets", dev, fpool, 1, sets + 8u * 19)), VK_SUCCESS);
    G("vkDestroyDescriptorPool", dev, fpool, 0);
    CHECK(ubo_sets(dev, ubo_pool(dev, 0), layout, 9) != 0);
    G("vkDestroyDescriptorSetLayout", dev, layout, 0);
    G("vkDestroyDevice", dev, 0);
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
}

static void record_fill(uint64_t cb, uint64_t buf, uint32_t value, uint32_t flags)
{
    VkCommandBufferInheritanceInfo inh = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = flags,
        .pInheritanceInfo = GP(gput(&inh, sizeof inh)),
    };
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    CHECK_EQ(VKR(G("vkBeginCommandBuffer", cb, gput(&bi, sizeof bi))), VK_SUCCESS);
    G("vkCmdFillBuffer", cb, buf, 0, VK_WHOLE_SIZE, value);
    G("vkCmdPipelineBarrier", cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, gput(&mb, sizeof mb), 0, 0, 0, 0);
    CHECK_EQ(VKR(G("vkEndCommandBuffer", cb)), VK_SUCCESS);
}

static uint32_t submit_and_read(uint64_t dev, uint64_t queue, uint64_t cb, uint64_t fence, const mapped_buf *b, bool two)
{
    uint64_t gfence = gput(&fence, 8);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = GP(gput(&cb, 8)) };
    VkCommandBufferSubmitInfo ci = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = (VkCommandBuffer)GP(cb) };
    VkSubmitInfo2 si2 = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = GP(gput(&ci, sizeof ci)) };
    VkResult r = two ? VKR(G("vkQueueSubmit2", queue, 1, gput(&si2, sizeof si2), fence))
                     : VKR(G("vkQueueSubmit", queue, 1, gput(&si, sizeof si), fence));
    if (r != VK_SUCCESS || VKR(G("vkWaitForFences", dev, 1, gfence, VK_TRUE, UINT64_MAX)) != VK_SUCCESS ||
        VKR(G("vkResetFences", dev, 1, gfence)) != VK_SUCCESS)
        return 0;
    return g32(b->ptr + 60);
}

TEST(vk_secondary_command_buffers_submit_through_a_primary)
{
    CHECK_EQ(boot(NULL), 0);
    CHECK(make_instance(NULL, 0, 0) != 0);
    uint64_t pds[8];
    CHECK(physical_devices(pds, 8) > 0);
    uint64_t dev = make_device(pds[0], queue_family(pds[0], 0), NULL, 0);
    CHECK(dev != 0);
    uint64_t gq = galloc(8);
    G("vkGetDeviceQueue", dev, 0, 0, gq);
    uint64_t queue = g64(gq);
    mapped_buf b;
    CHECK_EQ(make_mapped(pds[0], dev, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b), 0);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    uint64_t gpool = galloc(8);
    CHECK_EQ(VKR(G("vkCreateCommandPool", dev, gput(&pci, sizeof pci), 0, gpool)), VK_SUCCESS);
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = (VkCommandPool)GP(g64(gpool)),
        .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
        .commandBufferCount = 2,
    };
    uint64_t gcb = galloc(16);
    CHECK_EQ(VKR(G("vkAllocateCommandBuffers", dev, gput(&cai, sizeof cai), gcb)), VK_SUCCESS);
    uint64_t cb = g64(gcb), cb2 = g64(gcb + 8);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    uint64_t gf = galloc(8);
    CHECK_EQ(VKR(G("vkCreateFence", dev, gput(&fci, sizeof fci), 0, gf)), VK_SUCCESS);
    uint64_t fence = g64(gf);
    record_fill(cb, b.buf, 0x01020304u, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
    CHECK_EQ(submit_and_read(dev, queue, cb, fence, &b, false), 0x01020304u);
    memset(g_mem + b.ptr, 0, 64);
    CHECK_EQ(submit_and_read(dev, queue, cb, fence, &b, true), 0x01020304u);
    record_fill(cb, b.buf, 0x0A0B0C0Du, 0);
    CHECK_EQ(submit_and_read(dev, queue, cb, fence, &b, false), 0x0A0B0C0Du);
    record_fill(cb2, b.buf, 0x11111111u, 0);
    CHECK_EQ(submit_and_read(dev, queue, cb2, fence, &b, true), 0x11111111u);
    G("vkFreeCommandBuffers", dev, g64(gpool), 1, gcb);
    CHECK_EQ(VKR(G("vkAllocateCommandBuffers", dev, gput(&cai, sizeof cai), gcb)), VK_SUCCESS);
    record_fill(g64(gcb), b.buf, 0x22222222u, 0);
    CHECK_EQ(submit_and_read(dev, queue, g64(gcb), fence, &b, false), 0x22222222u);
    G("vkDestroyFence", dev, fence, 0);
    G("vkDestroyCommandPool", dev, g64(gpool), 0);
    G("vkDestroyBuffer", dev, b.buf, 0);
    G("vkFreeMemory", dev, b.mem, 0);
    G("vkDestroyDevice", dev, 0);
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
}
