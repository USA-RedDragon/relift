/**
 * @file nvn_dlss.c
 * @brief The Switch 2 driver's DLSS network procs and nvnCommandBufferEvaluateDlss, run by relift's upscaler stage
 * (relift/upscale.h) inside the game's frame.
 *
 * The builder and parameter procs stay the driver's own code (pure loads and stores); relift implements the procs
 * that reach the driver's DLSS library: GetOptimalSettings answers with the library's sizes, GetStorageSize runs the
 * driver's own arithmetic, Initialize, Resize and Finalize keep the network's sizes, and an evaluation runs the
 * player's `picture.upscaler` choice on the host device and writes the game's output texture, so the game's UI and
 * post effects run after it. The game's own network, translated, is lane work not wired here: the choice `game`
 * stops with a report.
 */
#include "nvn_vk.h"

#include <relift/settings.h>
#include <relift/upscale.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

const uint8_t nvn_dlss_texture_offsets[NVN_DLSS_PARAM_TEXTURES] = { 0, 16, 32, 48, 64, 104 };

/** @brief Bytes of the texture ids a DLSS network builder holds: 32 ints, at +0x38 of the builder, its texture pool's guest address at +8. */
#define DLSS_TEXTURE_IDS 0x80u

/**
 * @brief nvnDlssNetworkBuilderSetTextureIDs(NVNdlssNetworkBuilder *, const NVNtexturePool *, const int *ids): the
 * driver stores the pool at +8 of the builder and memmoves 128 bytes of ids to +0x38 (SDK 22.2.0, read in three
 * games). Only its call to memmove makes the driver's own code impure, so the layer does the same two stores; the
 * builder's other procs stay the driver's and read the same fields back.
 */
uint64_t nvnh_nvnDlssNetworkBuilderSetTextureIDs(rl_ctx *c, uint8_t *mem)
{
    uint64_t builder = c->x[0];
    memmove(mem + builder + 0x38, mem + c->x[2], DLSS_TEXTURE_IDS);
    rl_st64(mem, builder + 8, c->x[1]);
    return nvn_ret(c);
}

/** @brief nvnDlssNetworkBuilderGetTextureIDs(const NVNdlssNetworkBuilder *, int *ids): memmoves the 128 bytes at +0x38 of the builder to ids, as the driver does. */
uint64_t nvnh_nvnDlssNetworkBuilderGetTextureIDs(rl_ctx *c, uint8_t *mem)
{
    memmove(mem + c->x[1], mem + c->x[0] + 0x38, DLSS_TEXTURE_IDS);
    return nvn_ret(c);
}

/**
 * @brief The builder's flags, as the driver hands them to the DLSS library (bit 0 inverted into the library's HDR
 * flag, bits 1, 2, 3 and 6 as they are; lane dlss's loader reproduces it): the color is not HDR, motion vectors are at
 * render size, motion vectors include the jitter, depth is inverted, exposure is automatic.
 */
enum {
    NVN_DLSS_FLAG_LDR = 1u << 0,
    NVN_DLSS_FLAG_MV_LOW_RES = 1u << 1,
    NVN_DLSS_FLAG_MV_JITTERED = 1u << 2,
    NVN_DLSS_FLAG_DEPTH_INVERTED = 1u << 3,
    NVN_DLSS_FLAG_AUTO_EXPOSURE = 1u << 6,
};

/**
 * @brief The probe of driver callback slot `slot`: until a slot is implemented, the library's first call to it stops
 * the run with its arguments and caller, so each slot is filled from what the library really passes.
 */
static uint64_t callback_probe(rl_ctx *c, uint32_t slot)
{
    return (uint64_t)rl_guest_fatal(c, "nvn",
                                    "the console DLSS library called driver callback %u, which relift does not "
                                    "implement yet: x0 0x%llx x1 0x%llx x2 0x%llx x3 0x%llx x4 0x%llx x5 0x%llx "
                                    "x6 0x%llx x7 0x%llx, from 0x%llx",
                                    slot, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                                    (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                                    (unsigned long long)c->x[4], (unsigned long long)c->x[5],
                                    (unsigned long long)c->x[6], (unsigned long long)c->x[7],
                                    (unsigned long long)c->x[30]);
}

#define CALLBACK(i)                                                                                                    \
    static uint64_t callback_##i(rl_ctx *c, uint8_t *mem)                                                              \
    {                                                                                                                  \
        (void)mem;                                                                                                     \
        return callback_probe(c, i);                                                                                   \
    }
CALLBACK(0)
CALLBACK(2)
CALLBACK(3)
CALLBACK(4)
CALLBACK(5)
CALLBACK(6)
CALLBACK(7)
CALLBACK(8)
CALLBACK(9)
CALLBACK(10)
CALLBACK(11)
CALLBACK(12)
CALLBACK(14)
CALLBACK(15)
CALLBACK(16)
CALLBACK(17)
CALLBACK(19)
CALLBACK(20)
#undef CALLBACK

/** @brief Most CUDA kernels the library loads (it loads 64 modules of one kernel each for FE). */
#define MAX_KERNELS 256u
/** @brief The handles relift gives a loaded kernel: its index here plus this tag (the library keeps them opaque). */
#define KERNEL_TAG 0x6b65726e00000000ull

/** @brief The CUDA kernels the library loaded: name, the key of its translation, the guest image. */
static struct {
    char name[128];
    char key[65];
    uint64_t image;
} g_kernels[MAX_KERNELS];
static uint32_t g_nkernels;

/**
 * @brief Callback 18(cb, device, image, &module, &function, name): load a CUDA module image and get its kernel. The
 * console driver runs cuModuleLoadData and cuModuleGetFunction and writes the two handles (sdk 0x65a600). relift finds
 * the kernel's translation by its name (NVN_IFACE_KERNEL_NAME) and gives the kernel's index as both handles. True on
 * success.
 */
static uint64_t callback_18(rl_ctx *c, uint8_t *mem)
{
    char name[128];
    const char *n = nvn_str(mem, c->x[5], sizeof name);
    if (!n || strlen(n) >= sizeof name)
        return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS library loads a kernel without a readable name");
    snprintf(name, sizeof name, "%s", n);
    uint32_t i = 0;
    while (i < g_nkernels && strcmp(g_kernels[i].name, name))
        i++;
    if (i == g_nkernels) {
        if (g_nkernels == MAX_KERNELS)
            return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS library loads more than %u kernels", MAX_KERNELS);
        if (!nvn_shader_find_kernel(nvn_kernel_name_hash(name), g_kernels[i].key))
            return (uint64_t)rl_guest_fatal(c, "nvn",
                                            "the console DLSS library loads CUDA kernel %s, which the shader table has "
                                            "no translation of (a table from before kernel name records?)",
                                            name);
        snprintf(g_kernels[i].name, sizeof g_kernels[i].name, "%s", name);
        g_kernels[i].image = c->x[2];
        g_nkernels++;
    }
    rl_st64(mem, c->x[3], KERNEL_TAG | i);
    rl_st64(mem, c->x[4], KERNEL_TAG | i);
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "DLSS library loads kernel %u %s (image 0x%llx): translation %s", i, name,
               (unsigned long long)c->x[2], g_kernels[i].key);
    c->x[0] = 1;
    return nvn_ret(c);
}

/** @brief Callback 1: the console driver's returns 0 (sdk 0x65a030); the library keeps it at +0x120 of its backend. */
static uint64_t callback_1(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 0;
    return nvn_ret(c);
}

/**
 * @brief Callback 13(cb, device): the GPU's architecture code, which the console driver reads from +0xb8 of its
 * device (sdk 0x65a3b0). The library only compares it (backend 0xa7960): 0x703 and 0x705 pick its sm75 class, 0x806
 * and 0x809 its sm86 class, anything else class 3; the game ships SM88 cubins only, so the console's code is one of
 * the others. relift gives 0x808, compute capability 8.8.
 */
static uint64_t callback_13(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 0x808;
    return nvn_ret(c);
}

rl_fn nvn_dlss_callback(uint32_t i)
{
    static const rl_fn fns[NVN_DLSS_CALLBACKS] = {
        callback_0,  callback_1,  callback_2,  callback_3,  callback_4,  callback_5,  callback_6,
        callback_7,  callback_8,  callback_9,  callback_10, callback_11, callback_12, callback_13,
        callback_14, callback_15, callback_16, callback_17, callback_18, callback_19, callback_20,
    };
    return i < NVN_DLSS_CALLBACKS ? fns[i] : NULL;
}

/** @brief The console DLSS library's object, from its Handshake with relift's callbacks; 0 before. */
static uint64_t g_library;

/** @brief The console library's vtable slots relift calls (.work/dlss/launch.md): init(lib, device), CreateNetwork(lib, desc). */
enum { LIB_INIT = 0, LIB_CREATE_NETWORK = 4 };

/** @brief Calls slot `slot` of the library object's vtable with the library and up to 7 more arguments. */
static int library_call(rl_ctx *c, uint8_t *mem, uint32_t slot, const uint64_t *args, unsigned n, uint64_t *ret)
{
    uint64_t a[8] = { g_library };
    for (unsigned i = 0; i < n && i < 7; i++)
        a[1 + i] = args[i];
    uint64_t fn = rl_ld64(mem, rl_ld64(mem, g_library) + 8u * slot);
    return nvn_guest_call(c, mem, fn, a, 1 + n, ret);
}

/**
 * @brief Finds the game's DLSS library ("dlss"), hands it relift's driver callbacks, and runs its init with the
 * device, once, as the driver's nvnDeviceInitialize does for a device that enables DLSS (sdk 0x658390).
 */
static int library_up(rl_ctx *c, uint8_t *mem, uint64_t device)
{
    if (g_library)
        return RL_OK;
    uint64_t handshake = 0;
    int rc = nvn_os_library_symbol(c, mem, "dlss", "Handshake", &handshake);
    if (rc != RL_OK)
        return rc;
    if (!handshake)
        return rl_guest_fatal(c, "nvn", "picture.upscaler is the game's own DLSS, but the game has no DLSS library");
    uint64_t cb = nvn_dlss_callbacks(mem), lib = 0;
    rc = nvn_guest_call(c, mem, handshake, &cb, 1, &lib);
    if (rc != RL_OK)
        return rc;
    if (!lib)
        return rl_guest_fatal(c, "nvn", "the game's DLSS library refused relift's driver callbacks");
    g_library = lib;
    uint64_t ok = 0;
    rc = library_call(c, mem, LIB_INIT, &device, 1, &ok);
    if (rc != RL_OK)
        return rc;
    if (!(ok & 1)) {
        g_library = 0;
        return rl_guest_fatal(c, "nvn", "the game's DLSS library failed its init on device 0x%llx",
                              (unsigned long long)device);
    }
    rl_log(RL_LOG_INFO, "nvn", "console DLSS library up: Handshake 0x%llx, library object 0x%llx, device 0x%llx",
           (unsigned long long)handshake, (unsigned long long)lib, (unsigned long long)device);
    return RL_OK;
}

/** @brief A network relift initialized: what the builder asked for and the render size of the last Resize. */
typedef struct nvn_dlss_net {
    nvn_obj o;
    uint32_t flags, method, preset, out_w, out_h, render_w, render_h;
    /** @brief The console library's network (its CreateNetwork result), 0 when relift's upscaler runs. */
    uint64_t lib_net;
} nvn_dlss_net;

/**
 * @brief The network descriptor the driver gives the library's CreateNetwork, at +0x210 of the NVNdlssNetwork (sdk
 * 0x658b20; the driver clears 0x258 bytes of the network first): the builder's flags masked to 0x4e with bit 0 set
 * when the builder's bit 0 is clear, method, preset, the optimal render size, the output size, the builder's flag bit
 * 28, the minimum and the maximum render size.
 */
static void network_desc(uint8_t *mem, uint64_t net, uint64_t builder, const uint32_t opt[6])
{
    uint32_t flags = rl_ld32(mem, builder + 0x20), method = rl_ld32(mem, builder + 0x24);
    uint32_t f = flags & 0x4eu;
    if (!(flags & 1u))
        f |= 1u;
    uint64_t d = net + 0x210;
    rl_st32(mem, net + 0x20c, method);
    rl_st32(mem, d + 0x00, f);
    rl_st32(mem, d + 0x04, method);
    rl_st32(mem, d + 0x08, rl_ld32(mem, builder + 0x28));
    rl_st32(mem, d + 0x0c, opt[0]);
    rl_st32(mem, d + 0x10, opt[1]);
    rl_st32(mem, d + 0x14, rl_ld32(mem, builder + 0x2c));
    rl_st32(mem, d + 0x18, rl_ld32(mem, builder + 0x30));
    rl_st32(mem, d + 0x1c, (flags >> 28) & 1u);
    rl_st32(mem, d + 0x20, opt[2]);
    rl_st32(mem, d + 0x24, opt[3]);
    rl_st32(mem, d + 0x28, opt[4]);
    rl_st32(mem, d + 0x2c, opt[5]);
}

static void trace(const char *fmt, ...) RL_PRINTF(1, 2);
static void trace(const char *fmt, ...)
{
    if (!rl_cfg()->trace_os)
        return;
    va_list ap;
    va_start(ap, fmt);
    rl_vlog(RL_LOG_INFO, "nvn", fmt, ap);
    va_end(ap);
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static float rdf(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

void nvn_dlss_params_of(const uint8_t *raw, nvn_dlss_params *out)
{
    uint64_t *tex[NVN_DLSS_PARAM_TEXTURES][2] = {
        { &out->color, &out->color_view },   { &out->out, &out->out_view },   { &out->motion, &out->motion_view },
        { &out->depth, &out->depth_view },   { &out->bias, &out->bias_view }, { &out->exposure, &out->exposure_view },
    };
    for (uint32_t i = 0; i < NVN_DLSS_PARAM_TEXTURES; i++) {
        *tex[i][0] = rd64(raw + nvn_dlss_texture_offsets[i]);
        *tex[i][1] = rd64(raw + nvn_dlss_texture_offsets[i] + 8);
    }
    out->jitter[0] = rdf(raw + 80);
    out->jitter[1] = rdf(raw + 84);
    out->motion_scale[0] = rdf(raw + 88);
    out->motion_scale[1] = rdf(raw + 92);
    out->pre_exposure = rdf(raw + 96);
    out->reset = raw[120] != 0;
    out->evaluation_index = rd32(raw + 124);
    uint32_t *pairs[6] = { out->in_size,     out->color_base, out->depth_base,
                           out->motion_base, out->bias_base,  out->out_base };
    for (uint32_t i = 0; i < 6; i++) {
        pairs[i][0] = rd32(raw + 128 + 8 * i);
        pairs[i][1] = rd32(raw + 132 + 8 * i);
    }
}

/** @brief x times scale, both single precision, rounded half up: the library's arithmetic. */
static uint32_t scaled(uint32_t x, float scale)
{
    float p = (float)x * scale;
    return (uint32_t)floor((double)p + 0.5);
}

void nvn_dlss_optimal(uint32_t width, uint32_t height, uint32_t method, uint32_t out[6])
{
    float opt = 0.58f, lo = 0.5f, hi = 1.0f;
    if (method == 1)
        opt = lo = hi = 1.0f / 3.0f;
    else if (method == 2)
        opt = 1.0f, lo = 0.99f;
    const float s[3] = { opt, lo, hi };
    for (int i = 0; i < 3; i++) {
        out[2 * i] = scaled(width, s[i]);
        out[2 * i + 1] = scaled(height, s[i]);
    }
}

/**
 * @brief Calls the driver's getter name on obj with nout int outputs (into out) on the guest stack; *ret gets x0
 * when ret is not NULL. RL_OK or a pending stop.
 */
static int get(rl_ctx *c, uint8_t *mem, const char *name, uint64_t obj, unsigned nout, uint32_t *out, uint64_t *ret)
{
    uint64_t sp = c->sp, base = (sp - 32) & ~15ull, r = 0;
    memset(mem + base, 0, 16);
    uint64_t a[3] = { obj, base, base + 4 };
    c->sp = base;
    int rc = nvn_driver_call(c, mem, name, a, 1 + nout, &r);
    c->sp = sp;
    for (unsigned i = 0; rc == RL_OK && i < nout; i++)
        out[i] = rl_ld32(mem, base + 4u * i);
    if (ret)
        *ret = r;
    return rc;
}

/** @brief The builder's output size and method. */
static int builder_size(rl_ctx *c, uint8_t *mem, uint64_t builder, uint32_t wh[2], uint64_t *method)
{
    int rc = get(c, mem, "nvnDlssNetworkBuilderGetOutputResolution", builder, 2, wh, NULL);
    return rc != RL_OK ? rc : get(c, mem, "nvnDlssNetworkBuilderGetMethod", builder, 0, NULL, method);
}

/**
 * @brief nvnDlssNetworkBuilderGetOptimalSettings(const NVNdlssNetworkBuilder *, int *optimalWidth, int
 * *optimalHeight, int *minWidth, int *minHeight, int *maxWidth, int *maxHeight): the render sizes the library gives
 * for the builder's output size and method.
 */
uint64_t nvnh_nvnDlssNetworkBuilderGetOptimalSettings(rl_ctx *c, uint8_t *mem)
{
    uint64_t builder = c->x[0], ptr[6], method = 0;
    for (int i = 0; i < 6; i++)
        ptr[i] = c->x[1 + i];
    uint32_t wh[2] = { 0 }, out[6];
    int rc = builder_size(c, mem, builder, wh, &method);
    if (rc != RL_OK)
        return (uint64_t)rc;
    nvn_dlss_optimal(wh[0], wh[1], (uint32_t)method, out);
    for (int i = 0; i < 6; i++)
        if (ptr[i])
            rl_st32(mem, ptr[i], out[i]);
    trace("nvnDlssNetworkBuilderGetOptimalSettings(%ux%u, method %u) = %ux%u, min %ux%u, max %ux%u", wh[0], wh[1],
          (unsigned)method, out[0], out[1], out[2], out[3], out[4], out[5]);
    c->x[0] = 0;
    return nvn_ret(c);
}

/** @brief nvnDlssNetworkBuilderGetStorageSize(const NVNdlssNetworkBuilder *): the driver's own arithmetic. */
uint64_t nvnh_nvnDlssNetworkBuilderGetStorageSize(rl_ctx *c, uint8_t *mem)
{
    uint64_t builder = c->x[0], dev = 0, fn = 0, size = 0;
    int rc = get(c, mem, "nvnDlssNetworkBuilderGetDevice", builder, 0, NULL, &dev);
    if (rc == RL_OK)
        rc = nvn_device_fields(c, mem, dev);
    if (rc == RL_OK)
        rc = nvn_driver_lookup(c, mem, "nvnDlssNetworkBuilderGetStorageSize", &fn);
    if (rc == RL_OK && !fn)
        rc = rl_guest_fatal(c, "nvn", "the driver has no nvnDlssNetworkBuilderGetStorageSize");
    if (rc == RL_OK)
        rc = nvn_guest_call(c, mem, fn, &builder, 1, &size);
    if (rc != RL_OK)
        return (uint64_t)rc;
    trace("nvnDlssNetworkBuilderGetStorageSize(0x%llx) = 0x%llx", (unsigned long long)builder,
          (unsigned long long)size);
    c->x[0] = size;
    return nvn_ret(c);
}

/** @brief nvnDlssNetworkInitialize(NVNdlssNetwork *, const NVNdlssNetworkBuilder *): NVNboolean. */
uint64_t nvnh_nvnDlssNetworkInitialize(rl_ctx *c, uint8_t *mem)
{
    uint64_t net = c->x[0], builder = c->x[1], method = 0, flags = 0, preset = 0;
    uint32_t wh[2] = { 0 }, opt[6];
    int rc = builder_size(c, mem, builder, wh, &method);
    if (rc == RL_OK)
        rc = get(c, mem, "nvnDlssNetworkBuilderGetFlags", builder, 0, NULL, &flags);
    if (rc == RL_OK)
        rc = get(c, mem, "nvnDlssNetworkBuilderGetPreset", builder, 0, NULL, &preset);
    if (rc != RL_OK)
        return (uint64_t)rc;
    nvn_dlss_optimal(wh[0], wh[1], (uint32_t)method, opt);
    uint64_t lib_net = 0;
    if (rl_upscale_resolve(rl_setting_enum("picture.upscaler"), true, false) == RL_UPSCALE_GAME) {
        rc = library_up(c, mem, rl_ld64(mem, builder));
        if (rc != RL_OK)
            return (uint64_t)rc;
        memset(mem + net, 0, 0x258);
        network_desc(mem, net, builder, opt);
        uint64_t desc = net + 0x210;
        rc = library_call(c, mem, LIB_CREATE_NETWORK, &desc, 1, &lib_net);
        if (rc != RL_OK)
            return (uint64_t)rc;
        rl_st64(mem, net + 0xa0, lib_net);
        rl_log(RL_LOG_INFO, "nvn", "console DLSS network 0x%llx created: 0x%llx", (unsigned long long)net,
               (unsigned long long)lib_net);
        if (!lib_net) {
            c->x[0] = 0;
            return nvn_ret(c);
        }
    }
    nvn_dlss_net *n = (nvn_dlss_net *)nvn_obj_new(net, NVN_DLSS_NETWORK, sizeof *n);
    if (n) {
        n->lib_net = lib_net;
        n->flags = (uint32_t)flags;
        n->method = (uint32_t)method;
        n->preset = (uint32_t)preset;
        n->out_w = wh[0];
        n->out_h = wh[1];
        n->render_w = opt[0];
        n->render_h = opt[1];
    }
    n = (nvn_dlss_net *)nvn_obj_publish((nvn_obj *)n);
    trace("nvnDlssNetworkInitialize(0x%llx: %ux%u, method %u, preset %u, flags 0x%x)", (unsigned long long)net, wh[0],
          wh[1], (unsigned)method, (unsigned)preset, (unsigned)flags);
    c->x[0] = n != NULL;
    return nvn_ret(c);
}

/** @brief nvnDlssNetworkResize(NVNdlssNetwork *, int width, int height): NVNboolean; false past the output size. */
uint64_t nvnh_nvnDlssNetworkResize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_dlss_net *n = (nvn_dlss_net *)nvn_obj_get(c->x[0], NVN_DLSS_NETWORK);
    int32_t w = (int32_t)c->x[1], h = (int32_t)c->x[2];
    bool ok = n && w > 0 && h > 0 && (uint32_t)w <= n->out_w && (uint32_t)h <= n->out_h;
    if (ok) {
        n->render_w = (uint32_t)w;
        n->render_h = (uint32_t)h;
    }
    trace("nvnDlssNetworkResize(0x%llx, %dx%d) = %d", (unsigned long long)c->x[0], w, h, ok);
    c->x[0] = ok;
    return nvn_ret(c);
}

/** @brief nvnDlssNetworkFinalize(NVNdlssNetwork *). */
uint64_t nvnh_nvnDlssNetworkFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_DLSS_NETWORK));
    return nvn_ret(c);
}

bool nvn_dlss_continues(nvn_dlss_seen *seen, uint64_t net, const uint32_t sizes[4])
{
    bool same = seen->net == net && !memcmp(seen->sizes, sizes, sizeof seen->sizes);
    seen->net = net;
    memcpy(seen->sizes, sizes, sizeof seen->sizes);
    return same;
}

#define MAX_HELD 64u

/** @brief The stage on the host device, and the views the batch being recorded uses (freed after its wait). */
static struct {
    rl_upscaler *u;
    uint32_t choice;
    uint64_t evaluations;
    nvn_dlss_seen seen;
    /** @brief The views each slot's batch holds; held and nheld are the recording slot's. */
    VkImageView held_of[NVN_SLOTS][MAX_HELD];
    uint32_t nheld_of[NVN_SLOTS];
    VkImageView *held;
    uint32_t nheld;
    uint32_t slot;
} st = { .held = st.held_of[0] };

void nvn_dlss_reset(uint32_t slot)
{
    VkImageView *h = slot == st.slot ? st.held : st.held_of[slot];
    uint32_t *n = slot == st.slot ? &st.nheld : &st.nheld_of[slot];
    for (uint32_t i = 0; i < *n; i++)
        VK(vkDestroyImageView)(g.dev, h[i], NULL);
    *n = 0;
    rl_upscaler_retire(st.u);
}

void nvn_dlss_slot(uint32_t slot)
{
    st.nheld_of[st.slot] = st.nheld;
    st.slot = slot;
    st.held = st.held_of[slot];
    st.nheld = st.nheld_of[slot];
}

/** @brief The stage for choice on the host device, started the first time and again when the choice changes. */
static rl_upscaler *stage_for(replay *r, uint32_t choice)
{
    if (st.u && st.choice == choice)
        return st.u;
    if (st.u) {
        if (!nvn_vk_flush(r))
            return NULL;
        rl_upscaler_free(st.u);
        st.u = NULL;
    }
    void (*raw)(void) = g.fns.f[VKG_vkGetDeviceProcAddr];
    rl_post_device d = {
        .instance = (uint64_t)(uintptr_t)g.instance,
        .physical_device = (uint64_t)(uintptr_t)g.phys,
        .device = (uint64_t)(uintptr_t)g.dev,
        .queue = (uint64_t)(uintptr_t)g.queue,
        .queue_family = g.family,
    };
    memcpy(&d.get_device_proc_addr, &raw, sizeof raw);
    rl_upscale_in_frame();
    st.u = rl_upscaler_new(&d, choice, RL_UPSCALE_IMAGES);
    st.choice = choice;
    if (st.u)
        rl_log(RL_LOG_INFO, "nvn",
               "DLSS evaluations run relift's %s upscaler (picture.upscaler); the picture differs from the console's",
               rl_upscale_name(rl_upscaler_choice(st.u)));
    return st.u;
}

/** @brief Keeps view until the batch's wait; false when too many are held (the caller flushes first). */
static bool hold(VkImageView view)
{
    if (st.nheld == MAX_HELD)
        return false;
    st.held[st.nheld++] = view;
    return true;
}

/** @brief The texture view at the recorded pointer v (a snapshot), into nv; has is false for none. */
static int view_of(replay *r, uint64_t v, nvn_view *nv, bool *has)
{
    *has = false;
    const uint8_t *bytes = nvn_cmd_bytes(r->s, v);
    if (!bytes)
        return RL_OK;
    *has = true;
    return nvn_view_read(r->c, r->mem, bytes, nv);
}

/**
 * @brief Fills out's image, format, level, layer and size (the level's) from host image im seen through view nv (NULL
 * for the whole image).
 */
static void describe(const vimg *im, const nvn_view *nv, rl_upscale_image *out)
{
    VkImageAspectFlags aspect = 0;
    VkFormat f = nv && nv->has_format ? nvn_vk_view_format(im, nv->format, &aspect) : im->format;
    uint32_t level = nv && nv->has_levels ? (uint32_t)nv->base_level : 0;
    out->image = (uint64_t)(uintptr_t)im->image;
    out->format = (int32_t)(f ? f : im->format);
    out->level = level;
    out->layer = nv && nv->has_layers ? (uint32_t)nv->min_layer : 0;
    out->width = im->d.width >> level ? im->d.width >> level : 1;
    out->height = im->d.height >> level ? im->d.height >> level : 1;
}

/**
 * @brief A sampled host view of the texture at guest address tex through the recorded view pointer v (none when tex is
 * 0), held until the batch's wait when it is made for this view; RL_OK, RL_PENDING, or a stop.
 */
static int side_view(replay *r, uint64_t tex, uint64_t v, rl_upscale_image *out)
{
    memset(out, 0, sizeof *out);
    if (!tex)
        return RL_OK;
    nvn_view nv;
    bool has;
    int rc = view_of(r, v, &nv, &has);
    if (rc != RL_OK)
        return rc;
    vimg *im = nvn_vk_image(r, tex);
    if (!im)
        return RL_PENDING;
    VkImageView view = nvn_vk_view(im, VK_IMAGE_VIEW_TYPE_2D, has ? &nv : NULL);
    if (!view)
        return nvn_vk_stop(r, "nvnCommandBufferEvaluateDlss: no host view of texture 0x%llx", (unsigned long long)tex);
    if (has)
        hold(view);
    out->handle = (uint64_t)(uintptr_t)view;
    describe(im, has ? &nv : NULL, out);
    return RL_OK;
}

int nvn_dlss_run(replay *r, const nvn_cmd *e)
{
    const uint8_t *raw = nvn_cmd_bytes(r->s, e->x[2]);
    if (!raw)
        return nvn_vk_stop(r, "nvnCommandBufferEvaluateDlss without parameters");
    nvn_dlss_params p;
    nvn_dlss_params_of(raw, &p);
    nvn_dlss_net net_copy;
    nvn_dlss_net *held = (nvn_dlss_net *)nvn_obj_acquire(e->x[1], NVN_DLSS_NETWORK);
    if (held) {
        net_copy = *held;
        nvn_obj_release(&held->o);
    }
    const nvn_dlss_net *n = held ? &net_copy : NULL;
    uint32_t choice = rl_upscale_resolve(rl_setting_enum("picture.upscaler"), true, false);
    if (choice == RL_UPSCALE_GAME)
        return nvn_vk_stop(r, "nvnCommandBufferEvaluateDlss: the game's own DLSS does not run on the host yet; set "
                              "picture.upscaler to one of relift's upscalers");
    if (!p.color || !p.out)
        return nvn_vk_stop(r, "nvnCommandBufferEvaluateDlss without an input or output texture");
    nvn_view cv, ov;
    bool has_cv, has_ov;
    int rc = view_of(r, p.color_view, &cv, &has_cv);
    if (rc == RL_OK)
        rc = view_of(r, p.out_view, &ov, &has_ov);
    if (rc != RL_OK)
        return rc;
    vimg *in = nvn_vk_image(r, p.color), *out = in ? nvn_vk_image(r, p.out) : NULL;
    if (!in || !out)
        return RL_PENDING;
    if (!out->storage_ok)
        return nvn_vk_stop(r, "nvnCommandBufferEvaluateDlss: the output texture's format %u cannot be written by a "
                              "compute stage on the host",
                           out->d.format);
    rl_upscaler *u = stage_for(r, choice);
    if (!u)
        return nvn_vk_stop(r, "nvnCommandBufferEvaluateDlss: the %s upscaler did not start", rl_upscale_name(choice));
    uint32_t level = has_ov && ov.has_levels ? (uint32_t)ov.base_level : 0;
    uint32_t in_level = has_cv && cv.has_levels ? (uint32_t)cv.base_level : 0;
    uint32_t iw = in->d.width >> in_level, ih = in->d.height >> in_level;
    uint32_t ow = out->d.width >> level, oh = out->d.height >> level;
    iw = iw ? iw : 1, ih = ih ? ih : 1, ow = ow ? ow : 1, oh = oh ? oh : 1;
    rl_upscale_inputs ui = {
        .color = { .width = iw, .height = ih },
        .hdr = n && !(n->flags & NVN_DLSS_FLAG_LDR),
        .depth_inverted = n && (n->flags & NVN_DLSS_FLAG_DEPTH_INVERTED),
        .motion_display_size = n && !(n->flags & NVN_DLSS_FLAG_MV_LOW_RES),
        .motion_jittered = n && (n->flags & NVN_DLSS_FLAG_MV_JITTERED),
        .auto_exposure = n && (n->flags & NVN_DLSS_FLAG_AUTO_EXPOSURE),
        .motion_scale = { p.motion_scale[0], p.motion_scale[1] },
        .pre_exposure = p.pre_exposure,
        .jitter = { p.jitter[0], p.jitter[1] },
        .reset = p.reset,
        .in = { p.color_base[0], p.color_base[1], p.in_size[0], p.in_size[1] },
        .out_rect = { p.out_base[0], p.out_base[1], n ? n->out_w : ow - p.out_base[0],
                      n ? n->out_h : oh - p.out_base[1] },
        .out = { .width = ow, .height = oh },
    };
    if (!ui.in.width || !ui.in.height) {
        ui.in.width = n ? n->render_w : iw - ui.in.x;
        ui.in.height = n ? n->render_h : ih - ui.in.y;
    }
    if (ui.in.x + ui.in.width > iw)
        ui.in.width = iw > ui.in.x ? iw - ui.in.x : 0;
    if (ui.in.y + ui.in.height > ih)
        ui.in.height = ih > ui.in.y ? ih - ui.in.y : 0;
    if (ui.out_rect.x + ui.out_rect.width > ow)
        ui.out_rect.width = ow > ui.out_rect.x ? ow - ui.out_rect.x : 0;
    if (ui.out_rect.y + ui.out_rect.height > oh)
        ui.out_rect.height = oh > ui.out_rect.y ? oh - ui.out_rect.y : 0;
    uint32_t sizes[4] = { ui.in.width, ui.in.height, ui.out_rect.width, ui.out_rect.height };
    if (!nvn_dlss_continues(&st.seen, e->x[1], sizes)) {
        ui.reset = true;
        rl_log(RL_LOG_INFO, "nvn",
               "DLSS evaluation on network 0x%llx (method %u, preset %u, flags 0x%x): %ux%u of a %ux%u input to %ux%u, "
               "jitter %g,%g, motion vector scale %g,%g, pre-exposure %g, depth %s, motion vectors %s, exposure %s",
               (unsigned long long)e->x[1], n ? n->method : 0, n ? n->preset : 0, n ? n->flags : 0, ui.in.width,
               ui.in.height, iw, ih, ui.out_rect.width, ui.out_rect.height, (double)p.jitter[0], (double)p.jitter[1],
               (double)p.motion_scale[0], (double)p.motion_scale[1], (double)p.pre_exposure, p.depth ? "yes" : "no",
               p.motion ? "yes" : "no", p.exposure ? "yes" : "no");
    }
    for (int pass = 0;; pass++) {
        if (pass == 2)
            return nvn_vk_stop(r,
                               "nvnCommandBufferEvaluateDlss: %ux%u at %u,%u of a %ux%u input to %ux%u at %u,%u of a "
                               "%ux%u output is not a size the upscaler takes",
                               ui.in.width, ui.in.height, ui.in.x, ui.in.y, iw, ih, ui.out_rect.width,
                               ui.out_rect.height, ui.out_rect.x, ui.out_rect.y, ow, oh);
        if ((pass || st.nheld + 6 > MAX_HELD) && !nvn_vk_flush(r))
            return nvn_vk_stop(r, "the host device failed a submit");
        if (rl_upscale_temporal(rl_upscaler_choice(u))) {
            int vrc = side_view(r, p.depth, p.depth_view, &ui.depth);
            if (vrc == RL_OK)
                vrc = side_view(r, p.motion, p.motion_view, &ui.motion);
            if (vrc == RL_OK)
                vrc = side_view(r, p.exposure, p.exposure_view, &ui.exposure);
            if (vrc == RL_OK)
                vrc = side_view(r, p.bias, p.bias_view, &ui.reactive);
            if (vrc != RL_OK)
                return vrc;
        }
        VkImageView iv = nvn_vk_view(in, VK_IMAGE_VIEW_TYPE_2D, has_cv ? &cv : NULL);
        VkImageView owv = nvn_vk_storage_view(out, VK_IMAGE_VIEW_TYPE_2D, out->format, level, 0, 1);
        if (has_cv && iv)
            hold(iv);
        if (owv)
            hold(owv);
        if (!iv || !owv)
            return nvn_vk_stop(r, "nvnCommandBufferEvaluateDlss: no host view of the input or output texture");
        ui.color.handle = (uint64_t)(uintptr_t)iv;
        ui.out.handle = (uint64_t)(uintptr_t)owv;
        describe(in, has_cv ? &cv : NULL, &ui.color);
        ui.out.image = (uint64_t)(uintptr_t)out->image;
        ui.out.format = (int32_t)out->format;
        ui.out.level = level;
        if (!nvn_vk_begin(r))
            return nvn_vk_stop(r, "the host device could not begin a command buffer");
        nvn_vk_barrier(g.cb);
        if (rl_upscaler_record(u, (uint64_t)(uintptr_t)g.cb, &ui))
            break;
    }
    nvn_vk_barrier(g.cb);
    nvn_vk_written(out);
    if (nvn_flash_on()) {
        nvn_trace_note("upscale", "\"in\":\"0x%llx\",\"out\":\"0x%llx\",\"in_rect\":[%u,%u,%u,%u],\"reset\":%d,\"game_reset\":%d",
                       (unsigned long long)in->storage, (unsigned long long)out->storage, ui.in.x, ui.in.y, ui.in.width,
                       ui.in.height, (int)ui.reset, (int)p.reset);
        nvn_flash_wrote(out, level, 0);
        nvn_flash_thumbs(r, "upscale");
    }
    /* The evaluation writes its output texture and nothing else of the game's: a reader of other guest memory need
     * not wait for this batch (marking all of it made each frame's replay wait for the GPU, 20 ms a frame on a Thor). */
    nvn_vk_mark_write_range(out->storage, out->d.size);
    if (!st.evaluations++)
        trace("nvnCommandBufferEvaluateDlss: %ux%u -> %ux%u with %s", ui.in.width, ui.in.height, ui.out_rect.width,
              ui.out_rect.height, rl_upscale_name(rl_upscaler_choice(u)));
    return RL_OK;
}
