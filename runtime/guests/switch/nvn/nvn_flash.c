/**
 * @file nvn_flash.c
 * @brief The flash trigger: catches a single wrong frame between correct ones and saves what made it.
 *
 * RELIFT_NVN_FLASH=<dir>[:<threshold>] turns it on. Each present then gets a signature (a nearest-filter blit of the
 * presented picture to RL_JUMP_SIG_W by RL_JUMP_SIG_H, luminance read back after the present's own wait) for the
 * frame jump detector (core/framejump.h; <threshold> in luminance steps, default 10). The last NVN_TRACE_RING presents
 * are kept on the GPU in three forms: the presented picture, a thumbnail of every image the frame wrote (each render
 * pass's targets when the pass ends, labeled "clear" for a texture clear's passes, each dispatch's storage images, the
 * outputs of copies and upscaler evaluations, each upload's image), and the frame trace ring's lines (nvn_trace.c). When a present is an outlier,
 * <dir>/flash-<present>/ gets, for the present before it, it and the one after it: present-<n>.png, thumbs-<n>.png (a
 * grid of THUMB_W by THUMB_H tiles, THUMB_COLS to a row, in the order the frame wrote them), thumbs-<n>.jsonl (one
 * line per tile: the trace line it follows, what wrote it, the image's storage, format, size, level and layer) and
 * trace-<n>.jsonl, plus flash.json with the differences. runtime/tools/nvn_flash.py compares the tiles and names the
 * first image whose picture jumped. At most FLASH_MAX triggers are saved per run.
 */
#include "nvn_vk.h"

#include "framejump.h"
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#define THUMB_W 32u
#define THUMB_H 18u
#define THUMB_COLS 64u
#define THUMB_ROWS 64u
#define THUMB_MAX (THUMB_COLS * THUMB_ROWS)
#define FLASH_MAX 8u
#define PENDING_MAX 64u

/** @brief One thumbnail: what wrote which image, and where in the frame (the trace lines before it). */
typedef struct thumb {
    uint64_t after, storage;
    const char *what;
    uint32_t format, width, height, level, layer;
    int32_t vk_format;
} thumb;

/** @brief A device-local RGBA8 image of the flash trigger. */
typedef struct fimg {
    VkImage image;
    VkDeviceMemory memory;
    uint32_t w, h;
} fimg;

static struct {
    bool parsed, on;
    char dir[480];
    rl_jump jump;
    uint32_t saved;
    /** @brief The presented pictures and their frame numbers, by frame number modulo NVN_TRACE_RING. */
    fimg shown[NVN_TRACE_RING];
    uint64_t shown_frame[NVN_TRACE_RING];
    bool shown_ok[NVN_TRACE_RING];
    fimg sig;
    staging sig_buf;
    bool sig_recorded;
    /** @brief The thumbnail atlases and their records, by frame number modulo NVN_TRACE_RING. */
    fimg atlas[NVN_TRACE_RING];
    thumb *thumbs[NVN_TRACE_RING];
    uint32_t nthumbs[NVN_TRACE_RING];
    uint64_t thumb_frame[NVN_TRACE_RING];
    uint64_t thumbs_dropped[NVN_TRACE_RING];
    /** @brief Images written since the last thumbnails were taken. */
    struct {
        const vimg *v;
        uint32_t level, layer;
    } pending[PENDING_MAX];
    uint32_t npending;
    /** @brief Formats a blit can read, by VkFormat (feature query cache). */
    struct {
        int32_t f;
        bool blit;
    } fmt[128];
    uint32_t nfmt;
} fl;

static void make_dir(const char *path)
{
#if defined(_WIN32)
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

bool nvn_flash_on(void)
{
    if (fl.parsed)
        return fl.on;
    fl.parsed = true;
    const char *spec = getenv("RELIFT_NVN_FLASH");
    if (!spec || !*spec)
        return false;
    const char *colon = strrchr(spec, ':');
    size_t n = strlen(spec);
    if (colon && colon[1]) {
        char *end;
        double t = strtod(colon + 1, &end);
        if (!*end && t > 0) {
            fl.jump.threshold = (uint32_t)(t * 16.0 + 0.5);
            n = (size_t)(colon - spec);
        }
    }
    if (!n || n >= sizeof fl.dir)
        return false;
    memcpy(fl.dir, spec, n);
    fl.dir[n] = 0;
    make_dir(fl.dir);
    for (uint32_t i = 0; i < NVN_TRACE_RING; i++) {
        fl.thumbs[i] = calloc(THUMB_MAX, sizeof(thumb));
        if (!fl.thumbs[i])
            return false;
        fl.thumb_frame[i] = UINT64_MAX;
        fl.shown_frame[i] = UINT64_MAX;
    }
    fl.on = true;
    nvn_trace_ring_start();
    rl_log(RL_LOG_INFO, "nvn",
           "flash trigger on: a present unlike both its neighbours saves the presents, thumbnails and trace around it "
           "to %s (threshold %.2f luminance steps)",
           fl.dir, (double)(fl.jump.threshold ? fl.jump.threshold : RL_JUMP_DEFAULT_THRESHOLD) / 16.0);
    return true;
}

/** @brief Makes img a w by h RGBA8 image in GENERAL layout (the transition recorded in g.cb); false on failure. */
static bool fimg_make(replay *r, fimg *img, uint32_t w, uint32_t h)
{
    if (img->image && img->w == w && img->h == h)
        return true;
    if (img->image) {
        if (!nvn_vk_flush(r))
            return false;
        VK(vkDestroyImage)(g.dev, img->image, NULL);
        VK(vkFreeMemory)(g.dev, img->memory, NULL);
        memset(img, 0, sizeof *img);
    }
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM,
                             .extent = { w, h, 1 },
                             .mipLevels = 1,
                             .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    VkMemoryRequirements req;
    if (VK(vkCreateImage)(g.dev, &ci, NULL, &img->image) != VK_SUCCESS)
        return false;
    VK(vkGetImageMemoryRequirements)(g.dev, img->image, &req);
    int32_t t = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)t };
    if (t < 0 || VK(vkAllocateMemory)(g.dev, &mai, NULL, &img->memory) != VK_SUCCESS ||
        VK(vkBindImageMemory)(g.dev, img->image, img->memory, 0) != VK_SUCCESS) {
        VK(vkDestroyImage)(g.dev, img->image, NULL);
        if (img->memory)
            VK(vkFreeMemory)(g.dev, img->memory, NULL);
        memset(img, 0, sizeof *img);
        return false;
    }
    img->w = w;
    img->h = h;
    nvn_vk_begin(r);
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img->image,
                                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCmdPipelineBarrier)(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                             NULL, 1, &ib);
    VkClearColorValue black = { .float32 = { 0, 0, 0, 1 } };
    VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VK(vkCmdClearColorImage)(g.cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &all);
    nvn_vk_barrier(g.cb);
    return true;
}

/** @brief Whether a blit can read color format f into RGBA8: float or normalized with the blit source feature. */
static bool blit_reads(VkFormat f)
{
    for (uint32_t i = 0; i < fl.nfmt; i++)
        if (fl.fmt[i].f == (int32_t)f)
            return fl.fmt[i].blit;
    VkFormatProperties fp;
    VK(vkGetPhysicalDeviceFormatProperties)(g.phys, f, &fp);
    bool ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
    switch (f) {
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT: ok = false; break;
    default: break;
    }
    if (fl.nfmt < sizeof fl.fmt / sizeof fl.fmt[0]) {
        fl.fmt[fl.nfmt].f = (int32_t)f;
        fl.fmt[fl.nfmt++].blit = ok;
    }
    return ok;
}

void nvn_flash_wrote(const vimg *v, uint32_t level, uint32_t layer)
{
    if (!fl.on || !v)
        return;
    for (uint32_t i = 0; i < fl.npending; i++)
        if (fl.pending[i].v == v && fl.pending[i].level == level && fl.pending[i].layer == layer)
            return;
    if (fl.npending < PENDING_MAX) {
        fl.pending[fl.npending].v = v;
        fl.pending[fl.npending].level = level;
        fl.pending[fl.npending++].layer = layer;
    }
}

bool nvn_flash_pending(void) { return fl.npending != 0; }

void nvn_flash_thumbs(replay *r, const char *what)
{
    if (!fl.on || !fl.npending)
        return;
    if (!nvn_vk_recording() && r)
        nvn_vk_begin(r);
    uint32_t slot = (uint32_t)(g.presents % NVN_TRACE_RING);
    if (fl.thumb_frame[slot] != g.presents) {
        fl.thumb_frame[slot] = g.presents;
        fl.nthumbs[slot] = 0;
        fl.thumbs_dropped[slot] = 0;
    }
    if (!fl.atlas[slot].image && !fimg_make(r, &fl.atlas[slot], THUMB_W * THUMB_COLS, THUMB_H * THUMB_ROWS)) {
        fl.npending = 0;
        return;
    }
    nvn_vk_barrier(g.cb);
    for (uint32_t i = 0; i < fl.npending; i++) {
        const vimg *v = fl.pending[i].v;
        uint32_t level = fl.pending[i].level, layer = fl.pending[i].layer;
        if (v->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !blit_reads(v->format))
            continue;
        if (fl.nthumbs[slot] == THUMB_MAX) {
            fl.thumbs_dropped[slot]++;
            continue;
        }
        uint32_t k = fl.nthumbs[slot]++;
        thumb *t = &fl.thumbs[slot][k];
        uint32_t w = v->d.width >> level ? v->d.width >> level : 1, h = v->d.height >> level ? v->d.height >> level : 1;
        bool volume = v->d.target == NVN_TARGET_3D;
        *t = (thumb){ .after = nvn_trace_cmds(), .storage = v->storage, .what = what, .format = v->d.format,
                      .width = w, .height = h, .level = level, .layer = layer, .vk_format = (int32_t)v->format };
        int32_t x = (int32_t)((k % THUMB_COLS) * THUMB_W), y = (int32_t)((k / THUMB_COLS) * THUMB_H);
        VkImageBlit b = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, volume ? 0 : layer, 1 },
                          .srcOffsets = { { 0, 0, volume ? (int32_t)layer : 0 },
                                          { (int32_t)w, (int32_t)h, volume ? (int32_t)layer + 1 : 1 } },
                          .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                          .dstOffsets = { { x, y, 0 }, { x + (int32_t)THUMB_W, y + (int32_t)THUMB_H, 1 } } };
        VK(vkCmdBlitImage)(g.cb, v->image, VK_IMAGE_LAYOUT_GENERAL, fl.atlas[slot].image, VK_IMAGE_LAYOUT_GENERAL, 1,
                           &b, VK_FILTER_NEAREST);
    }
    fl.npending = 0;
    nvn_vk_barrier(g.cb);
}

bool nvn_flash_thumb_of(uint64_t frame, uint32_t k, const char **what, uint64_t *storage, uint32_t *level,
                        uint32_t *layer)
{
    uint32_t slot = (uint32_t)(frame % NVN_TRACE_RING);
    if (!fl.on || fl.thumb_frame[slot] != frame || k >= fl.nthumbs[slot])
        return false;
    const thumb *t = &fl.thumbs[slot][k];
    *what = t->what;
    *storage = t->storage;
    *level = t->level;
    *layer = t->layer;
    return true;
}

void nvn_flash_image_gone(const vimg *v)
{
    for (uint32_t i = 0; i < fl.npending; i++)
        if (fl.pending[i].v == v)
            fl.pending[i] = fl.pending[--fl.npending];
}

void nvn_flash_present_record(replay *r, VkImage src, const uint32_t rect[4], uint64_t frame)
{
    fl.sig_recorded = false;
    if (!fl.on)
        return;
    uint32_t slot = (uint32_t)(frame % NVN_TRACE_RING);
    fl.shown_ok[slot] = false;
    if (!fimg_make(r, &fl.shown[slot], rect[2], rect[3]) || !fimg_make(r, &fl.sig, RL_JUMP_SIG_W, RL_JUMP_SIG_H))
        return;
    if (!fl.sig_buf.buf && !nvn_vk_readback_new(&fl.sig_buf, (uint64_t)RL_JUMP_SIG * 4))
        return;
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    VkImageCopy ic = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                       .srcOffset = { (int32_t)rect[0], (int32_t)rect[1], 0 },
                       .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                       .extent = { rect[2], rect[3], 1 } };
    VK(vkCmdCopyImage)(g.cb, src, VK_IMAGE_LAYOUT_GENERAL, fl.shown[slot].image, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    nvn_vk_barrier(g.cb);
    VkImageBlit b = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                      .srcOffsets = { { 0, 0, 0 }, { (int32_t)rect[2], (int32_t)rect[3], 1 } },
                      .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                      .dstOffsets = { { 0, 0, 0 }, { (int32_t)RL_JUMP_SIG_W, (int32_t)RL_JUMP_SIG_H, 1 } } };
    VK(vkCmdBlitImage)(g.cb, fl.shown[slot].image, VK_IMAGE_LAYOUT_GENERAL, fl.sig.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       VK_FILTER_NEAREST);
    nvn_vk_barrier(g.cb);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                              .imageExtent = { RL_JUMP_SIG_W, RL_JUMP_SIG_H, 1 } };
    VK(vkCmdCopyImageToBuffer)(g.cb, fl.sig.image, VK_IMAGE_LAYOUT_GENERAL, fl.sig_buf.buf, 1, &bic);
    nvn_vk_barrier(g.cb);
    fl.sig_recorded = true;
    fl.shown_ok[slot] = true;
    fl.shown_frame[slot] = frame;
}

/** @brief Reads img back (flushing) and writes it as a PNG at path; false when it cannot. */
static bool save_png(replay *r, const fimg *img, uint32_t rows, const char *path)
{
    if (!img->image || !rows)
        return false;
    staging s;
    uint64_t bytes = (uint64_t)img->w * rows * 4;
    if (!nvn_vk_readback_new(&s, bytes))
        return false;
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { img->w, rows, 1 } };
    VK(vkCmdCopyImageToBuffer)(g.cb, img->image, VK_IMAGE_LAYOUT_GENERAL, s.buf, 1, &bic);
    nvn_vk_barrier(g.cb);
    bool ok = nvn_vk_flush(r) && rl_png_write_rgb(path, img->w, rows, s.map, (size_t)img->w * 4, false);
    nvn_vk_staging_free(&s);
    return ok;
}

/** @brief Writes the thumbnail records of frame (one JSON line each) to path. */
static bool save_thumbs(uint64_t frame, const char *path)
{
    uint32_t slot = (uint32_t)(frame % NVN_TRACE_RING);
    if (fl.thumb_frame[slot] != frame)
        return false;
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    for (uint32_t k = 0; k < fl.nthumbs[slot]; k++) {
        const thumb *t = &fl.thumbs[slot][k];
        fprintf(f,
                "{\"tile\":%u,\"after\":%llu,\"what\":\"%s\",\"storage\":\"0x%llx\",\"format\":%u,\"vk_format\":%d,"
                "\"width\":%u,\"height\":%u,\"level\":%u,\"layer\":%u}\n",
                k, (unsigned long long)t->after, t->what, (unsigned long long)t->storage, t->format, t->vk_format,
                t->width, t->height, t->level, t->layer);
    }
    if (fl.thumbs_dropped[slot])
        fprintf(f, "{\"dropped\":%llu}\n", (unsigned long long)fl.thumbs_dropped[slot]);
    return fclose(f) == 0;
}

/** @brief Saves the presents, thumbnails and trace lines of frames bad - 1 to bad + 1 for an outlier at bad. */
static void save_flash(replay *r, uint64_t bad)
{
    char dir[640], path[768];
    snprintf(dir, sizeof dir, "%s/flash-%06llu", fl.dir, (unsigned long long)bad);
    make_dir(dir);
    uint32_t saved = 0;
    for (uint64_t n = bad - 1; n <= bad + 1; n++) {
        uint32_t slot = (uint32_t)(n % NVN_TRACE_RING);
        snprintf(path, sizeof path, "%s/present-%06llu.png", dir, (unsigned long long)n);
        if (fl.shown_frame[slot] == n && fl.shown_ok[slot] && save_png(r, &fl.shown[slot], fl.shown[slot].h, path))
            saved++;
        if (fl.thumb_frame[slot] == n) {
            uint32_t rows = (fl.nthumbs[slot] + THUMB_COLS - 1) / THUMB_COLS * THUMB_H;
            snprintf(path, sizeof path, "%s/thumbs-%06llu.png", dir, (unsigned long long)n);
            save_png(r, &fl.atlas[slot], rows, path);
            snprintf(path, sizeof path, "%s/thumbs-%06llu.jsonl", dir, (unsigned long long)n);
            save_thumbs(n, path);
        }
        snprintf(path, sizeof path, "%s/trace-%06llu.jsonl", dir, (unsigned long long)n);
        nvn_trace_ring_write(n, path);
    }
    snprintf(path, sizeof path, "%s/flash.json", dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f,
                "{\"present\":%llu,\"before\":%.2f,\"after\":%.2f,\"around\":%.2f,\"threshold\":%.2f,\"thumb_w\":%u,"
                "\"thumb_h\":%u,\"thumb_cols\":%u}\n",
                (unsigned long long)bad, (double)fl.jump.d_prev / 16.0, (double)fl.jump.d_next / 16.0,
                (double)fl.jump.d_skip / 16.0,
                (double)(fl.jump.threshold ? fl.jump.threshold : RL_JUMP_DEFAULT_THRESHOLD) / 16.0, THUMB_W, THUMB_H,
                THUMB_COLS);
        fclose(f);
    }
    rl_log(RL_LOG_WARN, "nvn",
           "flash trigger: present %llu differs from the one before by %.1f and the one after by %.1f luminance steps "
           "while those differ by %.1f; %u presents, thumbnails and trace saved in %s",
           (unsigned long long)bad, (double)fl.jump.d_prev / 16.0, (double)fl.jump.d_next / 16.0,
           (double)fl.jump.d_skip / 16.0, saved, dir);
}

void nvn_flash_presented(replay *r, uint64_t frame)
{
    if (!fl.on || !fl.sig_recorded)
        return;
    fl.sig_recorded = false;
    uint8_t sig[RL_JUMP_SIG];
    rl_jump_sig_rgba(fl.sig_buf.map, RL_JUMP_SIG_W, RL_JUMP_SIG_H, (size_t)RL_JUMP_SIG_W * 4, false, sig);
    uint64_t bad;
    if (rl_jump_push(&fl.jump, frame, sig, &bad) && fl.saved < FLASH_MAX) {
        fl.saved++;
        save_flash(r, bad);
    }
}
