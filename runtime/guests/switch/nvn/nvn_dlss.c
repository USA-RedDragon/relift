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
CALLBACK(12)
CALLBACK(15)
CALLBACK(16)
CALLBACK(17)
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

/** @brief The network the library is working on (the driver's cb+0x28: set around CreateNetwork), 0 outside one. */
static uint64_t g_current;

/**
 * @brief Callback 0(cb, obj): the device, as the console driver gives it (sdk 0x65a000): +0x70 of obj when obj is
 * given, else +8 of the current network (the device its builder named), else 0.
 */
static uint64_t callback_0(rl_ctx *c, uint8_t *mem)
{
    uint64_t obj = c->x[1];
    c->x[0] = obj ? rl_ld64(mem, obj + 0x70) : g_current ? rl_ld64(mem, g_current + 8) : 0;
    return nvn_ret(c);
}

/** @brief A resource the library made through callback 3 (buffer) or 4 (texture). */
typedef struct dlss_res {
    uint64_t obj;
    bool texture;
} dlss_res;
#define MAX_RES 512u
static dlss_res g_res[MAX_RES];
static uint32_t g_nres;

/** @brief Calls proc name with up to 8 arguments; RL_OK or a stop, *ret gets x0. */
static int proc(rl_ctx *c, uint8_t *mem, const char *name, uint64_t *ret, unsigned n, ...)
{
    uint64_t a[8] = { 0 };
    va_list ap;
    va_start(ap, n);
    for (unsigned i = 0; i < n && i < 8; i++)
        a[i] = va_arg(ap, uint64_t);
    va_end(ap);
    uint64_t r = 0;
    int rc = nvn_proc_call(c, mem, name, a, n, &r);
    if (ret)
        *ret = r;
    return rc;
}

/**
 * @brief Takes n bytes aligned to align from the network's storage, as the driver does (its allocator at +0xa8 of the
 * network: the cursor at +0xc8, the bytes left at +0xd0); 0 when they do not fit.
 */
static uint64_t take_from(uint8_t *mem, uint64_t alloc, uint64_t n, uint64_t align)
{
    uint64_t cur = rl_ld64(mem, alloc + 0x20), left = rl_ld64(mem, alloc + 0x28);
    uint64_t at = (cur + align - 1) & ~(align - 1), need = at - cur + n;
    if (need > left)
        return 0;
    rl_st64(mem, alloc + 0x20, cur + need);
    rl_st64(mem, alloc + 0x28, left - need);
    return at;
}

static uint64_t take(uint8_t *mem, uint64_t net, uint64_t n, uint64_t align) { return take_from(mem, net + 0xa8, n, align); }

/** @brief Bytes of the device-wide DLSS arena the driver allocates at a DLSS device's initialize (sdk 0x6550b0). */
#define DEVICE_ARENA 0xb00000u

/**
 * @brief The allocator of the device-wide arena (its state at +DEVICE_ARENA of the arena, laid out as a network's
 * at +0xa8: device at +0x18, cursor +0x20, bytes left +0x28; sdk 0x658950), made at first use in guest memory relift
 * reserves; 0 when it cannot be made.
 */
static uint64_t device_arena(uint8_t *mem, uint64_t device)
{
    static uint64_t arena;
    if (arena)
        return arena + DEVICE_ARENA;
    uint64_t at = 0, bytes = DEVICE_ARENA + 0x1000;
    if (rl_hos_reserve(bytes, 0x10000, &at) != KR_SUCCESS || rl_hos_reserved_set(at, bytes, RL_VM_RW, RL_VM_FRESH) != 0)
        return 0;
    uint64_t st = at + DEVICE_ARENA;
    memset(mem + st, 0, 0x60);
    rl_st64(mem, st + 0x18, device);
    rl_st64(mem, st + 0x20, at);
    rl_st64(mem, st + 0x28, DEVICE_ARENA);
    rl_st64(mem, st + 0x30, DEVICE_ARENA);
    arena = at;
    return st;
}

/**
 * @brief The memory pool over [storage, storage + size) the driver makes for each resource (sdk 0x65a950): flags 0xa1
 * for kind 1 (0xa2 when the device has its +0x1d70 bit 4), 0xa2 for kind 2, 0xa4 for kind 3. The driver also sets a
 * private bit (0x800000 at +0x10) of the pool it made, which relift's pool does not have.
 */
static int make_pool(rl_ctx *c, uint8_t *mem, uint64_t pool, uint64_t device, uint32_t kind, uint64_t storage,
                     uint64_t size)
{
    uint32_t flags = kind == 3 ? 0xa4u : kind == 2 ? 0xa2u : (mem[device + 0x1d70] & 0x10) ? 0xa2u : 0xa1u;
    if (kind < 1 || kind > 3)
        return rl_guest_fatal(c, "nvn", "the console DLSS library asks for a memory pool of kind %u", kind);
    uint64_t sp = c->sp, b = (sp - 0x100) & ~15ull;
    c->sp = b;
    memset(mem + b, 0, 0x100);
    int rc = proc(c, mem, "nvnMemoryPoolBuilderSetDefaults", NULL, 1, b);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnMemoryPoolBuilderSetDevice", NULL, 2, b, device);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnMemoryPoolBuilderSetStorage", NULL, 3, b, storage, size);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnMemoryPoolBuilderSetFlags", NULL, 2, b, (uint64_t)flags);
    uint64_t ok = 0;
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnMemoryPoolInitialize", &ok, 2, pool, b);
    c->sp = sp;
    if (rc == RL_OK && !(ok & 1))
        return rl_guest_fatal(c, "nvn", "a memory pool for the console DLSS library did not initialize");
    return rc;
}

/** @brief Notes a resource; false when there are too many. */
static bool note_res(uint64_t obj, bool texture)
{
    if (g_nres == MAX_RES)
        return false;
    g_res[g_nres++] = (dlss_res){ obj, texture };
    return true;
}

/**
 * @brief Callback 4(cb, desc, &out): a texture, made as the console driver makes it (sdk 0x65a100, 0x658520): a
 * 0x1c8-byte object from the network's storage (the memory pool at +0x30, the texture at +0x130, the storage size at
 * +0x1c0), a builder with the format desc+0xc names, the size desc+0 and desc+4, levels desc+0x10 and flags 0x208,
 * its storage from the network's, in a pool of kind desc+0x18. True on success.
 */
static uint64_t callback_4(rl_ctx *c, uint8_t *mem)
{
    uint64_t desc = c->x[1], out = c->x[2], net = g_current;
    if (!net)
        return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS library makes a texture outside a network");
    static const struct { uint32_t code, format; } fmts[] = { { 1, 0x2e }, { 2, 0x29 }, { 5, 0x11 }, { 6, 5 },
                                                             { 0xe, 0x3f }, { 0xf, 0x25 }, { 0x13, 0x3d } };
    uint32_t code = rl_ld32(mem, desc + 0xc), format = 0;
    for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++)
        if (fmts[i].code == code)
            format = fmts[i].format;
    if (!format)
        return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS library makes a texture of format code %u", code);
    uint64_t device = rl_ld64(mem, net + 0xc0), obj = take(mem, net, 0x1c8, 8);
    if (!obj)
        return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS network's storage is full (texture object)");
    memset(mem + obj, 0, 0x1c8);
    uint64_t sp = c->sp, b = (sp - 0x100) & ~15ull, size = 0;
    c->sp = b;
    memset(mem + b, 0, 0x100);
    int rc = proc(c, mem, "nvnTextureBuilderSetDefaults", NULL, 1, b);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnTextureBuilderSetDevice", NULL, 2, b, device);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnTextureBuilderSetFormat", NULL, 2, b, (uint64_t)format);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnTextureBuilderSetSize2D", NULL, 3, b, (uint64_t)rl_ld32(mem, desc),
                  (uint64_t)rl_ld32(mem, desc + 4));
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnTextureBuilderSetLevels", NULL, 2, b, (uint64_t)rl_ld32(mem, desc + 0x10));
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnTextureBuilderSetFlags", NULL, 2, b, (uint64_t)0x208);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnTextureBuilderGetStorageSize", &size, 1, b);
    uint64_t bytes = (size + 0xfff) & ~0xfffull, storage = rc == RL_OK ? take(mem, net, bytes, 0x1000) : 0;
    if (rc == RL_OK && !storage)
        rc = rl_guest_fatal(c, "nvn", "the console DLSS network's storage is full (0x%llx texture bytes)",
                            (unsigned long long)bytes);
    if (rc == RL_OK)
        rc = make_pool(c, mem, obj + 0x30, device, rl_ld32(mem, desc + 0x18), storage, bytes);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnTextureBuilderSetStorage", NULL, 3, b, obj + 0x30, (uint64_t)0);
    uint64_t ok = 0;
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnTextureInitialize", &ok, 2, obj + 0x130, b);
    c->sp = sp;
    if (rc != RL_OK)
        return (uint64_t)rc;
    rl_st64(mem, obj + 0x1c0, size);
    rl_st64(mem, obj + 0x1b0, net + 0xa8);
    if (!(ok & 1) || !note_res(obj, true))
        return (uint64_t)rl_guest_fatal(c, "nvn", "a texture for the console DLSS library did not initialize");
    rl_st64(mem, out, obj);
    c->x[0] = 1;
    return nvn_ret(c);
}

/**
 * @brief Callback 3(cb, desc, &out): a buffer, made as the console driver makes it (sdk 0x65a070, 0x6587a0): a
 * 0x168-byte object from the network's storage (the device-wide arena's when desc+0x14 has bit 1) (the memory pool at +0x30, the buffer at +0x130), desc+0 bytes rounded
 * up to 4 KiB in a pool of kind desc+0x18. True on success.
 */
static uint64_t callback_3(rl_ctx *c, uint8_t *mem)
{
    uint64_t desc = c->x[1], out = c->x[2], net = g_current;
    if (!net)
        return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS library makes a buffer outside a network");
    uint64_t device = rl_ld64(mem, net + 0xc0), alloc = net + 0xa8;
    if (mem[desc + 0x14] & 2) {
        alloc = device_arena(mem, device);
        if (!alloc)
            return (uint64_t)rl_guest_fatal(c, "nvn", "no guest memory for the DLSS device arena");
    }
    uint64_t obj = take_from(mem, alloc, 0x168, 8);
    uint64_t bytes = ((uint64_t)rl_ld32(mem, desc) + 0xfff) & ~0xfffull;
    uint64_t storage = obj ? take_from(mem, alloc, bytes, 0x1000) : 0;
    if (!storage)
        return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS network's storage is full (0x%llx buffer bytes)",
                                        (unsigned long long)bytes);
    memset(mem + obj, 0, 0x168);
    int rc = make_pool(c, mem, obj + 0x30, device, rl_ld32(mem, desc + 0x18), storage, bytes);
    uint64_t sp = c->sp, b = (sp - 0x100) & ~15ull, ok = 0;
    c->sp = b;
    memset(mem + b, 0, 0x100);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnBufferBuilderSetDefaults", NULL, 1, b);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnBufferBuilderSetDevice", NULL, 2, b, device);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnBufferBuilderSetStorage", NULL, 4, b, obj + 0x30, (uint64_t)0, bytes);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnBufferInitialize", &ok, 2, obj + 0x130, b);
    c->sp = sp;
    if (rc != RL_OK)
        return (uint64_t)rc;
    rl_st64(mem, obj + 0x160, alloc);
    if (!(ok & 1) || !note_res(obj, false))
        return (uint64_t)rl_guest_fatal(c, "nvn", "a buffer for the console DLSS library did not initialize");
    rl_st64(mem, out, obj);
    c->x[0] = 1;
    return nvn_ret(c);
}

/** @brief Callback 5(cb, obj): releases a resource of callbacks 3 and 4 (the driver calls its destructor). */
static uint64_t callback_5(rl_ctx *c, uint8_t *mem)
{
    uint64_t obj = c->x[1];
    for (uint32_t i = 0; i < g_nres; i++)
        if (g_res[i].obj == obj) {
            int rc = proc(c, mem, g_res[i].texture ? "nvnTextureFinalize" : "nvnBufferFinalize", NULL, 1, obj + 0x130);
            if (rc == RL_OK)
                rc = proc(c, mem, "nvnMemoryPoolFinalize", NULL, 1, obj + 0x30);
            if (rc != RL_OK)
                return (uint64_t)rc;
            g_res[i] = g_res[--g_nres];
            c->x[0] = 1;
            return nvn_ret(c);
        }
    return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS library releases 0x%llx, which relift never made",
                                    (unsigned long long)obj);
}

/** @brief Callback 14(cb, obj, name): the resource's debug name, which the driver copies to +9 of its object (31 bytes, sdk 0x65a3c0). */
static uint64_t callback_14(rl_ctx *c, uint8_t *mem)
{
    uint64_t obj = c->x[1];
    const char *n = c->x[2] ? nvn_str(mem, c->x[2], 64) : "";
    size_t len = n ? strnlen(n, 31) : 0;
    memset(mem + obj + 9, 0, 32);
    if (len)
        memcpy(mem + obj + 9, n, len);
    return nvn_ret(c);
}

/** @brief The resource of callbacks 3 and 4 at obj, or NULL. */
static const dlss_res *res_of(uint64_t obj)
{
    for (uint32_t i = 0; i < g_nres; i++)
        if (g_res[i].obj == obj)
            return &g_res[i];
    return NULL;
}

/**
 * @brief Callback 7(cb, obj, &addr): a buffer's GPU address (its object's method +0x18 gives the NVNbuffer at +0x130,
 * then nvnBufferGetAddress; sdk 0x65a1c0); 0 for a texture or no object. True.
 */
static uint64_t callback_7(rl_ctx *c, uint8_t *mem)
{
    uint64_t obj = c->x[1], out = c->x[2], addr = 0;
    const dlss_res *r = obj ? res_of(obj) : NULL;
    if (obj && !r)
        return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS library asks the address of 0x%llx, which relift never made",
                                        (unsigned long long)obj);
    if (r && !r->texture) {
        int rc = proc(c, mem, "nvnBufferGetAddress", &addr, 1, obj + 0x130);
        if (rc != RL_OK)
            return (uint64_t)rc;
    }
    rl_st64(mem, out, addr);
    c->x[0] = 1;
    return nvn_ret(c);
}

/**
 * @brief Callback 8(cb, obj, &addr, &size): a texture's GPU address and storage size (method +0x30, sdk 0x65ad80:
 * nvnTextureGetTextureAddress and +0x1c0); false for a buffer, which writes nothing.
 */
static uint64_t callback_8(rl_ctx *c, uint8_t *mem)
{
    uint64_t obj = c->x[1];
    const dlss_res *r = res_of(obj);
    if (!r)
        return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS library asks the storage of 0x%llx, which relift never made",
                                        (unsigned long long)obj);
    c->x[0] = 0;
    if (r->texture) {
        uint64_t addr = 0;
        int rc = proc(c, mem, "nvnTextureGetTextureAddress", &addr, 1, obj + 0x130);
        if (rc != RL_OK)
            return (uint64_t)rc;
        rl_st64(mem, c->x[2], addr);
        rl_st64(mem, c->x[3], rl_ld64(mem, obj + 0x1c0));
        c->x[0] = 1;
    }
    return nvn_ret(c);
}

/**
 * @brief Callback 10(cb, cmd, size, src, staging, dst, staging_off, dst_off): uploads size bytes from guest memory
 * src into buffer dst at dst_off, as the console driver does (sdk 0x65a260): without a command buffer straight into
 * dst's mapping; with one, into the staging buffer's mapping at staging_off and then a copy command into dst. True.
 */
static uint64_t callback_10(rl_ctx *c, uint8_t *mem)
{
    uint64_t cmd = c->x[1], size = c->x[2], src = c->x[3], staging = c->x[4], dst = c->x[5];
    uint64_t soff = c->x[6], doff = c->x[7];
    const dlss_res *d = res_of(dst), *s = cmd ? res_of(staging) : NULL;
    if (!d || d->texture || (cmd && (!s || s->texture)))
        return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS library uploads to 0x%llx through 0x%llx, which are not relift's buffers",
                                        (unsigned long long)dst, (unsigned long long)staging);
    uint64_t into = cmd ? staging + 0x130 : dst + 0x130, off = cmd ? soff : doff, map = 0;
    int rc = proc(c, mem, "nvnBufferMap", &map, 1, into);
    if (rc != RL_OK)
        return (uint64_t)rc;
    c->x[0] = 0;
    if (!map)
        return nvn_ret(c);
    memmove(mem + map + off, mem + src, size);
    rc = proc(c, mem, "nvnBufferFlushMappedRange", NULL, 3, into, off, size);
    if (rc == RL_OK && cmd) {
        uint64_t from = 0, to = 0;
        rc = proc(c, mem, "nvnBufferGetAddress", &from, 1, staging + 0x130);
        if (rc == RL_OK)
            rc = proc(c, mem, "nvnBufferGetAddress", &to, 1, dst + 0x130);
        if (rc == RL_OK)
            rc = proc(c, mem, "nvnCommandBufferCopyBufferToBuffer", NULL, 5, cmd, from + soff, to + doff, size,
                      (uint64_t)0);
    }
    if (rc != RL_OK)
        return (uint64_t)rc;
    c->x[0] = 1;
    return nvn_ret(c);
}

/** @brief Callback 2(cb, cmd): a barrier in the command buffer, the one the console driver records (sdk 0x65a040: bits 0x80000031). True. */
static uint64_t callback_2(rl_ctx *c, uint8_t *mem)
{
    int rc = proc(c, mem, "nvnCommandBufferBarrier", NULL, 2, c->x[1], (uint64_t)0x80000031u);
    if (rc != RL_OK)
        return (uint64_t)rc;
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

static uint64_t callback_6(rl_ctx *c, uint8_t *mem);
static uint64_t callback_9(rl_ctx *c, uint8_t *mem);
static uint64_t callback_11(rl_ctx *c, uint8_t *mem);
static uint64_t callback_19(rl_ctx *c, uint8_t *mem);

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
 * @brief The network's state as the driver's nvnDlssNetworkInitialize sets it (sdk 0x658b20) for the callbacks: the
 * 0x258 bytes cleared; the device at +8 and +0xc0, the builder's +8 at +0x10, its 0x80 bytes at +0x38 (texture ids and
 * pool) at +0x18, 0x3f at +0x98, the device's +0x1dd0 at +0xb8; the builder's storage (+0x10, +0x18 bytes) cleared, its
 * allocator at +0xc8 (cursor) and +0xd0 (bytes left); a memory pool at +0x100 over the first builder+0x34 * 64 KiB of
 * it (flags 0x21). A builder with native handles (+0xb8) is not done here and stops.
 */
static int network_state(rl_ctx *c, uint8_t *mem, uint64_t net, uint64_t builder)
{
    uint64_t device = rl_ld64(mem, builder), storage = rl_ld64(mem, builder + 0x10), size = rl_ld64(mem, builder + 0x18);
    if (rl_ld64(mem, builder + 0xb8))
        return rl_guest_fatal(c, "nvn", "the game's DLSS network builder gives native memory handles (+0xb8)");
    memset(mem + net, 0, 0x258);
    rl_st64(mem, net + 8, device);
    rl_st64(mem, net + 0x10, rl_ld64(mem, builder + 8));
    memcpy(mem + net + 0x18, mem + builder + 0x38, 0x80);
    rl_st32(mem, net + 0x98, 0x3f);
    rl_st32(mem, net + 0xb8, rl_ld32(mem, device + 0x1dd0));
    rl_st64(mem, net + 0xc0, device);
    rl_st64(mem, net + 0xc8, storage);
    rl_st64(mem, net + 0xd0, size);
    rl_st64(mem, net + 0xd8, size);
    memset(mem + storage, 0, size);
    int32_t units = (int32_t)rl_ld32(mem, builder + 0x34);
    rl_st32(mem, net + 0x200, (uint32_t)units);
    uint64_t bytes = (uint64_t)(int64_t)units << 16, at = units > 0 ? take(mem, net, bytes, 0x1000) : 0;
    if (units <= 0)
        return RL_OK;
    if (!at)
        return rl_guest_fatal(c, "nvn", "the game's DLSS network storage is too small for its pool");
    uint64_t sp = c->sp, b = (sp - 0x100) & ~15ull, ok = 0;
    c->sp = b;
    memset(mem + b, 0, 0x100);
    int rc = proc(c, mem, "nvnMemoryPoolBuilderSetDefaults", NULL, 1, b);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnMemoryPoolBuilderSetDevice", NULL, 2, b, device);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnMemoryPoolBuilderSetStorage", NULL, 3, b, at, bytes);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnMemoryPoolBuilderSetFlags", NULL, 2, b, (uint64_t)0x21);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnMemoryPoolInitialize", &ok, 2, net + 0x100, b);
    c->sp = sp;
    if (rc == RL_OK && !(ok & 1))
        return rl_guest_fatal(c, "nvn", "the game's DLSS network pool did not initialize");
    return rc;
}

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
        rc = network_state(c, mem, net, builder);
        if (rc != RL_OK)
            return (uint64_t)rc;
        network_desc(mem, net, builder, opt);
        uint64_t desc = net + 0x210;
        g_current = net;
        rc = library_call(c, mem, LIB_CREATE_NETWORK, &desc, 1, &lib_net);
        g_current = 0;
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

/** @brief The vtable word of relift's texture wrappers for the library (callbacks 6 and 9 know them by it). */
#define WRAP_TAG 0x7772617000000000ull

/** @brief Library vtable slot of Evaluate(lib, struct). */
enum { LIB_EVALUATE = 7 };

bool nvn_dlss_game(void) { return g_library && rl_upscale_resolve(rl_setting_enum("picture.upscaler"), true, false) == RL_UPSCALE_GAME; }

/**
 * @brief Writes the 0x40-byte texture wrapper the driver gives the library for one input or output (vtable at +0, 1 at
 * +8, id at +0x2c, the texture and view at +0x30 and +0x38) at w, and registers the texture under id in the network's
 * texture pool (sdk 0x6594c0; the driver writes the descriptor through the command buffer, relift's pool registers it).
 */
static int wrap(rl_ctx *c, uint8_t *mem, uint64_t w, uint64_t pool, uint32_t id, uint64_t tex, uint64_t view)
{
    memset(mem + w, 0, 0x40);
    rl_st64(mem, w, WRAP_TAG);
    mem[w + 8] = 1;
    rl_st32(mem, w + 0x2c, id);
    rl_st64(mem, w + 0x30, tex);
    rl_st64(mem, w + 0x38, view);
    if (!tex)
        return RL_OK;
    return proc(c, mem, "nvnTexturePoolRegisterTexture", NULL, 4, pool, (uint64_t)id, tex, view);
}

uint64_t nvn_dlss_evaluate(rl_ctx *c, uint8_t *mem)
{
    uint64_t cmd = c->x[0], net = c->x[1], prm = c->x[2];
    uint64_t lib_net = rl_ld64(mem, net + 0xa0), pool = rl_ld64(mem, net + 0x10);
    if (!lib_net || !prm)
        return (uint64_t)rl_guest_fatal(c, "nvn",
                                        "nvnCommandBufferEvaluateDlss(0x%llx) on a network the game's DLSS did not create",
                                        (unsigned long long)net);
    uint64_t sp = c->sp, base = (sp - 0x300) & ~15ull, S = base, W0 = base + 0x100;
    c->sp = base;
    memset(mem + S, 0, 0xc0);
    /* params byte offset, network id offset, struct offset: color, output, motion, depth, bias, exposure */
    static const uint32_t tex[6][3] = { { 0x00, 0x18, 0x00 }, { 0x30, 0x1c, 0x08 }, { 0x20, 0x20, 0x10 },
                                        { 0x40, 0x24, 0x18 }, { 0x10, 0x28, 0x20 }, { 0x68, 0x2c, 0x28 } };
    uint32_t flags = rl_ld32(mem, net + 0x250);
    int rc = RL_OK;
    for (int i = 0; i < 6 && rc == RL_OK; i++) {
        uint64_t t = rl_ld64(mem, prm + tex[i][0]), v = rl_ld64(mem, prm + tex[i][0] + 8), w = W0 + 0x40u * (uint64_t)i;
        rc = wrap(c, mem, w, pool, rl_ld32(mem, net + tex[i][1]), t, v);
        bool give = i < 3 || i == 4 || (i == 3 && t) || (i == 5 && !(flags & 0x41) && t);
        if (give)
            rl_st64(mem, S + tex[i][2], w);
    }
    if (rc == RL_OK) {
        rl_st64(mem, S + 0x30, cmd);
        rl_st64(mem, S + 0x38, lib_net);
        memcpy(mem + S + 0x40, mem + prm + 0x50, 8);
        rl_st32(mem, S + 0x48, mem[prm + 0x78]);
        memcpy(mem + S + 0x4c, mem + prm + 0x58, 8);
        if (!(flags & 0x41))
            memcpy(mem + S + 0x54, mem + prm + 0x60, 4);
        uint32_t w = rl_ld32(mem, prm + 0x80), h = rl_ld32(mem, prm + 0x84);
        if (!(w | h)) {
            uint64_t ww = 0, hh = 0, color = rl_ld64(mem, prm);
            rc = proc(c, mem, "nvnTextureGetWidth", &ww, 1, color);
            if (rc == RL_OK)
                rc = proc(c, mem, "nvnTextureGetHeight", &hh, 1, color);
            w = (uint32_t)ww, h = (uint32_t)hh;
        }
        rl_st32(mem, S + 0x58, w);
        rl_st32(mem, S + 0x5c, h);
        memcpy(mem + S + 0x60, mem + prm + 0x88, 16);
        memcpy(mem + S + 0x70, mem + prm + 0x98, 16);
        if (flags & (1u << 28))
            memcpy(mem + S + 0x80, mem + prm + 0xa8, 8);
        rl_st32(mem, S + 0x88, rl_ld32(mem, rl_ld64(mem, net + 8)) == 0);
        memcpy(mem + S + 0x8c, mem + prm + 0xb0, 16);
        memcpy(mem + S + 0x9c, mem + net + 0x210, 32);
    }
    if (rc == RL_OK) {
        g_current = net;
        rc = library_call(c, mem, LIB_EVALUATE, &S, 1, NULL);
        g_current = 0;
    }
    c->sp = sp;
    if (rc != RL_OK)
        return (uint64_t)rc;
    return nvn_ret(c);
}

/**
 * @brief Callback 6(cb, obj, is_image, filtered, &handle): the texture or image handle of a texture wrapper of an
 * evaluate, as its driver method does (vtable 0xe76700 +0x20, sdk 0x65b0b0): an image handle of the wrapper's id, or a
 * texture handle of it with sampler 2 when filtered, else 0. True.
 */
static uint64_t callback_6(rl_ctx *c, uint8_t *mem)
{
    uint64_t obj = c->x[1], out = c->x[4], h = 0, device = g_current ? rl_ld64(mem, g_current + 8) : 0;
    if (!obj) {
        rl_st64(mem, out, 0);
        c->x[0] = 1;
        return nvn_ret(c);
    }
    const dlss_res *r = res_of(obj);
    uint64_t id;
    if (r && r->texture) {
        /* A texture of callback 4 takes the next free one of the game's texture ids in the network's pool once per
         * network (sdk 0x65ab60, 0x65b3a0: the bitmap at +0x98, the ids at +0x18). */
        uint64_t net = g_current;
        if (rl_ld64(mem, obj + 0x1b8) != net || !rl_ld32(mem, obj + 0x2c)) {
            uint32_t used = rl_ld32(mem, net + 0x98), bit = 0;
            while (bit < 32 && (used & (1u << bit)))
                bit++;
            if (bit == 32)
                return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS network has no free texture id left");
            rl_st32(mem, net + 0x98, used | 1u << bit);
            rl_st32(mem, obj + 0x2c, rl_ld32(mem, net + 0x18 + 4u * bit));
            rl_st64(mem, obj + 0x1b8, net);
            int rc = proc(c, mem, "nvnTexturePoolRegisterTexture", NULL, 4, rl_ld64(mem, net + 0x10),
                          (uint64_t)rl_ld32(mem, obj + 0x2c), obj + 0x130, (uint64_t)0);
            if (rc != RL_OK)
                return (uint64_t)rc;
        }
        id = rl_ld32(mem, obj + 0x2c);
    } else if (rl_ld64(mem, obj) == WRAP_TAG) {
        id = rl_ld32(mem, obj + 0x2c);
    } else {
        return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS library asks a handle of 0x%llx, which relift never made (callback 6)",
                                        (unsigned long long)obj);
    }
    int rc = c->x[2] & 1 ? proc(c, mem, "nvnDeviceGetImageHandle", &h, 2, device, id)
                         : proc(c, mem, "nvnDeviceGetTextureHandle", &h, 3, device, id, (uint64_t)((uint32_t)c->x[3] ? 2 : 0));
    if (rc != RL_OK)
        return (uint64_t)rc;
    rl_st64(mem, out, h);
    c->x[0] = 1;
    return nvn_ret(c);
}

/**
 * @brief Callback 9(cb, obj, out): describes a texture for the library, as the driver's method does (vtables
 * 0xe76700 and 0xe76610 +0x28, sdk 0x65ac50): width at +0, height at +4, the NVN format (the view's when it sets one)
 * at +8, the library's format code at +0xc, levels at +0x10, bits 0 and 2 set at +0x30, 1 at +0x34. obj is an
 * evaluate's texture wrapper or a texture of callback 4. True.
 */
static uint64_t callback_9(rl_ctx *c, uint8_t *mem)
{
    uint64_t obj = c->x[1], out = c->x[2], tex = 0, view = 0;
    const dlss_res *r = res_of(obj);
    if (r && r->texture)
        tex = obj + 0x130;
    else if (rl_ld64(mem, obj) == WRAP_TAG)
        tex = rl_ld64(mem, obj + 0x30), view = rl_ld64(mem, obj + 0x38);
    if (!tex)
        return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS library describes 0x%llx, which is no texture (callback 9)",
                                        (unsigned long long)obj);
    uint64_t w = 0, h = 0, fmt = 0, levels = 0, has = 0;
    int rc = proc(c, mem, "nvnTextureGetWidth", &w, 1, tex);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnTextureGetHeight", &h, 1, tex);
    if (rc == RL_OK)
        rc = proc(c, mem, "nvnTextureGetLevels", &levels, 1, tex);
    uint64_t sp = c->sp, tmp = (sp - 16) & ~15ull;
    c->sp = tmp;
    rl_st32(mem, tmp, 0);
    if (rc == RL_OK && view)
        rc = proc(c, mem, "nvnTextureViewGetFormat", &has, 2, view, tmp);
    c->sp = sp;
    if (rc == RL_OK && view && (has & 1))
        fmt = rl_ld32(mem, tmp);
    else if (rc == RL_OK)
        rc = proc(c, mem, "nvnTextureGetFormat", &fmt, 1, tex);
    if (rc != RL_OK)
        return (uint64_t)rc;
    static const uint32_t codes[][2] = { { 0x2e, 1 }, { 0x29, 2 }, { 0x11, 5 }, { 5, 6 },
                                         { 0x3f, 0xe }, { 0x25, 0xf }, { 0x3d, 0x13 } };
    uint32_t code = 0;
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++)
        if (codes[i][0] == (uint32_t)fmt)
            code = codes[i][1];
    rl_st32(mem, out + 0x30, rl_ld32(mem, out + 0x30) | 5u);
    rl_st32(mem, out, (uint32_t)w);
    rl_st32(mem, out + 4, (uint32_t)h & 0xffffu);
    rl_st32(mem, out + 8, (uint32_t)fmt);
    rl_st32(mem, out + 0xc, code);
    rl_st32(mem, out + 0x10, (uint32_t)(int8_t)levels);
    mem[out + 0x34] = 1;
    c->x[0] = 1;
    return nvn_ret(c);
}

/**
 * @brief Callback 11(cb, cmd, obj, color, rects, count, skip): clears a texture of callback 4 or an evaluate's texture
 * wrapper (with its view) in cmd, as their driver method does (vtables 0xe76610 and 0xe76700 +0x38, sdk 0x65ade0): each of count rectangles ({x0, y0, x1, y1}, 16 bytes) or, with
 * none, the whole texture, with the four floats at color and all four channels. False for a buffer, or when skip has
 * bit 0 (the driver then clears nothing).
 */
static uint64_t callback_11(rl_ctx *c, uint8_t *mem)
{
    uint64_t cmd = c->x[1], obj = c->x[2], color = c->x[3], rects = c->x[4], n = (uint32_t)c->x[5];
    const dlss_res *r = res_of(obj);
    uint64_t tex = 0, view = 0;
    if (r && r->texture)
        tex = obj + 0x130;
    else if (!r && rl_ld64(mem, obj) == WRAP_TAG)
        tex = rl_ld64(mem, obj + 0x30), view = rl_ld64(mem, obj + 0x38);
    else if (!r)
        return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS library clears 0x%llx, which relift never made",
                                        (unsigned long long)obj);
    c->x[0] = 0;
    if (!tex || (c->x[6] & 1))
        return nvn_ret(c);
    uint64_t sp = c->sp, reg = (sp - 32) & ~15ull, w = 0, h = 0;
    c->sp = reg;
    int rc = RL_OK;
    for (uint64_t i = 0; i < (n ? n : 1) && rc == RL_OK; i++) {
        uint32_t x = 0, y = 0;
        if (n) {
            uint64_t q = rects + 16 * i;
            x = rl_ld32(mem, q), y = rl_ld32(mem, q + 4);
            w = rl_ld32(mem, q + 8) - x, h = rl_ld32(mem, q + 12) - y;
        } else {
            rc = proc(c, mem, "nvnTextureGetWidth", &w, 1, tex);
            if (rc == RL_OK)
                rc = proc(c, mem, "nvnTextureGetHeight", &h, 1, tex);
            h &= 0xffffu;
        }
        rl_st32(mem, reg, x);
        rl_st32(mem, reg + 4, y);
        rl_st32(mem, reg + 8, 0);
        rl_st32(mem, reg + 12, (uint32_t)w);
        rl_st32(mem, reg + 16, (uint32_t)h);
        rl_st32(mem, reg + 20, 1);
        if (rc == RL_OK)
            rc = proc(c, mem, "nvnCommandBufferClearTexture", NULL, 6, cmd, tex, view, reg, color, (uint64_t)0xf);
    }
    c->sp = sp;
    if (rc != RL_OK)
        return (uint64_t)rc;
    c->x[0] = 1;
    return nvn_ret(c);
}

/**
 * @brief Callback 19(cb, cmd, function, gx, gy, gz, bx, by, [sp] bz, [sp+8] dynamic shared, [sp+16] extra): a CUDA
 * kernel launch into cmd (on the console cuLaunchKernel through the CUDA library, sdk 0x65a710). extra is
 * {1, parameter buffer, 2, &parameter bytes, 0} (CU_LAUNCH_PARAM_BUFFER_POINTER and _SIZE). relift records it with
 * the parameter bytes copied now, and runs it as a compute dispatch of the kernel's translation. True.
 */
static uint64_t callback_19(rl_ctx *c, uint8_t *mem)
{
    uint64_t fn = c->x[2], sp = c->sp, extra = rl_ld64(mem, sp + 16);
    uint32_t i = (uint32_t)(fn & 0xffffffffu), bz = rl_ld32(mem, sp), dyn = rl_ld32(mem, sp + 8);
    if ((fn & ~0xffffffffull) != KERNEL_TAG || i >= g_nkernels)
        return (uint64_t)rl_guest_fatal(c, "nvn", "the console DLSS library launches 0x%llx, which is no kernel it loaded",
                                        (unsigned long long)fn);
    uint64_t params = 0, size = 0;
    for (uint64_t at = extra; at && rl_ld64(mem, at); at += 16) {
        uint64_t tag = rl_ld64(mem, at), val = rl_ld64(mem, at + 8);
        if (tag == 1)
            params = val;
        else if (tag == 2)
            size = rl_ld64(mem, val);
        else
            return (uint64_t)rl_guest_fatal(c, "nvn", "a CUDA launch of the console DLSS library has extra tag 0x%llx",
                                            (unsigned long long)tag);
    }
    uint64_t saved[8];
    memcpy(saved, c->x, sizeof saved);
    uint64_t gx = c->x[3], gy = c->x[4], gz = c->x[5], bx = c->x[6], by = c->x[7];
    c->x[0] = c->x[1];
    c->x[1] = i;
    c->x[2] = (gx & 0xffffffffu) | gy << 32;
    c->x[3] = params;
    c->x[4] = size;
    c->x[5] = (gz & 0xffffffffu) | (uint64_t)dyn << 32;
    c->x[6] = (bx & 0xffffu) | (by & 0xffffu) << 16 | (uint64_t)bz << 32;
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "DLSS launch %s grid %llux%llux%llu block %llux%llux%u shared +%u, %llu parameter bytes",
               g_kernels[i].name, (unsigned long long)gx, (unsigned long long)gy, (unsigned long long)gz,
               (unsigned long long)bx, (unsigned long long)by, bz, dyn, (unsigned long long)size);
    uint64_t ret = c->x[30];
    uint64_t rc = nvn_cmd_record(c, mem, NVN_CMD_CUDA_LAUNCH);
    if (rc != ret)
        return rc;
    memcpy(c->x, saved, sizeof saved);
    c->x[0] = 1;
    return nvn_ret(c);
}

int nvn_dlss_launch(replay *r, const nvn_cmd *e)
{
    uint32_t i = (uint32_t)e->x[1];
    if (i >= g_nkernels)
        return nvn_vk_stop(r, "a recorded CUDA launch names kernel %u, which the library never loaded", i);
    const uint8_t *params = nvn_cmd_bytes(r->s, e->x[3]);
    if (e->x[4] && !params)
        return nvn_vk_stop(r, "a recorded CUDA launch of %s lost its parameters", g_kernels[i].name);
    if (getenv("RELIFT_DLSS_NO_LAUNCH"))
        return nvn_vk_stop(r, "a CUDA launch of %s: RELIFT_DLSS_NO_LAUNCH is set, so the console DLSS kernels do not run",
                           g_kernels[i].name);
    uint32_t grid[3] = { (uint32_t)e->x[2], (uint32_t)(e->x[2] >> 32), (uint32_t)e->x[5] };
    uint32_t block[3] = { (uint32_t)(e->x[6] & 0xffffu), (uint32_t)((e->x[6] >> 16) & 0xffffu), (uint32_t)(e->x[6] >> 32) };
    return nvn_cuda_dispatch(r, g_kernels[i].key, params, (uint32_t)e->x[4], grid, block, (uint32_t)(e->x[5] >> 32));
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
