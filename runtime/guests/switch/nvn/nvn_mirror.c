/**
 * @file nvn_mirror.c
 * @brief Memory pools as device buffers the layer keeps equal to guest memory, for hosts that cannot import guest
 * pages (no VK_EXT_external_memory_host: MoltenVK, many Android drivers, a capture layer that hides the extension).
 *
 * A mirror is a buffer with a device address over device memory, of its pool's size; its address is the pool's GPU
 * address, as an import's is. The two directions are kept exact against the console's memory model:
 *
 * - Guest to device: the pool's pages are tracked (nvn_watch.h). Before any GPU work that can read them is recorded,
 *   the pages written since the last copy are copied into the device buffer by commands in the batch being recorded,
 *   so the order of the game's writes and its commands is the order the device sees. Only dirty pages move.
 * - Device to guest: a command that can write a pool (a compute dispatch or a draw with storage buffers, an image to
 *   buffer copy) notes the bytes it can reach. The layer completes every batch before its submit returns, so when a
 *   batch completes, those bytes are copied back, and only the pages that differ, before the game can observe them.
 *
 * Shaders reach pools through the storage buffer ranges the game binds, which are noted exactly. A program whose
 * translation stores through addresses it builds from other data (NVN_IFACE_GLOBAL_STORE) can write anywhere. Its
 * translation for this host marks the 4 KiB page of each such store in the written-page bitmap (the translator's
 * PUSH_WRITTEN_PAGES): when its batch completes, the mirror pages whose bit is set are noted as written and copied
 * back by the readback batches that follow, before the game runs on. Where the translation does not mark pages (a
 * table made for a host that imports guest memory), every mirror is noted as written for its batch and the pages that
 * differ are copied back.
 */
#include "nvn_vk.h"
#include "vkdev.h"

#include "hos.h"
#include "nvn_watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Largest copy recorded into one staging slice, and the size of a staging chunk. */
#define SLICE (32ull << 20)
/**
 * @brief Staging chunks kept between batches. Each chunk is write-combined host memory whose allocation and release
 * change page attributes in the kernel (milliseconds each), so chunks are reused, not made again for every batch; a
 * batch's uploads are bounded to SLICE before it is submitted, so it rarely needs more than two.
 */
#define KEEP_CHUNKS 4
/** @brief Bytes of the readback buffer. */
#define READBACK (128ull << 20)
#define PAGE 4096u
/** @brief Bits of the written-page bitmap: the translator's WRITTEN_PAGE_BITS. */
#define WRITTEN_PAGE_BITS (1u << 20)

typedef struct mirror {
    uint64_t guest, size, gpu;
    VkBuffer buf;
    VkDeviceMemory mem;
    /** @brief Whether the device bytes were set to zero, which pages never written by the guest rely on. */
    bool cleared;
} mirror;

typedef struct upload_chunk {
    staging s;
    uint64_t used;
} upload_chunk;

/** @brief A byte range of a mirror the device may have written. */
typedef struct span {
    mirror *m;
    uint64_t off, len;
} span;

/** @brief One copy of a readback buffer fill: the span's bytes at rb. */
typedef struct segment {
    mirror *m;
    uint64_t off, len, rb;
} segment;

static struct {
    pthread_mutex_t lock;
    mirror **v;
    uint32_t n, cap;
    uint64_t epoch, map_gen;
    upload_chunk *chunks;
    uint32_t nchunks, capchunks;
    span *pending;
    uint32_t npending, cappending;
    uint32_t cursor;
    uint64_t cursor_off;
    staging rb;
    segment seg[64];
    uint32_t nseg;
    uint64_t stat_up, stat_down, stat_syncs;
    /**
     * @brief The written-page bitmap. Programs mark it in device memory (pages_dev, at pages_gpu), all zero between
     * batches; a batch that ran marking programs copies it into the host-visible pages_buf (mapped at pages_map) and
     * clears it. Marking straight into host memory made every marked store a round trip over the bus: Ocarina's
     * opening ran its video compute at up to 50 ms a dispatch.
     */
    VkBuffer pages_buf, pages_dev;
    VkDeviceMemory pages_mem, pages_dev_mem;
    uint32_t *pages_map;
    uint64_t pages_gpu;
    /** @brief pages_dev was made and is not cleared yet: the next sync records its clear. */
    bool pages_fresh;
    /** @brief The batch being recorded runs a program that marks pages, and so the batch that waits to be read. */
    bool marked, marked_sent;
    uint64_t stat_marked_pages, stat_read;
    /** @brief Staging chunks made so far (each a host allocation of write-combined memory). */
    uint64_t stat_chunks;
    /** @brief Why the last sync failed, for the stop report. */
    char fail[160];
} mi;

static pthread_once_t mi_once = PTHREAD_ONCE_INIT;

/** @brief The lock is recursive: a sync that fills a slice completes the batch, which resets the staging under it. */
static void mi_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mi.lock, &a);
    pthread_mutexattr_destroy(&a);
}

static bool grow(void **v, uint32_t n, uint32_t *cap, size_t each)
{
    if (n < *cap)
        return true;
    uint32_t c = *cap ? 2 * *cap : 16;
    void *p = realloc(*v, c * each);
    if (!p)
        return false;
    *v = p;
    *cap = c;
    return true;
}

bool nvn_mirror_make(uint64_t guest, uint64_t size, nvn_import *out)
{
    memset(out, 0, sizeof *out);
    if (!nvn_vk_ready() || !size)
        return false;
    pthread_once(&mi_once, mi_init);
    mirror *m = calloc(1, sizeof *m);
    if (!m)
        return false;
    m->guest = guest;
    m->size = size;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size,
                               .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                        (g.transform_feedback ? NVN_BUFFER_USAGE_XFB : 0) };
    if (VK(vkCreateBuffer)(g.dev, &bci, NULL, &m->buf) != VK_SUCCESS) {
        free(m);
        return false;
    }
    VkMemoryRequirements req;
    VK(vkGetBufferMemoryRequirements)(g.dev, m->buf, &req);
    int32_t t = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0)
        t = nvn_vk_mem_type(req.memoryTypeBits, 0);
    VkMemoryAllocateFlagsInfo fl = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                     .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .pNext = &fl,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)t };
    if (t < 0 || VK(vkAllocateMemory)(g.dev, &mai, NULL, &m->mem) != VK_SUCCESS ||
        VK(vkBindBufferMemory)(g.dev, m->buf, m->mem, 0) != VK_SUCCESS) {
        if (m->mem)
            VK(vkFreeMemory)(g.dev, m->mem, NULL);
        VK(vkDestroyBuffer)(g.dev, m->buf, NULL);
        free(m);
        return false;
    }
    VkBufferDeviceAddressInfo ai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = m->buf };
    m->gpu = VK(vkGetBufferDeviceAddress)(g.dev, &ai);
    pthread_mutex_lock(&mi.lock);
    bool ok = m->gpu && grow((void **)&mi.v, mi.n, &mi.cap, sizeof *mi.v);
    if (ok) {
        mi.v[mi.n++] = m;
        ok = nvn_track_add(guest, size);
        if (!ok)
            mi.n--;
    }
    pthread_mutex_unlock(&mi.lock);
    if (!ok) {
        VK(vkDestroyBuffer)(g.dev, m->buf, NULL);
        VK(vkFreeMemory)(g.dev, m->mem, NULL);
        free(m);
        return false;
    }
    out->buf = m->buf;
    out->memory = m->mem;
    out->gpu = m->gpu;
    return true;
}

static mirror *by_buf(const void *buf)
{
    for (uint32_t i = 0; i < mi.n; i++)
        if (mi.v[i]->buf == buf)
            return mi.v[i];
    return NULL;
}

void nvn_mirror_free(nvn_import *im)
{
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    mirror *m = by_buf(im->buf);
    if (m) {
        nvn_vk_wait_idle();
        uint32_t i = 0;
        while (mi.v[i] != m)
            i++;
        memmove(&mi.v[i], &mi.v[i + 1], (mi.n - i - 1) * sizeof *mi.v);
        mi.n--;
        uint32_t keep = 0;
        for (uint32_t k = 0; k < mi.npending; k++)
            if (mi.pending[k].m != m)
                mi.pending[keep++] = mi.pending[k];
        mi.npending = keep;
        mi.cursor = 0;
        mi.cursor_off = 0;
        nvn_track_remove(m->guest, m->size);
        for (uint32_t k = 0; k < mi.n; k++)
            nvn_track_add(mi.v[k]->guest, mi.v[k]->size);
        VK(vkDestroyBuffer)(g.dev, m->buf, NULL);
        VK(vkFreeMemory)(g.dev, m->mem, NULL);
        free(m);
    }
    pthread_mutex_unlock(&mi.lock);
    memset(im, 0, sizeof *im);
}

bool nvn_mirror_buffer(const void *buf)
{
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    bool is = by_buf(buf) != NULL;
    pthread_mutex_unlock(&mi.lock);
    return is;
}

/** @brief Room for n bytes in a staging chunk (in chunk *buf at *off, mapped at *map); false when memory is out. */
static bool stage_alloc(uint64_t n, VkBuffer *buf, uint64_t *off, uint8_t **map)
{
    n = (n + 15) & ~15ull;
    for (uint32_t i = 0; i < mi.nchunks; i++) {
        upload_chunk *c = &mi.chunks[i];
        if (c->s.size - c->used >= n) {
            *buf = c->s.buf;
            *off = c->used;
            *map = c->s.map + c->used;
            c->used += n;
            return true;
        }
    }
    if (!grow((void **)&mi.chunks, mi.nchunks, &mi.capchunks, sizeof *mi.chunks))
        return false;
    upload_chunk *c = &mi.chunks[mi.nchunks];
    memset(c, 0, sizeof *c);
    if (!nvn_vk_staging_new(&c->s, n > SLICE ? n : SLICE)) {
        snprintf(mi.fail, sizeof mi.fail, "a %llu MiB staging buffer could not be made (%u in use)",
                 (unsigned long long)((n > SLICE ? n : SLICE) >> 20), mi.nchunks);
        return false;
    }
    mi.stat_chunks++;
    mi.nchunks++;
    c->used = n;
    *buf = c->s.buf;
    *off = 0;
    *map = c->s.map;
    return true;
}

bool nvn_mirror_any(void) { return __atomic_load_n(&mi.n, __ATOMIC_ACQUIRE) != 0; }

void nvn_mirror_flushed(void)
{
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    for (uint32_t i = 0; i < mi.nchunks;) {
        upload_chunk *c = &mi.chunks[i];
        c->used = 0;
        if (c->s.size > SLICE || i >= KEEP_CHUNKS) {
            nvn_vk_staging_free(&c->s);
            memmove(c, c + 1, (mi.nchunks - i - 1) * sizeof *c);
            mi.nchunks--;
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&mi.lock);
}

typedef struct sync_ctx {
    replay *r;
    bool can_flush, ok;
    uint64_t bytes;
} sync_ctx;

static bool zero_page(const uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i += 8)
        if (*(const uint64_t *)(p + i))
            return false;
    return true;
}

/** @brief Copies n guest bytes at guest into m: staged, then a copy command in the batch being recorded. */
static void upload_piece(sync_ctx *c, mirror *m, uint64_t guest, uint64_t n)
{
    while (n && c->ok) {
        uint64_t len = n > SLICE ? SLICE : n;
        VkBuffer sb;
        uint64_t so;
        uint8_t *map;
        if (!stage_alloc(len, &sb, &so, &map) || !nvn_vk_begin(c->r)) {
            if (!mi.fail[0])
                snprintf(mi.fail, sizeof mi.fail, "a batch could not be started for the upload");
            c->ok = false;
            return;
        }
        memcpy(map, rl_mem_base() + guest, len);
        VkBufferCopy bc = { .srcOffset = so, .dstOffset = guest - m->guest, .size = len };
        VK(vkCmdCopyBuffer)(g.cb, sb, m->buf, 1, &bc);
        mi.stat_up += len;
        c->bytes += len;
        guest += len;
        n -= len;
        if (c->can_flush && c->bytes >= SLICE) {
            nvn_vk_barrier(g.cb);
            c->ok = nvn_vk_flush(c->r);
            if (!c->ok)
                snprintf(mi.fail, sizeof mi.fail, "the device failed a batch of uploads");
            c->bytes = 0;
        }
    }
}

static void upload_run(sync_ctx *c, mirror *m, uint64_t a, uint64_t b, bool fresh)
{
    if (!fresh) {
        upload_piece(c, m, a, b - a);
        return;
    }
    const uint8_t *host = rl_mem_base();
    for (uint64_t p = a; p < b && c->ok;) {
        uint64_t e = p + PAGE < b ? p + PAGE : b;
        if (zero_page(host + p, e - p)) {
            p = e;
            continue;
        }
        uint64_t s = p;
        while (e < b && !zero_page(host + e, (e + PAGE < b ? e + PAGE : b) - e) && e - s < SLICE)
            e = e + PAGE < b ? e + PAGE : b;
        upload_piece(c, m, s, e - s);
        p = e;
    }
}

static void on_run(void *user, uint64_t addr, uint64_t size, bool fresh)
{
    sync_ctx *c = user;
    for (uint32_t i = 0; i < mi.n; i++) {
        mirror *m = mi.v[i];
        uint64_t a = addr > m->guest ? addr : m->guest, b = addr + size < m->guest + m->size ? addr + size : m->guest + m->size;
        if (a < b)
            upload_run(c, m, a, b, fresh);
    }
}

bool nvn_mirror_sync(replay *r, bool may_flush)
{
    if (!mi.n)
        return true;
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    uint64_t gen = __atomic_load_n(&hos.vm.map_gen, __ATOMIC_ACQUIRE);
    if (gen != mi.map_gen) {
        /* A change of the map can drop a pool's write tracking, so a pool it touched is copied whole; the others
         * keep their tracking and move only their written pages. */
        for (uint32_t i = 0; i < mi.n; i++)
            if (rl_vm_changed_since(&hos.vm, mi.map_gen, mi.v[i]->guest, mi.v[i]->size))
                nvn_track_dirty_all(mi.v[i]->guest, mi.v[i]->size);
        mi.map_gen = gen;
    }
    uint64_t epoch = nvn_track_epoch();
    bool ok = true;
    mi.fail[0] = 0;
    if (mi.pages_fresh && nvn_vk_begin(r)) {
        VK(vkCmdFillBuffer)(g.cb, mi.pages_dev, 0, VK_WHOLE_SIZE, 0);
        nvn_vk_barrier(g.cb);
        mi.pages_fresh = false;
    }
    if (epoch != mi.epoch) {
        mi.epoch = epoch;
        sync_ctx c = { .r = r, .can_flush = may_flush, .ok = true };
        for (uint32_t i = 0; i < mi.n && c.ok; i++) {
            mirror *m = mi.v[i];
            if (m->cleared)
                continue;
            c.ok = nvn_vk_begin(r);
            if (c.ok) {
                c.ok = nvn_vk_fill(g.cb, m->buf, 0, m->size & ~3ull, 0);
                m->cleared = c.ok;
            }
        }
        if (c.ok && nvn_vk_recording())
            nvn_vk_barrier(g.cb);
        nvn_track_collect(on_run, &c);
        if (c.ok && nvn_vk_recording())
            nvn_vk_barrier(g.cb);
        mi.stat_syncs++;
        ok = c.ok;
    }
    pthread_mutex_unlock(&mi.lock);
    return ok;
}

void nvn_mirror_written(uint64_t gpu, uint64_t size)
{
    if (!mi.n || !size)
        return;
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    for (uint32_t i = 0; i < mi.n; i++) {
        mirror *m = mi.v[i];
        if (gpu < m->gpu || gpu - m->gpu >= m->size)
            continue;
        uint64_t off = gpu - m->gpu, len = size < m->size - off ? size : m->size - off;
        if (grow((void **)&mi.pending, mi.npending, &mi.cappending, sizeof *mi.pending))
            mi.pending[mi.npending++] = (span){ m, off, len };
        break;
    }
    pthread_mutex_unlock(&mi.lock);
}

void nvn_mirror_written_guest(uint64_t guest, uint64_t size)
{
    if (!mi.n || !size)
        return;
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    for (uint32_t i = 0; i < mi.n; i++) {
        mirror *m = mi.v[i];
        if (guest < m->guest || guest - m->guest >= m->size)
            continue;
        uint64_t off = guest - m->guest, len = size < m->size - off ? size : m->size - off;
        if (grow((void **)&mi.pending, mi.npending, &mi.cappending, sizeof *mi.pending))
            mi.pending[mi.npending++] = (span){ m, off, len };
        break;
    }
    pthread_mutex_unlock(&mi.lock);
}

bool nvn_mirror_takes_unmarked(void) { return __atomic_load_n(&mi.n, __ATOMIC_ACQUIRE) == 0; }

int nvn_mirror_refuse_unmarked(replay *r, const char *key)
{
    return nvn_vk_stop(r,
                       "program %s stores through addresses it builds and its translation does not mark the pages it "
                       "writes: the shader table was made for a device that imports guest memory, and nvn.memory "
                       "mirrors the pools here, so every batch would read all of them back. Make the table for this "
                       "run's shaders/device.json (guestMemoryMirrored true): the on-device translation does that "
                       "unless RELIFT_NVN_XLATE=off, or run relift shaders with that device.json",
                       key);
}

void nvn_mirror_written_all(void)
{
    if (!mi.n)
        return;
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    for (uint32_t i = 0; i < mi.n; i++)
        if (grow((void **)&mi.pending, mi.npending, &mi.cappending, sizeof *mi.pending))
            mi.pending[mi.npending++] = (span){ mi.v[i], 0, mi.v[i]->size };
    pthread_mutex_unlock(&mi.lock);
}

/** @brief Makes pages_dev, the device-local bitmap programs mark, and returns its device address, 0 on failure. */
static uint64_t device_bitmap(void)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = WRITTEN_PAGE_BITS / 8,
                               .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if (VK(vkCreateBuffer)(g.dev, &bci, NULL, &mi.pages_dev) != VK_SUCCESS)
        return 0;
    VkMemoryRequirements req;
    VK(vkGetBufferMemoryRequirements)(g.dev, mi.pages_dev, &req);
    int32_t t = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateFlagsInfo fl = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                     .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .pNext = &fl,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)t };
    if (t < 0 || VK(vkAllocateMemory)(g.dev, &mai, NULL, &mi.pages_dev_mem) != VK_SUCCESS ||
        VK(vkBindBufferMemory)(g.dev, mi.pages_dev, mi.pages_dev_mem, 0) != VK_SUCCESS)
        return 0;
    VkBufferDeviceAddressInfo ai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = mi.pages_dev };
    return VK(vkGetBufferDeviceAddress)(g.dev, &ai);
}

uint64_t nvn_mirror_written_pages_address(void)
{
    /* Made even with no mirror: a table translated for a mirroring host marks pages from any program that stores
     * through a pointer, and a mark through address 0 faults on the device. */
    uint64_t made = __atomic_load_n(&mi.pages_gpu, __ATOMIC_ACQUIRE);
    if (made)
        return made;
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    if (!mi.pages_gpu && g.ok) {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                   .size = WRITTEN_PAGE_BITS / 8,
                                   .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        VkMemoryRequirements req = { 0 };
        int32_t t = -1;
        if (VK(vkCreateBuffer)(g.dev, &bci, NULL, &mi.pages_buf) == VK_SUCCESS) {
            VK(vkGetBufferMemoryRequirements)(g.dev, mi.pages_buf, &req);
            t = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            if (t < 0)
                t = nvn_vk_mem_type(req.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .allocationSize = req.size,
                                     .memoryTypeIndex = (uint32_t)t };
        void *p = NULL;
        if (t >= 0 && VK(vkAllocateMemory)(g.dev, &mai, NULL, &mi.pages_mem) == VK_SUCCESS &&
            VK(vkBindBufferMemory)(g.dev, mi.pages_buf, mi.pages_mem, 0) == VK_SUCCESS &&
            VK(vkMapMemory)(g.dev, mi.pages_mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
            memset(p, 0, WRITTEN_PAGE_BITS / 8);
            mi.pages_map = p;
            uint64_t dev = device_bitmap();
            if (dev) {
                mi.pages_fresh = true;
                __atomic_store_n(&mi.pages_gpu, dev, __ATOMIC_RELEASE);
            } else {
                rl_log(RL_LOG_ERROR, "nvn", "cannot make the written-page bitmap in device memory");
            }
        } else {
            rl_log(RL_LOG_ERROR, "nvn", "cannot make the written-page bitmap");
        }
    }
    uint64_t a = mi.pages_gpu;
    pthread_mutex_unlock(&mi.lock);
    return a;
}

void nvn_mirror_marks_take(void (*each)(uint64_t gpu_page, void *user), void *user)
{
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    uint32_t *bits = mi.pages_map;
    for (uint32_t w = 0; bits && w < WRITTEN_PAGE_BITS / 32; w++) {
        uint32_t v = __atomic_exchange_n(&bits[w], 0u, __ATOMIC_ACQ_REL);
        for (; v; v &= v - 1)
            each((uint64_t)w * 32u + (uint64_t)__builtin_ctz(v), user);
    }
    pthread_mutex_unlock(&mi.lock);
}

void nvn_mirror_written_marked(void)
{
    if (!mi.n)
        return;
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    mi.marked = true;
    pthread_mutex_unlock(&mi.lock);
}

/**
 * @brief Notes as written the mirror pages whose bit the batch set in the bitmap, and clears it. A bit stands for every
 * page whose GPU page number is equal modulo the bitmap's size, so a page can be noted that the batch did not write;
 * the readback copies only the bytes that differ.
 */
static void note_marked_pages(void)
{
    const uint32_t *bits = mi.pages_map;
    for (uint32_t i = 0; i < mi.n; i++) {
        mirror *m = mi.v[i];
        uint64_t run = 0, run_len = 0;
        for (uint64_t off = 0; off < m->size; off += PAGE) {
            uint64_t b = ((m->gpu + off) / PAGE) & (WRITTEN_PAGE_BITS - 1);
            bool set = bits[b / 32] >> (b % 32) & 1;
            if (set && run_len && run + run_len == off) {
                run_len += PAGE;
            } else if (set) {
                if (run_len && grow((void **)&mi.pending, mi.npending, &mi.cappending, sizeof *mi.pending))
                    mi.pending[mi.npending++] = (span){ m, run, run_len };
                run = off;
                run_len = PAGE;
            }
            mi.stat_marked_pages += set;
        }
        if (run_len) {
            if (run + run_len > m->size)
                run_len = m->size - run;
            if (grow((void **)&mi.pending, mi.npending, &mi.cappending, sizeof *mi.pending))
                mi.pending[mi.npending++] = (span){ m, run, run_len };
        }
    }
    memset(mi.pages_map, 0, WRITTEN_PAGE_BITS / 8);
}

static int span_cmp(const void *a, const void *b)
{
    const span *x = a, *y = b;
    if (x->m != y->m)
        return x->m < y->m ? -1 : 1;
    return x->off < y->off ? -1 : x->off > y->off;
}

/** @brief Sorts and merges the pending spans of the batch that completes, so each byte is read back once. */
static void coalesce(void)
{
    if (mi.npending < 2)
        return;
    qsort(mi.pending, mi.npending, sizeof *mi.pending, span_cmp);
    uint32_t keep = 0;
    for (uint32_t i = 1; i < mi.npending; i++) {
        span *k = &mi.pending[keep], *s = &mi.pending[i];
        if (s->m == k->m && s->off <= k->off + k->len) {
            if (s->off + s->len > k->off + k->len)
                k->len = s->off + s->len - k->off;
        } else {
            mi.pending[++keep] = *s;
        }
    }
    mi.npending = keep + 1;
}

bool nvn_mirror_readback_pending(void)
{
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    bool more = mi.cursor < mi.npending || mi.marked || mi.marked_sent;
    pthread_mutex_unlock(&mi.lock);
    return more;
}

bool nvn_mirror_record_readback(VkCommandBuffer cb)
{
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    if (mi.cursor == 0 && mi.cursor_off == 0)
        coalesce();
    bool ok = true;
    mi.nseg = 0;
    if (mi.cursor < mi.npending) {
        if (!mi.rb.buf)
            ok = nvn_vk_readback_new(&mi.rb, READBACK);
        uint64_t at = 0;
        while (ok && mi.cursor < mi.npending && at < READBACK && mi.nseg < sizeof mi.seg / sizeof *mi.seg) {
            span *s = &mi.pending[mi.cursor];
            uint64_t len = s->len - mi.cursor_off;
            if (len > READBACK - at)
                len = READBACK - at;
            if (at == 0)
                nvn_vk_barrier(cb);
            VkBufferCopy bc = { .srcOffset = s->off + mi.cursor_off, .dstOffset = at, .size = len };
            VK(vkCmdCopyBuffer)(cb, s->m->buf, mi.rb.buf, 1, &bc);
            mi.seg[mi.nseg++] = (segment){ s->m, s->off + mi.cursor_off, len, at };
            mi.stat_read += len;
            at += len;
            mi.cursor_off += len;
            if (mi.cursor_off == s->len) {
                mi.cursor++;
                mi.cursor_off = 0;
            }
        }
        if (at)
            nvn_vk_barrier(cb);
    }
    if (mi.marked && mi.pages_dev) {
        nvn_vk_barrier(cb);
        VkBufferCopy bc = { .size = WRITTEN_PAGE_BITS / 8 };
        VK(vkCmdCopyBuffer)(cb, mi.pages_dev, mi.pages_buf, 1, &bc);
        nvn_vk_barrier(cb);
        VK(vkCmdFillBuffer)(cb, mi.pages_dev, 0, VK_WHOLE_SIZE, 0);
        nvn_vk_barrier(cb);
    }
    if (mi.marked) {
        mi.marked = false;
        mi.marked_sent = true;
    }
    pthread_mutex_unlock(&mi.lock);
    return ok;
}

void nvn_mirror_apply_readback(void)
{
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    for (uint32_t i = 0; i < mi.nseg; i++) {
        segment *s = &mi.seg[i];
        uint8_t *dst = rl_mem_base() + s->m->guest + s->off;
        const uint8_t *src = mi.rb.map + s->rb;
        uint64_t done = 0;
        while (done < s->len) {
            uint64_t guest = s->m->guest + s->off + done;
            uint64_t n = PAGE - (guest & (PAGE - 1));
            if (n > s->len - done)
                n = s->len - done;
            if (memcmp(dst + done, src + done, n)) {
                nvn_wwatch_note(guest, n, "a mirror readback");
                memcpy(dst + done, src + done, n);
                mi.stat_down += n;
            }
            done += n;
        }
    }
    mi.nseg = 0;
    if (mi.cursor >= mi.npending) {
        mi.npending = 0;
        mi.cursor = 0;
        mi.cursor_off = 0;
    }
    if (mi.marked_sent && mi.pages_map) {
        mi.marked_sent = false;
        note_marked_pages();
    }
    pthread_mutex_unlock(&mi.lock);
}

void nvn_mirror_drop_readback(void)
{
    pthread_once(&mi_once, mi_init);
    pthread_mutex_lock(&mi.lock);
    mi.npending = 0;
    mi.cursor = 0;
    mi.cursor_off = 0;
    mi.nseg = 0;
    if (mi.marked_sent && mi.pages_map)
        memset(mi.pages_map, 0, WRITTEN_PAGE_BITS / 8);
    mi.marked_sent = false;
    pthread_mutex_unlock(&mi.lock);
}

void nvn_mirror_stats(uint64_t *up, uint64_t *down, uint64_t *syncs, uint64_t *read, uint64_t *marked)
{
    *read = mi.stat_read;
    *up = mi.stat_up;
    *down = mi.stat_down;
    *syncs = mi.stat_syncs;
    *marked = mi.stat_marked_pages;
}

uint64_t nvn_mirror_staging_made(void) { return mi.stat_chunks; }

const char *nvn_mirror_failure(void) { return mi.fail[0] ? mi.fail : "the device failed"; }
