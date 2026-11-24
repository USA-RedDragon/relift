/**
 * @file vulkan_hand.c
 * @brief Hand-written Vulkan thunks: vkGet*ProcAddr give guest addresses of
 * the thunks, and the memory-mapping calls, which must place the host
 * mapping inside the guest window (VK_EXT_map_memory_placed) so coherent
 * memory stays exact; until the guest layer offers page-aligned window
 * ranges they stop the program rather than map a copy.
 */
#include "tg.h"

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
