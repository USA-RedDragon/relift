/**
 * @file nvn_pools.c
 * @brief The pool table: every imported guest memory pool as pieces of storage buffers, for the translation's search
 * of global accesses no route or slot takes (gpus/nv translate/mem/pools.rs).
 *
 * Set NVN_POOL_SET holds at binding NVN_POOL_ARRAY_BINDING an array of storage buffers, one per piece, and at
 * NVN_POOL_TABLE_BINDING a buffer of words: the number of pieces, then from word NVN_POOL_TABLE_FIRST, four words a
 * piece, the low and high words of the GPU address its descriptor starts at and its size, sorted by address, piece k
 * element k. A pool past the device's storage buffer range is cut into pieces of at most that range, each starting at
 * the storage buffer alignment below its first byte. Each batch slot has its own set and table, rewritten when the
 * pools changed since it was written last; both bindings are update-after-bind, so a rewrite before the batch's
 * submit reaches the draws it recorded. A pool's buffer is destroyed only after the device is idle (nvn_vk_unimport),
 * so no piece is gone under a batch in flight.
 */
#include "nvn_priv.h"
#include "nvn_vk.h"

#include <stdlib.h>
#include <string.h>

#define MAX_PIECES (1u << NVN_POOL_SEARCH_STEPS)

static bool write_slot_held(uint32_t slot);
static bool write_pieces(uint32_t slot, nvn_span *sp, uint32_t n, uint64_t gen);

static struct {
    VkDescriptorSetLayout layout, empty;
    VkDescriptorPool pool;
    VkDescriptorSet set[NVN_SLOTS];
    staging table[NVN_SLOTS];
    uint64_t gen[NVN_SLOTS];
    bool written[NVN_SLOTS];
    uint32_t cap, pieces, spans, dropped;
    bool failed;
} P;

VkDescriptorSetLayout nvn_empty_layout(void)
{
    if (!P.empty) {
        VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        if (VK(vkCreateDescriptorSetLayout)(g.dev, &li, NULL, &P.empty) != VK_SUCCESS)
            P.empty = NULL;
    }
    return P.empty;
}

VkDescriptorSetLayout nvn_pools_layout(void)
{
    if (P.layout || P.failed)
        return P.layout;
    VkPhysicalDeviceVulkan12Properties p12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12 };
    VK(vkGetPhysicalDeviceProperties2)(g.phys, &p2);
    uint32_t lim = p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers;
    if (p12.maxDescriptorSetUpdateAfterBindStorageBuffers < lim)
        lim = p12.maxDescriptorSetUpdateAfterBindStorageBuffers;
    /* room for the stage's other storage buffers */
    P.cap = lim > MAX_PIECES + 256 ? MAX_PIECES : lim > 512 ? lim - 256 : 0;
    if (!P.cap) {
        P.failed = true;
        rl_log(RL_LOG_WARN, "nvn", "no pool table: the device binds %u update-after-bind storage buffers", lim);
        return NULL;
    }
    VkDescriptorSetLayoutBinding b[2] = {
        { NVN_POOL_ARRAY_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, P.cap, VK_SHADER_STAGE_ALL, NULL },
        { NVN_POOL_TABLE_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL },
    };
    VkDescriptorBindingFlags f[2] = { VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT };
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = 2, .pBindingFlags = f
    };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .pNext = &fi,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                           .bindingCount = 2,
                                           .pBindings = b };
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (P.cap + 1) * NVN_SLOTS };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
                                       .maxSets = NVN_SLOTS,
                                       .poolSizeCount = 1,
                                       .pPoolSizes = &ps };
    if (VK(vkCreateDescriptorSetLayout)(g.dev, &li, NULL, &P.layout) != VK_SUCCESS ||
        VK(vkCreateDescriptorPool)(g.dev, &pci, NULL, &P.pool) != VK_SUCCESS) {
        P.failed = true;
        P.layout = NULL;
        rl_log(RL_LOG_WARN, "nvn", "no pool table: the device cannot make its set");
        return NULL;
    }
    VkDescriptorSetLayout ls[NVN_SLOTS];
    for (uint32_t i = 0; i < NVN_SLOTS; i++)
        ls[i] = P.layout;
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = P.pool,
                                       .descriptorSetCount = NVN_SLOTS,
                                       .pSetLayouts = ls };
    bool ok = VK(vkAllocateDescriptorSets)(g.dev, &ai, P.set) == VK_SUCCESS;
    for (uint32_t i = 0; ok && i < NVN_SLOTS; i++)
        ok = nvn_vk_staging_new(&P.table[i], 4ull * (NVN_POOL_TABLE_FIRST + NVN_POOL_TABLE_STRIDE * P.cap));
    if (!ok) {
        P.failed = true;
        rl_log(RL_LOG_WARN, "nvn", "no pool table: the device cannot make its set or table");
    }
    rl_log(RL_LOG_INFO, "nvn", "pool table: up to %u pieces", P.cap);
    return P.layout;
}

static int piece_cmp(const void *a, const void *b)
{
    uint64_t x = ((const nvn_pool_piece *)a)->gpu, y = ((const nvn_pool_piece *)b)->gpu;
    return x < y ? -1 : x > y;
}

uint32_t nvn_pool_pieces(const nvn_span *sp, uint32_t n, uint64_t range, uint64_t align, nvn_pool_piece *pc,
                         uint32_t cap, uint32_t *dropped)
{
    uint32_t np = 0;
    *dropped = 0;
    range -= range % align;
    for (uint32_t i = 0; i < n; i++) {
        /* the buffer offset of the span's first byte, down to the alignment */
        uint64_t first = sp[i].memory - sp[i].base, lead = first % align;
        uint64_t off = first - lead, gpu = sp[i].gpu - lead, left = sp[i].size + lead;
        while (left) {
            uint64_t size = left > range ? range : left;
            if (np == cap) {
                (*dropped)++;
                break;
            }
            pc[np++] = (nvn_pool_piece){ gpu, size, off, sp[i].buf };
            gpu += size;
            off += size;
            left -= size;
        }
    }
    qsort(pc, np, sizeof *pc, piece_cmp);
    return np;
}

/**
 * @brief Writes slot's table and descriptors for the current pools. The pool table's read lock is held from the spans'
 * snapshot through the descriptor update: a finalized or discarded pool leaves the table under its write lock before
 * its buffer is destroyed, so no descriptor is written with a buffer another thread destroyed meanwhile (which the
 * host driver does not survive).
 */
static bool write_slot(uint32_t slot)
{
    nvn_pools_hold();
    bool ok = write_slot_held(slot);
    nvn_pools_release();
    return ok;
}

/** @brief Writes slot's table and descriptors for the n spans sp (generation gen), which it frees. */
static bool write_pieces(uint32_t slot, nvn_span *sp, uint32_t n, uint64_t gen)
{
    uint64_t range = g.max_storage_range ? g.max_storage_range : UINT32_MAX;
    uint32_t cap = P.cap;
    nvn_pool_piece *pc = malloc(cap * sizeof *pc);
    VkDescriptorBufferInfo *bi = malloc(cap * sizeof *bi);
    if (!pc || !bi) {
        free(sp);
        free(pc);
        free(bi);
        return false;
    }
    uint32_t np = nvn_pool_pieces(sp, n, range, g.storage_align, pc, cap, &P.dropped);
    uint32_t *w = (uint32_t *)(void *)P.table[slot].map;
    w[0] = np;
    VkWriteDescriptorSet wr[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = P.set[slot],
          .dstBinding = NVN_POOL_ARRAY_BINDING,
          .descriptorCount = np,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = bi },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = P.set[slot],
          .dstBinding = NVN_POOL_TABLE_BINDING,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &(VkDescriptorBufferInfo){ P.table[slot].buf, 0, VK_WHOLE_SIZE } },
    };
    for (uint32_t k = 0; k < np; k++) {
        uint32_t *e = w + NVN_POOL_TABLE_FIRST + NVN_POOL_TABLE_STRIDE * k;
        e[0] = (uint32_t)pc[k].gpu;
        e[1] = (uint32_t)(pc[k].gpu >> 32);
        e[2] = (uint32_t)pc[k].size;
        e[3] = 0;
        bi[k] = (VkDescriptorBufferInfo){ (VkBuffer)pc[k].buf, pc[k].off, pc[k].size };
    }
    if (np)
        VK(vkUpdateDescriptorSets)(g.dev, 2, wr, 0, NULL);
    else
        VK(vkUpdateDescriptorSets)(g.dev, 1, &wr[1], 0, NULL);
    P.gen[slot] = gen;
    P.written[slot] = true;
    P.pieces = np;
    P.spans = n;
    free(sp);
    free(pc);
    free(bi);
    return true;
}

void (*nvn_pools_between_hook)(void);

/** @brief write_slot with the pool table's read lock held (nvn_pools_hold). */
static bool write_slot_held(uint32_t slot)
{
    uint32_t n = 0;
    uint64_t gen = nvn_pool_spans_held(NULL, 0, &n);
    nvn_span *sp = malloc((n ? n : 1) * sizeof *sp);
    if (!sp)
        return false;
    gen = nvn_pool_spans_held(sp, n, &n);
    if (nvn_pools_between_hook)
        nvn_pools_between_hook();
    return write_pieces(slot, sp, n, gen);
}

VkDescriptorSet nvn_pools_set(uint32_t slot)
{
    if (!nvn_pools_layout() || P.failed || slot >= NVN_SLOTS)
        return NULL;
    if ((!P.written[slot] || P.gen[slot] != nvn_pool_generation()) && !write_slot(slot))
        return NULL;
    return P.set[slot];
}

void nvn_pools_counts(uint32_t *pieces, uint32_t *spans, uint32_t *dropped)
{
    *pieces = P.pieces;
    *spans = P.spans;
    *dropped = P.dropped;
}
