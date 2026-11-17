/**
 * @file prdp.cpp
 * @brief The parallel-rdp renderer behind prdp.h, after ares's
 * n64/vulkan/vulkan.cpp (ISC, see ../NOTICE): one command processor on a
 * headless Vulkan device over RDRAM itself, hidden RDRAM half the size of
 * RDRAM, crashes reported through its validation interface, and VI scanned
 * out by its VideoInterface with ares's options: every register write passed
 * on, at each frame edge the field as the current line, then the scan-out
 * and a new frame context. The picture is read back when the host asks.
 * The picture settings (rl_n64_picture_get) pick the internal resolution,
 * the read back, the upscaling quirks and the scan-out options; the
 * console's picture is ares's defaults. The device's pipeline cache lives in
 * the per-user cache folder, one file per GPU and driver version, so a later
 * run does not compile the rasterizer variants an earlier one did.
 */
#include "prdp.h"

#include "context.hpp"
#include "device.hpp"
#include "rdp_device.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

/** @brief The frames between two saves of a grown pipeline cache. */
#define RL_PRDP_CACHE_FRAMES 300u

namespace {

struct Validation : RDP::ValidationInterface {
    const char *error = nullptr;
    void report_rdp_crash(RDP::ValidationError, const char *msg) override { error = msg; }
};

struct Renderer {
    Vulkan::Context context;
    Vulkan::Device device;
    RDP::CommandProcessor *processor = nullptr;
    Validation validation;
    RDP::VIScanoutBuffer scanout;
    RDP::ScanoutOptions options;
    std::vector<uint8_t> picture;
    uint32_t width = 0, height = 0;
    bool fresh = false;
    /** The pipeline cache file, empty for none; the size last saved; frames until the next look. */
    char cache[4096] = {};
    size_t cache_saved = 0;
    unsigned cache_wait = RL_PRDP_CACHE_FRAMES;

    ~Renderer();
};

/** @brief Saves the device's pipeline cache when it grew since the last save. */
void save_cache(Renderer *r)
{
    if (!r->cache[0])
        return;
    size_t size = r->device.get_pipeline_cache_size();
    if (size <= r->cache_saved)
        return;
    std::vector<uint8_t> data(size);
    if (!r->device.get_pipeline_cache_data(data.data(), size))
        return;
    if (rl_n64_write_file(r->cache, data.data(), size))
        r->cache_saved = size;
}

Renderer::~Renderer()
{
    if (processor)
        save_cache(this);
    delete processor;
}

/** @brief Loads the pipeline cache of this GPU and driver, or starts an empty one. */
void load_cache(Renderer *r)
{
    const VkPhysicalDeviceProperties &p = r->device.get_gpu_properties();
    char name[96];
    std::snprintf(name, sizeof name, "vulkan-pipelines-%04x-%08x-%08x.bin", unsigned(p.vendorID), unsigned(p.deviceID),
                  unsigned(p.driverVersion));
    std::vector<uint8_t> data;
    if (rl_n64_cache_path(r->cache, sizeof r->cache, name)) {
        if (FILE *f = std::fopen(r->cache, "rb")) {
            uint8_t buf[65536];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
                data.insert(data.end(), buf, buf + n);
            std::fclose(f);
        }
    }
    r->device.init_pipeline_cache(data.empty() ? nullptr : data.data(), data.size());
    r->cache_saved = data.size();
}

void command(void *user, const uint32_t *words, unsigned count)
{
    static_cast<Renderer *>(user)->processor->enqueue_command(count, words);
}

void sync_full(void *user)
{
    RDP::CommandProcessor *p = static_cast<Renderer *>(user)->processor;
    p->wait_for_timeline(p->signal_timeline());
}

bool crashed(void *user) { return static_cast<Renderer *>(user)->validation.error != nullptr; }

void vi_register(void *user, unsigned index, uint32_t value)
{
    if (index < unsigned(RDP::VIRegister::Count))
        static_cast<Renderer *>(user)->processor->set_vi_register(RDP::VIRegister(index), value);
}

void vi_frame(void *user, unsigned field)
{
    Renderer *r = static_cast<Renderer *>(user);
    r->processor->set_vi_register(RDP::VIRegister::VCurrentLine, field);
    if (r->scanout.fence)
        r->scanout.fence->wait();
    r->processor->scanout_async_buffer(r->scanout, r->options);
    r->processor->begin_frame_context();
    r->fresh = true;
    if (--r->cache_wait == 0) {
        r->cache_wait = RL_PRDP_CACHE_FRAMES;
        save_cache(r);
    }
}

const uint8_t *vi_picture(void *user, uint32_t *width, uint32_t *height)
{
    Renderer *r = static_cast<Renderer *>(user);
    if (r->fresh) {
        r->fresh = false;
        r->width = r->height = 0;
        if (r->scanout.fence && r->scanout.width && r->scanout.height) {
            r->scanout.fence->wait();
            size_t bytes = size_t(r->scanout.width) * r->scanout.height * 4u;
            const void *map = r->device.map_host_buffer(*r->scanout.buffer, Vulkan::MEMORY_ACCESS_READ_BIT);
            r->picture.resize(bytes);
            std::memcpy(r->picture.data(), map, bytes);
            r->device.unmap_host_buffer(*r->scanout.buffer, Vulkan::MEMORY_ACCESS_READ_BIT);
            r->width = r->scanout.width;
            r->height = r->scanout.height;
        }
    }
    if (!r->width)
        return nullptr;
    *width = r->width;
    *height = r->height;
    return r->picture.data();
}

const rl_n64_rdp_ops g_ops = {command, sync_full, crashed, vi_register, vi_frame, vi_picture};

void *create(rl_n64 *m, void *gipa, char *why, size_t why_len)
{
    auto fail = [&](Renderer *r, const char *msg) -> void * {
        delete r;
        std::snprintf(why, why_len, "%s", msg);
        return nullptr;
    };
    if (!gipa || !Vulkan::Context::init_loader(reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa)))
        return fail(nullptr, "no Vulkan loader");
    Renderer *r = new Renderer;
    if (!r->context.init_instance_and_device(nullptr, 0, nullptr, 0, 0))
        return fail(r, "no Vulkan device");
    r->device.set_context(r->context);
    load_cache(r);
    r->device.init_frame_contexts(3);
    rl_n64_picture pic;
    rl_n64_picture_get(&pic);
    RDP::CommandProcessorFlags flags = RDP::COMMAND_PROCESSOR_FLAG_HOST_VISIBLE_HIDDEN_RDRAM_BIT;
    if (pic.upscale == 2)
        flags |= RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_2X_BIT;
    else if (pic.upscale == 4)
        flags |= RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_4X_BIT;
    else if (pic.upscale == 8)
        flags |= RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_8X_BIT;
    if (pic.upscale > 1 && pic.super_sampled_readback)
        flags |= RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_READ_BACK_BIT;
    if (pic.upscale > 1 && pic.readback_dither)
        flags |= RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;
    r->processor =
        new RDP::CommandProcessor(r->device, rl_n64_rdram(m), 0, RL_N64_RDRAM_SIZE, RL_N64_RDRAM_SIZE / 2, flags);
    if (!r->processor->device_is_supported())
        return fail(r, "the Vulkan device cannot run parallel-rdp");
    r->processor->set_validation_interface(&r->validation);
    RDP::Quirks quirks;
    quirks.set_native_resolution_tex_rect(pic.native_tex_rect);
    quirks.set_native_texture_lod(pic.native_texture_lod);
    r->processor->set_quirks(quirks);
    r->options.persist_frame_on_invalid_input = true;
    r->options.downscale_steps = pic.downscale_steps;
    r->options.crop_overscan_pixels = pic.crop_overscan;
    r->options.blend_previous_frame = pic.weave;
    r->options.upscale_deinterlacing = !pic.weave;
    r->options.vi.aa = pic.vi_aa;
    r->options.vi.scale = pic.vi_resample;
    r->options.vi.serrate = pic.vi_serrate;
    r->options.vi.dither_filter = pic.dither_filter;
    r->options.vi.divot_filter = pic.divot;
    r->options.vi.gamma_dither = pic.gamma_dither;
    return r;
}

void destroy(void *r) { delete static_cast<Renderer *>(r); }

} // namespace

extern "C" const rl_n64_renderer rl_n64_prdp = {create, &g_ops, destroy};

/**
 * @brief What parallel-rdp's sources call for dlopen and LoadLibraryA (see
 * noload.h): they open the Vulkan loader only when given no
 * vkGetInstanceProcAddr, which create never does.
 */
extern "C" void *rl_n64_prdp_no_dlopen(const char *, int) { return nullptr; }
/** @brief See rl_n64_prdp_no_dlopen. */
extern "C" void *rl_n64_prdp_no_load_library(const char *) { return nullptr; }
