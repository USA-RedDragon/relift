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
 * run does not compile the rasterizer variants an earlier one did. Every
 * compute pipeline parallel-rdp makes is recorded by its shader and its
 * specialization constants (rdp-variants.txt in that folder); the first
 * time a later run makes a pipeline of a shader, worker threads build every
 * recorded variant of that shader into the cache, so the driver has them
 * before parallel-rdp asks.
 */
#include "prdp.h"

#include "context.hpp"
#include "device.hpp"
#include "rdp_device.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

/** @brief The frames between two saves of a grown pipeline cache. */
#define RL_PRDP_CACHE_FRAMES 300u

namespace {

/** @brief A compute pipeline's shader (by the FNV-1a hash of its SPIR-V) and specialization constants. */
struct Variant {
    uint64_t shader = 0;
    std::vector<uint32_t> ids, values;

    bool operator<(const Variant &o) const
    {
        return std::tie(shader, ids, values) < std::tie(o.shader, o.ids, o.values);
    }
};

/** @brief What the pipeline hooks keep: the run's variants, the shaders' hashes, the shaders warmed. */
struct Warm {
    std::mutex lock;
    PFN_vkCreateShaderModule create_module = nullptr;
    PFN_vkCreateComputePipelines create_pipelines = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline = nullptr;
    std::unordered_map<uint64_t, uint64_t> module_hash;
    std::set<Variant> known, seen;
    std::set<uint64_t> warmed;
    std::vector<std::thread> threads;
    char path[4096] = {};
    bool dirty = false;
};

Warm g_warm;

uint64_t fnv(const void *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ static_cast<const uint8_t *>(p)[i]) * 0x100000001B3ull;
    return h;
}

VKAPI_ATTR VkResult VKAPI_CALL hook_module(VkDevice device, const VkShaderModuleCreateInfo *info,
                                           const VkAllocationCallbacks *alloc, VkShaderModule *module)
{
    VkResult vr = g_warm.create_module(device, info, alloc, module);
    if (vr == VK_SUCCESS) {
        std::lock_guard<std::mutex> l(g_warm.lock);
        g_warm.module_hash[uint64_t(*module)] = fnv(info->pCode, info->codeSize);
    }
    return vr;
}

/** @brief Builds each variant into cache with the stage, layout and flags of info, and drops the pipelines. */
void warm_variants(VkDevice device, VkPipelineCache cache, VkComputePipelineCreateInfo info,
                   VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup, bool has_subgroup,
                   std::vector<Variant> todo)
{
    if (has_subgroup) {
        subgroup.pNext = nullptr;
        info.stage.pNext = &subgroup;
    } else {
        info.stage.pNext = nullptr;
    }
    info.flags &= ~VkPipelineCreateFlags(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
    info.basePipelineHandle = VK_NULL_HANDLE;
    info.pNext = nullptr;
    for (const Variant &v : todo) {
        std::vector<VkSpecializationMapEntry> map(v.ids.size());
        for (size_t i = 0; i < v.ids.size(); i++)
            map[i] = { v.ids[i], uint32_t(i * 4u), 4u };
        VkSpecializationInfo spec = { uint32_t(map.size()), map.data(), v.values.size() * 4u, v.values.data() };
        info.stage.pSpecializationInfo = map.empty() ? nullptr : &spec;
        VkPipeline pipe = VK_NULL_HANDLE;
        if (g_warm.create_pipelines(device, cache, 1, &info, nullptr, &pipe) == VK_SUCCESS && pipe)
            g_warm.destroy_pipeline(device, pipe, nullptr);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL hook_pipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                              const VkComputePipelineCreateInfo *infos,
                                              const VkAllocationCallbacks *alloc, VkPipeline *pipes)
{
    {
        std::lock_guard<std::mutex> l(g_warm.lock);
        for (uint32_t k = 0; k < count; k++) {
            const VkComputePipelineCreateInfo &info = infos[k];
            auto h = g_warm.module_hash.find(uint64_t(info.stage.module));
            if (h == g_warm.module_hash.end())
                continue;
            Variant v;
            v.shader = h->second;
            if (const VkSpecializationInfo *sp = info.stage.pSpecializationInfo) {
                for (uint32_t i = 0; i < sp->mapEntryCount; i++) {
                    const VkSpecializationMapEntry &e = sp->pMapEntries[i];
                    uint32_t value = 0;
                    if (e.size == 4 && e.offset + 4u <= sp->dataSize)
                        std::memcpy(&value, static_cast<const uint8_t *>(sp->pData) + e.offset, 4);
                    v.ids.push_back(e.constantID);
                    v.values.push_back(value);
                }
            }
            if (g_warm.seen.insert(v).second && !g_warm.known.count(v))
                g_warm.dirty = true;
            if (!g_warm.warmed.insert(v.shader).second)
                continue;
            std::vector<Variant> todo;
            for (const Variant &w : g_warm.known)
                if (w.shader == v.shader && !(w.ids == v.ids && w.values == v.values))
                    todo.push_back(w);
            if (todo.empty())
                continue;
            VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sub = {};
            bool has_sub = false;
            for (auto *n = static_cast<const VkBaseInStructure *>(info.stage.pNext); n; n = n->pNext)
                if (n->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO) {
                    sub = *reinterpret_cast<const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *>(n);
                    has_sub = true;
                }
            unsigned workers = std::thread::hardware_concurrency() > 4 ? 2u : 1u;
            for (unsigned t = 0; t < workers; t++) {
                std::vector<Variant> part;
                for (size_t i = t; i < todo.size(); i += workers)
                    part.push_back(todo[i]);
                g_warm.threads.emplace_back(warm_variants, device, cache, info, sub, has_sub, std::move(part));
            }
        }
    }
    return g_warm.create_pipelines(device, cache, count, infos, alloc, pipes);
}

/** @brief Reads the variants in text (one per line: shader hash, then id=value pairs) into out. */
void parse_variants(const char *text, std::set<Variant> &out)
{
    while (text && *text) {
        const char *end = std::strchr(text, '\n');
        std::string line(text, end ? size_t(end - text) : std::strlen(text));
        text = end ? end + 1 : nullptr;
        Variant v;
        const char *p = line.c_str();
        char *q;
        v.shader = std::strtoull(p, &q, 16);
        if (q == p)
            continue;
        p = q;
        bool ok = true;
        while (*p == ' ') {
            unsigned long id = std::strtoul(p + 1, &q, 10);
            if (*q != '=') {
                ok = false;
                break;
            }
            unsigned long value = std::strtoul(q + 1, &q, 10);
            v.ids.push_back(uint32_t(id));
            v.values.push_back(uint32_t(value));
            p = q;
        }
        if (ok)
            out.insert(v);
    }
}

/** @brief Writes the run's variants and the known ones when the run made a new one. */
void save_variants()
{
    std::lock_guard<std::mutex> l(g_warm.lock);
    if (!g_warm.dirty || !g_warm.path[0])
        return;
    std::set<Variant> all = g_warm.known;
    all.insert(g_warm.seen.begin(), g_warm.seen.end());
    std::string text;
    for (const Variant &v : all) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v.shader);
        text += buf;
        for (size_t i = 0; i < v.ids.size(); i++) {
            std::snprintf(buf, sizeof buf, " %u=%u", v.ids[i], v.values[i]);
            text += buf;
        }
        text += '\n';
    }
    if (rl_n64_write_file(g_warm.path, reinterpret_cast<const uint8_t *>(text.data()), text.size())) {
        g_warm.known = all;
        g_warm.dirty = false;
    }
}

/** @brief Hooks the device's shader module and compute pipeline creation and loads the known variants. */
void hook_device(Vulkan::Context &context)
{
    auto &table = const_cast<VolkDeviceTable &>(context.get_device_table());
    g_warm.create_module = table.vkCreateShaderModule;
    g_warm.create_pipelines = table.vkCreateComputePipelines;
    g_warm.destroy_pipeline = table.vkDestroyPipeline;
    table.vkCreateShaderModule = hook_module;
    table.vkCreateComputePipelines = hook_pipelines;
    parse_variants(rl_n64_rdp_variants, g_warm.known);
    if (rl_n64_cache_path(g_warm.path, sizeof g_warm.path, "rdp-variants.txt")) {
        if (FILE *f = std::fopen(g_warm.path, "rb")) {
            std::string text;
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
                text.append(buf, n);
            std::fclose(f);
            parse_variants(text.c_str(), g_warm.known);
        }
    }
}

/** @brief Waits for the warm threads; the device must outlive them. */
void join_warm()
{
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> l(g_warm.lock);
        threads.swap(g_warm.threads);
    }
    for (std::thread &t : threads)
        t.join();
}

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
    join_warm();
    if (processor) {
        save_cache(this);
        save_variants();
    }
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
        save_variants();
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

/**
 * @brief The largest upscale up to want the device can hold: parallel-rdp keeps RDRAM upscale squared times over
 * in one storage buffer, and that and the hidden RDRAM beside it should take at most a quarter of the largest
 * device-local heap.
 */
uint32_t fitting_upscale(const Vulkan::Context &ctx, uint32_t want)
{
    const VkPhysicalDeviceMemoryProperties &mem = ctx.get_mem_props();
    VkDeviceSize heap = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
        if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mem.memoryHeaps[i].size > heap)
            heap = mem.memoryHeaps[i].size;
    uint64_t range = ctx.get_gpu_props().limits.maxStorageBufferRange;
    uint32_t s = want;
    while (s > 1 && (uint64_t(RL_N64_RDRAM_SIZE) * s * s > range ||
                     uint64_t(RL_N64_RDRAM_SIZE) * 3 / 2 * s * s > heap / 4))
        s /= 2;
    return s;
}

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
    hook_device(r->context);
    r->device.set_context(r->context);
    load_cache(r);
    r->device.init_frame_contexts(3);
    rl_n64_picture pic;
    rl_n64_picture_get(&pic);
    uint32_t fit = fitting_upscale(r->context, pic.upscale);
    rl_n64_picture_upscaled(pic.upscale, fit);
    pic.upscale = fit;
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
