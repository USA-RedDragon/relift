/**
 * @file gpu_driver.h
 * @brief Custom Vulkan driver packages: the video.gpu_driver setting and the
 * packages installed in the app's storage.
 *
 * A package is a folder <drivers>/<name>/ holding meta.json (the
 * AdrenoToolsDrivers format: schemaVersion, name, vendor, driverVersion,
 * libraryName, ...) and the driver library meta.json names. <drivers> is
 * RELIFT_GPU_DRIVERS, or $HOME/gpu_drivers (on Android HOME is the app's
 * private files folder, see android.h). video.gpu_driver is "system" (the
 * device's own driver) or the name of an installed package. Only Android
 * builds with libadrenotools load a package (hostlib.c).
 */
#ifndef RELIFT_GPU_DRIVER_H
#define RELIFT_GPU_DRIVER_H

#include <stdbool.h>
#include <stddef.h>

/** @brief The setting that picks the driver. */
#define RL_GPU_DRIVER_KEY "video.gpu_driver"
/** @brief The value of RL_GPU_DRIVER_KEY that keeps the device's own driver. */
#define RL_GPU_DRIVER_SYSTEM "system"
/** @brief Environment variable naming the folder of installed packages. */
#define RL_GPU_DRIVERS_ENV "RELIFT_GPU_DRIVERS"
/** @brief Longest package name. */
#define RL_GPU_DRIVER_NAME_MAX 96

/** @brief An installed package. */
typedef struct rl_gpu_driver {
    /** @brief The package name (its folder name). */
    char name[RL_GPU_DRIVER_NAME_MAX];
    /** @brief The package folder, with a trailing '/'. */
    char dir[1024];
    /** @brief The driver library's file name in dir (meta.json libraryName). */
    char library[256];
} rl_gpu_driver;

/** @brief Whether name can name a package: letters, digits, '.', '_', '-', not starting with '.', not "system". */
bool rl_gpu_driver_name_ok(const char *name);

/** @brief The folder of installed packages, without a trailing '/'; false when neither RELIFT_GPU_DRIVERS nor HOME is set. */
bool rl_gpu_drivers_dir(char *out, size_t size);

/** @brief Reads the package called name; false (and a warning) when it is not installed or its meta.json is unusable. */
bool rl_gpu_driver_find(const char *name, rl_gpu_driver *out);

/**
 * @brief Writes the names of the installed packages (folders with a usable
 * meta.json and library), sorted, into names; returns how many there are,
 * which may exceed max.
 */
size_t rl_gpu_driver_list(char (*names)[RL_GPU_DRIVER_NAME_MAX], size_t max);

#endif
