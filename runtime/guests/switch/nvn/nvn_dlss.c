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

/** @brief A network relift initialized: what the builder asked for and the render size of the last Resize. */
typedef struct nvn_dlss_net {
    nvn_obj o;
    uint32_t flags, method, preset, out_w, out_h, render_w, render_h;
} nvn_dlss_net;

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
    nvn_dlss_net *n = (nvn_dlss_net *)nvn_obj_new(net, NVN_DLSS_NETWORK, sizeof *n);
    if (n) {
        nvn_dlss_optimal(wh[0], wh[1], (uint32_t)method, opt);
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

#define MAX_HELD 64u

/** @brief The stage on the host device, and the views the batch being recorded uses (freed after its wait). */
static struct {
    rl_upscaler *u;
    uint32_t choice;
    uint64_t evaluations;
    VkImageView held[MAX_HELD];
    uint32_t nheld;
} st;

void nvn_dlss_reset(void)
{
    for (uint32_t i = 0; i < st.nheld; i++)
        VK(vkDestroyImageView)(g.dev, st.held[i], NULL);
    st.nheld = 0;
    rl_upscaler_retire(st.u);
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
               rl_upscale_name(choice));
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
        .color = { 0, iw, ih },
        .hdr = n && !(n->flags & NVN_DLSS_FLAG_LDR),
        .depth_inverted = n && (n->flags & NVN_DLSS_FLAG_DEPTH_INVERTED),
        .motion_display_size = n && !(n->flags & NVN_DLSS_FLAG_MV_LOW_RES),
        .motion_scale = { p.motion_scale[0], p.motion_scale[1] },
        .pre_exposure = p.pre_exposure,
        .jitter = { p.jitter[0], p.jitter[1] },
        .reset = p.reset,
        .in = { p.color_base[0], p.color_base[1], p.in_size[0], p.in_size[1] },
        .out_rect = { p.out_base[0], p.out_base[1], n ? n->out_w : ow - p.out_base[0],
                      n ? n->out_h : oh - p.out_base[1] },
        .out = { 0, ow, oh },
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
    for (int pass = 0;; pass++) {
        if (pass == 2)
            return nvn_vk_stop(r,
                               "nvnCommandBufferEvaluateDlss: %ux%u at %u,%u of a %ux%u input to %ux%u at %u,%u of a "
                               "%ux%u output is not a size the upscaler takes",
                               ui.in.width, ui.in.height, ui.in.x, ui.in.y, iw, ih, ui.out_rect.width,
                               ui.out_rect.height, ui.out_rect.x, ui.out_rect.y, ow, oh);
        if ((pass || st.nheld + 2 > MAX_HELD) && !nvn_vk_flush(r))
            return nvn_vk_stop(r, "the host device failed a submit");
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
        if (!nvn_vk_begin(r))
            return nvn_vk_stop(r, "the host device could not begin a command buffer");
        nvn_vk_barrier(g.cb);
        if (rl_upscaler_record(u, (uint64_t)(uintptr_t)g.cb, &ui))
            break;
    }
    nvn_vk_barrier(g.cb);
    nvn_vk_written(out);
    if (!st.evaluations++)
        trace("nvnCommandBufferEvaluateDlss: %ux%u -> %ux%u with %s", ui.in.width, ui.in.height, ui.out_rect.width,
              ui.out_rect.height, rl_upscale_name(choice));
    return RL_OK;
}
