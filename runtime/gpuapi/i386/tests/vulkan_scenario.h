/**
 * @file vulkan_scenario.h
 * @brief One Vulkan workload, built natively for i386 (vulkan_native.c) and
 * through the generated thunks (vulkan_harness.c): instance and device
 * creation with nested arrays and strings, physical device queries hashed
 * byte for byte in their i386 layout (size_t and 64-bit members included),
 * pNext chains in both directions, 64-bit handles, command buffers, a queue
 * submit and a fence.
 *
 * Two answers belong to the driver build, not to the thunks, and are left
 * out: pipelineCacheUUID (each driver build has its own caches) and the
 * number of queue families (the x86-64 NVIDIA driver offers one more). Every call is its own statement before the printf
 * that reports it. The includer defines gaddr, galloc, gp and vk(name,
 * slots, n).
 */
#include <stdio.h>
#include <string.h>

/** @brief i386 layouts (sizes and offsets) and Vulkan constants. */
enum {
    SZ_PROPS = 800,
    SZ_QFP = 24,
    SZ_MEMPROPS = 456,
    SZ_FEATURES2 = 228,
    SZ_VK11F = 56,
    SZ_PROPS2 = 808,
    SZ_DRIVERP = 528,
    SZ_MEMREQ = 20,
    OF_PROPS_name = 20,
    OF_PROPS_uuid = 276,
    OF_PROPS_limits = 292,
    ST_APPLICATION_INFO = 0,
    ST_INSTANCE_CREATE_INFO = 1,
    ST_DEVICE_QUEUE_CREATE_INFO = 2,
    ST_DEVICE_CREATE_INFO = 3,
    ST_SUBMIT_INFO = 4,
    ST_FENCE_CREATE_INFO = 8,
    ST_BUFFER_CREATE_INFO = 12,
    ST_COMMAND_POOL_CREATE_INFO = 39,
    ST_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    ST_COMMAND_BUFFER_BEGIN_INFO = 42,
    ST_VK11_FEATURES = 49,
    ST_FEATURES2 = 1000059000,
    ST_PROPS2 = 1000059001,
    ST_DRIVER_PROPERTIES = 1000196000,
};

#define VK(name, ...)                                                                              \
    ({                                                                                             \
        const uint32_t s_[] = {0, __VA_ARGS__};                                                    \
        vk(name, s_ + 1, sizeof s_ / 4 - 1);                                                       \
    })

static uint32_t rd(gaddr a)
{
    uint32_t v;
    memcpy(&v, gp(a), 4);
    return v;
}

static void wr(gaddr a, uint32_t v)
{
    memcpy(gp(a), &v, 4);
}

static gaddr gstr(const char *s)
{
    gaddr a = galloc(strlen(s) + 1);
    memcpy(gp(a), s, strlen(s) + 1);
    return a;
}

static uint32_t fnv(gaddr a, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ gp(a)[i]) * 16777619u;
    return h;
}

static void scenario(void)
{
    gaddr ver = galloc(4);
    int r = (int)VK("vkEnumerateInstanceVersion", ver);
    printf("instance version %d %u.%u\n", r, rd(ver) >> 22, (rd(ver) >> 12) & 0x3ff);

    gaddr app = galloc(28);
    wr(app, ST_APPLICATION_INFO);
    wr(app + 8, gstr("relift thunks"));
    wr(app + 12, 1);
    wr(app + 16, gstr("relift"));
    wr(app + 24, (1u << 22) | (1u << 12));
    gaddr ici = galloc(32);
    wr(ici, ST_INSTANCE_CREATE_INFO);
    wr(ici + 12, app);
    gaddr instp = galloc(4);
    r = (int)VK("vkCreateInstance", ici, 0, instp);
    gaddr inst = rd(instp);
    printf("create instance %d\n", r);
    gaddr pa = galloc(16);
    memcpy(gp(pa), "vkCreateDevice", 15);
    int proc = VK("vkGetInstanceProcAddr", inst, pa) != 0;
    printf("vkGetInstanceProcAddr %d\n", proc);

    gaddr n = galloc(4);
    r = (int)VK("vkEnumeratePhysicalDevices", inst, n, 0);
    gaddr pds = galloc(4 * (rd(n) + 1));
    r = (int)VK("vkEnumeratePhysicalDevices", inst, n, pds);
    printf("physical devices %d %d\n", r, rd(n) > 0);
    gaddr pd = rd(pds);

    gaddr props = galloc(SZ_PROPS);
    VK("vkGetPhysicalDeviceProperties", pd, props);
    printf("device %s type %u api %u.%u properties %08x limits %08x\n", (const char *)gp(props + OF_PROPS_name), rd(props + 16),
           rd(props) >> 22, (rd(props) >> 12) & 0x3ff, fnv(props, OF_PROPS_uuid), fnv(props + OF_PROPS_limits, SZ_PROPS - OF_PROPS_limits));
    gaddr qn = galloc(4);
    VK("vkGetPhysicalDeviceQueueFamilyProperties", pd, qn, 0);
    gaddr qf = galloc(SZ_QFP * rd(qn));
    VK("vkGetPhysicalDeviceQueueFamilyProperties", pd, qn, qf);
    printf("queue family 0 %08x flags %x\n", fnv(qf, SZ_QFP), rd(qf));
    gaddr mp = galloc(SZ_MEMPROPS);
    VK("vkGetPhysicalDeviceMemoryProperties", pd, mp);
    printf("memory properties %08x types %u heaps %u\n", fnv(mp, SZ_MEMPROPS), rd(mp), rd(mp + 260));

    gaddr v11 = galloc(SZ_VK11F);
    wr(v11, ST_VK11_FEATURES);
    gaddr f2 = galloc(SZ_FEATURES2);
    wr(f2, ST_FEATURES2);
    wr(f2 + 4, v11);
    VK("vkGetPhysicalDeviceFeatures2", pd, f2);
    printf("features2 %08x chain %08x next kept %d\n", fnv(f2 + 8, SZ_FEATURES2 - 8), fnv(v11 + 8, SZ_VK11F - 8),
           rd(f2 + 4) == v11);
    gaddr dp = galloc(SZ_DRIVERP);
    wr(dp, ST_DRIVER_PROPERTIES);
    gaddr p2 = galloc(SZ_PROPS2);
    wr(p2, ST_PROPS2);
    wr(p2 + 4, dp);
    VK("vkGetPhysicalDeviceProperties2", pd, p2);
    printf("properties2 same %d driver %s info %s conformance %08x\n", memcmp(gp(p2 + 8), gp(props), SZ_PROPS) == 0,
           (const char *)gp(dp + 12), (const char *)gp(dp + 268), fnv(dp + 524, 4));

    gaddr prio = galloc(4);
    float one = 1.0f;
    memcpy(gp(prio), &one, 4);
    gaddr qci = galloc(24);
    wr(qci, ST_DEVICE_QUEUE_CREATE_INFO);
    wr(qci + 12, 0);
    wr(qci + 16, 1);
    wr(qci + 20, prio);
    gaddr dci = galloc(40);
    wr(dci, ST_DEVICE_CREATE_INFO);
    wr(dci + 12, 1);
    wr(dci + 16, qci);
    gaddr devp = galloc(4);
    r = (int)VK("vkCreateDevice", pd, dci, 0, devp);
    gaddr dev = rd(devp);
    gaddr qp = galloc(4);
    VK("vkGetDeviceQueue", dev, 0, 0, qp);
    printf("create device %d queue %d\n", r, rd(qp) != 0);

    gaddr bci = galloc(36);
    wr(bci, ST_BUFFER_CREATE_INFO);
    wr(bci + 12, 65536);
    wr(bci + 20, 0x3);
    gaddr buf = galloc(8);
    r = (int)VK("vkCreateBuffer", dev, bci, 0, buf);
    gaddr req = galloc(SZ_MEMREQ);
    VK("vkGetBufferMemoryRequirements", dev, rd(buf), rd(buf + 4), req);
    printf("buffer %d requirements size %u alignment %u types %x\n", r, rd(req), rd(req + 8), rd(req + 16));
    uint32_t type = 0;
    while (type < 32 && !(rd(req + 16) & (1u << type)))
        type++;
    gaddr mai = galloc(24);
    wr(mai, 5);
    memcpy(gp(mai + 8), gp(req), 8);
    wr(mai + 16, type);
    gaddr memo = galloc(8);
    int rm = (int)VK("vkAllocateMemory", dev, mai, 0, memo);
    int rbind = (int)VK("vkBindBufferMemory", dev, rd(buf), rd(buf + 4), rd(memo), rd(memo + 4), 0, 0);
    printf("memory %d bind %d\n", rm, rbind);

    gaddr pci = galloc(16);
    wr(pci, ST_COMMAND_POOL_CREATE_INFO);
    wr(pci + 8, 0x2);
    gaddr pool = galloc(8);
    r = (int)VK("vkCreateCommandPool", dev, pci, 0, pool);
    gaddr cai = galloc(24);
    wr(cai, ST_COMMAND_BUFFER_ALLOCATE_INFO);
    memcpy(gp(cai + 8), gp(pool), 8);
    wr(cai + 16, 0);
    wr(cai + 20, 2);
    gaddr cbs = galloc(8);
    int ra = (int)VK("vkAllocateCommandBuffers", dev, cai, cbs);
    printf("command pool %d buffers %d both %d\n", r, ra, rd(cbs) != 0 && rd(cbs + 4) != 0 && rd(cbs) != rd(cbs + 4));
    gaddr cbi = galloc(16);
    wr(cbi, ST_COMMAND_BUFFER_BEGIN_INFO);
    wr(cbi + 8, 0x1);
    int rb = (int)VK("vkBeginCommandBuffer", rd(cbs), cbi);
    VK("vkCmdFillBuffer", rd(cbs), rd(buf), rd(buf + 4), 0, 0, 256, 0, 0x01020304);
    int re = (int)VK("vkEndCommandBuffer", rd(cbs));
    gaddr fci = galloc(12);
    wr(fci, ST_FENCE_CREATE_INFO);
    gaddr fence = galloc(8);
    int rf = (int)VK("vkCreateFence", dev, fci, 0, fence);
    gaddr si = galloc(36);
    wr(si, ST_SUBMIT_INFO);
    wr(si + 20, 1);
    wr(si + 24, cbs);
    int rs = (int)VK("vkQueueSubmit", rd(qp), 1, si, rd(fence), rd(fence + 4));
    int rw = (int)VK("vkWaitForFences", dev, 1, fence, 1, 0xffffffffu, 0xffffffffu);
    int st = (int)VK("vkGetFenceStatus", dev, rd(fence), rd(fence + 4));
    printf("record %d %d fence %d submit %d wait %d status %d\n", rb, re, rf, rs, rw, st);

    VK("vkDestroyFence", dev, rd(fence), rd(fence + 4), 0);
    VK("vkFreeCommandBuffers", dev, rd(pool), rd(pool + 4), 2, cbs);
    VK("vkDestroyCommandPool", dev, rd(pool), rd(pool + 4), 0);
    VK("vkDestroyBuffer", dev, rd(buf), rd(buf + 4), 0);
    VK("vkFreeMemory", dev, rd(memo), rd(memo + 4), 0);
    int idle = (int)VK("vkDeviceWaitIdle", dev);
    VK("vkDestroyDevice", dev, 0);
    VK("vkDestroyInstance", inst, 0);
    printf("teardown %d\n", idle);
}
