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
#include "arm64.h"
#include "hostlib.h"
#include "runner.h"
#include "services.h"
#include "vi.h"
#include "vkf.h"

#include <relift/settings.h>
#include <relift/touchpad.h>

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
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
    if (rl_core_init(&vk_guest, rl_arm64_cpu()) != 0)
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

static int g_cap_gate, g_cap_done;

static void cap_gate_wait(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&g_cap_gate, __ATOMIC_ACQUIRE)) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
}

static void cap_mark_done(void *arg)
{
    (void)arg;
    __atomic_store_n(&g_cap_done, 1, __ATOMIC_RELEASE);
}

static int cap_files(const char *dir, char *path, size_t n)
{
    DIR *dp = opendir(dir);
    int count = 0;
    struct dirent *e;
    while (dp && (e = readdir(dp)))
        if (!strncmp(e->d_name, "frame-", 6)) {
            snprintf(path, n, "%s/%s", dir, e->d_name);
            count++;
        }
    if (dp)
        closedir(dp);
    return count;
}

TEST(vk_capture_is_written_off_the_presenting_thread)
{
    char dir[512], path[1024] = { 0 };
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/relift-vk-capture-async-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    rl_vk_capture(dir);
    static const uint8_t px[2 * 2 * 4] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
    g_cap_gate = g_cap_done = 0;
    vkf_capture_job job = { .frame = 7, .guest_ns = 1000000, .w = 2, .h = 2, .stride = 8, .px = px,
                            .ready = cap_gate_wait, .done = cap_mark_done };
    vkf_capture_queue(&job);
    CHECK_EQ(cap_files(dir, path, sizeof path), 0);
    CHECK_EQ(__atomic_load_n(&g_cap_done, __ATOMIC_ACQUIRE), 0);
    __atomic_store_n(&g_cap_gate, 1, __ATOMIC_RELEASE);
    vkf_capture_flush();
    CHECK_EQ(__atomic_load_n(&g_cap_done, __ATOMIC_ACQUIRE), 1);
    CHECK_EQ(cap_files(dir, path, sizeof path), 1);
    CHECK(strstr(path, "frame-000007-00000001ms.png") != NULL);
    FILE *f = fopen(path, "rb");
    uint8_t head[8] = { 0 };
    CHECK(f && fread(head, 1, 8, f) == 8);
    if (f)
        fclose(f);
    CHECK(!memcmp(head, "\x89PNG\r\n\x1a\n", 8));
    remove(path);
    rmdir(dir);
    rl_vk_capture(NULL);
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
        CHECK_EQ(VKR(G("vkWaitForFences", dev, 1, gput(&fence, 8), VK_TRUE, 10000000000ull)), VK_SUCCESS);

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

static const char *const g_headless_exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, NULL };

static int32_t headless_surface(void *host_instance, void *gipa, uint64_t *surface)
{
    PFN_vkGetInstanceProcAddr get;
    memcpy(&get, &gipa, sizeof get);
    PFN_vkVoidFunction raw = get(host_instance, "vkCreateHeadlessSurfaceEXT");
    PFN_vkCreateHeadlessSurfaceEXT f;
    memcpy(&f, &raw, sizeof f);
    if (!f)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkHeadlessSurfaceCreateInfoEXT ci = { .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
    VkSurfaceKHR s = NULL;
    VkResult r = f(host_instance, &ci, NULL, &s);
    *surface = (uint64_t)(uintptr_t)s;
    return r;
}

static int32_t headless_create(void *user, void *host_instance, void *gipa, uint64_t guest_window, uint64_t *surface)
{
    (void)user;
    (void)guest_window;
    return headless_surface(host_instance, gipa, surface);
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

static rl_vk_surface_provider g_resizable = { .extent = resizable_extent };

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

/** @brief Whether the one PNG in dir is 64x64 with every pixel the clear color (0, 0.5, 1); removes it and dir. */
static bool capture_is_the_clear_color(const char *dir, int format)
{
    DIR *dp = opendir(dir);
    if (!dp)
        return false;
    char path[1024] = { 0 };
    struct dirent *e;
    while ((e = readdir(dp)))
        if (!strncmp(e->d_name, "frame-000000-", 13))
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    closedir(dp);
    FILE *f = path[0] ? fopen(path, "rb") : NULL;
    static uint8_t png[64 * 1024];
    size_t n = f ? fread(png, 1, sizeof png, f) : 0;
    if (f)
        fclose(f);
    remove(path);
    rmdir(dir);
    bool srgb = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
    uint8_t g = srgb ? 188 : 128;
    if (n < 8 + 25 + 8 + 2 + 5 || memcmp(png, "\x89PNG\r\n\x1a\n", 8) || memcmp(png + 12, "IHDR", 4))
        return false;
    uint32_t w = (uint32_t)png[16] << 24 | (uint32_t)png[17] << 16 | (uint32_t)png[18] << 8 | png[19];
    uint32_t h = (uint32_t)png[20] << 24 | (uint32_t)png[21] << 16 | (uint32_t)png[22] << 8 | png[23];
    if (w != 64 || h != 64 || memcmp(png + 37, "IDAT", 4))
        return false;
    const uint8_t *raw = png + 41 + 2 + 5;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = raw + y * (1 + w * 3);
        if (row[0] != 0)
            return false;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = row + 1 + x * 3;
            if (p[0] > 1 || p[1] < g - 1 || p[1] > g + 1 || p[2] < 254)
                return false;
        }
    }
    return true;
}

TEST(vk_vi_surface_presents_headless)
{
    g_resizable.instance_extensions = rl_vk_headless_surfaces()->instance_extensions;
    g_resizable.offscreen = true;
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
        char cap[512];
        const char *tmp = getenv("TMPDIR");
        snprintf(cap, sizeof cap, "%s/relift-vk-capture-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
        rl_vk_capture(cap);
        int32_t pr = VKR(G("vkQueuePresentKHR", queue, gput(&pi, sizeof pi)));
        rl_vk_capture(NULL);
        CHECK(pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR);
        CHECK(capture_is_the_clear_color(cap, fmt.format));
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

static int g_recreates, g_recreate_ready_after;
static PFN_vkAcquireNextImageKHR g_real_acquire;
static int g_lose_acquires;

static int32_t lost_recreate(void *user, void *host_instance, void *gipa, uint64_t surface, uint64_t *fresh)
{
    (void)user;
    (void)surface;
    if (++g_recreates < g_recreate_ready_after)
        return VK_NOT_READY;
    return headless_surface(host_instance, gipa, fresh);
}

static VkResult losing_acquire(VkDevice dev, VkSwapchainKHR sw, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *i)
{
    if (g_lose_acquires > 0) {
        g_lose_acquires--;
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    return g_real_acquire(dev, sw, timeout, sem, fence, i);
}

static rl_vk_surface_provider g_losable = { .instance_extensions = g_headless_exts, .create = headless_create, .recreate = lost_recreate };

static bool vi_surface_offered(void)
{
    uint64_t gn = galloc(4);
    if (VKR(G("vkEnumerateInstanceExtensionProperties", 0, gn, 0)) != VK_SUCCESS)
        return false;
    uint64_t props = galloc(g32(gn) * sizeof(VkExtensionProperties));
    if (VKR(G("vkEnumerateInstanceExtensionProperties", 0, gn, props)) != VK_SUCCESS)
        return false;
    for (uint32_t i = 0; i < g32(gn); i++)
        if (!strcmp((const char *)g_mem + props + i * sizeof(VkExtensionProperties), "VK_NN_vi_surface"))
            return true;
    return false;
}

TEST(vk_a_lost_host_surface_is_made_again_behind_the_guest)
{
    CHECK_EQ(boot(&g_losable), 0);
    if (!vi_surface_offered()) {
        fprintf(stderr, "  the host has no VK_EXT_headless_surface: lost surface test skipped\n");
        return;
    }
    const char *exts[] = { "VK_KHR_surface", "VK_NN_vi_surface" };
    CHECK(make_instance(exts, 2, 0) != 0);
    VkViSurfaceCreateInfoNN sci = { .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, .window = GP(galloc(64)) };
    uint64_t gs = galloc(8);
    CHECK_EQ(VKR(G("vkCreateViSurfaceNN", g_inst, gput(&sci, sizeof sci), 0, gs)), VK_SUCCESS);
    uint64_t surface = g64(gs);
    uint64_t pds[8];
    uint32_t np = physical_devices(pds, 8);
    unsigned tested = 0;
    for (uint32_t p = 0; p < np && !tested; p++) {
        uint32_t qf = queue_family(pds[p], VK_QUEUE_GRAPHICS_BIT);
        uint64_t gsup = galloc(4);
        if (qf == UINT32_MAX || VKR(G("vkGetPhysicalDeviceSurfaceSupportKHR", pds[p], qf, surface, gsup)) != VK_SUCCESS ||
            !g32(gsup))
            continue;
        const char *dexts[] = { "VK_KHR_swapchain" };
        uint64_t dev = make_device(pds[p], qf, dexts, 1);
        CHECK(dev != 0);
        VkSwapchainCreateInfoKHR swci = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = (VkSurfaceKHR)GP(surface),
            .minImageCount = 2,
            .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
            .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
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
        vkf_obj *d = vkf_obj_of(dev, VKG_KIND_VkDevice);
        g_real_acquire = (PFN_vkAcquireNextImageKHR)d->fns->f[VKG_vkAcquireNextImageKHR];
        d->fns->f[VKG_vkAcquireNextImageKHR] = (void (*)(void))losing_acquire;
        g_recreates = 0;
        g_recreate_ready_after = 3;
        g_lose_acquires = 1;
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        uint64_t gf = galloc(8);
        CHECK_EQ(VKR(G("vkCreateFence", dev, gput(&fci, sizeof fci), 0, gf)), VK_SUCCESS);
        uint64_t fence = g64(gf), gidx = galloc(4);
        uint32_t made0 = 0, hw, hh;
        CHECK(rl_vk_swapchain_host(sw, &hw, &hh, &made0));
        CHECK_EQ(VKR(G("vkAcquireNextImageKHR", dev, sw, 0, 0, fence, gidx)), VK_NOT_READY);
        CHECK_EQ(g_recreates, 1);
        CHECK_EQ(VKR(G("vkAcquireNextImageKHR", dev, sw, UINT64_MAX, 0, fence, gidx)), VK_SUCCESS);
        CHECK_EQ(g_recreates, 3);
        uint32_t made = 0;
        CHECK(rl_vk_swapchain_host(sw, &hw, &hh, &made));
        CHECK_EQ(made, made0 + 1);
        CHECK_EQ(VKR(G("vkWaitForFences", dev, 1, gput(&fence, 8), VK_TRUE, UINT64_MAX)), VK_SUCCESS);
        uint64_t gq = galloc(8);
        G("vkGetDeviceQueue", dev, qf, 0, gq);
        uint64_t queue = g64(gq);
        uint32_t idx = g32(gidx);
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1,
            .pSwapchains = GP(gput(&sw, 8)),
            .pImageIndices = GP(gput(&idx, 4)),
        };
        int32_t pr = VKR(G("vkQueuePresentKHR", queue, gput(&pi, sizeof pi)));
        CHECK(pr == VK_SUCCESS);
        d->fns->f[VKG_vkAcquireNextImageKHR] = (void (*)(void))g_real_acquire;
        CHECK_EQ(VKR(G("vkDeviceWaitIdle", dev)), VK_SUCCESS);
        G("vkDestroyFence", dev, fence, 0);
        G("vkDestroySwapchainKHR", dev, sw, 0);
        G("vkDestroyDevice", dev, 0);
        tested++;
    }
    G("vkDestroySurfaceKHR", g_inst, surface, 0);
    G("vkDestroyInstance", g_inst, 0);
    if (!tested)
        fprintf(stderr, "  no host device presents to a headless surface: lost surface test skipped\n");
    CHECK_EQ(rl_vk_live_handles(), 0);
}

static VkResult failing_semaphore(VkDevice dev, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *out)
{
    (void)dev;
    (void)ci;
    (void)a;
    (void)out;
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

TEST(vk_a_host_swapchain_without_its_semaphores_is_an_error)
{
    CHECK_EQ(boot(&g_losable), 0);
    if (!vi_surface_offered()) {
        fprintf(stderr, "  the host has no VK_EXT_headless_surface: swapchain semaphore test skipped\n");
        return;
    }
    const char *exts[] = { "VK_KHR_surface", "VK_NN_vi_surface" };
    CHECK(make_instance(exts, 2, 0) != 0);
    VkViSurfaceCreateInfoNN sci = { .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, .window = GP(galloc(64)) };
    uint64_t gs = galloc(8);
    CHECK_EQ(VKR(G("vkCreateViSurfaceNN", g_inst, gput(&sci, sizeof sci), 0, gs)), VK_SUCCESS);
    uint64_t surface = g64(gs);
    uint64_t pds[8];
    uint32_t np = physical_devices(pds, 8);
    unsigned tested = 0;
    for (uint32_t p = 0; p < np && !tested; p++) {
        uint32_t qf = queue_family(pds[p], VK_QUEUE_GRAPHICS_BIT);
        uint64_t gsup = galloc(4);
        if (qf == UINT32_MAX || VKR(G("vkGetPhysicalDeviceSurfaceSupportKHR", pds[p], qf, surface, gsup)) != VK_SUCCESS ||
            !g32(gsup))
            continue;
        const char *dexts[] = { "VK_KHR_swapchain" };
        uint64_t dev = make_device(pds[p], qf, dexts, 1);
        CHECK(dev != 0);
        VkSwapchainCreateInfoKHR swci = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = (VkSurfaceKHR)GP(surface),
            .minImageCount = 2,
            .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
            .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
            .imageExtent = { 64, 64 },
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_FIFO_KHR,
            .clipped = VK_TRUE,
        };
        vkf_obj *d = vkf_obj_of(dev, VKG_KIND_VkDevice);
        void (*real)(void) = d->fns->f[VKG_vkCreateSemaphore];
        d->fns->f[VKG_vkCreateSemaphore] = (void (*)(void))failing_semaphore;
        uint64_t gsw = galloc(8);
        CHECK(VKR(G("vkCreateSwapchainKHR", dev, gput(&swci, sizeof swci), 0, gsw)) < 0);
        d->fns->f[VKG_vkCreateSemaphore] = real;
        CHECK_EQ(VKR(G("vkCreateSwapchainKHR", dev, gput(&swci, sizeof swci), 0, gsw)), VK_SUCCESS);
        G("vkDestroySwapchainKHR", dev, g64(gsw), 0);
        G("vkDestroyDevice", dev, 0);
        tested++;
    }
    G("vkDestroySurfaceKHR", g_inst, surface, 0);
    G("vkDestroyInstance", g_inst, 0);
    if (!tested)
        fprintf(stderr, "  no host device presents to a headless surface: semaphore test skipped\n");
    CHECK_EQ(rl_vk_live_handles(), 0);
}

static PFN_vkQueuePresentKHR g_real_present;
static uint32_t g_suboptimal_presents;

static VkResult suboptimal_present(VkQueue q, const VkPresentInfoKHR *pi)
{
    VkResult r = g_real_present(q, pi);
    if (++g_suboptimal_presents >= 4 && rl_touchpad_paused())
        rl_touchpad_back();
    return r == VK_SUCCESS ? VK_SUBOPTIMAL_KHR : r;
}

static void *resume_later(void *arg)
{
    (void)arg;
    struct timespec d = { 0, 50000000 };
    for (int i = 0; i < 60 && rl_touchpad_paused(); i++)
        nanosleep(&d, NULL);
    if (rl_touchpad_paused())
        rl_touchpad_back();
    return NULL;
}

static const rl_touchpad_item g_pause_items[] = { { .shape = RL_TOUCHPAD_MENU, .x = 0.5f, .y = 0.05f, .w = 0.1f, .h = 0.05f } };
static const rl_touchpad_guest g_pause_guest = {
    .name = "vktest",
    .picture_width = 64,
    .picture_height = 64,
    .portrait = { 64, 64, g_pause_items, 1, NULL, 0 },
    .landscape = { 64, 64, g_pause_items, 1, NULL, 0 },
};

TEST(vk_a_paused_frame_repaints_through_suboptimal_presents)
{
    CHECK_EQ(boot(&g_losable), 0);
    if (!vi_surface_offered()) {
        fprintf(stderr, "  the host has no VK_EXT_headless_surface: paused repaint test skipped\n");
        return;
    }
    rl_touchpad_declare(&g_pause_guest);
    rl_touchpad_set_touch_screen(true);
    rl_touchpad_view(64, 64, 1.0f, NULL);
    const char *exts[] = { "VK_KHR_surface", "VK_NN_vi_surface" };
    CHECK(make_instance(exts, 2, 0) != 0);
    VkViSurfaceCreateInfoNN sci = { .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, .window = GP(galloc(64)) };
    uint64_t gs = galloc(8);
    CHECK_EQ(VKR(G("vkCreateViSurfaceNN", g_inst, gput(&sci, sizeof sci), 0, gs)), VK_SUCCESS);
    uint64_t surface = g64(gs);
    uint64_t pds[8];
    uint32_t np = physical_devices(pds, 8);
    unsigned tested = 0;
    for (uint32_t p = 0; p < np && !tested; p++) {
        uint32_t qf = queue_family(pds[p], VK_QUEUE_GRAPHICS_BIT);
        uint64_t gsup = galloc(4);
        if (qf == UINT32_MAX || VKR(G("vkGetPhysicalDeviceSurfaceSupportKHR", pds[p], qf, surface, gsup)) != VK_SUCCESS ||
            !g32(gsup))
            continue;
        const char *dexts[] = { "VK_KHR_swapchain" };
        uint64_t dev = make_device(pds[p], qf, dexts, 1);
        CHECK(dev != 0);
        VkSwapchainCreateInfoKHR swci = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = (VkSurfaceKHR)GP(surface),
            .minImageCount = 2,
            .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
            .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
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
        vkf_obj *d = vkf_obj_of(dev, VKG_KIND_VkDevice);
        g_real_present = (PFN_vkQueuePresentKHR)d->fns->f[VKG_vkQueuePresentKHR];
        d->fns->f[VKG_vkQueuePresentKHR] = (void (*)(void))suboptimal_present;
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        uint64_t gf = galloc(8);
        CHECK_EQ(VKR(G("vkCreateFence", dev, gput(&fci, sizeof fci), 0, gf)), VK_SUCCESS);
        uint64_t fence = g64(gf), gidx = galloc(4);
        CHECK_EQ(VKR(G("vkAcquireNextImageKHR", dev, sw, UINT64_MAX, 0, fence, gidx)), VK_SUCCESS);
        CHECK_EQ(VKR(G("vkWaitForFences", dev, 1, gput(&fence, 8), VK_TRUE, UINT64_MAX)), VK_SUCCESS);
        uint64_t gq = galloc(8);
        G("vkGetDeviceQueue", dev, qf, 0, gq);
        uint64_t queue = g64(gq);
        uint32_t idx = g32(gidx), made0 = 0, made = 0, hw, hh;
        CHECK(rl_vk_swapchain_host(sw, &hw, &hh, &made0));
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1,
            .pSwapchains = GP(gput(&sw, 8)),
            .pImageIndices = GP(gput(&idx, 4)),
        };
        g_suboptimal_presents = 0;
        rl_touchpad_back();
        CHECK(rl_touchpad_paused());
        pthread_t watchdog;
        CHECK_EQ(pthread_create(&watchdog, NULL, resume_later, NULL), 0);
        CHECK_EQ(VKR(G("vkQueuePresentKHR", queue, gput(&pi, sizeof pi))), VK_SUCCESS);
        pthread_join(watchdog, NULL);
        CHECK(!rl_touchpad_paused());
        CHECK(rl_vk_swapchain_repaints(sw) >= 3);
        CHECK(g_suboptimal_presents >= 4);
        CHECK(rl_vk_swapchain_host(sw, &hw, &hh, &made));
        CHECK_EQ(made, made0);
        d->fns->f[VKG_vkQueuePresentKHR] = (void (*)(void))g_real_present;
        CHECK_EQ(VKR(G("vkDeviceWaitIdle", dev)), VK_SUCCESS);
        G("vkDestroyFence", dev, fence, 0);
        G("vkDestroySwapchainKHR", dev, sw, 0);
        G("vkDestroyDevice", dev, 0);
        tested++;
    }
    G("vkDestroySurfaceKHR", g_inst, surface, 0);
    G("vkDestroyInstance", g_inst, 0);
    rl_touchpad_reset();
    if (!tested)
        fprintf(stderr, "  no host device presents to a headless surface: paused repaint test skipped\n");
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

/* A fake host driver: the real one without VK_EXT_headless_surface (NVIDIA's), counting surface calls that reach it. */
static PFN_vkEnumerateInstanceExtensionProperties g_real_inst_exts;
static PFN_vkCreateInstance g_real_create_instance;
static PFN_vkDestroySurfaceKHR g_real_destroy_surface;
static PFN_vkGetPhysicalDeviceSurfaceFormats2KHR g_real_formats2;
static PFN_vkGetPhysicalDevicePresentRectanglesKHR g_real_rects;
static unsigned g_host_surface_calls;

static VkResult noheadless_inst_exts(const char *layer, uint32_t *n, VkExtensionProperties *p)
{
    uint32_t all = 0;
    VkResult r = g_real_inst_exts(layer, &all, NULL);
    if (r < 0)
        return r;
    VkExtensionProperties *e = calloc(all + 1, sizeof *e);
    r = g_real_inst_exts(layer, &all, e);
    uint32_t m = 0;
    for (uint32_t i = 0; i < all; i++)
        if (strcmp(e[i].extensionName, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME))
            e[m++] = e[i];
    if (!p)
        *n = m;
    else {
        uint32_t w = *n < m ? *n : m;
        memcpy(p, e, w * sizeof *e);
        r = w < m ? VK_INCOMPLETE : r;
        *n = w;
    }
    free(e);
    return r;
}

static VkResult noheadless_create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out)
{
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME))
            return VK_ERROR_EXTENSION_NOT_PRESENT;
    return g_real_create_instance(ci, a, out);
}

static void counting_destroy_surface(VkInstance inst, VkSurfaceKHR s, const VkAllocationCallbacks *a)
{
    if (s) {
        g_host_surface_calls++;
        g_real_destroy_surface(inst, s, a);
    }
}

static VkResult counting_formats2(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *i, uint32_t *n, VkSurfaceFormat2KHR *f)
{
    g_host_surface_calls++;
    return g_real_formats2(pd, i, n, f);
}

static VkResult counting_rects(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkRect2D *r)
{
    g_host_surface_calls++;
    return g_real_rects(pd, s, n, r);
}

static void *noheadless_gipa(void *inst, const char *name)
{
    if (!strcmp(name, "vkCreateHeadlessSurfaceEXT"))
        return NULL;
    void *r = g_real_gipa(inst, name);
    if (!r)
        return r;
    void (*f)(void) = NULL;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
        memcpy(&g_real_inst_exts, &r, sizeof r), f = (void (*)(void))noheadless_inst_exts;
    else if (!strcmp(name, "vkCreateInstance"))
        memcpy(&g_real_create_instance, &r, sizeof r), f = (void (*)(void))noheadless_create_instance;
    else if (!strcmp(name, "vkDestroySurfaceKHR"))
        memcpy(&g_real_destroy_surface, &r, sizeof r), f = (void (*)(void))counting_destroy_surface;
    else if (!strcmp(name, "vkGetPhysicalDeviceSurfaceFormats2KHR"))
        memcpy(&g_real_formats2, &r, sizeof r), f = (void (*)(void))counting_formats2;
    else if (!strcmp(name, "vkGetPhysicalDevicePresentRectanglesKHR"))
        memcpy(&g_real_rects, &r, sizeof r), f = (void (*)(void))counting_rects;
    if (f)
        memcpy(&r, &f, sizeof r);
    return r;
}

TEST(vk_vi_surface_works_offscreen_without_headless_surface)
{
    void *real = rl_hostlib_vulkan_gipa();
    memcpy(&g_real_gipa, &real, sizeof real);
    void *(*fg)(void *, const char *) = noheadless_gipa;
    void *fake;
    memcpy(&fake, &fg, sizeof fake);
    rl_vk_surface_provider offscreen = *rl_vk_headless_surfaces();
    offscreen.extent = resizable_extent;
    g_host_w = g_host_h = 64;
    g_host_gen = 0;
    g_host_surface_calls = 0;
    CHECK_EQ(boot_with(&offscreen, fake), 0);
    uint64_t gn = galloc(4);
    CHECK_EQ(VKR(G("vkEnumerateInstanceExtensionProperties", 0, gn, 0)), VK_SUCCESS);
    uint32_t n = g32(gn);
    uint64_t props = galloc(n * sizeof(VkExtensionProperties));
    CHECK_EQ(VKR(G("vkEnumerateInstanceExtensionProperties", 0, gn, props)), VK_SUCCESS);
    bool vi = false;
    for (uint32_t i = 0; i < g32(gn); i++)
        vi = vi || !strcmp((const char *)g_mem + props + i * sizeof(VkExtensionProperties), "VK_NN_vi_surface");
    CHECK(vi);
    const char *exts[] = { "VK_KHR_surface", "VK_KHR_get_surface_capabilities2", "VK_NN_vi_surface" };
    CHECK(make_instance(exts, 3, 0) != 0);
    CHECK(gipa(g_inst, "vkCreateViSurfaceNN") != 0);
    VkViSurfaceCreateInfoNN sci = { .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, .window = GP(galloc(64)) };
    uint64_t gs = galloc(8);
    CHECK_EQ(VKR(G("vkCreateViSurfaceNN", g_inst, gput(&sci, sizeof sci), 0, gs)), VK_SUCCESS);
    uint64_t surface = g64(gs);
    CHECK(surface != 0);
    uint64_t pds[8];
    CHECK_EQ(physical_devices(pds, 8), 1);
    char name[256];
    device_name(pds[0], name);
    uint32_t qf = queue_family(pds[0], VK_QUEUE_GRAPHICS_BIT);
    CHECK(qf != UINT32_MAX);
    uint64_t gsup = galloc(4);
    CHECK_EQ(VKR(G("vkGetPhysicalDeviceSurfaceSupportKHR", pds[0], qf, surface, gsup)), VK_SUCCESS);
    CHECK_EQ(g32(gsup), VK_TRUE);
    uint64_t gcaps = galloc(sizeof(VkSurfaceCapabilitiesKHR));
    CHECK_EQ(VKR(G("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", pds[0], surface, gcaps)), VK_SUCCESS);
    VkSurfaceCapabilitiesKHR caps;
    memcpy(&caps, g_mem + gcaps, sizeof caps);
    CHECK_EQ(caps.currentExtent.width, 64);
    CHECK_EQ(caps.currentExtent.height, 64);
    uint64_t gfn = galloc(4), gfmts = galloc(2 * sizeof(VkSurfaceFormatKHR));
    rl_st32(g_mem, gfn, 2);
    CHECK_EQ(VKR(G("vkGetPhysicalDeviceSurfaceFormatsKHR", pds[0], surface, gfn, gfmts)), VK_SUCCESS);
    CHECK_EQ(g32(gfn), 2);
    CHECK_EQ(g32(gfmts), VK_FORMAT_R8G8B8A8_SRGB);
    VkPhysicalDeviceSurfaceInfo2KHR si2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, .surface = (VkSurfaceKHR)GP(surface) };
    uint64_t gsi2 = gput(&si2, sizeof si2);
    CHECK_EQ(VKR(G("vkGetPhysicalDeviceSurfaceFormats2KHR", pds[0], gsi2, gfn, 0)), VK_SUCCESS);
    CHECK_EQ(g32(gfn), 2);
    VkSurfaceFormat2KHR f2[2] = { { .sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR }, { .sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR } };
    uint64_t gf2 = gput(f2, sizeof f2);
    CHECK_EQ(VKR(G("vkGetPhysicalDeviceSurfaceFormats2KHR", pds[0], gsi2, gfn, gf2)), VK_SUCCESS);
    CHECK_EQ(g32(gf2 + offsetof(VkSurfaceFormat2KHR, surfaceFormat)), VK_FORMAT_R8G8B8A8_SRGB);
    CHECK_EQ(g32(gf2 + sizeof(VkSurfaceFormat2KHR) + offsetof(VkSurfaceFormat2KHR, surfaceFormat)), VK_FORMAT_R8G8B8A8_UNORM);
    uint64_t grn = galloc(4), grects = galloc(4 * sizeof(VkRect2D));
    CHECK_EQ(VKR(G("vkGetPhysicalDevicePresentRectanglesKHR", pds[0], surface, grn, 0)), VK_SUCCESS);
    CHECK_EQ(g32(grn), 1);
    CHECK_EQ(VKR(G("vkGetPhysicalDevicePresentRectanglesKHR", pds[0], surface, grn, grects)), VK_SUCCESS);
    CHECK_EQ(g32(grects + offsetof(VkRect2D, extent)), 64);
    const char *dexts[] = { "VK_KHR_swapchain" };
    uint64_t dev = make_device(pds[0], qf, dexts, 1);
    CHECK(dev != 0);
    uint64_t gmodes = galloc(4);
    CHECK_EQ(VKR(G("vkGetDeviceGroupSurfacePresentModesKHR", dev, surface, gmodes)), VK_SUCCESS);
    CHECK_EQ(g32(gmodes), VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR);
    VkSwapchainCreateInfoKHR swci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = (VkSurfaceKHR)GP(surface),
        .minImageCount = 2,
        .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
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
    uint64_t gimgs = galloc(8ull * g32(gin));
    CHECK_EQ(VKR(G("vkGetSwapchainImagesKHR", dev, sw, gin, gimgs)), VK_SUCCESS);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    uint64_t gf = galloc(8);
    CHECK_EQ(VKR(G("vkCreateFence", dev, gput(&fci, sizeof fci), 0, gf)), VK_SUCCESS);
    uint64_t fence = g64(gf), gidx = galloc(4);
    CHECK_EQ(VKR(G("vkAcquireNextImageKHR", dev, sw, UINT64_MAX, 0, fence, gidx)), VK_SUCCESS);
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
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = GP(gput(&sw, 8)),
        .pImageIndices = GP(gput(&idx, 4)),
    };
    char cap[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(cap, sizeof cap, "%s/relift-vk-capture-nh-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    uint64_t frames = vi_guest_frames();
    rl_vk_capture(cap);
    CHECK_EQ(VKR(G("vkQueuePresentKHR", queue, gput(&pi, sizeof pi))), VK_SUCCESS);
    rl_vk_capture(NULL);
    CHECK_EQ(vi_guest_frames(), frames + 1);
    CHECK(capture_is_the_clear_color(cap, VK_FORMAT_R8G8B8A8_UNORM));
    uint32_t hw = 0, hh = 0, made = 0;
    CHECK(rl_vk_swapchain_host(sw, &hw, &hh, &made));
    CHECK_EQ(hw, 64);
    fprintf(stderr, "  %s: presented offscreen without VK_EXT_headless_surface\n", name);
    CHECK_EQ(VKR(G("vkDeviceWaitIdle", dev)), VK_SUCCESS);
    G("vkDestroyCommandPool", dev, g64(gpool), 0);
    G("vkDestroyFence", dev, fence, 0);
    G("vkDestroySwapchainKHR", dev, sw, 0);
    G("vkDestroyDevice", dev, 0);
    G("vkDestroySurfaceKHR", g_inst, surface, 0);
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(g_host_surface_calls, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
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

TEST(vk_memory_types_are_ones_buffers_and_images_may_use)
{
    VkPhysicalDeviceMemoryProperties h = { .memoryTypeCount = 7, .memoryHeapCount = 1 };
    h.memoryHeaps[0] = (VkMemoryHeap){ .size = 8ull << 30, .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT };
    const uint32_t dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const uint32_t coh = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const uint32_t flags[7] = { dl, dl, dl, dl | hv | cached, dl | hv | coh, dl | hv | coh | cached, dl | hv | coh | cached };
    for (uint32_t t = 0; t < 7; t++)
        h.memoryTypes[t] = (VkMemoryType){ .propertyFlags = flags[t], .heapIndex = 0 };
    const VkPhysicalDeviceMemoryProperties *s = &vkf_switch.memory;
    uint8_t any[VK_MAX_MEMORY_TYPES], used[VK_MAX_MEMORY_TYPES];
    vkf_mem_pick(&h, s, ~0u, ~0u, any);
    vkf_mem_pick(&h, s, 0x41u, 0x1u, used);
    vkf_dev d;
    memset(&d, 0, sizeof d);
    memcpy(d.host_type, used, sizeof used);
    for (uint32_t i = 0; i < s->memoryTypeCount; i++) {
        uint32_t want = s->memoryTypes[i].propertyFlags & (hv | coh | cached);
        CHECK_EQ(h.memoryTypes[used[i]].propertyFlags & want, want);
        CHECK(0x41u & (1u << used[i]));
        if (s->memoryTypes[i].propertyFlags & hv)
            CHECK_EQ(used[i], 6);
        else
            CHECK_EQ(used[i], 0);
    }
    bool moved = false;
    for (uint32_t i = 0; i < s->memoryTypeCount; i++)
        moved = moved || any[i] != used[i];
    CHECK(moved);
    CHECK_EQ(vkf_guest_type_bits(&d, 0x41u), (1u << s->memoryTypeCount) - 1u);
    uint32_t local = 0;
    for (uint32_t i = 0; i < s->memoryTypeCount; i++)
        if (!(s->memoryTypes[i].propertyFlags & hv))
            local |= 1u << i;
    CHECK_EQ(vkf_guest_type_bits(&d, 0x1u), local);
}

TEST(vk_a_failing_result_is_logged_once_with_its_caller)
{
    static rl_ctx c;
    c.x[30] = 0x8123456;
    vkf_call k;
    memset(&k, 0, sizeof k);
    k.c = &c;
    int fds[2];
    fflush(stderr);
    CHECK_EQ(pipe(fds), 0);
    int saved = dup(2);
    dup2(fds[1], 2);
    CHECK_EQ(vkf_result(&k, VKG_vkCreateBuffer, VK_SUCCESS), VK_SUCCESS);
    CHECK_EQ(vkf_result(&k, VKG_vkCreateBuffer, VK_INCOMPLETE), VK_INCOMPLETE);
    CHECK_EQ(vkf_result(&k, VKG_vkCreateBuffer, VK_ERROR_FEATURE_NOT_PRESENT), VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK_EQ(vkf_result(&k, VKG_vkCreateBuffer, VK_ERROR_FEATURE_NOT_PRESENT), VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK_EQ(vkf_result(&k, VKG_vkCreateImage, VK_ERROR_FEATURE_NOT_PRESENT), VK_ERROR_FEATURE_NOT_PRESENT);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(fds[1]);
    static char out[4096];
    ssize_t got = read(fds[0], out, sizeof out - 1);
    out[got > 0 ? got : 0] = 0;
    close(fds[0]);
    const char *one = "vkCreateBuffer: VK_ERROR_FEATURE_NOT_PRESENT (-8), called from 0x8123456";
    const char *at = strstr(out, one);
    CHECK(at != NULL);
    CHECK(at && !strstr(at + 1, one));
    CHECK(strstr(out, "vkCreateImage: VK_ERROR_FEATURE_NOT_PRESENT (-8)") != NULL);
    CHECK(!strstr(out, "VK_INCOMPLETE") && !strstr(out, "(5)"));
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

static void keep_render_pass(uint64_t dev, bool two, uint64_t *pass)
{
    uint64_t out = galloc(8);
    *pass = 0;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = GP(gput(&ref, sizeof ref)),
    };
    VkAttachmentDescription a = {
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    if (!two) {
        VkRenderPassCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = GP(gput(&a, sizeof a)),
            .subpassCount = 1,
            .pSubpasses = GP(gput(&sub, sizeof sub)),
        };
        uint64_t gci = gput(&ci, sizeof ci);
        uint64_t gatt = (uint64_t)(uintptr_t)ci.pAttachments;
        CHECK_EQ(VKR(G("vkCreateRenderPass", dev, gci, 0, out)), VK_SUCCESS);
        CHECK_EQ(g32(gatt + offsetof(VkAttachmentDescription, loadOp)), VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        *pass = g64(out);
        return;
    }
    VkAttachmentReference2 ref2 = {
        .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    };
    VkSubpassDescription2 sub2 = {
        .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = GP(gput(&ref2, sizeof ref2)),
    };
    VkAttachmentDescription2 a2 = {
        .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
        .format = a.format,
        .samples = a.samples,
        .loadOp = a.loadOp,
        .storeOp = a.storeOp,
        .stencilLoadOp = a.stencilLoadOp,
        .stencilStoreOp = a.stencilStoreOp,
        .initialLayout = a.initialLayout,
        .finalLayout = a.finalLayout,
    };
    VkRenderPassCreateInfo2 ci2 = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
        .attachmentCount = 1,
        .pAttachments = GP(gput(&a2, sizeof a2)),
        .subpassCount = 1,
        .pSubpasses = GP(gput(&sub2, sizeof sub2)),
    };
    CHECK_EQ(VKR(G("vkCreateRenderPass2", dev, gput(&ci2, sizeof ci2), 0, out)), VK_SUCCESS);
    *pass = g64(out);
}

/** @brief Draws half of a known image in a DONT_CARE render pass; *kept is the pixels of the other half that kept their contents. */
static void dont_care_scene(const char *keep, bool two, char *name, uint32_t *kept)
{
    *kept = 0;
    CHECK_EQ(rl_settings_set("vk.keep_attachments", keep), RL_SETTING_OK);
    CHECK(make_instance(NULL, 0, 0) != 0);
    uint64_t pds[8];
    CHECK(physical_devices(pds, 8) > 0);
    uint64_t pd = pds[0];
    device_name(pd, name);
    uint32_t qf = queue_family(pd, VK_QUEUE_GRAPHICS_BIT);
    uint64_t dev = make_device(pd, qf, NULL, 0);
    CHECK(dev != 0);
    uint64_t gq = galloc(8);
    G("vkGetDeviceQueue", dev, qf, 0, gq);
    uint64_t queue = g64(gq);
    mapped_buf rb;
    CHECK_EQ(make_mapped(pd, dev, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb), 0);
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    uint64_t gi = galloc(8);
    CHECK_EQ(VKR(G("vkCreateImage", dev, gput(&ici, sizeof ici), 0, gi)), VK_SUCCESS);
    uint64_t image = g64(gi);
    uint64_t greq = galloc(sizeof(VkMemoryRequirements));
    G("vkGetImageMemoryRequirements", dev, image, greq);
    VkMemoryRequirements req;
    memcpy(&req, g_mem + greq, sizeof req);
    uint32_t t = memory_type(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = t };
    uint64_t gim = galloc(8);
    CHECK_EQ(VKR(G("vkAllocateMemory", dev, gput(&mai, sizeof mai), 0, gim)), VK_SUCCESS);
    CHECK_EQ(VKR(G("vkBindImageMemory", dev, image, g64(gim), 0)), VK_SUCCESS);
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = (VkImage)GP(image),
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    uint64_t gv = galloc(8);
    CHECK_EQ(VKR(G("vkCreateImageView", dev, gput(&vci, sizeof vci), 0, gv)), VK_SUCCESS);
    uint64_t view = g64(gv);
    uint64_t pass;
    keep_render_pass(dev, two, &pass);
    CHECK(pass != 0);
    VkFramebufferCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = (VkRenderPass)GP(pass),
        .attachmentCount = 1,
        .pAttachments = GP(gput(&view, 8)),
        .width = W,
        .height = H,
        .layers = 1,
    };
    uint64_t gfb = galloc(8);
    CHECK_EQ(VKR(G("vkCreateFramebuffer", dev, gput(&fci, sizeof fci), 0, gfb)), VK_SUCCESS);
    uint64_t fb = g64(gfb);

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
    VkClearColorValue green = { .float32 = { 0.0f, 1.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    G("vkCmdClearColorImage", cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, gput(&green, sizeof green), 1, gput(&range, sizeof range));
    barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderPassBeginInfo rbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = (VkRenderPass)GP(pass),
        .framebuffer = (VkFramebuffer)GP(fb),
        .renderArea = { { 0, 0 }, { W, H } },
    };
    G("vkCmdBeginRenderPass", cb, gput(&rbi, sizeof rbi), VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .clearValue.color.float32 = { 1.0f, 0.0f, 0.0f, 1.0f } };
    VkClearRect rect = { .rect = { { 0, 0 }, { W / 2, H } }, .layerCount = 1 };
    G("vkCmdClearAttachments", cb, 1, gput(&ca, sizeof ca), 1, gput(&rect, sizeof rect));
    G("vkCmdEndRenderPass", cb);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    G("vkCmdCopyImageToBuffer", cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, gput(&bic, sizeof bic));
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    G("vkCmdPipelineBarrier", cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, gput(&mb, sizeof mb), 0, 0, 0, 0);
    CHECK_EQ(VKR(G("vkEndCommandBuffer", cb)), VK_SUCCESS);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = GP(gput(&cb, 8)) };
    CHECK_EQ(VKR(G("vkQueueSubmit", queue, 1, gput(&si, sizeof si), 0)), VK_SUCCESS);
    CHECK_EQ(VKR(G("vkQueueWaitIdle", queue)), VK_SUCCESS);

    uint32_t drawn = 0, left = 0;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint32_t v = g32(rb.ptr + 4 * (y * W + x));
            if (x < W / 2)
                drawn += v == 0xFF0000FFu;
            else
                left += v == 0xFF00FF00u;
        }
    CHECK_EQ(drawn, W * H / 2);
    G("vkDestroyCommandPool", dev, g64(gpool), 0);
    G("vkDestroyFramebuffer", dev, fb, 0);
    G("vkDestroyRenderPass", dev, pass, 0);
    G("vkDestroyImageView", dev, view, 0);
    G("vkDestroyImage", dev, image, 0);
    G("vkFreeMemory", dev, g64(gim), 0);
    G("vkDestroyBuffer", dev, rb.buf, 0);
    G("vkFreeMemory", dev, rb.mem, 0);
    G("vkDestroyDevice", dev, 0);
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
    *kept = left;
}

TEST(vk_dont_care_attachments_keep_contents)
{
    CHECK_EQ(boot(NULL), 0);
    rl_switch_declare_settings();
    char name[256];
    for (int two = 0; two < 2; two++) {
        uint32_t kept;
        dont_care_scene("off", two, name, &kept);
        fprintf(stderr, "  %s: render pass%s with the guest's DONT_CARE ops kept %u of %u pixels\n", name, two ? "2" : "", kept,
                W * H / 2);
        dont_care_scene("on", two, name, &kept);
        CHECK_EQ(kept, W * H / 2);
        dont_care_scene("auto", two, name, &kept);
        CHECK_EQ(kept, W * H / 2);
    }
    rl_settings_reset();
}

static uint32_t g_seen_bindings[4], g_seen_counts[4], g_seen_flags[4], g_seen_n;
static bool g_seen_sampler;
static PFN_vkCreateDescriptorSetLayout g_real_layout;

static VkResult seen_layout(VkDevice dev, const VkDescriptorSetLayoutCreateInfo *ci,
                            const VkAllocationCallbacks *a, VkDescriptorSetLayout *out)
{
    g_seen_n = ci->bindingCount;
    for (uint32_t i = 0; i < ci->bindingCount && i < 4; i++) {
        g_seen_bindings[i] = ci->pBindings[i].binding;
        g_seen_counts[i] = ci->pBindings[i].descriptorCount;
        if (ci->pBindings[i].pImmutableSamplers)
            g_seen_sampler = ci->pBindings[i].binding == 7 && ci->pBindings[i].pImmutableSamplers[0] != NULL;
    }
    const VkDescriptorSetLayoutBindingFlagsCreateInfo *f = ci->pNext;
    for (uint32_t i = 0; f && i < f->bindingCount && i < 4; i++)
        g_seen_flags[i] = f->pBindingFlags[i];
    VkDescriptorSetLayoutCreateInfo plain = *ci;
    plain.pNext = NULL;
    return g_real_layout(dev, &plain, a, out);
}

TEST(vk_set_layout_bindings_reach_the_host_in_order)
{
    CHECK_EQ(boot(NULL), 0);
    CHECK(make_instance(NULL, 0, 0) != 0);
    uint64_t pds[8];
    CHECK(physical_devices(pds, 8) > 0);
    uint64_t dev = make_device(pds[0], queue_family(pds[0], 0), NULL, 0);
    CHECK(dev != 0);
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .maxLod = 1.0f };
    uint64_t gs = galloc(8);
    CHECK_EQ(VKR(G("vkCreateSampler", dev, gput(&sci, sizeof sci), 0, gs)), VK_SUCCESS);
    uint64_t sampler = g64(gs);
    VkDescriptorSetLayoutBinding b[3] = {
        { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_ALL },
        { .binding = 8, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 64,
          .stageFlags = VK_SHADER_STAGE_ALL },
        { .binding = 7, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_ALL, .pImmutableSamplers = GP(gput(&sampler, sizeof sampler)) },
    };
    uint64_t gb = gput(b, sizeof b);
    const VkDescriptorBindingFlags flags[3] = { 1, 2, 4 };
    uint64_t gf = gput(flags, sizeof flags);
    VkDescriptorSetLayoutBindingFlagsCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 3,
        .pBindingFlags = GP(gf),
    };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = GP(gput(&fci, sizeof fci)),
        .bindingCount = 3,
        .pBindings = GP(gb),
    };
    vkf_obj *o = vkf_obj_of(dev, VKG_KIND_VkDevice);
    CHECK(o != NULL);
    g_real_layout = (PFN_vkCreateDescriptorSetLayout)o->fns->f[VKG_vkCreateDescriptorSetLayout];
    o->fns->f[VKG_vkCreateDescriptorSetLayout] = (void (*)(void))seen_layout;
    uint64_t gl = galloc(8);
    CHECK_EQ(VKR(G("vkCreateDescriptorSetLayout", dev, gput(&lci, sizeof lci), 0, gl)), VK_SUCCESS);
    o->fns->f[VKG_vkCreateDescriptorSetLayout] = (void (*)(void))g_real_layout;
    CHECK_EQ(g_seen_n, 3);
    CHECK_EQ(g_seen_bindings[0], 3);
    CHECK_EQ(g_seen_bindings[1], 7);
    CHECK_EQ(g_seen_bindings[2], 8);
    CHECK_EQ(g_seen_counts[1], 1);
    CHECK_EQ(g_seen_counts[2], 64);
    CHECK(g_seen_sampler);
    CHECK_EQ(g_seen_flags[0], 1);
    CHECK_EQ(g_seen_flags[1], 4);
    CHECK_EQ(g_seen_flags[2], 2);
    CHECK_EQ(memcmp(g_mem + gb, b, sizeof b), 0);
    CHECK_EQ(memcmp(g_mem + gf, flags, sizeof flags), 0);
    lci.pNext = NULL;
    VkDescriptorSetLayoutSupport sup = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT };
    uint64_t gsup = gput(&sup, sizeof sup);
    G("vkGetDescriptorSetLayoutSupport", dev, gput(&lci, sizeof lci), gsup);
    CHECK_EQ(rl_ld32(g_mem, gsup + 16), VK_TRUE);
    G("vkDestroyDescriptorSetLayout", dev, g64(gl), 0);
    G("vkDestroySampler", dev, sampler, 0);
    G("vkDestroyDevice", dev, 0);
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
}

static PFN_vkBeginCommandBuffer g_real_begin;
static bool g_begin_inherits, g_begin_bare;
static VkCommandBuffer g_begin_cb;

static VkResult seen_begin(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    g_begin_cb = cb;
    g_begin_bare = !bi->pInheritanceInfo;
    g_begin_inherits = bi->pInheritanceInfo &&
                       bi->pInheritanceInfo->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO &&
                       !bi->pInheritanceInfo->renderPass;
    return g_real_begin(cb, bi);
}

static void record_fill_bare(uint64_t cb, uint64_t buf, uint32_t value, const VkCommandBufferInheritanceInfo *inh)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pInheritanceInfo = inh ? GP(gput(inh, sizeof *inh)) : NULL,
    };
    uint64_t gbi = gput(&bi, sizeof bi);
    CHECK_EQ(VKR(G("vkBeginCommandBuffer", cb, gbi)), VK_SUCCESS);
    if (!inh)
        CHECK_EQ(g64(gbi + 24), 0);
    G("vkCmdFillBuffer", cb, buf, 0, VK_WHOLE_SIZE, value);
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    G("vkCmdPipelineBarrier", cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
      gput(&mb, sizeof mb), 0, 0, 0, 0);
    CHECK_EQ(VKR(G("vkEndCommandBuffer", cb)), VK_SUCCESS);
}

TEST(vk_secondary_begun_without_inheritance_info_records_into_a_primary)
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
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
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
    uint64_t cb = g64(gcb);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    uint64_t gf = galloc(8);
    CHECK_EQ(VKR(G("vkCreateFence", dev, gput(&fci, sizeof fci), 0, gf)), VK_SUCCESS);
    uint64_t fence = g64(gf);
    vkf_obj *o = vkf_obj_of(cb, VKG_KIND_VkCommandBuffer);
    CHECK(o != NULL);
    void *secondary = o->host;
    g_real_begin = (PFN_vkBeginCommandBuffer)o->fns->f[VKG_vkBeginCommandBuffer];
    o->fns->f[VKG_vkBeginCommandBuffer] = (void (*)(void))seen_begin;
    record_fill_bare(cb, b.buf, 0x33333333u, NULL);
    CHECK(g_begin_bare);
    CHECK(o->as_primary);
    CHECK(o->host != secondary && o->other == secondary);
    CHECK(g_begin_cb == o->host);
    CHECK(vkf_obj_find_host(VKG_KIND_VkCommandBuffer, o->host) == o);
    CHECK(vkf_obj_find_host(VKG_KIND_VkCommandBuffer, secondary) == NULL);
    CHECK_EQ(submit_and_read(dev, queue, cb, fence, &b, false), 0x33333333u);
    CHECK_EQ(submit_and_read(dev, queue, cb, fence, &b, true), 0x33333333u);
    VkCommandBufferInheritanceInfo inh = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
    record_fill_bare(cb, b.buf, 0x44444444u, &inh);
    CHECK(g_begin_inherits);
    CHECK(!o->as_primary);
    CHECK(o->host == secondary && g_begin_cb == secondary);
    CHECK_EQ(submit_and_read(dev, queue, cb, fence, &b, false), 0x44444444u);
    record_fill_bare(cb, b.buf, 0x55555555u, NULL);
    CHECK(o->as_primary && g_begin_bare);
    CHECK_EQ(submit_and_read(dev, queue, cb, fence, &b, false), 0x55555555u);
    o->fns->f[VKG_vkBeginCommandBuffer] = (void (*)(void))g_real_begin;
    G("vkFreeCommandBuffers", dev, g64(gpool), 1, gcb);
    record_fill_bare(g64(gcb + 8), b.buf, 0x66666666u, NULL);
    CHECK_EQ(submit_and_read(dev, queue, g64(gcb + 8), fence, &b, false), 0x66666666u);
    G("vkDestroyFence", dev, fence, 0);
    G("vkDestroyCommandPool", dev, g64(gpool), 0);
    G("vkDestroyBuffer", dev, b.buf, 0);
    G("vkFreeMemory", dev, b.mem, 0);
    G("vkDestroyDevice", dev, 0);
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
}

static void bits_put(uint8_t *b, unsigned *at, uint32_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++, (*at)++)
        if (v >> i & 1)
            b[*at / 8] |= (uint8_t)(1u << (*at % 8));
}

/** @brief A BC7 mode 6 block: endpoint 0 red, endpoint 1 green, both opaque; texel 0 takes endpoint 0, the rest endpoint 1. */
static void bc7_red_green(uint8_t *b)
{
    memset(b, 0, 16);
    unsigned at = 0;
    bits_put(b, &at, 1u << 6, 7);
    const uint32_t ep[8] = { 127, 0, 0, 127, 0, 0, 127, 127 };
    for (int i = 0; i < 8; i++)
        bits_put(b, &at, ep[i], 7);
    bits_put(b, &at, 1, 1);
    bits_put(b, &at, 1, 1);
    bits_put(b, &at, 0, 3);
    for (int i = 1; i < 16; i++)
        bits_put(b, &at, 15, 4);
}

static uint32_t texel(const uint8_t *out, int i) { return rl_ld32(out, 4u * (unsigned)i); }

TEST(vk_bc_decoder_vectors)
{
    uint8_t out[4 * 4 * 8];
    const vkf_decode_format *bc1 = vkf_decode_format_of(VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    const vkf_decode_format *bc1rgb = vkf_decode_format_of(VK_FORMAT_BC1_RGB_SRGB_BLOCK);
    CHECK(bc1 && bc1rgb && !vkf_decode_format_of(VK_FORMAT_R8G8B8A8_UNORM));
    CHECK_EQ(bc1->host, VK_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(bc1rgb->host, VK_FORMAT_R8G8B8A8_SRGB);
    const uint8_t red_blue[8] = { 0x00, 0xF8, 0x1F, 0x00, 0x00, 0x55, 0xAA, 0xFF };
    vkf_decode_block(bc1, red_blue, out);
    CHECK_EQ(texel(out, 0), 0xFF0000FFu);
    CHECK_EQ(texel(out, 4), 0xFFFF0000u);
    CHECK_EQ(texel(out, 8), 0xFF5500AAu);
    CHECK_EQ(texel(out, 12), 0xFFAA0055u);
    const uint8_t three[8] = { 0x1F, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF };
    vkf_decode_block(bc1, three, out);
    CHECK_EQ(texel(out, 5), 0);
    vkf_decode_block(bc1rgb, three, out);
    CHECK_EQ(texel(out, 5), 0xFF000000u);

    uint8_t bc3[16] = { 255, 0 };
    unsigned at = 16;
    for (int i = 0; i < 16; i++)
        bits_put(bc3, &at, i & 1, 3);
    memcpy(bc3 + 8, red_blue, 8);
    vkf_decode_block(vkf_decode_format_of(VK_FORMAT_BC3_UNORM_BLOCK), bc3, out);
    CHECK_EQ(texel(out, 0), 0xFF0000FFu);
    CHECK_EQ(texel(out, 1), 0x000000FFu);
    CHECK_EQ(texel(out, 5), 0x00FF0000u);

    uint8_t bc4[8] = { 200, 100 };
    at = 16;
    for (int i = 0; i < 16; i++)
        bits_put(bc4, &at, (uint32_t)i % 3, 3);
    vkf_decode_block(vkf_decode_format_of(VK_FORMAT_BC4_UNORM_BLOCK), bc4, out);
    CHECK_EQ(out[0], 200);
    CHECK_EQ(out[1], 100);
    CHECK_EQ(out[2], 186);
    const uint8_t bc4s[8] = { 0x80, 0x7F, 0, 0, 0, 0, 0, 0 };
    vkf_decode_block(vkf_decode_format_of(VK_FORMAT_BC4_SNORM_BLOCK), bc4s, out);
    CHECK_EQ(out[0], 0x81);
    CHECK_EQ(out[15], 0x81);

    uint8_t bc5[16];
    memcpy(bc5, bc4, 8);
    memcpy(bc5 + 8, bc4s, 8);
    vkf_decode_block(vkf_decode_format_of(VK_FORMAT_BC5_UNORM_BLOCK), bc5, out);
    CHECK_EQ(out[0], 200);
    CHECK_EQ(out[1], 0x80);
    CHECK_EQ(out[2], 100);
    CHECK_EQ(out[3], 0x80);

    uint8_t bc7[16];
    bc7_red_green(bc7);
    vkf_decode_block(vkf_decode_format_of(VK_FORMAT_BC7_UNORM_BLOCK), bc7, out);
    CHECK_EQ(texel(out, 0), 0xFF0101FFu);
    for (int i = 1; i < 16; i++)
        CHECK_EQ(texel(out, i), 0xFF01FF01u);

    const uint8_t r4g4 = 0xA5;
    vkf_decode_block(vkf_decode_format_of(VK_FORMAT_R4G4_UNORM_PACK8), &r4g4, out);
    CHECK_EQ(out[0], 0xAA);
    CHECK_EQ(out[1], 0x55);
}

/** @brief Allocates guest memory of the type the guest would pick and binds the image to it; returns the memory. */
static uint64_t bind_image(uint64_t pd, uint64_t dev, uint64_t image, VkMemoryRequirements *req)
{
    uint64_t greq = galloc(sizeof *req);
    G("vkGetImageMemoryRequirements", dev, image, greq);
    memcpy(req, g_mem + greq, sizeof *req);
    uint32_t t = memory_type(pd, req->memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req->size, .memoryTypeIndex = t };
    uint64_t gm = galloc(8);
    if (t == UINT32_MAX || VKR(G("vkAllocateMemory", dev, gput(&mai, sizeof mai), 0, gm)) != VK_SUCCESS)
        return 0;
    if (VKR(G("vkBindImageMemory", dev, image, g64(gm), 0)) != VK_SUCCESS)
        return 0;
    return g64(gm);
}

static uint64_t make_bc_image(uint64_t dev, VkFormat format, uint32_t w, uint32_t h)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    uint64_t gi = galloc(8);
    if (VKR(G("vkCreateImage", dev, gput(&ici, sizeof ici), 0, gi)) != VK_SUCCESS)
        return 0;
    return g64(gi);
}

/** @brief Records a copy of the host image behind a guest image to a buffer, on the host side (the guest's would be dropped). */
static void host_read_back(uint64_t cb, uint64_t image, uint64_t buf, uint64_t offset, uint32_t w, uint32_t h)
{
    vkf_obj *o = vkf_obj_of(cb, VKG_KIND_VkCommandBuffer);
    VkBufferImageCopy r = { .bufferOffset = offset, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
    ((PFN_vkCmdCopyImageToBuffer)o->fns->f[VKG_vkCmdCopyImageToBuffer])(o->host, (VkImage)GP(image), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                                        (VkBuffer)GP(buf), 1, &r);
}

#define BW 8
#define BH 8

/** @brief The texels of a w x h image of format f whose blocks are blocks, rows of blocks packed. */
static void decode_image(VkFormat format, const uint8_t *blocks, uint32_t w, uint32_t h, uint8_t *out)
{
    const vkf_decode_format *f = vkf_decode_format_of(format);
    vkf_decode_region(f, blocks, (w / 4) * f->block_bytes, (w / 4) * (h / 4) * f->block_bytes, w, h, 1, out);
}

/**
 * @brief Uploads a BC1 and a BC7 image through vkCmdCopyBufferToImage (the
 * BC7 one by the copy2 command), copies the BC7 one to another BC7 image,
 * reads the host images back and counts the texels that differ from the
 * decoded blocks. The blocks are written after the commands are recorded.
 */
static uint32_t bc_upload_scene(uint64_t pd, uint64_t dev, uint32_t qf)
{
    uint64_t gq = galloc(8);
    G("vkGetDeviceQueue", dev, qf, 0, gq);
    uint64_t queue = g64(gq);
    mapped_buf up, rb;
    const uint32_t bc1_bytes = (BW / 4) * (BH / 4) * 8, bc7_bytes = (BW / 4) * (BH / 4) * 16;
    if (make_mapped(pd, dev, 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &up) ||
        make_mapped(pd, dev, 3 * BW * BH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb))
        return UINT32_MAX;
    memset(g_mem + rb.ptr, 0xEE, 3 * BW * BH * 4);
    uint64_t img1 = make_bc_image(dev, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, BW, BH);
    uint64_t img7 = make_bc_image(dev, VK_FORMAT_BC7_UNORM_BLOCK, BW, BH);
    uint64_t img7b = make_bc_image(dev, VK_FORMAT_BC7_UNORM_BLOCK, BW, BH);
    if (!img1 || !img7 || !img7b)
        return UINT32_MAX;
    VkMemoryRequirements req;
    uint64_t m1 = bind_image(pd, dev, img1, &req);
    if (req.size != (bc1_bytes + req.alignment - 1) / req.alignment * req.alignment)
        return UINT32_MAX - 1;
    uint64_t m7 = bind_image(pd, dev, img7, &req), m7b = bind_image(pd, dev, img7b, &req);
    if (!m1 || !m7 || !m7b)
        return UINT32_MAX - 2;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = (VkImage)GP(img7),
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_BC7_UNORM_BLOCK,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    uint64_t gv = galloc(8);
    if (VKR(G("vkCreateImageView", dev, gput(&vci, sizeof vci), 0, gv)) != VK_SUCCESS)
        return UINT32_MAX - 3;

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    uint64_t gpool = galloc(8);
    G("vkCreateCommandPool", dev, gput(&pci, sizeof pci), 0, gpool);
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = (VkCommandPool)GP(g64(gpool)),
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    uint64_t gcb = galloc(8);
    G("vkAllocateCommandBuffers", dev, gput(&cai, sizeof cai), gcb);
    uint64_t cb = g64(gcb);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    G("vkBeginCommandBuffer", cb, gput(&bi, sizeof bi));
    const uint64_t imgs[3] = { img1, img7, img7b };
    for (int i = 0; i < 3; i++)
        barrier(cb, imgs[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy r1 = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { BW, BH, 1 } };
    G("vkCmdCopyBufferToImage", cb, up.buf, img1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, gput(&r1, sizeof r1));
    VkBufferImageCopy2 r7 = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = 64,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { BW, BH, 1 },
    };
    VkCopyBufferToImageInfo2 ci2 = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer = (VkBuffer)GP(up.buf),
        .dstImage = (VkImage)GP(img7),
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1,
        .pRegions = GP(gput(&r7, sizeof r7)),
    };
    G("vkCmdCopyBufferToImage2", cb, gput(&ci2, sizeof ci2));
    barrier(cb, img7, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    VkImageCopy ic = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { BW, BH, 1 },
    };
    G("vkCmdCopyImage", cb, img7, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img7b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
      gput(&ic, sizeof ic));
    barrier(cb, img1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    barrier(cb, img7b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    host_read_back(cb, img1, rb.buf, 0, BW, BH);
    host_read_back(cb, img7, rb.buf, BW * BH * 4, BW, BH);
    host_read_back(cb, img7b, rb.buf, 2 * BW * BH * 4, BW, BH);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    G("vkCmdPipelineBarrier", cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, gput(&mb, sizeof mb), 0, 0, 0, 0);
    G("vkEndCommandBuffer", cb);

    const uint8_t bc1[4][8] = {
        { 0x00, 0xF8, 0x1F, 0x00, 0x00, 0x55, 0xAA, 0xFF },
        { 0x1F, 0x00, 0x00, 0xF8, 0xE4, 0xE4, 0xE4, 0xE4 },
        { 0xE0, 0x07, 0x00, 0xF8, 0x1B, 0x1B, 0x1B, 0x1B },
        { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    };
    uint8_t *src = g_mem + up.ptr;
    memcpy(src, bc1, bc1_bytes);
    for (uint32_t b = 0; b < bc7_bytes / 16; b++) {
        bc7_red_green(src + 64 + 16 * b);
        src[64 + 16 * b + 15] ^= (uint8_t)(b << 4);
    }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = GP(gput(&cb, 8)) };
    if (VKR(G("vkQueueSubmit", queue, 1, gput(&si, sizeof si), 0)) != VK_SUCCESS || VKR(G("vkQueueWaitIdle", queue)) != VK_SUCCESS)
        return UINT32_MAX - 4;

    uint8_t want[3][BW * BH * 4];
    decode_image(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, src, BW, BH, want[0]);
    decode_image(VK_FORMAT_BC7_UNORM_BLOCK, src + 64, BW, BH, want[1]);
    memcpy(want[2], want[1], sizeof want[1]);
    uint32_t bad = 0;
    for (int i = 0; i < 3; i++)
        for (uint32_t p = 0; p < BW * BH; p++)
            bad += rl_ld32(g_mem, rb.ptr + (uint64_t)i * BW * BH * 4 + 4 * p) != rl_ld32(want[i], 4 * p);
    fprintf(stderr, "  BC1 texel 0 0x%08x, BC7 texel 1 0x%08x, copied BC7 texel 1 0x%08x, %u bad\n", rl_ld32(g_mem, rb.ptr),
            rl_ld32(g_mem, rb.ptr + BW * BH * 4 + 4), rl_ld32(g_mem, rb.ptr + 2 * BW * BH * 4 + 4), bad);

    G("vkDestroyCommandPool", dev, g64(gpool), 0);
    G("vkDestroyImageView", dev, g64(gv), 0);
    for (int i = 0; i < 3; i++)
        G("vkDestroyImage", dev, imgs[i], 0);
    G("vkFreeMemory", dev, m1, 0);
    G("vkFreeMemory", dev, m7, 0);
    G("vkFreeMemory", dev, m7b, 0);
    G("vkDestroyBuffer", dev, up.buf, 0);
    G("vkFreeMemory", dev, up.mem, 0);
    G("vkDestroyBuffer", dev, rb.buf, 0);
    G("vkFreeMemory", dev, rb.mem, 0);
    return bad;
}

TEST(vk_bc_uploads_decode_at_submit)
{
    CHECK_EQ(boot(NULL), 0);
    rl_switch_declare_settings();
    CHECK_EQ(rl_settings_set("vk.decode_bc", "on"), RL_SETTING_OK);
    CHECK(make_instance(NULL, 0, 0) != 0);
    uint64_t pds[8];
    CHECK(physical_devices(pds, 8) > 0);
    uint32_t qf = queue_family(pds[0], VK_QUEUE_GRAPHICS_BIT);
    uint64_t dev = make_device(pds[0], qf, NULL, 0);
    CHECK(dev != 0);
    CHECK(vkf_obj_of(dev, VKG_KIND_VkDevice)->dev->decoding);
    CHECK_EQ(bc_upload_scene(pds[0], dev, qf), 0);
    G("vkDestroyDevice", dev, 0);
    G("vkDestroyInstance", g_inst, 0);
    CHECK_EQ(rl_vk_live_handles(), 0);
    rl_settings_reset();
}

/** @brief The features the Switch has for BC formats that every host format standing in for them has too (not min/max filtering). */
#define BC_BASE_FEATURES                                                                                               \
    (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |               \
     VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)

typedef struct bc_answers {
    VkFormatProperties props[VKF_DECODE_FORMATS];
    VkImageFormatProperties image[VKF_DECODE_FORMATS];
    int32_t image_rc[VKF_DECODE_FORMATS];
    VkBool32 bc;
} bc_answers;

static void bc_queries(uint64_t pd, bc_answers *a)
{
    uint64_t gfp = galloc(sizeof(VkFormatProperties)), gip = galloc(sizeof(VkImageFormatProperties));
    for (uint32_t i = 0; i < VKF_DECODE_FORMATS; i++) {
        VkFormat f = i ? (VkFormat)(VK_FORMAT_BC1_RGB_UNORM_BLOCK + i - 1) : VK_FORMAT_R4G4_UNORM_PACK8;
        G("vkGetPhysicalDeviceFormatProperties", pd, f, gfp);
        memcpy(&a->props[i], g_mem + gfp, sizeof a->props[i]);
        memset(g_mem + gip, 0, sizeof(VkImageFormatProperties));
        a->image_rc[i] = VKR(G("vkGetPhysicalDeviceImageFormatProperties", pd, f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, gip));
        memcpy(&a->image[i], g_mem + gip, sizeof a->image[i]);
    }
    uint64_t gf = galloc(sizeof(VkPhysicalDeviceFeatures));
    G("vkGetPhysicalDeviceFeatures", pd, gf);
    a->bc = ((const VkPhysicalDeviceFeatures *)(g_mem + gf))->textureCompressionBC;
}

TEST(vk_bc_queries_do_not_change_with_decoding)
{
    CHECK_EQ(boot(NULL), 0);
    rl_switch_declare_settings();
    CHECK(make_instance(NULL, 0, 0) != 0);
    uint64_t pd = 0;
    CHECK(physical_devices(&pd, 1) == 1);
    static bc_answers off, on;
    CHECK_EQ(rl_settings_set("vk.decode_bc", "off"), RL_SETTING_OK);
    bc_queries(pd, &off);
    CHECK_EQ(rl_settings_set("vk.decode_bc", "on"), RL_SETTING_OK);
    bc_queries(pd, &on);
    unsigned compared = 0;
    for (uint32_t i = 0; i < VKF_DECODE_FORMATS; i++) {
        if (!(off.props[i].optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            continue;
        const VkImageFormatProperties *a = &off.image[i], *b = &on.image[i];
        if (a->maxResourceSize != b->maxResourceSize)
            fprintf(stderr, "  format index %u: maxResourceSize 0x%llx, decoded 0x%llx\n", i, (unsigned long long)a->maxResourceSize,
                    (unsigned long long)b->maxResourceSize);
        if (on.props[i].optimalTilingFeatures != off.props[i].optimalTilingFeatures)
            fprintf(stderr, "  format index %u: optimal features 0x%x, decoded 0x%x\n", i, off.props[i].optimalTilingFeatures,
                    on.props[i].optimalTilingFeatures);
        CHECK_EQ(on.props[i].optimalTilingFeatures & BC_BASE_FEATURES, off.props[i].optimalTilingFeatures & BC_BASE_FEATURES);
        CHECK_EQ(on.props[i].linearTilingFeatures, off.props[i].linearTilingFeatures);
        CHECK_EQ(on.image_rc[i], off.image_rc[i]);
        CHECK(!memcmp(&a->maxExtent, &b->maxExtent, sizeof a->maxExtent));
        CHECK_EQ(a->maxMipLevels, b->maxMipLevels);
        CHECK_EQ(a->maxArrayLayers, b->maxArrayLayers);
        CHECK_EQ(a->sampleCounts, b->sampleCounts);
        compared++;
    }
    fprintf(stderr, "  %u of %u formats the host samples answer the same with vk.decode_bc on\n", compared, VKF_DECODE_FORMATS);
    CHECK_EQ(on.bc, off.bc);
    G("vkDestroyInstance", g_inst, 0);
    rl_settings_reset();
}

static PFN_vkGetPhysicalDeviceFormatProperties g_real_format_props;

/** @brief The host's format features, none for BC1 to BC7 (a GPU without them). */
static void no_bc_format_props(VkPhysicalDevice pd, VkFormat f, VkFormatProperties *p)
{
    g_real_format_props(pd, f, p);
    if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK)
        memset(p, 0, sizeof *p);
}

static void *no_bc_gipa(void *inst, const char *name)
{
    void *r = fake_gipa(inst, name);
    if (r && inst && !strcmp(name, "vkGetPhysicalDeviceFormatProperties")) {
        memcpy(&g_real_format_props, &r, sizeof r);
        void (*f)(void) = (void (*)(void))no_bc_format_props;
        memcpy(&r, &f, sizeof r);
    }
    return r;
}

TEST(vk_bc_a_host_without_bc_shows_the_switch_features)
{
    void *real = rl_hostlib_vulkan_gipa();
    memcpy(&g_real_gipa, &real, sizeof real);
    g_drop_feature = (int)(offsetof(VkPhysicalDeviceFeatures, textureCompressionBC) / sizeof(VkBool32));
    void *(*fg)(void *, const char *) = no_bc_gipa;
    void *gipa;
    memcpy(&gipa, &fg, sizeof gipa);
    CHECK_EQ(boot_with(NULL, gipa), 0);
    rl_switch_declare_settings();
    CHECK(make_instance(NULL, 0, 0) != 0);
    uint64_t pd = 0;
    CHECK(physical_devices(&pd, 1) == 1);
    static bc_answers a;
    bc_queries(pd, &a);
    CHECK(a.bc);
    for (uint32_t i = 1; i < VKF_DECODE_FORMATS; i++) {
        VkFormatProperties s = vkf_switch_format_props((VkFormat)(VK_FORMAT_BC1_RGB_UNORM_BLOCK + i - 1));
        CHECK_EQ(a.props[i].optimalTilingFeatures & BC_BASE_FEATURES, s.optimalTilingFeatures & BC_BASE_FEATURES);
        CHECK_EQ(a.props[i].optimalTilingFeatures & ~s.optimalTilingFeatures, 0);
        CHECK_EQ(a.props[i].linearTilingFeatures, 0);
        CHECK_EQ(a.image_rc[i], VK_SUCCESS);
        CHECK(a.image[i].maxExtent.width >= 4096);
    }
    VkPhysicalDeviceFeatures want = { .textureCompressionBC = VK_TRUE };
    uint64_t dev;
    CHECK_EQ(create_device(pd, gput(&want, sizeof want), &dev), VK_SUCCESS);
    vkf_dev *d = vkf_obj_of(dev, VKG_KIND_VkDevice)->dev;
    CHECK(d->decoding);
    CHECK_EQ(d->decode_host[vkf_decode_index(VK_FORMAT_BC7_SRGB_BLOCK)], VK_FORMAT_R8G8B8A8_SRGB);
    CHECK_EQ(bc_upload_scene(pd, dev, 0), 0);
    finish(dev);
    rl_settings_reset();
}
