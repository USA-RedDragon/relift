/**
 * @file vk_decode.c
 * @brief Formats the Switch samples that a host GPU may not: BC1 to BC7
 * (Apple GPUs through MoltenVK, some Adreno and Mali) and R4G4.
 *
 * The guest sees the Switch's features for them. An optimal-tiling image of
 * one is created on the host in an uncompressed format of the same channels
 * and sRGB-ness (vkf_decode_format.host), and its uploads are decoded on the
 * CPU (vk_image.c). The vk.decode_bc setting picks when: auto for the
 * formats the host cannot sample, on for every BC format (to test the path
 * on a host that has them) and R4G4 when the host cannot sample it, off
 * never.
 */
#include "bcdec.h"
#include "vkf.h"

#include <relift/settings.h>

#include <stdio.h>
#include <string.h>

enum { D_R4G4, D_BC1_RGB, D_BC1, D_BC2, D_BC3, D_BC4, D_BC4S, D_BC5, D_BC5S, D_BC6, D_BC6S, D_BC7 };

static const vkf_decode_format g_formats[] = {
    { VK_FORMAT_R4G4_UNORM_PACK8, VK_FORMAT_R8G8_UNORM, 1, 1, 1, 2, D_R4G4 },
    { VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 8, 4, D_BC1_RGB },
    { VK_FORMAT_BC1_RGB_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 8, 4, D_BC1_RGB },
    { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 8, 4, D_BC1 },
    { VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 8, 4, D_BC1 },
    { VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 16, 4, D_BC2 },
    { VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 16, 4, D_BC2 },
    { VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 16, 4, D_BC3 },
    { VK_FORMAT_BC3_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 16, 4, D_BC3 },
    { VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R8_UNORM, 4, 4, 8, 1, D_BC4 },
    { VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_R8_SNORM, 4, 4, 8, 1, D_BC4S },
    { VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_R8G8_UNORM, 4, 4, 16, 2, D_BC5 },
    { VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_R8G8_SNORM, 4, 4, 16, 2, D_BC5S },
    { VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_R16G16B16A16_SFLOAT, 4, 4, 16, 8, D_BC6 },
    { VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_R16G16B16A16_SFLOAT, 4, 4, 16, 8, D_BC6S },
    { VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 16, 4, D_BC7 },
    { VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 16, 4, D_BC7 },
};

_Static_assert(sizeof g_formats / sizeof g_formats[0] == VKF_DECODE_FORMATS, "one entry per decoded format");

int vkf_decode_index(VkFormat f)
{
    if (f == VK_FORMAT_R4G4_UNORM_PACK8)
        return 0;
    if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK)
        return 1 + (int)(f - VK_FORMAT_BC1_RGB_UNORM_BLOCK);
    return -1;
}

const vkf_decode_format *vkf_decode_format_of(VkFormat f)
{
    int i = vkf_decode_index(f);
    return i < 0 ? NULL : &g_formats[i];
}

uint32_t vkf_decode_mode(void) { return rl_setting_enum("vk.decode_bc"); }

static bool host_has_bc(const vkf_fns *fns, VkPhysicalDevice pd)
{
    VkPhysicalDeviceFeatures f;
    memset(&f, 0, sizeof f);
    VKF_FN(fns, vkGetPhysicalDeviceFeatures)(pd, &f);
    return f.textureCompressionBC;
}

VkFormat vkf_decode_host_format(const vkf_fns *fns, VkPhysicalDevice pd, VkFormat f)
{
    const vkf_decode_format *d = vkf_decode_format_of(f);
    uint32_t mode = vkf_decode_mode();
    if (!d || mode == RL_VK_DECODE_OFF)
        return f;
    bool bc = f != VK_FORMAT_R4G4_UNORM_PACK8;
    if (mode == RL_VK_DECODE_AUTO || !bc) {
        VkFormatProperties fp;
        memset(&fp, 0, sizeof fp);
        VKF_FN(fns, vkGetPhysicalDeviceFormatProperties)(pd, f, &fp);
        if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && (!bc || host_has_bc(fns, pd)))
            return f;
    }
    if (rl_once(0x4445434F44450000ull | (uint64_t)f))
        rl_log(RL_LOG_INFO, "vk", "format %d: optimal images are format %d on the host, their uploads decoded on the CPU%s",
               (int)f, (int)d->host, mode == RL_VK_DECODE_ON && bc ? " (vk.decode_bc on)" : "; the host cannot sample it");
    return d->host;
}

void vkf_decode_device_setup(vkf_call *k, vkf_dev *d, VkPhysicalDevice pd)
{
    d->decoding = false;
    for (int i = 0; i < VKF_DECODE_FORMATS; i++) {
        d->decode_host[i] = vkf_decode_host_format(k->fns, pd, g_formats[i].format);
        d->decoding = d->decoding || d->decode_host[i] != g_formats[i].format;
    }
}

const vkf_decode_format *vkf_decode_for(const vkf_dev *d, VkFormat f)
{
    int i = vkf_decode_index(f);
    return i >= 0 && d->decode_host[i] != f ? &g_formats[i] : NULL;
}

static void expand_half4(const uint16_t *rgb, uint8_t *out, size_t pitch)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            uint16_t px[4] = { rgb[(y * 4 + x) * 3], rgb[(y * 4 + x) * 3 + 1], rgb[(y * 4 + x) * 3 + 2], 0x3C00 };
            memcpy(out + y * pitch + x * 8, px, 8);
        }
}

void vkf_decode_block(const vkf_decode_format *f, const uint8_t *src, uint8_t *out)
{
    int pitch = 4 * f->texel_bytes;
    switch (f->kind) {
    case D_R4G4:
        out[0] = (uint8_t)((src[0] >> 4) * 17);
        out[1] = (uint8_t)((src[0] & 15) * 17);
        break;
    case D_BC1_RGB:
        bcdec_bc1(src, out, pitch);
        for (int i = 0; i < 16; i++)
            out[4 * i + 3] = 255;
        break;
    case D_BC1:
        bcdec_bc1(src, out, pitch);
        break;
    case D_BC2:
        bcdec_bc2(src, out, pitch);
        break;
    case D_BC3:
        bcdec_bc3(src, out, pitch);
        break;
    case D_BC4:
    case D_BC4S:
        bcdec_bc4(src, out, pitch, f->kind == D_BC4S);
        break;
    case D_BC5:
    case D_BC5S:
        bcdec_bc5(src, out, pitch, f->kind == D_BC5S);
        break;
    case D_BC6:
    case D_BC6S: {
        uint16_t rgb[48];
        bcdec_bc6h_half(src, rgb, 12, f->kind == D_BC6S);
        expand_half4(rgb, out, (size_t)pitch);
        break;
    }
    default:
        bcdec_bc7(src, out, pitch);
        break;
    }
}

void vkf_decode_region(const vkf_decode_format *f, const uint8_t *src, uint64_t row_pitch, uint64_t slice_pitch, uint32_t w,
                       uint32_t h, uint32_t slices, uint8_t *dst)
{
    uint8_t block[4 * 4 * 8];
    uint32_t bw = f->block_w, bh = f->block_h, tb = f->texel_bytes;
    size_t dst_row = (size_t)w * tb;
    for (uint32_t z = 0; z < slices; z++)
        for (uint32_t by = 0; by * bh < h; by++) {
            const uint8_t *row = src + z * slice_pitch + by * row_pitch;
            uint32_t rows = h - by * bh < bh ? h - by * bh : bh;
            for (uint32_t bx = 0; bx * bw < w; bx++) {
                vkf_decode_block(f, row + (size_t)bx * f->block_bytes, block);
                uint32_t cols = w - bx * bw < bw ? w - bx * bw : bw;
                uint8_t *o = dst + ((size_t)z * h + (size_t)by * bh) * dst_row + (size_t)bx * bw * tb;
                for (uint32_t r = 0; r < rows; r++)
                    memcpy(o + r * dst_row, block + r * bw * tb, (size_t)cols * tb);
            }
        }
}

static uint64_t g_src_bytes, g_out_bytes, g_ns, g_regions;
static uint64_t g_images, g_host_bytes, g_guest_bytes;
static uint64_t g_last_src, g_last_ns;

void vkf_decode_note(uint64_t src_bytes, uint64_t out_bytes, uint64_t ns)
{
    __atomic_add_fetch(&g_src_bytes, src_bytes, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_out_bytes, out_bytes, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_ns, ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_regions, 1, __ATOMIC_RELAXED);
}

void vkf_decode_note_image(int64_t count, int64_t host_bytes, int64_t guest_bytes)
{
    __atomic_add_fetch(&g_images, (uint64_t)count, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_host_bytes, (uint64_t)host_bytes, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_guest_bytes, (uint64_t)guest_bytes, __ATOMIC_RELAXED);
}

static void stats_line(const char *when)
{
    uint64_t src = __atomic_load_n(&g_src_bytes, __ATOMIC_RELAXED), ns = __atomic_load_n(&g_ns, __ATOMIC_RELAXED);
    rl_log(RL_LOG_INFO, "stats",
           "%s: decoded %.2f MiB of BC to %.2f MiB in %llu regions, %.1f ms (%.1f ms since the last line); %llu images "
           "in host formats hold %.2f MiB for %.2f MiB of the guest's",
           when, (double)src / 1048576.0, (double)__atomic_load_n(&g_out_bytes, __ATOMIC_RELAXED) / 1048576.0,
           (unsigned long long)__atomic_load_n(&g_regions, __ATOMIC_RELAXED), (double)ns / 1e6,
           (double)(ns - g_last_ns) / 1e6, (unsigned long long)__atomic_load_n(&g_images, __ATOMIC_RELAXED),
           (double)__atomic_load_n(&g_host_bytes, __ATOMIC_RELAXED) / 1048576.0,
           (double)__atomic_load_n(&g_guest_bytes, __ATOMIC_RELAXED) / 1048576.0);
    g_last_src = src;
    g_last_ns = ns;
}

void vkf_decode_stats_frame(uint64_t frame)
{
    static uint64_t every;
    static bool read;
    if (!read) {
        int64_t n = rl_setting_int("stats.every");
        every = n > 0 ? (uint64_t)n : 0;
        read = true;
    }
    if (!every || frame % every || __atomic_load_n(&g_src_bytes, __ATOMIC_RELAXED) == g_last_src)
        return;
    char when[48];
    snprintf(when, sizeof when, "frame %llu", (unsigned long long)frame);
    stats_line(when);
}

void vkf_decode_stats_exit(void)
{
    if (__atomic_load_n(&g_regions, __ATOMIC_RELAXED))
        stats_line("at exit");
}
