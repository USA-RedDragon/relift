/**
 * @file nvn.h
 * @brief NVN (the Switch's NVN1 and the Switch 2's NVN2 graphics APIs)
 * replaced by name.
 *
 * Every game reaches NVN through nvnBootstrapLoader, which the SDK's sdk
 * module defines and the game's modules import. The loader hook of
 * this layer replaces nvnBootstrapLoader in every module, so the game gets
 * relift's nvnDeviceGetProcAddress and, from it, relift's function for every
 * proc name; the SDK driver's own device, queue and command buffer code
 * never runs. One layer serves both generations; what differs is data
 * chosen by the driver record the build writes (nvn_version.c). Each name gets a guest
 * address in a page of thunks next to the lifted code. A name relift does
 * not implement yet still gets an address, and a call to it stops the game
 * with a report that names the proc.
 */
#ifndef RELIFT_SWITCH_NVN_H
#define RELIFT_SWITCH_NVN_H

#include "core.h"
#include "loader.h"

#include <stdint.h>

/** @brief Most proc names the layer hands out addresses for (implemented and not). */
#define RL_NVN_SLOTS 1024u

/** @brief The loader hook; it adds itself to a program whose modules define or import nvnBootstrapLoader. */
RL_RUNTIME_API const rl_switch_hook *rl_nvn_switch_hook(void);

/** @brief The guest address of proc name, or 0 when the layer is not up or its slots are all taken. */
RL_RUNTIME_API uint64_t rl_nvn_proc(const char *name);

/** @brief The name of the proc at guest address addr, or NULL. */
RL_RUNTIME_API const char *rl_nvn_proc_name(uint64_t addr);

/** @brief A device field and the bytes the driver's own nvnDeviceInitialize leaves in it. */
struct rl_nvn_field {
    uint32_t off, len;
    const uint8_t *bytes;
};

/** @brief A layout offset or info the driver has none of (RL_NVN_NONE). */
#define RL_NVN_NONE 0xffffffffu

/**
 * @brief What the build read from the program's NVN driver: the guest
 * address of its nvnTextureBuilderGetStorageClass and of the word holding
 * its settings hook (0 for none), the offsets of a builder's device pointer
 * and of a device's flags word and NvRmGpuDevice pointer (RL_NVN_NONE when
 * the driver has none: NVN1 has no device flags word), the other device
 * fields that function reads, and the settings the driver's device start
 * reads, which must all be unset for the fields to hold.
 */
struct rl_nvn_layout {
    uint64_t storage_class, settings_hook;
    uint32_t builder_device, flags, gpu_device, nfields, nsettings;
    const struct rl_nvn_field *fields;
    const char *const *settings;
    /** @brief Number of NVNdeviceInfo values the driver's nvnDeviceGetInteger knows. */
    uint32_t nints;
    /** @brief Its answer for each, where int_device is 0. */
    const int32_t *ints;
    /**
     * @brief Where each answer comes from: 0 the constant in ints, 1 device
     * state the driver's initialization measured (the runtime's own value), 2
     * the u32 at byte ints[i] of what NvRmGpuDeviceGetInfo returns for the
     * opened GPU (the driver's initialization copies it into the device).
     */
    const uint8_t *int_device;
    /**
     * @brief The NVNdeviceInfo whose answer is the page size
     * nvnMemoryPoolMapVirtual checks against, RL_NVN_NONE when the build
     * found none.
     */
    uint32_t pool_page_info;
};

/** @brief Registers the driver layout (rl_nvn_set_driver does for a build). */
RL_RUNTIME_API void rl_nvn_set_layout(const struct rl_nvn_layout *l);

/**
 * @brief What the build read from the program's NVN driver: its API
 * generation (1 for NVN1, the Switch's; 2 for NVN2, the Switch 2's), its
 * NVN_DEVICE_INFO_API_MAJOR_VERSION and _MINOR_VERSION, the NVN SDK version
 * its middleware string names (NULL for none), the device layout (NULL with
 * layout_error saying why it could not be read), and the procs whose own
 * code is not pure, as nimpure pairs of name and reason in impure.
 */
struct rl_nvn_driver {
    uint32_t generation;
    int32_t api_major, api_minor;
    const char *sdk;
    const struct rl_nvn_layout *layout;
    const char *layout_error;
    uint32_t nimpure;
    const char *const *impure;
};

/** @brief Registers the driver record and its layout; the build's generated nvn_driver.c calls it before main. */
RL_RUNTIME_API void rl_nvn_set_driver(const struct rl_nvn_driver *d);

#endif
