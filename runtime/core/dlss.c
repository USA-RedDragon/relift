/**
 * @file dlss.c
 * @brief NVIDIA DLSS Super Resolution for the upscaler stage (dlss.h), through NGX. relift ships no NVIDIA file: the
 * NGX core library comes with the NVIDIA driver (libnvidia-ngx.so.1 on Linux, found where the system loader finds
 * the driver's libraries), and the DLSS library (libnvidia-ngx-dlss.so.*) is one the user copies into the folder of
 * the running program. Called directly, the core looks for it there and nowhere else: not in the working folder,
 * not in its models folder; a list of other folders goes only through NVIDIA's static SDK library, which relift
 * does not link. The core exports its Vulkan entry points itself (Init_Ext2 is the one that starts it for SDK
 * version 1.5; Init there has another signature and refuses), so relift calls them directly with the types NVIDIA's
 * public headers (github.com/NVIDIA/DLSS) declare: the C entry points, the Vulkan resource description, the
 * parameter names, and the parameter object's interface (a C++ object whose table of virtual functions holds Set and
 * Get for each value type, then Reset). Windows hosts are not wired yet: the NGX core there is found through the
 * driver's registry entries, which this file does not read.
 */
#include "dlss.h"

#include "core.h"
#include "hostlib.h"
#include "vkmin.h"

#include <relift/settings.h>

#include <dirent.h>
#if defined(_WIN32)
#include <direct.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/** @brief NGX results: success is 1; failures have 0xBAD00000 in their high bits. */
#define NGX_SUCCESS 1u
#define NGX_FAILED(r) (((r) & 0xfff00000u) == 0xbad00000u)
/** @brief The NGX API version the declarations here follow (1.5.0). */
#define NGX_VERSION 0x15u
#define NGX_FEATURE_SUPER_SAMPLING 1
#define NGX_QUALITY_DLAA 5
#define NGX_PRESET_K 11
#define NGX_FLAG_HDR (1 << 0)
#define NGX_FLAG_MV_LOW_RES (1 << 1)
#define NGX_FLAG_MV_JITTERED (1 << 2)
#define NGX_FLAG_DEPTH_INVERTED (1 << 3)
#define NGX_FLAG_AUTO_EXPOSURE (1 << 6)
#define NVIDIA_VENDOR 0x10deu

/** @brief The parameter object: a pointer to its table of virtual functions. */
typedef struct ngx_params {
    void **vtable;
} ngx_params;

/**
 * @brief Slots of the parameter object's virtual functions. The Itanium C++ ABI lays them out in declaration order:
 * Set for unsigned long long, float, double, unsigned int, int, two Direct3D resources and void *, then Get for the
 * same types, then Reset.
 */
enum { SET_F = 1, SET_UI = 3, SET_I = 4, SET_PTR = 7, GET_UI = 11, GET_I = 12 };

/** @brief A feature instance NGX made. */
typedef struct ngx_handle ngx_handle;

/** @brief The Vulkan resource NGX reads or writes: an image view with its image, range, format and size. */
typedef struct ngx_resource {
    uint64_t view, image;
    rlvk_image_subresource_range range;
    int32_t format;
    uint32_t width, height;
    int32_t type;
    bool read_write;
} ngx_resource;

typedef void *(*gipa_fn)(void *, const char *);

/** @brief The NGX core entry points relift calls. */
typedef struct ngx_fns {
    uint32_t (*init)(unsigned long long, const wchar_t *, uint64_t, uint64_t, uint64_t, void *, void *, uint32_t,
                     const ngx_params *);
    uint32_t (*capability)(ngx_params **);
    uint32_t (*allocate)(ngx_params **);
    uint32_t (*destroy)(ngx_params *);
    uint32_t (*create)(uint64_t, uint64_t, int32_t, ngx_params *, ngx_handle **);
    uint32_t (*evaluate)(uint64_t, const ngx_handle *, const ngx_params *, void *);
    uint32_t (*release)(ngx_handle *);
    uint32_t (*required)(uint32_t *, const char ***, uint32_t *, const char ***);
} ngx_fns;

struct rl_dlss {
    void *lib;
    ngx_fns f;
    uint64_t device;
    ngx_params *params;
    ngx_handle *feature;
    /** @brief The feature's render and display sizes and flags; a change makes a new feature. */
    uint32_t made[4];
    int32_t made_flags;
    bool pending, first;
    char data_dir[1024];
    wchar_t *wdata_dir;
};

static void set_ui(ngx_params *p, const char *name, uint32_t v)
{
    ((void (*)(ngx_params *, const char *, unsigned int))p->vtable[SET_UI])(p, name, v);
}

static void set_i(ngx_params *p, const char *name, int32_t v)
{
    ((void (*)(ngx_params *, const char *, int))p->vtable[SET_I])(p, name, v);
}

static void set_f(ngx_params *p, const char *name, float v)
{
    ((void (*)(ngx_params *, const char *, float))p->vtable[SET_F])(p, name, v);
}

static void set_ptr(ngx_params *p, const char *name, void *v)
{
    ((void (*)(ngx_params *, const char *, void *))p->vtable[SET_PTR])(p, name, v);
}

static int32_t get_i(ngx_params *p, const char *name, int32_t fallback)
{
    int v = fallback;
    uint32_t r = ((uint32_t(*)(ngx_params *, const char *, int *))p->vtable[GET_I])(p, name, &v);
    return r == NGX_SUCCESS ? v : fallback;
}

/** @brief Opens the NGX core and reads its entry points into f; NULL (with why set) when it is not there. */
static void *open_core(ngx_fns *f, const char **why)
{
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
    (void)f;
    *why = "this host has no NGX core relift can load";
    return NULL;
#else
    static const char *const core_names[] = { "libnvidia-ngx.so.1", "libnvidia-ngx.so", NULL };
    void *lib = rl_hostlib_open(core_names);
    if (!lib) {
        *why = "the NVIDIA driver's libnvidia-ngx.so.1 is not installed";
        return NULL;
    }
    struct {
        const char *name;
        void **slot;
    } syms[] = {
        { "NVSDK_NGX_VULKAN_Init_Ext2", (void **)&f->init },
        { "NVSDK_NGX_VULKAN_GetCapabilityParameters", (void **)&f->capability },
        { "NVSDK_NGX_VULKAN_AllocateParameters", (void **)&f->allocate },
        { "NVSDK_NGX_VULKAN_DestroyParameters", (void **)&f->destroy },
        { "NVSDK_NGX_VULKAN_CreateFeature1", (void **)&f->create },
        { "NVSDK_NGX_VULKAN_EvaluateFeature", (void **)&f->evaluate },
        { "NVSDK_NGX_VULKAN_ReleaseFeature", (void **)&f->release },
        { "NVSDK_NGX_VULKAN_RequiredExtensions", (void **)&f->required },
    };
    for (size_t i = 0; i < sizeof syms / sizeof syms[0]; i++) {
        void *s = rl_hostlib_sym(lib, syms[i].name);
        if (!s) {
            *why = "the driver's libnvidia-ngx.so.1 lacks an NGX entry point";
            rl_log(RL_LOG_INFO, "upscale", "DLSS: libnvidia-ngx.so.1 has no %s", syms[i].name);
            return NULL;
        }
        memcpy(syms[i].slot, &s, sizeof s);
    }
    return lib;
#endif
}

uint32_t rl_upscale_device_extensions(uint32_t vendor_id, const char *const **names)
{
    *names = NULL;
    uint32_t c = rl_setting_enum("picture.upscaler");
    if (vendor_id != NVIDIA_VENDOR || (c != RL_UPSCALE_AUTO && c != RL_UPSCALE_DLSS))
        return 0;
    static ngx_fns f;
    static void *lib;
    static uint32_t n;
    static const char **dev;
    if (!lib) {
        const char *why = NULL;
        if (!(lib = open_core(&f, &why)))
            return 0;
        uint32_t ni = 0;
        const char **inst = NULL;
        if (f.required(&ni, &inst, &n, &dev) != NGX_SUCCESS)
            n = 0, dev = NULL;
    }
    *names = (const char *const *)dev;
    return n;
}

/** @brief A wide copy of s (malloc), or NULL. */
static wchar_t *widen(const char *s)
{
    size_t n = strlen(s) + 1;
    wchar_t *w = malloc(n * sizeof *w);
    if (w && mbstowcs(w, s, n) == (size_t)-1) {
        free(w);
        return NULL;
    }
    return w;
}

/** @brief Whether folder dir holds a DLSS library; its name goes to found. */
static bool has_dlss(const char *dir, char *found, size_t size)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool yes = false;
    for (struct dirent *e; !yes && (e = readdir(d));)
        if (!strncmp(e->d_name, "libnvidia-ngx-dlss.so.", 22)) {
            snprintf(found, size, "%s/%s", dir, e->d_name);
            yes = true;
        }
    closedir(d);
    return yes;
}

/** @brief The PCI vendor id of device's physical device, or 0. */
static uint32_t vendor_of(const rl_post_device *device)
{
    void *raw = rl_hostlib_vulkan_gipa();
    gipa_fn gipa;
    memcpy(&gipa, &raw, sizeof gipa);
    void *fp = raw ? gipa((void *)(uintptr_t)device->instance, "vkGetPhysicalDeviceProperties") : NULL;
    if (!fp)
        return 0;
    struct {
        uint32_t api, driver, vendor, device;
        uint8_t rest[1024];
    } props;
    void (*get)(uint64_t, void *);
    memcpy(&get, &fp, sizeof get);
    get(device->physical_device, &props);
    return props.vendor;
}

bool rl_dlss_find(const char *dir, char *found, size_t found_size, char *why, size_t why_size)
{
    if (dir && *dir && has_dlss(dir, found, found_size))
        return true;
    snprintf(why, why_size,
             "no libnvidia-ngx-dlss.so.* in %s, the folder of the running program and the one folder NVIDIA's NGX "
             "looks in: copy NVIDIA's DLSS library (from github.com/NVIDIA/DLSS) into it",
             dir && *dir ? dir : "(unknown)");
    return false;
}

static rl_dlss *fail(rl_dlss *d, const char *why)
{
    rl_log(RL_LOG_INFO, "upscale", "DLSS does not run: %s", why);
    rl_dlss_free(d);
    return NULL;
}

rl_dlss *rl_dlss_new(const rl_post_device *device)
{
    if (!device || !device->device)
        return NULL;
    rl_dlss *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    if (vendor_of(device) != NVIDIA_VENDOR)
        return fail(d, "the GPU is not an NVIDIA one");
    const char *why = NULL;
    if (!(d->lib = open_core(&d->f, &why)))
        return fail(d, why);
    char dir[1024], found[1200] = "", msg[1600];
    if (!rl_exe_dir(dir, sizeof dir))
        dir[0] = 0;
    if (!rl_dlss_find(dir, found, sizeof found, msg, sizeof msg))
        return fail(d, msg);
    rl_log(RL_LOG_INFO, "upscale", "DLSS: the NGX core is the driver's libnvidia-ngx.so.1; the DLSS library is %s",
           found);
    char user[1024];
    if (rl_settings_user_dir(user, sizeof user, "relift"))
        snprintf(d->data_dir, sizeof d->data_dir, "%s", user);
    else
        snprintf(d->data_dir, sizeof d->data_dir, "%s", dir);
#if defined(_WIN32)
    _mkdir(d->data_dir);
#else
    mkdir(d->data_dir, 0755);
#endif
    if (!(d->wdata_dir = widen(d->data_dir)))
        return fail(d, "the data folder name is not valid in this locale");
    void *gdpa = NULL;
    memcpy(&gdpa, &device->get_device_proc_addr, sizeof gdpa);
    void *gipa = rl_hostlib_vulkan_gipa();
    uint32_t r = d->f.init(0x52454c4946540001ull, d->wdata_dir, device->instance, device->physical_device,
                           device->device, gipa, gdpa, NGX_VERSION, NULL);
    if (r != NGX_SUCCESS) {
        snprintf(msg, sizeof msg, "NGX did not start on this device (result 0x%08x)", r);
        return fail(d, msg);
    }
    d->device = device->device;
    ngx_params *caps = NULL;
    if (d->f.capability(&caps) != NGX_SUCCESS || !caps)
        return fail(d, "NGX gave no capability parameters");
    int32_t avail = get_i(caps, "SuperSampling.Available", 0);
    int32_t driver = get_i(caps, "SuperSampling.NeedsUpdatedDriver", 0);
    int32_t init = get_i(caps, "SuperSampling.FeatureInitResult", 0);
    d->f.destroy(caps);
    if (driver)
        return fail(d, "the DLSS library needs a newer NVIDIA driver");
    if (!avail) {
        snprintf(msg, sizeof msg, "NGX says DLSS is not available on this GPU (result 0x%08x)", (uint32_t)init);
        return fail(d, msg);
    }
    if (d->f.allocate(&d->params) != NGX_SUCCESS || !d->params)
        return fail(d, "NGX gave no parameter object");
    d->first = true;
    return d;
}

bool rl_dlss_takes(const rl_upscale_inputs *in)
{
    return in->depth.handle && in->depth.image && in->motion.handle && in->motion.image && in->color.image &&
           in->out.image && !in->in.x && !in->in.y && !in->out_rect.x && !in->out_rect.y &&
           in->in.width <= in->out_rect.width && in->in.height <= in->out_rect.height;
}

/** @brief Whether a VkFormat is a depth or depth-stencil one. */
static bool is_depth(int32_t format) { return (format >= 124 && format <= 130); }

/** @brief The NGX resource for picture im (a view of one level and layer of its image). */
static ngx_resource resource(const rl_upscale_image *im, bool write)
{
    ngx_resource r = { .view = im->handle,
                       .image = im->image,
                       .range = { is_depth(im->format) ? RLVK_IMAGE_ASPECT_DEPTH_BIT : RLVK_IMAGE_ASPECT_COLOR_BIT,
                                  im->level, 1, im->layer, 1 },
                       .format = im->format,
                       .width = im->width,
                       .height = im->height,
                       .read_write = write };
    return r;
}

bool rl_dlss_record(rl_dlss *d, uint64_t cmd, const rl_upscale_inputs *in)
{
    if (!d || !cmd || !rl_dlss_takes(in))
        return false;
    int32_t flags = (in->hdr ? NGX_FLAG_HDR : 0) | (in->motion_display_size ? 0 : NGX_FLAG_MV_LOW_RES) |
                    (in->motion_jittered ? NGX_FLAG_MV_JITTERED : 0) |
                    (in->depth_inverted ? NGX_FLAG_DEPTH_INVERTED : 0) |
                    (in->auto_exposure || !in->exposure.handle ? NGX_FLAG_AUTO_EXPOSURE : 0);
    uint32_t want[4] = { in->in.width, in->in.height, in->out_rect.width, in->out_rect.height };
    if (!d->feature || memcmp(want, d->made, sizeof want) || flags != d->made_flags) {
        if (d->feature) {
            if (d->pending)
                return false;
            d->f.release(d->feature);
            d->feature = NULL;
        }
        ngx_params *p = d->params;
        set_ui(p, "CreationNodeMask", 1);
        set_ui(p, "VisibilityNodeMask", 1);
        set_ui(p, "Width", want[0]);
        set_ui(p, "Height", want[1]);
        set_ui(p, "OutWidth", want[2]);
        set_ui(p, "OutHeight", want[3]);
        set_i(p, "PerfQualityValue", NGX_QUALITY_DLAA);
        set_i(p, "DLSS.Feature.Create.Flags", flags);
        set_i(p, "DLSS.Enable.Output.Subrects", 0);
        static const char *const presets[] = { "DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality",
                                               "DLSS.Hint.Render.Preset.Balanced",
                                               "DLSS.Hint.Render.Preset.Performance" };
        for (size_t i = 0; i < sizeof presets / sizeof presets[0]; i++)
            set_ui(p, presets[i], NGX_PRESET_K);
        uint32_t r = d->f.create(d->device, cmd, NGX_FEATURE_SUPER_SAMPLING, p, &d->feature);
        if (r != NGX_SUCCESS || !d->feature) {
            rl_log(RL_LOG_WARN, "upscale", "DLSS: NGX made no feature for %ux%u to %ux%u (result 0x%08x)", want[0],
                   want[1], want[2], want[3], r);
            d->feature = NULL;
            return false;
        }
        memcpy(d->made, want, sizeof want);
        d->made_flags = flags;
        d->first = true;
    }
    ngx_resource color = resource(&in->color, false), out = resource(&in->out, true);
    ngx_resource depth = resource(&in->depth, false), motion = resource(&in->motion, false);
    ngx_resource exposure = resource(&in->exposure, false), bias = resource(&in->reactive, false);
    ngx_params *p = d->params;
    set_ptr(p, "Color", &color);
    set_ptr(p, "Output", &out);
    set_ptr(p, "Depth", &depth);
    set_ptr(p, "MotionVectors", &motion);
    set_ptr(p, "ExposureTexture", in->exposure.handle && in->exposure.image ? &exposure : NULL);
    set_ptr(p, "DLSS.Input.Bias.Current.Color.Mask", in->reactive.handle && in->reactive.image ? &bias : NULL);
    set_f(p, "Jitter.Offset.X", in->jitter[0]);
    set_f(p, "Jitter.Offset.Y", in->jitter[1]);
    set_f(p, "Sharpness", 0.0f);
    set_i(p, "Reset", in->reset || d->first ? 1 : 0);
    set_f(p, "MV.Scale.X", in->motion_scale[0] != 0.0f ? in->motion_scale[0] : 1.0f);
    set_f(p, "MV.Scale.Y", in->motion_scale[1] != 0.0f ? in->motion_scale[1] : 1.0f);
    set_ui(p, "DLSS.Render.Subrect.Dimensions.Width", in->in.width);
    set_ui(p, "DLSS.Render.Subrect.Dimensions.Height", in->in.height);
    set_f(p, "DLSS.Pre.Exposure", in->pre_exposure != 0.0f ? in->pre_exposure : 1.0f);
    set_f(p, "DLSS.Exposure.Scale", 1.0f);
    set_f(p, "FrameTimeDeltaInMsec", 1000.0f / 60.0f);
    uint32_t r = d->f.evaluate(cmd, d->feature, p, NULL);
    if (r != NGX_SUCCESS) {
        rl_log(RL_LOG_WARN, "upscale", "DLSS: the evaluation failed (result 0x%08x)", r);
        return false;
    }
    d->first = false;
    d->pending = true;
    return true;
}

void rl_dlss_retire(rl_dlss *d)
{
    if (d)
        d->pending = false;
}

/**
 * @brief Frees d's feature and parameters but leaves NGX itself running until the process exits: the driver's own
 * Shutdown1 entry point, called directly, takes an argument NVIDIA's public declaration lacks and writes through it,
 * so relift never calls it.
 */
void rl_dlss_free(rl_dlss *d)
{
    if (!d)
        return;
    if (d->feature)
        d->f.release(d->feature);
    if (d->params)
        d->f.destroy(d->params);
    free(d->wdata_dir);
    free(d);
}
