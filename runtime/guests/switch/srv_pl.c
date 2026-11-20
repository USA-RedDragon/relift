/**
 * @file srv_pl.c
 * @brief The shared font service pl:u (nn::pl::detail::IPlatformServiceManager).
 *
 * Command ids and layouts follow switchbrew.org (Shared Database services):
 * the fonts sit in one read-only shared memory of 0x1100000 bytes, each a
 * plain TrueType font at the offset GetSharedMemoryAddressOffset gives.
 * The fonts are the player's own, decoded from their console's system data
 * by relift import into the folder RELIFT_FONTS names (`<type>.ttf`,
 * `<type>` the SharedFontType); a type the player has not imported comes from
 * the open fallback relift build makes (Noto Sans CJK cut to the type's
 * standard character set). The program finds the fallback by itself, as the
 * `fonts` folder of the files it ships (rl_asset_load): beside the executable
 * in a build folder, an iOS bundle or a Windows folder, in `../share/relift`
 * of an AppImage, in `../Resources` of a macOS bundle, in an Android APK's
 * assets. RELIFT_FONTS_FALLBACK, when set, names the folder instead. Each font is placed as the console's pl
 * service places it (inferred from Ryujinx and yuzu, which load the same
 * files): one after another in SharedFontType order, each behind an 8-byte
 * header (the .bfttf magic, and the size big-endian XOR the key), the offset naming the
 * font's first byte. A Switch 2 program gets the same fonts (inferred: no
 * Switch 2 firmware has been read).
 */
#include "hos.h"
#include "services.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_SHMEM_SIZE 0x1100000u
#define PL_TYPES 6u
#define PL_BFTTF_MAGIC 0x18029a7fu
#define PL_BFTTF_KEY 0x06186249u

static const char *const type_names[PL_TYPES] = {
    "standard",           "Chinese Simplified", "Chinese Simplified extension",
    "Chinese Traditional", "Korean",            "Nintendo extension",
};

static struct {
    bool loaded;
    hos_shmem *shm;
    uint32_t offset[PL_TYPES], size[PL_TYPES];
    bool present[PL_TYPES], asked[PL_TYPES];
} g_pl;

/**
 * @brief The bytes of font type t from folder dir, or for the fallback with no folder named (shipped) from the files
 * the program ships (rl_asset_load: `fonts` beside the program or in its package). path names where it looked.
 */
static uint8_t *font_file(const char *dir, bool fallback, uint32_t t, char *path, size_t path_size, size_t *n)
{
    if (dir && *dir) {
        snprintf(path, path_size, "%s/%u.ttf", dir, t);
        FILE *f = fopen(path, "rb");
        if (!f)
            return NULL;
        long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
        rewind(f);
        uint8_t *data = size > 0 ? malloc((size_t)size) : NULL;
        if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
            free(data);
            data = NULL;
        }
        fclose(f);
        *n = data ? (size_t)size : 0;
        return data;
    }
    if (!fallback)
        return NULL;
    snprintf(path, path_size, "fonts/%u.ttf", t);
    return rl_asset_load(path, n);
}

/** @brief Reads the fonts into the shared memory once; false when it cannot be made. */
static bool pl_load(void)
{
    if (g_pl.loaded)
        return g_pl.shm != NULL;
    g_pl.loaded = true;
    g_pl.shm = rl_hos_shmem_create(PL_SHMEM_SIZE, HOS_PERM_R);
    if (!g_pl.shm)
        return false;
    uint8_t *base = rl_hos_shmem_ptr(g_pl.shm);
    const char *own = getenv("RELIFT_FONTS"), *override = getenv("RELIFT_FONTS_FALLBACK");
    static const char *const from[2] = { "the player's system data", "the open fallback (Noto Sans CJK)" };
    uint32_t at = 0;
    for (uint32_t t = 0; t < PL_TYPES; t++) {
        for (int d = 0; d < 2 && !g_pl.present[t]; d++) {
            size_t n = 0;
            char path[4096];
            uint8_t *font = font_file(d ? override : own, d == 1, t, path, sizeof path, &n);
            if (!font)
                continue;
            if (n > 0 && (uint64_t)at + 8 + (uint64_t)n <= PL_SHMEM_SIZE) {
                uint32_t hdr[2] = { PL_BFTTF_MAGIC, __builtin_bswap32((uint32_t)n) ^ PL_BFTTF_KEY };
                memcpy(base + at, hdr, sizeof hdr);
                memcpy(base + at + 8, font, n);
                g_pl.offset[t] = at + 8;
                g_pl.size[t] = (uint32_t)n;
                g_pl.present[t] = true;
                at += 8 + (uint32_t)n;
                rl_log(RL_LOG_INFO, "pl", "%s font: %s (%u bytes)", type_names[t], from[d], (uint32_t)n);
            } else {
                rl_log(RL_LOG_ERROR, "pl", "%s font: %s does not fit or cannot be read", type_names[t], path);
            }
            free(font);
        }
    }
    return true;
}

/** @brief The SharedFontType in r's input, or a named stop when relift has no such font. */
static rl_result font_type(rl_ipc_req *r, uint32_t *t)
{
    if (!rl_srv_in(r, t, 4))
        return SF_RESULT_INVALID_HEADER;
    if (*t >= PL_TYPES)
        return SF_RESULT_INVALID_HEADER;
    if (!pl_load())
        return KR_OUT_OF_RESOURCE;
    if (!g_pl.asked[*t]) {
        g_pl.asked[*t] = true;
        rl_log(RL_LOG_INFO, "pl", "the game asked for the %s font", type_names[*t]);
    }
    if (!g_pl.present[*t]) {
        (void)rl_report_unknown(0x504C00ull ^ *t, "pl",
                                "the game needs the %s shared font, and neither the player's system data "
                                "(relift import --system-data <dir>) nor relift's open fallback has it",
                                type_names[*t]);
        return SF_RESULT_NOT_SUPPORTED;
    }
    return KR_SUCCESS;
}

/** @brief RequestLoad: a SharedFontType; nothing back. The fonts are loaded at the first request. */
static rl_result request_load(rl_ipc_req *r)
{
    uint32_t t;
    return font_type(r, &t);
}

/** @brief GetLoadState: a SharedFontType; the LoadState, 1 (loaded). */
static rl_result get_load_state(rl_ipc_req *r)
{
    uint32_t t;
    rl_result rc = font_type(r, &t);
    uint32_t *out = rc == KR_SUCCESS ? rl_ipc_out(r, 4) : NULL;
    if (rc != KR_SUCCESS || !out)
        return rc != KR_SUCCESS ? rc : SF_RESULT_INVALID_HEADER;
    *out = 1;
    return KR_SUCCESS;
}

static rl_result font_u32(rl_ipc_req *r, const uint32_t *values)
{
    uint32_t t;
    rl_result rc = font_type(r, &t);
    if (rc != KR_SUCCESS)
        return rc;
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = values[t];
    return KR_SUCCESS;
}

/** @brief GetSize: a SharedFontType; the font's size in bytes. */
static rl_result get_size(rl_ipc_req *r) { return font_u32(r, g_pl.size); }

/** @brief GetSharedMemoryAddressOffset: a SharedFontType; the font's offset in the shared memory. */
static rl_result get_offset(rl_ipc_req *r) { return font_u32(r, g_pl.offset); }

/** @brief GetSharedMemoryNativeHandle: the shared memory, which the client maps read-only. */
static rl_result get_shared_memory(rl_ipc_req *r)
{
    if (!pl_load())
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_copy(r, &g_pl.shm->obj);
}

/**
 * @brief GetSharedFontInOrderOfPriority: a u64 LanguageCode and three type-6
 * buffers (types, offsets, sizes); a bool (loaded) and the count. Every font
 * relift has, in SharedFontType order whatever the language (inferred:
 * Ryujinx and yuzu answer the same; the console's per-language order is not
 * documented).
 */
static rl_result get_in_order_of_priority(rl_ipc_req *r)
{
    uint64_t language;
    if (!rl_srv_in(r, &language, 8))
        return SF_RESULT_INVALID_HEADER;
    rl_ipc_buf out_buf[3];
    for (uint32_t b = 0; b < 3; b++)
        out_buf[b] = rl_ipc_auto_out(r, b);
    if (!pl_load())
        return KR_OUT_OF_RESOURCE;
    uint32_t n = 0;
    for (uint32_t t = 0; t < PL_TYPES; t++) {
        if (!g_pl.present[t])
            continue;
        uint32_t v[3] = { t, g_pl.offset[t], g_pl.size[t] };
        for (uint32_t b = 0; b < 3; b++)
            if ((uint64_t)(n + 1) * 4 <= out_buf[b].size) {
                rl_ipc_buf one = { out_buf[b].addr + 4ull * n, 4, out_buf[b].attr };
                (void)rl_ipc_buf_write(&one, &v[b], 4);
            }
        n++;
    }
    uint8_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    memset(out, 0, 8);
    out[0] = 1;
    memcpy(out + 4, &n, 4);
    return KR_SUCCESS;
}

static const rl_ipc_cmd pl_cmds[] = {
    { 0, "RequestLoad", request_load },
    { 1, "GetLoadState", get_load_state },
    { 2, "GetSize", get_size },
    { 3, "GetSharedMemoryAddressOffset", get_offset },
    { 4, "GetSharedMemoryNativeHandle", get_shared_memory },
    { 5, "GetSharedFontInOrderOfPriority", get_in_order_of_priority },
};

static const rl_ipc_class pl_class = { "nn::pl::detail::IPlatformServiceManager", pl_cmds,
                                       sizeof pl_cmds / sizeof pl_cmds[0], 0, NULL };

static rl_ipc_object *pl_factory(void) { return rl_srv_object(&pl_class, NULL); }

void rl_srv_install_pl(void)
{
    if (g_pl.shm)
        hos_kobj_unref(&g_pl.shm->obj);
    memset(&g_pl, 0, sizeof g_pl);
    rl_ipc_register_service("pl:u", pl_factory);
}
