/**
 * @file vulkan_hand.c
 * @brief Hand-written Vulkan thunks: vkGet*ProcAddr give guest addresses of
 * the thunks, and the memory-mapping calls, which must place the host
 * mapping inside the guest window (VK_EXT_map_memory_placed) so coherent
 * memory stays exact; until the guest layer offers page-aligned window
 * ranges they stop the program rather than map a copy.
 */
#include "tg.h"
#include "vulkan_heaps.h"

#include <pthread.h>
#include <stdio.h>

/** @brief The Vulkan soname. */
#define TG_VK "libvulkan.so.1"

/** @brief vkGet*ProcAddr: the guest thunk of the name when the host resolves it. */
static uint64_t tg_vk_proc(rl_ctx *c, uint8_t *mem, const char *fn)
{
    tg_call kk, *k = &kk;
    tg_begin(k, c, mem);
    uint32_t name = tg_arg32(k, 4);
    void *(*get)(void *, const char *) = (void *(*)(void *, const char *))tg_dlsym(TG_VK, fn, NULL);
    if (!get)
        tg_fail(fn, "missing in the host library " TG_VK);
    void *h = name ? get(tg_unhandle(tg_arg32(k, 0), fn), (const char *)mem + name) : NULL;
    uint32_t g = h ? tg_guest_symbol(TG_VK, (const char *)mem + name) : 0;
    if (h && !g)
        fprintf(stderr, "relift: %s: %s has no thunk; the guest gets NULL\n", fn, (const char *)mem + name);
    tg_i386_set_eax(c, g);
    return tg_end(k, 0);
}

uint64_t tgh_vkGetInstanceProcAddr(rl_ctx *c, uint8_t *mem);
uint64_t tgh_vkGetDeviceProcAddr(rl_ctx *c, uint8_t *mem);
uint64_t tgh_vkMapMemory(rl_ctx *c, uint8_t *mem);
uint64_t tgh_vkGetPhysicalDeviceMemoryProperties(rl_ctx *c, uint8_t *mem);
uint64_t tgh_vkGetPhysicalDeviceMemoryProperties2(rl_ctx *c, uint8_t *mem);

uint64_t tgh_vkGetInstanceProcAddr(rl_ctx *c, uint8_t *mem)
{
    return tg_vk_proc(c, mem, "vkGetInstanceProcAddr");
}

uint64_t tgh_vkGetDeviceProcAddr(rl_ctx *c, uint8_t *mem)
{
    return tg_vk_proc(c, mem, "vkGetDeviceProcAddr");
}

/** @brief A mapping call the thunks cannot make exactly yet. */
static uint64_t tg_vk_unmapped(rl_ctx *c, uint8_t *mem, const char *fn)
{
    tg_call kk, *k = &kk;
    tg_begin(k, c, mem);
    tg_fail(fn, "host memory mapped into the guest window needs page-aligned window ranges from the guest layer");
}

uint64_t tgh_vkMapMemory(rl_ctx *c, uint8_t *mem)
{
    return tg_vk_unmapped(c, mem, "vkMapMemory");
}

/** @brief VkDriverId of a physical device (0 when the host cannot tell), asked once per device. */
static uint32_t tg_vk_driver_id(void *pd)
{
    static struct {
        void *pd;
        uint32_t id;
    } cache[16];
    static unsigned used;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < used; i++)
        if (cache[i].pd == pd) {
            uint32_t id = cache[i].id;
            pthread_mutex_unlock(&lock);
            return id;
        }
    pthread_mutex_unlock(&lock);
    struct {
        uint32_t stype, pad;
        void *next;
        uint32_t id;
        char rest[1024];
    } d;
    struct {
        uint32_t stype, pad;
        void *next;
        char props[1024];
    } p2;
    memset(&d, 0, sizeof d);
    memset(&p2, 0, sizeof p2);
    d.stype = 1000196000;
    p2.stype = 1000059001;
    p2.next = &d;
    void (*get2)(void *, void *) = (void (*)(void *, void *))tg_dlsym(TG_VK, "vkGetPhysicalDeviceProperties2", NULL);
    if (get2)
        get2(pd, &p2);
    pthread_mutex_lock(&lock);
    if (used < 16) {
        cache[used].pd = pd;
        cache[used].id = d.id;
        used++;
    }
    pthread_mutex_unlock(&lock);
    return d.id;
}

/** @brief The host's memory properties (x86-64 layout, with the budget struct when budget is set) as the i386 layout of both, as the driver's i386 build gives them. */
static void tg_vk_memory_properties(tg_call *k, uint32_t pd, uint32_t out, uint32_t budget, const char *fn)
{
    struct {
        uint32_t type_count;
        struct {
            uint32_t flags, heap;
        } types[32];
        uint32_t heap_count;
        struct {
            uint64_t size;
            uint32_t flags, pad;
        } heaps[16];
    } h;
    struct {
        uint32_t stype, pad;
        void *next;
        uint64_t budget[16], usage[16];
    } hb;
    struct {
        uint32_t stype, pad;
        void *next;
        struct {
            uint32_t type_count;
            struct {
                uint32_t flags, heap;
            } types[32];
            uint32_t heap_count;
            struct {
                uint64_t size;
                uint32_t flags, pad;
            } heaps[16];
        } props;
    } h2;
    void (*get2)(void *, void *) = (void (*)(void *, void *))tg_dlsym(TG_VK, "vkGetPhysicalDeviceMemoryProperties2", NULL);
    void (*get)(void *, void *) = (void (*)(void *, void *))tg_dlsym(TG_VK, "vkGetPhysicalDeviceMemoryProperties", NULL);
    if (!get)
        tg_fail(fn, "missing in the host library " TG_VK);
    memset(&h, 0, sizeof h);
    memset(&hb, 0, sizeof hb);
    memset(&h2, 0, sizeof h2);
    void *pdh = tg_unhandle(pd, fn);
    uint32_t driver_id = tg_vk_driver_id(pdh);
    if (budget && get2) {
        hb.stype = TG_VK_BUDGET_STYPE;
        h2.stype = 1000059006;
        h2.next = &hb;
        get2(pdh, &h2);
        memcpy(&h, &h2.props, sizeof h);
    } else {
        get(pdh, &h);
    }
    uint8_t g[TG_VK_MEMPROPS_SIZE] = { 0 };
    memcpy(g, &h.type_count, 4);
    for (uint32_t t = 0; t < 32; t++)
        memcpy(g + 4 + 8 * t, &h.types[t], 8);
    memcpy(g + 260, &h.heap_count, 4);
    for (uint32_t i = 0; i < 16; i++) {
        memcpy(g + 264 + 12 * i, &h.heaps[i].size, 8);
        memcpy(g + 272 + 12 * i, &h.heaps[i].flags, 4);
    }
    tg_vk_cap_heaps(g, driver_id);
    tg_put(k, out, g, sizeof g);
    if (budget) {
        uint8_t full[TG_VK_BUDGET_SIZE] = { 0 };
        memcpy(full + TG_VK_BUDGET_BUDGET, hb.budget, sizeof hb.budget);
        memcpy(full + TG_VK_BUDGET_USAGE, hb.usage, sizeof hb.usage);
        tg_vk_cap_budget(full, driver_id);
        tg_put(k, budget + 8, full + 8, sizeof full - 8);
    }
}

uint64_t tgh_vkGetPhysicalDeviceMemoryProperties(rl_ctx *c, uint8_t *mem)
{
    tg_call kk, *k = &kk;
    tg_begin(k, c, mem);
    uint32_t out = tg_arg32(k, 4);
    if (out)
        tg_vk_memory_properties(k, tg_arg32(k, 0), out, 0, "vkGetPhysicalDeviceMemoryProperties");
    return tg_end(k, 0);
}

uint64_t tgh_vkGetPhysicalDeviceMemoryProperties2(rl_ctx *c, uint8_t *mem)
{
    tg_call kk, *k = &kk;
    tg_begin(k, c, mem);
    uint32_t out = tg_arg32(k, 4);
    uint32_t budget = 0;
    for (uint32_t n = out ? tg_ld(mem + out + 4, 4) : 0; n; n = (uint32_t)tg_ld(mem + n + 4, 4))
        if (tg_ld(mem + n, 4) == TG_VK_BUDGET_STYPE)
            budget = n;
    if (out)
        tg_vk_memory_properties(k, tg_arg32(k, 0), out + 8, budget, "vkGetPhysicalDeviceMemoryProperties2");
    return tg_end(k, 0);
}
