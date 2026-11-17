/**
 * @file vk_image.c
 * @brief Images in a format the layer decodes (vk_decode.c): BC1 to BC7 and
 * R4G4 on a host that cannot sample them.
 *
 * Such an optimal-tiling image is created in the substitute host format, and
 * its views in the substitute of theirs. The guest's memory requirements are
 * those of the compressed texels; the host image gets its own allocation at
 * bind time, freed with the image.
 *
 * Command buffers are recorded before the data they copy is final, so a copy
 * from a buffer to such an image becomes a copy from a staging buffer the
 * layer owns, and the region is decoded from the guest buffer's bytes into
 * the staging buffer at each submit of the command buffer (or of a primary
 * that executes it). The source buffer's bytes are read through its memory
 * binding: the guest's mapping, or a host mapping of host-visible memory
 * the guest has not mapped; a region in device-only memory stays undecoded,
 * with a log line. Memory the CPU does not cache is copied out in one pass
 * first (a decoder reading it in small loads runs several times slower).
 * Staging prefers host-cached memory for the same reason. Copies between two such images copy the substitute
 * texels; copies from such an image to a buffer would need an encoder and
 * are dropped, with a log line. Linear images keep their format.
 */
#include "vkf.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/** @brief An image in a substitute format. */
typedef struct vkf_image {
    const vkf_decode_format *f;
    VkExtent3D extent;
    uint32_t mips, layers;
    /** @brief The host allocation it is bound to, NULL before vkBindImageMemory. */
    VkDeviceMemory memory;
    uint64_t host_bytes, guest_bytes;
} vkf_image;

/** @brief A host-visible staging buffer the decoded texels of uploads go to. */
typedef struct chunk {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *ptr;
    uint64_t size, used;
} chunk;

/** @brief One region to decode at submit. */
typedef struct upload {
    const vkf_decode_format *f;
    /** @brief Guest source buffer and the byte offset of the region in it. */
    uint64_t buffer, offset;
    uint64_t row_pitch, slice_pitch, src_bytes;
    uint32_t w, h, slices;
    uint8_t *dst;
} upload;

/** @brief The uploads of one command buffer since it was begun. */
typedef struct cb_uploads {
    uint64_t pool;
    chunk *chunks;
    uint32_t nchunks, cchunks;
    upload *ups;
    uint32_t nups, cups;
    VkCommandBuffer *secs;
    uint32_t nsecs, csecs;
} cb_uploads;

/** @brief The memory a buffer is bound to. */
typedef struct binding {
    uint64_t memory, offset;
} binding;

enum { CHUNK_SIZE = 4 << 20, STAGE_ALIGN = 16 };

static void grow(void **p, uint32_t *cap, uint32_t n, size_t size)
{
    if (n < *cap)
        return;
    *cap = *cap ? *cap * 2 : 8;
    *p = realloc(*p, *cap * size);
    if (!*p)
        abort();
}

static vkf_image *image_of(vkf_dev *d, uint64_t image)
{
    if (!d->decoding || !image)
        return NULL;
    pthread_mutex_lock(&d->decode_lock);
    vkf_image *r = vkf_map_get(d->images, image);
    pthread_mutex_unlock(&d->decode_lock);
    return r;
}

static uint32_t mip_dim(uint32_t v, uint32_t level) { return v >> level ? v >> level : 1; }

static uint32_t blocks(uint32_t texels, uint32_t block) { return (texels + block - 1) / block; }

/** @brief Bytes of the compressed texels of the image: what the guest allocates for it. */
static uint64_t guest_bytes(const vkf_image *im)
{
    uint64_t n = 0;
    for (uint32_t m = 0; m < im->mips; m++)
        n += (uint64_t)blocks(mip_dim(im->extent.width, m), im->f->block_w) * blocks(mip_dim(im->extent.height, m), im->f->block_h) *
             mip_dim(im->extent.depth, m) * im->layers * im->f->block_bytes;
    return n;
}

static void substitute_formats(vkf_call *k, VkImageCreateInfo *ci, const vkf_decode_format *f)
{
    ci->format = f->host;
    ci->flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext) {
        if (b->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)
            continue;
        VkImageFormatListCreateInfo *l = (VkImageFormatListCreateInfo *)b;
        VkFormat *v = vkf_alloc(k, (l->viewFormatCount + 1) * sizeof *v);
        for (uint32_t i = 0; i < l->viewFormatCount; i++) {
            const vkf_decode_format *g = vkf_decode_format_of(l->pViewFormats[i]);
            v[i] = g ? g->host : l->pViewFormats[i];
        }
        l->pViewFormats = v;
    }
}

uint64_t vkh_vkCreateImage(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkCreateImage, c->x[0]);
    VkImageCreateInfo *ci = vkg_ain_VkImageCreateInfo(&k, c->x[1], 1);
    VkResult r = vkf_check_image(&k);
    if (r < 0) {
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateImage, r);
        return vkf_end(&k);
    }
    vkf_dev *d = k.obj->dev;
    VkImageCreateInfo guest = ci ? *ci : (VkImageCreateInfo){ 0 };
    if (ci && vkf_depth_substituted(d, ci->format)) {
        ci->format = vkf_depth_format(d, ci->format);
        vkf_depth_format_list(&k, d, ci->pNext);
    }
    const vkf_decode_format *f = ci ? vkf_decode_for(d, ci->format) : NULL;
    if (f && ci->tiling != VK_IMAGE_TILING_OPTIMAL) {
        if (rl_once(0x4C494E4541520000ull | (uint64_t)ci->format))
            rl_log(RL_LOG_WARN, "vk", "vkCreateImage: a linear image of format %d keeps its format on the host", (int)ci->format);
        f = NULL;
    }
    vkf_image *im = NULL;
    if (f) {
        im = calloc(1, sizeof *im);
        if (!im)
            abort();
        *im = (vkf_image){ .f = f, .extent = ci->extent, .mips = ci->mipLevels, .layers = ci->arrayLayers };
        im->guest_bytes = guest_bytes(im);
        substitute_formats(&k, ci, f);
    }
    VkImage img = NULL;
    r = VKF_FN(k.fns, vkCreateImage)(dev, ci, NULL, &img);
    if (r >= 0 && c->x[3])
        vkf_s64(&k, c->x[3], (uint64_t)(uintptr_t)img);
    if (r >= 0)
        vkf_depth_note_image(d, (uint64_t)(uintptr_t)img, &guest);
    if (r >= 0 && im) {
        pthread_mutex_lock(&d->decode_lock);
        vkf_map_put(d->images, (uint64_t)(uintptr_t)img, im);
        pthread_mutex_unlock(&d->decode_lock);
    } else
        free(im);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateImage, r);
    return vkf_end(&k);
}

void vkf_image_destroyed(vkf_call *k)
{
    vkf_dev *d = k->obj->dev;
    uint64_t h = k->c->x[1];
    if (d->depth24)
        vkf_depth_forget_image(h);
    if (!d->decoding || !h)
        return;
    pthread_mutex_lock(&d->decode_lock);
    vkf_image *im = vkf_map_get(d->images, h);
    vkf_map_put(d->images, h, NULL);
    pthread_mutex_unlock(&d->decode_lock);
    if (!im)
        return;
    if (im->memory) {
        VKF_FN(k->fns, vkFreeMemory)((VkDevice)k->obj->host, im->memory, NULL);
        vkf_decode_note_image(-1, -(int64_t)im->host_bytes, -(int64_t)im->guest_bytes);
    }
    free(im);
}

static void reqs_out(vkf_call *k, uint64_t image, uint64_t g)
{
    vkf_image *im = g ? image_of(k->obj->dev, image) : NULL;
    if (!im)
        return;
    uint64_t align = vkf_g64(k, g + offsetof(VkMemoryRequirements, alignment));
    uint64_t n = im->guest_bytes;
    if (align > 1)
        n = (n + align - 1) / align * align;
    vkf_s64(k, g + offsetof(VkMemoryRequirements, size), n);
}

void vkf_image_reqs_out(vkf_call *k)
{
    reqs_out(k, k->c->x[1], k->c->x[2]);
    vkf_mem_reqs_out(k);
}

void vkf_image_reqs2_out(vkf_call *k)
{
    uint64_t info = k->c->x[1];
    if (info && k->c->x[2])
        reqs_out(k, vkf_g64(k, info + offsetof(VkImageMemoryRequirementsInfo2, image)),
                 k->c->x[2] + offsetof(VkMemoryRequirements2, memoryRequirements));
    vkf_mem_reqs2_out(k);
}

static uint32_t pick_type(const vkf_dev *d, uint32_t bits, uint32_t want)
{
    for (uint32_t t = 0; t < d->type_count; t++)
        if ((bits & (1u << t)) && (d->type_flags[t] & want) == want)
            return t;
    return UINT32_MAX;
}

/** @brief Gives the host image of im its own allocation and binds it. */
static VkResult bind_own(vkf_call *k, VkDevice dev, VkImage img, vkf_image *im)
{
    vkf_dev *d = k->obj->dev;
    if (im->memory) {
        rl_log(RL_LOG_ERROR, "vk", "vkBindImageMemory: an image in a decoded format is bound twice");
        return VK_ERROR_UNKNOWN;
    }
    VkMemoryRequirements req;
    VKF_FN(k->fns, vkGetImageMemoryRequirements)(dev, img, &req);
    uint32_t t = pick_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX)
        t = pick_type(d, req.memoryTypeBits, 0);
    if (t == UINT32_MAX)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img };
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &ded,
        .allocationSize = req.size,
        .memoryTypeIndex = t,
    };
    VkDeviceMemory m = NULL;
    VkResult r = VKF_FN(k->fns, vkAllocateMemory)(dev, &ai, NULL, &m);
    if (r < 0)
        return r;
    r = VKF_FN(k->fns, vkBindImageMemory)(dev, img, m, 0);
    if (r < 0) {
        VKF_FN(k->fns, vkFreeMemory)(dev, m, NULL);
        return r;
    }
    im->memory = m;
    im->host_bytes = req.size;
    vkf_decode_note_image(1, (int64_t)req.size, (int64_t)im->guest_bytes);
    return VK_SUCCESS;
}

uint64_t vkh_vkBindImageMemory(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkBindImageMemory, c->x[0]);
    VkImage img = (VkImage)(uintptr_t)c->x[1];
    vkf_image *im = image_of(k.obj->dev, c->x[1]);
    VkResult r = im ? bind_own(&k, dev, img, im)
                    : VKF_FN(k.fns, vkBindImageMemory)(dev, img, (VkDeviceMemory)(uintptr_t)c->x[2], c->x[3]);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkBindImageMemory, r);
    return vkf_end(&k);
}

uint64_t vkh_vkBindImageMemory2(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkBindImageMemory2, c->x[0]);
    PFN_vkBindImageMemory2 fn = VKF_FN(k.fns, vkBindImageMemory2);
    if (!fn)
        return vkf_no_fn(&k, VKG_vkBindImageMemory2);
    uint32_t n = (uint32_t)c->x[1];
    VkBindImageMemoryInfo *bi = vkg_ain_VkBindImageMemoryInfo(&k, c->x[2], n);
    VkBindImageMemoryInfo *rest = vkf_alloc(&k, (n + 1) * sizeof *rest);
    uint32_t m = 0;
    VkResult r = VK_SUCCESS;
    for (uint32_t i = 0; bi && i < n && r >= 0; i++) {
        vkf_image *im = image_of(k.obj->dev, (uint64_t)(uintptr_t)bi[i].image);
        if (im)
            r = bind_own(&k, dev, bi[i].image, im);
        else
            rest[m++] = bi[i];
    }
    if (r >= 0 && m)
        r = fn(dev, m, rest);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkBindImageMemory2, r);
    return vkf_end(&k);
}

uint64_t vkh_vkCreateImageView(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkCreateImageView, c->x[0]);
    VkImageViewCreateInfo *ci = vkg_ain_VkImageViewCreateInfo(&k, c->x[1], 1);
    vkf_image *im = ci ? image_of(k.obj->dev, (uint64_t)(uintptr_t)ci->image) : NULL;
    if (im) {
        const vkf_decode_format *f = vkf_decode_format_of(ci->format);
        if (!f || f->texel_bytes != im->f->texel_bytes) {
            if (rl_once(0x5649455700000000ull | (uint64_t)ci->format))
                rl_log(RL_LOG_WARN, "vk", "vkCreateImageView: a view of format %d of an image of format %d is a view of format %d",
                       (int)ci->format, (int)im->f->format, (int)im->f->host);
            f = im->f;
        }
        ci->format = f->host;
    }
    VkFormat guest = ci ? ci->format : VK_FORMAT_UNDEFINED;
    if (ci)
        ci->format = vkf_depth_format(k.obj->dev, ci->format);
    VkImageView v = NULL;
    VkResult r = VKF_FN(k.fns, vkCreateImageView)(dev, ci, NULL, &v);
    if (r >= 0)
        vkf_depth_note_view(k.obj->dev, (uint64_t)(uintptr_t)v, guest);
    if (r >= 0 && c->x[3])
        vkf_s64(&k, c->x[3], (uint64_t)(uintptr_t)v);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateImageView, r);
    return vkf_end(&k);
}

static void destroy_chunk(const vkf_fns *fns, VkDevice dev, chunk *ch)
{
    VKF_FN(fns, vkDestroyBuffer)(dev, ch->buf, NULL);
    VKF_FN(fns, vkFreeMemory)(dev, ch->mem, NULL);
}

static cb_uploads *uploads_of(vkf_dev *d, VkCommandBuffer cb, uint64_t pool)
{
    cb_uploads *u = vkf_map_get(d->uploads, (uint64_t)(uintptr_t)cb);
    if (!u) {
        u = calloc(1, sizeof *u);
        if (!u)
            abort();
        u->pool = pool;
        vkf_map_put(d->uploads, (uint64_t)(uintptr_t)cb, u);
    }
    return u;
}

/** @brief Room for n bytes of staging in the command buffer's chunks; the buffer and byte offset in *buf and *off. */
static uint8_t *stage(vkf_call *k, VkDevice dev, cb_uploads *u, uint64_t n, VkBuffer *buf, uint64_t *off)
{
    vkf_dev *d = k->obj->dev;
    for (uint32_t i = 0; i < u->nchunks; i++) {
        chunk *ch = &u->chunks[i];
        uint64_t at = (ch->used + STAGE_ALIGN - 1) & ~(uint64_t)(STAGE_ALIGN - 1);
        if (at + n <= ch->size) {
            ch->used = at + n;
            *buf = ch->buf;
            *off = at;
            return ch->ptr + at;
        }
    }
    chunk ch = { .size = n > CHUNK_SIZE ? n : CHUNK_SIZE };
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = ch.size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (VKF_FN(k->fns, vkCreateBuffer)(dev, &bci, NULL, &ch.buf) < 0)
        return NULL;
    VkMemoryRequirements req;
    VKF_FN(k->fns, vkGetBufferMemoryRequirements)(dev, ch.buf, &req);
    const VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t t = pick_type(d, req.memoryTypeBits, coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t == UINT32_MAX)
        t = pick_type(d, req.memoryTypeBits, coherent);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = t };
    void *p = NULL;
    if (t == UINT32_MAX || VKF_FN(k->fns, vkAllocateMemory)(dev, &ai, NULL, &ch.mem) < 0) {
        VKF_FN(k->fns, vkDestroyBuffer)(dev, ch.buf, NULL);
        return NULL;
    }
    if (VKF_FN(k->fns, vkBindBufferMemory)(dev, ch.buf, ch.mem, 0) < 0 ||
        VKF_FN(k->fns, vkMapMemory)(dev, ch.mem, 0, VK_WHOLE_SIZE, 0, &p) < 0) {
        destroy_chunk(k->fns, dev, &ch);
        return NULL;
    }
    ch.ptr = p;
    ch.used = n;
    grow((void **)&u->chunks, &u->cchunks, u->nchunks, sizeof *u->chunks);
    u->chunks[u->nchunks++] = ch;
    *buf = ch.buf;
    *off = 0;
    return ch.ptr;
}

/** @brief The part of a buffer-image region inside the mip level, in texels. */
static VkExtent3D clip(const vkf_image *im, const VkImageSubresourceLayers *s, VkOffset3D o, VkExtent3D e)
{
    uint32_t mw = mip_dim(im->extent.width, s->mipLevel), mh = mip_dim(im->extent.height, s->mipLevel),
             md = mip_dim(im->extent.depth, s->mipLevel);
    VkExtent3D r = { 0, 0, 0 };
    if (o.x < 0 || o.y < 0 || o.z < 0 || (uint32_t)o.x >= mw || (uint32_t)o.y >= mh || (uint32_t)o.z >= md)
        return r;
    r.width = e.width < mw - (uint32_t)o.x ? e.width : mw - (uint32_t)o.x;
    r.height = e.height < mh - (uint32_t)o.y ? e.height : mh - (uint32_t)o.y;
    r.depth = e.depth < md - (uint32_t)o.z ? e.depth : md - (uint32_t)o.z;
    return r;
}

/**
 * @brief Turns the guest's buffer-to-image regions for im into regions from
 * staging and records their uploads; returns the staging buffer, NULL when
 * nothing is left to copy (*m regions are written to out).
 */
static VkBuffer record_uploads(vkf_call *k, VkCommandBuffer cb, uint64_t buffer, const vkf_image *im, const VkBufferImageCopy *in,
                               uint32_t n, VkBufferImageCopy *out, uint32_t *m)
{
    vkf_dev *d = k->obj->dev;
    VkDevice dev = (VkDevice)k->obj->parent->host;
    const vkf_decode_format *f = im->f;
    uint64_t *at = vkf_alloc(k, (n + 1) * sizeof *at), total = 0;
    VkExtent3D *ext = vkf_alloc(k, (n + 1) * sizeof *ext);
    for (uint32_t i = 0; i < n; i++) {
        ext[i] = clip(im, &in[i].imageSubresource, in[i].imageOffset, in[i].imageExtent);
        uint64_t bytes = (uint64_t)ext[i].width * ext[i].height * ext[i].depth * in[i].imageSubresource.layerCount * f->texel_bytes;
        at[i] = (total + STAGE_ALIGN - 1) & ~(uint64_t)(STAGE_ALIGN - 1);
        total = at[i] + bytes;
    }
    *m = 0;
    if (!total)
        return NULL;
    pthread_mutex_lock(&d->decode_lock);
    cb_uploads *u = uploads_of(d, cb, k->obj->pool);
    VkBuffer buf = NULL;
    uint64_t base = 0;
    uint8_t *p = stage(k, dev, u, total, &buf, &base);
    for (uint32_t i = 0; p && i < n; i++) {
        if (!ext[i].width || !ext[i].height || !ext[i].depth || !in[i].imageSubresource.layerCount)
            continue;
        uint32_t row = in[i].bufferRowLength ? in[i].bufferRowLength : in[i].imageExtent.width;
        uint32_t height = in[i].bufferImageHeight ? in[i].bufferImageHeight : in[i].imageExtent.height;
        upload up = {
            .f = f,
            .buffer = buffer,
            .offset = in[i].bufferOffset,
            .row_pitch = (uint64_t)blocks(row, f->block_w) * f->block_bytes,
            .w = ext[i].width,
            .h = ext[i].height,
            .slices = ext[i].depth * in[i].imageSubresource.layerCount,
            .dst = p + at[i],
        };
        up.slice_pitch = up.row_pitch * blocks(height, f->block_h);
        up.src_bytes = (uint64_t)(up.slices - 1) * up.slice_pitch + (uint64_t)(blocks(up.h, f->block_h) - 1) * up.row_pitch +
                       (uint64_t)blocks(up.w, f->block_w) * f->block_bytes;
        grow((void **)&u->ups, &u->cups, u->nups, sizeof *u->ups);
        u->ups[u->nups++] = up;
        out[*m] = in[i];
        out[*m].bufferOffset = base + at[i];
        out[*m].bufferRowLength = 0;
        out[*m].bufferImageHeight = 0;
        out[*m].imageExtent = ext[i];
        (*m)++;
    }
    pthread_mutex_unlock(&d->decode_lock);
    if (!p)
        rl_log(RL_LOG_ERROR, "vk", "vkCmdCopyBufferToImage: no host staging memory for %llu decoded bytes; the copy is dropped",
               (unsigned long long)total);
    return p ? buf : NULL;
}

/**
 * @brief Splits buffer-to-image regions for an image whose 24-bit depth
 * format reaches the host as D32: the depth-aspect ones are recorded as
 * uploads decoded to D32 float at submit (*dm regions from staging buffer
 * *stg into dout), the others (stencil) stay as they are (*pm into pout).
 * False, with nothing written, when the image is not such an image.
 */
static bool depth_uploads(vkf_call *k, VkCommandBuffer cb, uint64_t buffer, uint64_t img, const VkBufferImageCopy *in, uint32_t n,
                          VkBuffer *stg, VkBufferImageCopy *dout, uint32_t *dm, VkBufferImageCopy *pout, uint32_t *pm)
{
    vkf_image tmp = { 0 };
    VkFormat guest;
    if (!k->obj->dev->depth24 || !vkf_depth_image_of(img, &guest, &tmp.extent, &tmp.mips, &tmp.layers))
        return false;
    tmp.f = vkf_decode_depth_format(guest);
    VkBufferImageCopy *din = vkf_alloc(k, (n + 1) * sizeof *din);
    uint32_t nd = 0;
    *dm = *pm = 0;
    *stg = NULL;
    for (uint32_t i = 0; i < n; i++) {
        if ((in[i].imageSubresource.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) && tmp.f)
            din[nd++] = in[i];
        else
            pout[(*pm)++] = in[i];
    }
    if (nd) {
        if (rl_once(0x4432345550000000ull))
            rl_log(RL_LOG_INFO, "vk", "depth uploads to format %d images are converted to D32 float at submit", (int)guest);
        __atomic_store_n(&k->obj->dev->decoding, true, __ATOMIC_RELAXED);
        *stg = record_uploads(k, cb, buffer, &tmp, din, nd, dout, dm);
    }
    return true;
}

uint64_t vkh_vkCmdCopyBufferToImage(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkCommandBuffer cb = vkf_first(&k, VKG_KIND_VkCommandBuffer, c->x[0]);
    if (!cb)
        return vkf_bad_handle(&k, VKG_vkCmdCopyBufferToImage, c->x[0]);
    vkf_split_out(&k);
    VkImage img = (VkImage)(uintptr_t)c->x[2];
    uint32_t n = (uint32_t)c->x[4];
    const VkBufferImageCopy *rg = vkf_ptr(&k, c->x[5]);
    vkf_image *im = image_of(k.obj->dev, c->x[2]);
    VkBuffer src = (VkBuffer)(uintptr_t)c->x[1];
    if (!im && rg && k.obj->dev->depth24) {
        VkBufferImageCopy *dout = vkf_alloc(&k, (n + 1) * sizeof *dout), *pout = vkf_alloc(&k, (n + 1) * sizeof *pout);
        uint32_t dm = 0, pm = 0;
        VkBuffer stg = NULL;
        if (depth_uploads(&k, cb, c->x[1], c->x[2], rg, n, &stg, dout, &dm, pout, &pm)) {
            if (stg && dm)
                VKF_FN(k.fns, vkCmdCopyBufferToImage)(cb, stg, img, (VkImageLayout)(uint32_t)c->x[3], dm, dout);
            rg = pout;
            n = pm;
        }
    }
    if (im && rg) {
        VkBufferImageCopy *out = vkf_alloc(&k, (n + 1) * sizeof *out);
        src = record_uploads(&k, cb, c->x[1], im, rg, n, out, &n);
        rg = out;
    }
    if (src && n)
        VKF_FN(k.fns, vkCmdCopyBufferToImage)(cb, src, img, (VkImageLayout)(uint32_t)c->x[3], n, rg);
    return vkf_end(&k);
}

uint64_t vkh_vkCmdCopyBufferToImage2(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkCommandBuffer cb = vkf_first(&k, VKG_KIND_VkCommandBuffer, c->x[0]);
    if (!cb)
        return vkf_bad_handle(&k, VKG_vkCmdCopyBufferToImage2, c->x[0]);
    vkf_split_out(&k);
    PFN_vkCmdCopyBufferToImage2 fn = VKF_FN(k.fns, vkCmdCopyBufferToImage2);
    if (!fn)
        return vkf_no_fn(&k, VKG_vkCmdCopyBufferToImage2);
    VkCopyBufferToImageInfo2 *info = vkg_ain_VkCopyBufferToImageInfo2(&k, c->x[1], 1);
    vkf_image *im = info ? image_of(k.obj->dev, (uint64_t)(uintptr_t)info->dstImage) : NULL;
    if (!im && info && info->pRegions && k.obj->dev->depth24) {
        uint32_t n = info->regionCount, dm = 0, pm = 0;
        VkBufferImageCopy *in = vkf_alloc(&k, (n + 1) * sizeof *in), *dout = vkf_alloc(&k, (n + 1) * sizeof *dout),
                          *pout = vkf_alloc(&k, (n + 1) * sizeof *pout);
        for (uint32_t i = 0; i < n; i++) {
            const VkBufferImageCopy2 *r = &info->pRegions[i];
            in[i] = (VkBufferImageCopy){ r->bufferOffset, r->bufferRowLength, r->bufferImageHeight, r->imageSubresource, r->imageOffset,
                                         r->imageExtent };
        }
        VkBuffer stg = NULL;
        if (depth_uploads(&k, cb, (uint64_t)(uintptr_t)info->srcBuffer, (uint64_t)(uintptr_t)info->dstImage, in, n, &stg, dout, &dm, pout,
                          &pm)) {
            VkCopyBufferToImageInfo2 parts[2] = { *info, *info };
            const VkBufferImageCopy *src[2] = { dout, pout };
            uint32_t cnt[2] = { stg ? dm : 0, pm };
            parts[0].srcBuffer = stg;
            for (int p = 0; p < 2; p++) {
                if (!cnt[p])
                    continue;
                VkBufferImageCopy2 *r2 = vkf_alloc(&k, cnt[p] * sizeof *r2);
                for (uint32_t i = 0; i < cnt[p]; i++)
                    r2[i] = (VkBufferImageCopy2){ .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                                                  .bufferOffset = src[p][i].bufferOffset,
                                                  .bufferRowLength = src[p][i].bufferRowLength,
                                                  .bufferImageHeight = src[p][i].bufferImageHeight,
                                                  .imageSubresource = src[p][i].imageSubresource,
                                                  .imageOffset = src[p][i].imageOffset,
                                                  .imageExtent = src[p][i].imageExtent };
                parts[p].pRegions = r2;
                parts[p].regionCount = cnt[p];
                fn(cb, &parts[p]);
            }
            return vkf_end(&k);
        }
    }
    if (im && info->pRegions) {
        uint32_t n = info->regionCount, m = 0;
        VkBufferImageCopy *in = vkf_alloc(&k, (n + 1) * sizeof *in), *out = vkf_alloc(&k, (n + 1) * sizeof *out);
        for (uint32_t i = 0; i < n; i++) {
            const VkBufferImageCopy2 *r = &info->pRegions[i];
            in[i] = (VkBufferImageCopy){ r->bufferOffset, r->bufferRowLength, r->bufferImageHeight, r->imageSubresource,
                                         r->imageOffset, r->imageExtent };
        }
        info->srcBuffer = record_uploads(&k, cb, (uint64_t)(uintptr_t)info->srcBuffer, im, in, n, out, &m);
        VkBufferImageCopy2 *r2 = vkf_alloc(&k, (m + 1) * sizeof *r2);
        for (uint32_t i = 0; i < m; i++)
            r2[i] = (VkBufferImageCopy2){ .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                                          .bufferOffset = out[i].bufferOffset,
                                          .imageSubresource = out[i].imageSubresource,
                                          .imageOffset = out[i].imageOffset,
                                          .imageExtent = out[i].imageExtent };
        info->pRegions = r2;
        info->regionCount = m;
        if (!info->srcBuffer || !m)
            return vkf_end(&k);
    }
    if (info)
        fn(cb, info);
    return vkf_end(&k);
}

static void drop_once(const char *cmd, uint64_t key, const char *why)
{
    if (rl_once(0x44524F5000000000ull | key))
        rl_log(RL_LOG_WARN, "vk", "%s: %s; the region is dropped", cmd, why);
}

/** @brief Fits the guest's image-to-image regions to the host images; *n is the regions kept. */
static VkImageCopy *copy_regions(vkf_call *k, const char *cmd, const vkf_image *s, const vkf_image *t, const VkImageCopy *in,
                                 uint32_t *n)
{
    VkImageCopy *out = vkf_alloc(k, (*n + 1) * sizeof *out);
    uint32_t m = 0;
    for (uint32_t i = 0; i < *n; i++) {
        if (!s || !t) {
            drop_once(cmd, 1, "a copy between an image in a decoded format and one in another format");
            continue;
        }
        if (s->f->texel_bytes != t->f->texel_bytes) {
            drop_once(cmd, 2, "a copy between images whose decoded texels differ in size");
            continue;
        }
        VkExtent3D a = clip(s, &in[i].srcSubresource, in[i].srcOffset, in[i].extent);
        VkExtent3D b = clip(t, &in[i].dstSubresource, in[i].dstOffset, a);
        if (!b.width || !b.height || !b.depth)
            continue;
        out[m] = in[i];
        out[m++].extent = b;
    }
    *n = m;
    return out;
}

uint64_t vkh_vkCmdCopyImage(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkCommandBuffer cb = vkf_first(&k, VKG_KIND_VkCommandBuffer, c->x[0]);
    if (!cb)
        return vkf_bad_handle(&k, VKG_vkCmdCopyImage, c->x[0]);
    vkf_split_out(&k);
    uint32_t n = (uint32_t)c->x[5];
    const VkImageCopy *rg = vkf_ptr(&k, c->x[6]);
    vkf_image *s = image_of(k.obj->dev, c->x[1]), *t = image_of(k.obj->dev, c->x[3]);
    if ((s || t) && rg)
        rg = copy_regions(&k, "vkCmdCopyImage", s, t, rg, &n);
    if (n)
        VKF_FN(k.fns, vkCmdCopyImage)(cb, (VkImage)(uintptr_t)c->x[1], (VkImageLayout)(uint32_t)c->x[2],
                                      (VkImage)(uintptr_t)c->x[3], (VkImageLayout)(uint32_t)c->x[4], n, rg);
    return vkf_end(&k);
}

uint64_t vkh_vkCmdCopyImage2(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkCommandBuffer cb = vkf_first(&k, VKG_KIND_VkCommandBuffer, c->x[0]);
    if (!cb)
        return vkf_bad_handle(&k, VKG_vkCmdCopyImage2, c->x[0]);
    vkf_split_out(&k);
    PFN_vkCmdCopyImage2 fn = VKF_FN(k.fns, vkCmdCopyImage2);
    if (!fn)
        return vkf_no_fn(&k, VKG_vkCmdCopyImage2);
    VkCopyImageInfo2 *info = vkg_ain_VkCopyImageInfo2(&k, c->x[1], 1);
    vkf_image *s = info ? image_of(k.obj->dev, (uint64_t)(uintptr_t)info->srcImage) : NULL;
    vkf_image *t = info ? image_of(k.obj->dev, (uint64_t)(uintptr_t)info->dstImage) : NULL;
    if ((s || t) && info->pRegions) {
        uint32_t n = info->regionCount;
        VkImageCopy *in = vkf_alloc(&k, (n + 1) * sizeof *in);
        for (uint32_t i = 0; i < n; i++) {
            const VkImageCopy2 *r = &info->pRegions[i];
            in[i] = (VkImageCopy){ r->srcSubresource, r->srcOffset, r->dstSubresource, r->dstOffset, r->extent };
        }
        VkImageCopy *out = copy_regions(&k, "vkCmdCopyImage2", s, t, in, &n);
        VkImageCopy2 *r2 = vkf_alloc(&k, (n + 1) * sizeof *r2);
        for (uint32_t i = 0; i < n; i++)
            r2[i] = (VkImageCopy2){ .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
                                    .srcSubresource = out[i].srcSubresource,
                                    .srcOffset = out[i].srcOffset,
                                    .dstSubresource = out[i].dstSubresource,
                                    .dstOffset = out[i].dstOffset,
                                    .extent = out[i].extent };
        info->pRegions = r2;
        info->regionCount = n;
        if (!n)
            return vkf_end(&k);
    }
    if (info)
        fn(cb, info);
    return vkf_end(&k);
}

/** @brief Logs once a depth-aspect read back of an image whose 24-bit depth reaches the host as D32 (its texels go out as D32 float). */
static void depth_readback(vkf_call *k, uint64_t img, const VkImageSubresourceLayers *s)
{
    VkFormat guest;
    VkExtent3D e;
    uint32_t mips, layers;
    if (k->obj->dev->depth24 && (s->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) && vkf_depth_image_of(img, &guest, &e, &mips, &layers) &&
        rl_once(0x4433325244000000ull))
        rl_log(RL_LOG_WARN, "vk", "a depth read back of a format %d image gives D32 float texels, not the console's 24-bit ones", (int)guest);
}

uint64_t vkh_vkCmdCopyImageToBuffer(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkCommandBuffer cb = vkf_first(&k, VKG_KIND_VkCommandBuffer, c->x[0]);
    if (!cb)
        return vkf_bad_handle(&k, VKG_vkCmdCopyImageToBuffer, c->x[0]);
    vkf_split_out(&k);
    if (image_of(k.obj->dev, c->x[1])) {
        drop_once("vkCmdCopyImageToBuffer", 3, "reading an image in a decoded format back would need an encoder");
        return vkf_end(&k);
    }
    const VkBufferImageCopy *rg = vkf_ptr(&k, c->x[5]);
    for (uint32_t i = 0; rg && i < (uint32_t)c->x[4]; i++)
        depth_readback(&k, c->x[1], &rg[i].imageSubresource);
    VKF_FN(k.fns, vkCmdCopyImageToBuffer)(cb, (VkImage)(uintptr_t)c->x[1], (VkImageLayout)(uint32_t)c->x[2],
                                          (VkBuffer)(uintptr_t)c->x[3], (uint32_t)c->x[4], vkf_ptr(&k, c->x[5]));
    return vkf_end(&k);
}

uint64_t vkh_vkCmdCopyImageToBuffer2(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkCommandBuffer cb = vkf_first(&k, VKG_KIND_VkCommandBuffer, c->x[0]);
    if (!cb)
        return vkf_bad_handle(&k, VKG_vkCmdCopyImageToBuffer2, c->x[0]);
    vkf_split_out(&k);
    PFN_vkCmdCopyImageToBuffer2 fn = VKF_FN(k.fns, vkCmdCopyImageToBuffer2);
    if (!fn)
        return vkf_no_fn(&k, VKG_vkCmdCopyImageToBuffer2);
    VkCopyImageToBufferInfo2 *info = vkg_ain_VkCopyImageToBufferInfo2(&k, c->x[1], 1);
    if (info && image_of(k.obj->dev, (uint64_t)(uintptr_t)info->srcImage)) {
        drop_once("vkCmdCopyImageToBuffer2", 3, "reading an image in a decoded format back would need an encoder");
        return vkf_end(&k);
    }
    for (uint32_t i = 0; info && info->pRegions && i < info->regionCount; i++)
        depth_readback(&k, (uint64_t)(uintptr_t)info->srcImage, &info->pRegions[i].imageSubresource);
    if (info)
        fn(cb, info);
    return vkf_end(&k);
}

void vkf_note_execute(vkf_call *k)
{
    vkf_dev *d = k->obj->dev;
    uint32_t n = (uint32_t)k->c->x[1];
    uint64_t g = k->c->x[2];
    for (uint32_t i = 0; g && i < n; i++) {
        vkf_obj *o = vkf_obj_of(vkf_g64(k, g + 8ull * i), VKG_KIND_VkCommandBuffer);
        if (o && o->as_primary && rl_once(0x5457494e45580000ull))
            rl_log(RL_LOG_ERROR, "vk", "vkCmdExecuteCommands: a secondary the guest began without inheritance info runs as a primary");
    }
    if (!d->decoding || !g || !n)
        return;
    VkCommandBuffer cb = k->obj->host;
    uint64_t pool = k->obj->pool;
    VkCommandBuffer *secs = vkf_alloc(k, n * sizeof *secs);
    for (uint32_t i = 0; i < n; i++) {
        vkf_obj *o = vkf_obj_of(vkf_g64(k, g + 8ull * i), VKG_KIND_VkCommandBuffer);
        secs[i] = o ? o->host : NULL;
    }
    pthread_mutex_lock(&d->decode_lock);
    cb_uploads *u = uploads_of(d, cb, pool);
    for (uint32_t i = 0; i < n; i++) {
        if (!secs[i])
            continue;
        grow((void **)&u->secs, &u->csecs, u->nsecs, sizeof *u->secs);
        u->secs[u->nsecs++] = secs[i];
    }
    pthread_mutex_unlock(&d->decode_lock);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void run_upload(vkf_call *k, VkDevice dev, const upload *up)
{
    vkf_dev *d = k->obj->dev;
    const binding *b = vkf_map_get(d->buffers, up->buffer);
    bool unmap = false, cached = false;
    const uint8_t *src = b ? vkf_mem_bytes(k, dev, b->memory, b->offset + up->offset, up->src_bytes, &unmap, &cached) : NULL;
    if (!src) {
        if (rl_once(0x55504C4F41440000ull | (b ? 1u : 0u)))
            rl_log(RL_LOG_WARN, "vk", "an upload to an image in a decoded format reads %s; its texels stay unset",
                   b ? "memory the CPU cannot reach" : "a buffer with no memory bound");
        return;
    }
    uint64_t t0 = now_ns();
    if (!cached) {
        static _Thread_local uint8_t *copy;
        static _Thread_local uint64_t copy_size;
        if (copy_size < up->src_bytes) {
            free(copy);
            copy = malloc(up->src_bytes);
            copy_size = copy ? up->src_bytes : 0;
        }
        if (copy) {
            memcpy(copy, src, up->src_bytes);
            src = copy;
        }
    }
    vkf_decode_region(up->f, src, up->row_pitch, up->slice_pitch, up->w, up->h, up->slices, up->dst);
    vkf_decode_note(up->src_bytes, (uint64_t)up->w * up->h * up->slices * up->f->texel_bytes, now_ns() - t0);
    vkf_mem_bytes_done(k, dev, b->memory, unmap);
}

static void run_cb(vkf_call *k, VkDevice dev, VkCommandBuffer cb, unsigned depth)
{
    cb_uploads *u = vkf_map_get(k->obj->dev->uploads, (uint64_t)(uintptr_t)cb);
    if (!u)
        return;
    for (uint32_t i = 0; i < u->nups; i++)
        run_upload(k, dev, &u->ups[i]);
    for (uint32_t i = 0; depth < 2 && i < u->nsecs; i++)
        run_cb(k, dev, u->secs[i], depth + 1);
}

void vkf_decode_uploads(vkf_call *k, VkDevice dev, const VkCommandBuffer *cbs, uint32_t n)
{
    vkf_dev *d = k->obj->dev;
    if (!d->decoding || !cbs)
        return;
    pthread_mutex_lock(&d->decode_lock);
    for (uint32_t i = 0; i < n; i++)
        run_cb(k, dev, cbs[i], 0);
    pthread_mutex_unlock(&d->decode_lock);
}

static void free_uploads(const vkf_fns *fns, VkDevice dev, cb_uploads *u, bool all)
{
    uint32_t keep = 0;
    for (uint32_t i = 0; i < u->nchunks; i++) {
        if (!all && !keep && u->chunks[i].size == CHUNK_SIZE) {
            u->chunks[keep] = u->chunks[i];
            u->chunks[keep++].used = 0;
            continue;
        }
        if (fns)
            destroy_chunk(fns, dev, &u->chunks[i]);
    }
    u->nchunks = keep;
    u->nups = 0;
    u->nsecs = 0;
    if (!all)
        return;
    free(u->chunks);
    free(u->ups);
    free(u->secs);
    free(u);
}

void vkf_decode_cb_reset(const vkf_fns *fns, void *dev, vkf_dev *d, void *cb, bool freed)
{
    if (!d->decoding || !cb)
        return;
    pthread_mutex_lock(&d->decode_lock);
    cb_uploads *u = vkf_map_get(d->uploads, (uint64_t)(uintptr_t)cb);
    if (u) {
        if (freed)
            vkf_map_put(d->uploads, (uint64_t)(uintptr_t)cb, NULL);
        free_uploads(fns, dev, u, freed);
    }
    pthread_mutex_unlock(&d->decode_lock);
}

void vkf_cb_reset_hook(vkf_call *k)
{
    if ((int32_t)(uint32_t)k->c->x[0] >= 0)
        vkf_decode_cb_reset(k->fns, k->obj->parent->host, k->obj->dev, k->obj->host, false);
}

typedef struct pool_reset {
    const vkf_fns *fns;
    VkDevice dev;
    uint64_t pool;
} pool_reset;

static void reset_in_pool(uint64_t key, void *value, void *user)
{
    (void)key;
    const pool_reset *p = user;
    cb_uploads *u = value;
    if (u->pool == p->pool)
        free_uploads(p->fns, p->dev, u, false);
}

void vkf_pool_reset_hook(vkf_call *k)
{
    vkf_dev *d = k->obj->dev;
    if (!d->decoding || (int32_t)(uint32_t)k->c->x[0] < 0)
        return;
    pool_reset p = { k->fns, k->obj->host, k->c->x[1] };
    pthread_mutex_lock(&d->decode_lock);
    vkf_map_each(d->uploads, reset_in_pool, &p);
    pthread_mutex_unlock(&d->decode_lock);
}

static void put_binding(vkf_dev *d, uint64_t buffer, uint64_t memory, uint64_t offset)
{
    binding *b = malloc(sizeof *b);
    if (!b)
        abort();
    *b = (binding){ memory, offset };
    pthread_mutex_lock(&d->decode_lock);
    free(vkf_map_get(d->buffers, buffer));
    vkf_map_put(d->buffers, buffer, b);
    pthread_mutex_unlock(&d->decode_lock);
}

void vkf_buffer_bound(vkf_call *k)
{
    vkf_dev *d = k->obj->dev;
    if ((d->decoding || d->depth24) && (int32_t)(uint32_t)k->c->x[0] >= 0 && k->c->x[1])
        put_binding(d, k->c->x[1], k->c->x[2], k->c->x[3]);
}

void vkf_buffers_bound(vkf_call *k)
{
    vkf_dev *d = k->obj->dev;
    uint32_t n = (uint32_t)k->c->x[1];
    uint64_t g = k->c->x[2];
    if (!(d->decoding || d->depth24) || (int32_t)(uint32_t)k->c->x[0] < 0 || !g)
        return;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t e = g + i * (uint64_t)sizeof(VkBindBufferMemoryInfo);
        put_binding(d, vkf_g64(k, e + offsetof(VkBindBufferMemoryInfo, buffer)), vkf_g64(k, e + offsetof(VkBindBufferMemoryInfo, memory)),
                    vkf_g64(k, e + offsetof(VkBindBufferMemoryInfo, memoryOffset)));
    }
}

void vkf_buffer_destroyed(vkf_call *k)
{
    vkf_dev *d = k->obj->dev;
    if (!(d->decoding || d->depth24) || !k->c->x[1])
        return;
    pthread_mutex_lock(&d->decode_lock);
    free(vkf_map_get(d->buffers, k->c->x[1]));
    vkf_map_put(d->buffers, k->c->x[1], NULL);
    pthread_mutex_unlock(&d->decode_lock);
}

void vkf_decode_device_features(vkf_call *k, VkPhysicalDevice pd, VkDeviceCreateInfo *ci)
{
    VkPhysicalDeviceFeatures have;
    memset(&have, 0, sizeof have);
    VKF_FN(k->fns, vkGetPhysicalDeviceFeatures)(pd, &have);
    if (have.textureCompressionBC || vkf_decode_mode() == RL_VK_DECODE_OFF)
        return;
    if (ci->pEnabledFeatures && ci->pEnabledFeatures->textureCompressionBC) {
        VkPhysicalDeviceFeatures *f = vkf_alloc(k, sizeof *f);
        *f = *ci->pEnabledFeatures;
        f->textureCompressionBC = VK_FALSE;
        ci->pEnabledFeatures = f;
    }
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            ((VkPhysicalDeviceFeatures2 *)b)->features.textureCompressionBC = VK_FALSE;
}

typedef struct teardown {
    const vkf_fns *fns;
    VkDevice dev;
} teardown;

static void free_image(uint64_t key, void *value, void *user)
{
    (void)key;
    const teardown *t = user;
    vkf_image *im = value;
    if (im->memory) {
        VKF_FN(t->fns, vkFreeMemory)(t->dev, im->memory, NULL);
        vkf_decode_note_image(-1, -(int64_t)im->host_bytes, -(int64_t)im->guest_bytes);
    }
    free(im);
}

static void free_cb(uint64_t key, void *value, void *user)
{
    (void)key;
    const teardown *t = user;
    free_uploads(t->fns, t->dev, value, true);
}

static void free_value(uint64_t key, void *value, void *user)
{
    (void)key;
    (void)user;
    free(value);
}

void vkf_image_device_teardown(const vkf_fns *fns, void *dev, vkf_dev *d)
{
    teardown t = { fns, dev };
    vkf_map_each(d->uploads, free_cb, &t);
    vkf_map_each(d->images, free_image, &t);
    vkf_map_each(d->buffers, free_value, NULL);
}
