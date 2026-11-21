/**
 * @file nvn_version.c
 * @brief What differs between NVN1 (the Switch's API, on the Maxwell
 * GM20B) and NVN2 (the Switch 2's), as data the one NVN layer reads.
 *
 * The build reads the program's own driver and writes a record of it
 * (generation, API version, SDK version, device layout and the procs whose
 * driver code is not pure) to nvn_driver.c, which registers it here before
 * main. Without a record the layer serves NVN2, as it did before NVN1.
 * Every NVN1 value below cites the driver code it was read from: SDK 16.2.0
 * offsets are of the sweep's s1-0007 sdk module, and the same code was
 * checked in the drivers of SDK 4.5 to 16.2 the sweep holds. Values only a
 * console settles are labeled inferred and listed in
 * .work/s2-console-questions.md.
 */
#include "nvn_priv.h"

#include <stdio.h>

static const struct rl_nvn_driver *g_driver;

void rl_nvn_set_driver(const struct rl_nvn_driver *d)
{
    g_driver = d;
    rl_nvn_set_layout(d ? d->layout : NULL);
}

const void *nvn_driver_record(void) { return g_driver; }

uint32_t nvn_generation(void) { return g_driver && g_driver->generation == 1 ? 1u : 2u; }

const char *nvn_impure(const char *name)
{
    if (!g_driver || !name)
        return NULL;
    for (uint32_t i = 0; i < g_driver->nimpure; i++)
        if (!strcmp(g_driver->impure[2 * i], name))
            return g_driver->impure[2 * i + 1];
    return NULL;
}

const char *nvn_layout_error(void) { return g_driver ? g_driver->layout_error : NULL; }

const char *nvn_version_name(void)
{
    static char name[96];
    if (!g_driver)
        return "NVN2";
    snprintf(name, sizeof name, "NVN%u %d.%d (SDK %s)", (unsigned)nvn_generation(), (int)g_driver->api_major,
             (int)g_driver->api_minor, g_driver->sdk ? g_driver->sdk : "unknown");
    return name;
}

/**
 * NVN1 marks the handles a shader samples through with bit 32:
 * nvnDeviceGetTextureHandle is (texture | sampler << 20) | 1 << 32 (16.2.0
 * 0x44d970: orr w8, w1, w2, lsl #20; sxtw; orr #0x100000000),
 * nvnDeviceGetSeparateTextureHandle and nvnDeviceGetTexelFetchHandle are
 * id | 1 << 32 (0x44d950, 0x44d960), and nvnDeviceGetSeparateSamplerHandle
 * (id << 20, 0x44d940) and nvnDeviceGetImageHandle (the id, 0x44d8f0) carry
 * no mark. NVN2 marks none of them.
 */
uint64_t nvn_sampled_handle_mark(void) { return nvn_generation() == 1 ? 1ull << 32 : 0; }

/**
 * NVN1 knows NVNformat 0 to 160: the driver's format table (16.2.0: the
 * word at 0xd4be80 points to it, 0x30 bytes per format, bytes per block at
 * +0x10 and block width and height at +8 and +9) has 161 entries, each
 * equal to the layer's own row. NVN2 adds 161 and 162.
 */
uint32_t nvn_format_count(void)
{
    return nvn_generation() == 1 ? 161u : 163u;
}

/**
 * @brief The NVN1 answers of nvnDeviceGetInteger that the driver reads from
 * device state its initialization measured, by NVNdeviceInfo.
 */
static const struct {
    uint32_t info;
    int32_t value;
} nvn1_measured[] = {
    /** Scratch memory scale factors (16.2.0 reads device +0xb4, +0xac, +0xb4 for 23, 24, 55): inferred, relift's NVN2 picks; no Switch source answers them. */
    { 23, 2 },
    { 24, 256 },
    /** L2 size (device +0x20): inferred, 256 KiB, the GM20B L2 that Ryujinx's and yuzu's nvhost-ctrl-gpu characteristics report (0x40000). */
    { 43, 262144 },
    /** Most texture layers (read from the device in every NVN1 driver): inferred, 2048, the Maxwell limit NVN2's driver also answers. */
    { 45, 2048 },
    { 55, 128 },
    /** Linear texture stride alignment, the texture size limits: SDK 7 drivers read them from the device; 16.2.0 answers these constants. */
    { 56, 32 },
    /** Memory pool page size (device +0xc8): inferred, 64 KiB, the Maxwell big page Ryujinx and yuzu map with (and NVN2's answer). */
    { 58, 65536 },
    { 61, 16384 },
    { 62, 134217728 },
    { 63, 2048 },
    /** Info 98 (device +0xbc): inferred, 0 as NVN2 answers. */
    { 98, 0 },
};

bool nvn_version_measured(uint32_t info, int32_t *out)
{
    if (nvn_generation() != 1)
        return false;
    for (size_t i = 0; i < sizeof nvn1_measured / sizeof nvn1_measured[0]; i++)
        if (nvn1_measured[i].info == info) {
            *out = nvn1_measured[i].value;
            return true;
        }
    return false;
}
