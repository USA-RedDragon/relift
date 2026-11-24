/**
 * @file ffx.c
 * @brief The Vulkan plumbing the FidelityFX ports share (ffx.h).
 */
#include "ffx.h"

#include "core.h"
#include "hostlib.h"
#include "spirvval.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define DUMP_MKDIR(p) _mkdir(p)
#else
#define DUMP_MKDIR(p) mkdir((p), 0755)
#endif

/** @brief The loader's vkGetInstanceProcAddr, or NULL. */
static void *(RLVK_CALL *instance_proc(void))(rlvk_handle, const char *)
{
    void *raw = rl_hostlib_vulkan_gipa();
    void *(RLVK_CALL *gipa)(rlvk_handle, const char *);
    memcpy(&gipa, &raw, sizeof gipa);
    return raw ? gipa : NULL;
}

bool rl_ffx_vk_load(rl_ffx_vk *vk, const rl_post_device *d, const char *who)
{
    memset(vk, 0, sizeof *vk);
    vk->device = d->device;
    void *(RLVK_CALL *gdpa)(rlvk_handle, const char *);
    memcpy(&gdpa, &d->get_device_proc_addr, sizeof gdpa);
#define RL_FFX_LOAD(name, ret, args)                                                                                   \
    vk->name = (ret(RLVK_CALL *) args)gdpa(vk->device, #name);                                                       \
    if (!vk->name) {                                                                                                   \
        rl_log(RL_LOG_WARN, "upscale", "%s did not start: the device has no " #name, who);                            \
        return false;                                                                                                  \
    }
    RL_FFX_FNS(RL_FFX_LOAD)
#undef RL_FFX_LOAD
    void *(RLVK_CALL *gipa)(rlvk_handle, const char *) = instance_proc();
    void *fp = gipa ? gipa(d->instance, "vkGetPhysicalDeviceMemoryProperties") : NULL;
    if (!fp) {
        rl_log(RL_LOG_WARN, "upscale", "%s did not start: no vkGetPhysicalDeviceMemoryProperties", who);
        return false;
    }
    void(RLVK_CALL * props)(rlvk_handle, rlvk_memory_properties *);
    memcpy(&props, &fp, sizeof props);
    props(d->physical_device, &vk->mem);
    return true;
}

bool rl_ffx_has_quads(const rl_post_device *d)
{
    void *(RLVK_CALL *gipa)(rlvk_handle, const char *) = instance_proc();
    void *fp = gipa ? gipa(d->instance, "vkGetPhysicalDeviceProperties2") : NULL;
    if (!fp)
        return false;
    void(RLVK_CALL * props2)(rlvk_handle, void *);
    memcpy(&props2, &fp, sizeof props2);
    rlvk_subgroup_properties sg = { .sType = RLVK_ST_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    struct {
        int32_t sType;
        void *pNext;
        uint8_t properties[1024];
    } p2 = { .sType = RLVK_ST_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg };
    props2(d->physical_device, &p2);
    return (sg.supportedOperations & RLVK_SUBGROUP_FEATURE_QUAD_BIT) &&
           (sg.supportedStages & RLVK_SHADER_STAGE_COMPUTE_BIT);
}

static int32_t memory_type(const rl_ffx_vk *vk, uint32_t bits)
{
    for (uint32_t pass = 0; pass < 2; pass++)
        for (uint32_t t = 0; t < vk->mem.memoryTypeCount; t++)
            if ((bits & (1u << t)) &&
                (pass || (vk->mem.memoryTypes[t].propertyFlags & RLVK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
                return (int32_t)t;
    return -1;
}

bool rl_ffx_bind_memory(const rl_ffx_vk *vk, const rlvk_memory_requirements *req, rlvk_handle *memory)
{
    int32_t t = memory_type(vk, req->memoryTypeBits);
    rlvk_memory_allocate_info ai = {
        .sType = RLVK_ST_MEMORY_ALLOCATE_INFO, .allocationSize = req->size, .memoryTypeIndex = (uint32_t)t
    };
    return t >= 0 && vk->vkAllocateMemory(vk->device, &ai, NULL, memory) == RLVK_SUCCESS;
}

rlvk_handle rl_ffx_view(const rl_ffx_vk *vk, rlvk_handle img, int32_t format, uint32_t level, uint32_t levels)
{
    rlvk_image_view_create_info vi = {
        .sType = RLVK_ST_IMAGE_VIEW_CREATE_INFO,
        .image = img,
        .viewType = RLVK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { RLVK_IMAGE_ASPECT_COLOR_BIT, level, levels, 0, 1 },
    };
    rlvk_handle v = 0;
    return vk->vkCreateImageView(vk->device, &vi, NULL, &v) == RLVK_SUCCESS ? v : 0;
}

bool rl_ffx_image_new(const rl_ffx_vk *vk, int32_t format, uint32_t w, uint32_t h, uint32_t mips, rl_ffx_image *im)
{
    rlvk_image_create_info ci = {
        .sType = RLVK_ST_IMAGE_CREATE_INFO,
        .imageType = RLVK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { w, h, 1 },
        .mipLevels = mips,
        .arrayLayers = 1,
        .samples = RLVK_SAMPLE_COUNT_1_BIT,
        .tiling = RLVK_IMAGE_TILING_OPTIMAL,
        .usage = RLVK_IMAGE_USAGE_SAMPLED_BIT | RLVK_IMAGE_USAGE_STORAGE_BIT | RLVK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 RLVK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = RLVK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vk->vkCreateImage(vk->device, &ci, NULL, &im->image) != RLVK_SUCCESS)
        return false;
    rlvk_memory_requirements req;
    vk->vkGetImageMemoryRequirements(vk->device, im->image, &req);
    return rl_ffx_bind_memory(vk, &req, &im->memory) &&
           vk->vkBindImageMemory(vk->device, im->image, im->memory, 0) == RLVK_SUCCESS &&
           (im->view = rl_ffx_view(vk, im->image, format, 0, mips)) != 0;
}

void rl_ffx_image_free(const rl_ffx_vk *vk, rl_ffx_image *im)
{
    if (im->view)
        vk->vkDestroyImageView(vk->device, im->view, NULL);
    if (im->image)
        vk->vkDestroyImage(vk->device, im->image, NULL);
    if (im->memory)
        vk->vkFreeMemory(vk->device, im->memory, NULL);
    memset(im, 0, sizeof *im);
}

bool rl_ffx_pipeline_new(const rl_ffx_vk *vk, const uint32_t *code, uint32_t size, const rl_ffx_bind *binds,
                         uint32_t n, uint32_t set, const rlvk_handle *before, uint32_t nbefore, rlvk_handle sampler,
                         rl_ffx_pipeline *p)
{
    enum { MAX_BINDS = 48, MAX_SETS = 4 };
    rlvk_descriptor_set_layout_binding lb[MAX_BINDS];
    uint32_t k = 0;
    for (uint32_t i = 0; i < n && k < MAX_BINDS; i++)
        if (binds[i].set == set)
            lb[k++] = (rlvk_descriptor_set_layout_binding){
                .binding = binds[i].binding,
                .descriptorType = (int32_t)binds[i].type,
                .descriptorCount = 1,
                .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT,
                .pImmutableSamplers = binds[i].type == RLVK_DESCRIPTOR_TYPE_SAMPLER ? &sampler : NULL,
            };
    rlvk_descriptor_set_layout_create_info sl = {
        .sType = RLVK_ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = k, .pBindings = lb
    };
    if (nbefore + 1 > MAX_SETS)
        return false;
    if (vk->vkCreateDescriptorSetLayout(vk->device, &sl, NULL, &p->set_layout) != RLVK_SUCCESS)
        return false;
    rlvk_handle layouts[MAX_SETS];
    for (uint32_t i = 0; i < nbefore; i++)
        layouts[i] = before[i];
    layouts[nbefore] = p->set_layout;
    rlvk_pipeline_layout_create_info pl = {
        .sType = RLVK_ST_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = nbefore + 1, .pSetLayouts = layouts
    };
    if (vk->vkCreatePipelineLayout(vk->device, &pl, NULL, &p->layout) != RLVK_SUCCESS)
        return false;
    rlvk_shader_module_create_info sm = { .sType = RLVK_ST_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    rlvk_handle shader = 0;
    rl_spirv_check(code, size / 4, "an FFX program");
    if (vk->vkCreateShaderModule(vk->device, &sm, NULL, &shader) != RLVK_SUCCESS)
        return false;
    rlvk_compute_pipeline_create_info cp = {
        .sType = RLVK_ST_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = RLVK_ST_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = RLVK_SHADER_STAGE_COMPUTE_BIT,
                   .module = shader,
                   .pName = "main" },
        .layout = p->layout,
        .basePipelineIndex = -1,
    };
    rlvk_result r = vk->vkCreateComputePipelines(vk->device, 0, 1, &cp, NULL, &p->pipe);
    vk->vkDestroyShaderModule(vk->device, shader, NULL);
    return r == RLVK_SUCCESS;
}

void rl_ffx_pipeline_free(const rl_ffx_vk *vk, rl_ffx_pipeline *p)
{
    if (p->pipe)
        vk->vkDestroyPipeline(vk->device, p->pipe, NULL);
    if (p->layout)
        vk->vkDestroyPipelineLayout(vk->device, p->layout, NULL);
    if (p->set_layout)
        vk->vkDestroyDescriptorSetLayout(vk->device, p->set_layout, NULL);
    memset(p, 0, sizeof *p);
}

void rl_ffx_barrier(const rl_ffx_vk *vk, uint64_t cmd, uint32_t src_stage, uint32_t src, uint32_t dst_stage,
                    uint32_t dst)
{
    rlvk_memory_barrier mb = { .sType = RLVK_ST_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst };
    vk->vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 1, &mb, 0, NULL, 0, NULL);
}

void rl_ffx_clear(const rl_ffx_vk *vk, uint64_t cmd, rlvk_handle image, float r, float g)
{
    float color[4] = { r, g, 0.0f, 0.0f };
    rlvk_image_subresource_range rr = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, ~0u, 0, 1 };
    vk->vkCmdClearColorImage(cmd, image, RLVK_IMAGE_LAYOUT_GENERAL, color, 1, &rr);
}

/** @brief The half-precision bits for v (round to nearest even, no NaN payloads), as FsrRcasCon packs them. */
static uint16_t half_bits(float v)
{
    uint32_t x;
    memcpy(&x, &v, 4);
    uint32_t sign = (x >> 16) & 0x8000u, e = (x >> 23) & 0xffu, m = x & 0x7fffffu;
    if (e >= 143)
        return (uint16_t)(sign | 0x7c00u);
    if (e < 113)
        return (uint16_t)sign;
    uint32_t h = ((e - 112) << 10) | (m >> 13), rest = m & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1)))
        h++;
    return (uint16_t)(sign | h);
}

void rl_ffx_rcas(float sharpness, uint32_t rcas[4])
{
    float sharp = sharpness < 0.0f ? 0.0f : sharpness > 1.0f ? 1.0f : sharpness;
    float lin = exp2f(-(-2.0f * sharp + 2.0f));
    memcpy(&rcas[0], &lin, 4);
    rcas[1] = (uint32_t)half_bits(lin) | (uint32_t)half_bits(lin) << 16;
    rcas[2] = rcas[3] = 0;
}

#define DUMP_FRAMES 16u
#define DUMP_IMAGES 32u

/** @brief One evaluation being dumped: a host-visible buffer per image and what to write. */
typedef struct dump_frame {
    uint32_t evaluation, n, render[2], nconsts;
    rl_ffx_dump_image images[DUMP_IMAGES];
    rlvk_handle buffer[DUMP_IMAGES], memory[DUMP_IMAGES];
    rlvk_size bytes[DUMP_IMAGES];
    uint32_t consts[256];
} dump_frame;

struct rl_ffx_dump {
    char dir[512], who[16];
    uint32_t first, last, count, pending;
    dump_frame frames[DUMP_FRAMES];
};

/** @brief Bytes per pixel of the formats the ports' images have, 0 for another. */
static uint32_t dump_bpp(int32_t format)
{
    switch (format) {
    case RLVK_FORMAT_R8_UNORM:
        return 1;
    case RLVK_FORMAT_R8G8_UNORM:
    case RLVK_FORMAT_R16_SFLOAT:
        return 2;
    case RLVK_FORMAT_R8G8B8A8_UNORM:
    case RLVK_FORMAT_R16G16_SFLOAT:
    case RLVK_FORMAT_R32_SFLOAT:
    case RLVK_FORMAT_R32_UINT:
        return 4;
    case RLVK_FORMAT_R16G16B16A16_SFLOAT:
    case RLVK_FORMAT_R32G32_SFLOAT:
        return 8;
    case RLVK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    default:
        return 0;
    }
}

rl_ffx_dump *rl_ffx_dump_new(const char *who)
{
    const char *e = getenv("RELIFT_UPSCALER_DUMP");
    const char *colon = e ? strrchr(e, ':') : NULL;
    if (!colon || colon == e)
        return NULL;
    rl_ffx_dump *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    size_t len = (size_t)(colon - e) < sizeof d->dir - 1 ? (size_t)(colon - e) : sizeof d->dir - 1;
    memcpy(d->dir, e, len);
    snprintf(d->who, sizeof d->who, "%s", who);
    char *end;
    d->first = (uint32_t)strtoul(colon + 1, &end, 10);
    d->last = *end == '-' ? (uint32_t)strtoul(end + 1, NULL, 10) : d->first;
    if (d->last < d->first)
        d->last = d->first;
    if (d->last - d->first >= DUMP_FRAMES)
        d->last = d->first + DUMP_FRAMES - 1;
    for (char *s = d->dir + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            DUMP_MKDIR(d->dir);
            *s = '/';
        }
    DUMP_MKDIR(d->dir);
    rl_log(RL_LOG_INFO, "upscale", "%s dumps its images of evaluations %u to %u into %s", who, d->first, d->last,
           d->dir);
    return d;
}

/** @brief A host-visible memory type of bits, or -1. */
static int32_t host_type(const rl_ffx_vk *vk, uint32_t bits)
{
    const uint32_t want = RLVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RLVK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t t = 0; t < vk->mem.memoryTypeCount; t++)
        if ((bits & (1u << t)) && (vk->mem.memoryTypes[t].propertyFlags & want) == want)
            return (int32_t)t;
    return -1;
}

void rl_ffx_dump_record(rl_ffx_dump *d, const rl_ffx_vk *vk, uint64_t cmd, const rl_ffx_dump_image *images, uint32_t n,
                        const uint32_t render[2], const void *consts, uint32_t bytes)
{
    if (!d)
        return;
    uint32_t ev = ++d->count;
    if (ev < d->first || ev > d->last || d->pending == DUMP_FRAMES)
        return;
    dump_frame *f = &d->frames[d->pending];
    memset(f, 0, sizeof *f);
    f->evaluation = ev;
    f->render[0] = render[0];
    f->render[1] = render[1];
    f->nconsts = bytes / 4 < 256 ? bytes / 4 : 256;
    memcpy(f->consts, consts, f->nconsts * 4);
    const uint32_t cs = RLVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, xfer = RLVK_PIPELINE_STAGE_TRANSFER_BIT;
    rl_ffx_barrier(vk, cmd, cs, RLVK_ACCESS_SHADER_WRITE_BIT, xfer, RLVK_ACCESS_TRANSFER_READ_BIT);
    for (uint32_t i = 0; i < n && f->n < DUMP_IMAGES; i++) {
        uint32_t bpp = dump_bpp(images[i].format);
        if (!bpp || !images[i].image || !images[i].width || !images[i].height)
            continue;
        uint32_t k = f->n;
        rlvk_size size = (rlvk_size)images[i].width * images[i].height * bpp;
        rlvk_buffer_create_info bi = { .sType = RLVK_ST_BUFFER_CREATE_INFO,
                                       .size = size,
                                       .usage = RLVK_BUFFER_USAGE_TRANSFER_DST_BIT };
        if (vk->vkCreateBuffer(vk->device, &bi, NULL, &f->buffer[k]) != RLVK_SUCCESS)
            continue;
        rlvk_memory_requirements req;
        vk->vkGetBufferMemoryRequirements(vk->device, f->buffer[k], &req);
        int32_t t = host_type(vk, req.memoryTypeBits);
        rlvk_memory_allocate_info ai = {
            .sType = RLVK_ST_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t
        };
        if (t < 0 || vk->vkAllocateMemory(vk->device, &ai, NULL, &f->memory[k]) != RLVK_SUCCESS ||
            vk->vkBindBufferMemory(vk->device, f->buffer[k], f->memory[k], 0) != RLVK_SUCCESS) {
            if (f->memory[k])
                vk->vkFreeMemory(vk->device, f->memory[k], NULL);
            vk->vkDestroyBuffer(vk->device, f->buffer[k], NULL);
            f->buffer[k] = f->memory[k] = 0;
            continue;
        }
        rlvk_buffer_image_copy r = { .imageSubresource = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                     .imageExtent = { images[i].width, images[i].height, 1 } };
        vk->vkCmdCopyImageToBuffer(cmd, images[i].image, RLVK_IMAGE_LAYOUT_GENERAL, f->buffer[k], 1, &r);
        f->images[k] = images[i];
        f->bytes[k] = size;
        f->n++;
    }
    rl_ffx_barrier(vk, cmd, xfer, RLVK_ACCESS_TRANSFER_WRITE_BIT | RLVK_ACCESS_TRANSFER_READ_BIT,
                   cs | RLVK_PIPELINE_STAGE_HOST_BIT, RLVK_ACCESS_HOST_READ_BIT | RLVK_ACCESS_SHADER_WRITE_BIT);
    d->pending++;
}

/** @brief Frees the buffers of f. */
static void dump_drop(const rl_ffx_vk *vk, dump_frame *f)
{
    for (uint32_t k = 0; k < f->n; k++) {
        vk->vkDestroyBuffer(vk->device, f->buffer[k], NULL);
        vk->vkFreeMemory(vk->device, f->memory[k], NULL);
    }
    f->n = 0;
}

void rl_ffx_dump_retire(rl_ffx_dump *d, const rl_ffx_vk *vk)
{
    if (!d || !d->pending)
        return;
    char path[700];
    snprintf(path, sizeof path, "%s/%s-manifest.txt", d->dir, d->who);
    FILE *m = fopen(path, "a");
    for (uint32_t i = 0; i < d->pending; i++) {
        dump_frame *f = &d->frames[i];
        for (uint32_t k = 0; k < f->n; k++) {
            void *p = NULL;
            if (vk->vkMapMemory(vk->device, f->memory[k], 0, f->bytes[k], 0, &p) != RLVK_SUCCESS)
                continue;
            snprintf(path, sizeof path, "%s/%s-%04u-%s.raw", d->dir, d->who, f->evaluation, f->images[k].name);
            FILE *o = fopen(path, "wb");
            if (o) {
                fwrite(p, 1, (size_t)f->bytes[k], o);
                fclose(o);
            }
            vk->vkUnmapMemory(vk->device, f->memory[k]);
            if (m) {
                fprintf(m, "%u %s %d %u %u %u %u %s %s-%04u-%s.raw", f->evaluation, f->images[k].name,
                        (int)f->images[k].format, f->images[k].width, f->images[k].height, f->render[0],
                        f->render[1], f->images[k].render_sized ? "render" : "other", d->who, f->evaluation,
                        f->images[k].name);
                for (uint32_t c = 0; c < f->nconsts; c++)
                    fprintf(m, " %08x", f->consts[c]);
                fputc('\n', m);
            }
        }
        rl_log(RL_LOG_INFO, "upscale", "%s dumped evaluation %u: %u images into %s", d->who, f->evaluation, f->n,
               d->dir);
        dump_drop(vk, f);
    }
    if (m)
        fclose(m);
    d->pending = 0;
}

void rl_ffx_dump_free(rl_ffx_dump *d, const rl_ffx_vk *vk)
{
    if (!d)
        return;
    for (uint32_t i = 0; i < d->pending; i++)
        dump_drop(vk, &d->frames[i]);
    free(d);
}
