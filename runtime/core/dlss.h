/**
 * @file dlss.h
 * @brief NVIDIA DLSS Super Resolution for the upscaler stage (upscale.c), through the NGX library the NVIDIA driver
 * installs and the DLSS library the user puts beside the program; relift ships neither. A temporal choice, for
 * callers that give depth and motion vectors with image views (place 1). Private to the core.
 */
#ifndef RELIFT_CORE_DLSS_H
#define RELIFT_CORE_DLSS_H

#include <relift/upscale.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief DLSS started on one device. */
typedef struct rl_dlss rl_dlss;

/**
 * @brief Starts DLSS on device; NULL when it cannot run (no NVIDIA driver NGX library, no DLSS library beside the
 * program, a device without the extensions NGX needs, or a GPU DLSS does not run on). The log says which library
 * was found where, or why none was.
 */
rl_dlss *rl_dlss_new(const rl_post_device *device);

/** @brief Whether in has what DLSS needs: depth and motion vectors with their images, the rectangles at the origin. */
bool rl_dlss_takes(const rl_upscale_inputs *in);

/**
 * @brief Records one frame into cmd: writes in->out over in->out_rect. False when it recorded nothing (the sizes or
 * flags changed while records are pending): retire after the GPU is done and record again.
 */
bool rl_dlss_record(rl_dlss *d, uint64_t cmd, const rl_upscale_inputs *in);

/** @brief Says the GPU has finished everything recorded so far. */
void rl_dlss_retire(rl_dlss *d);

/**
 * @brief Looks for libnvidia-ngx-dlss.so.* in dir (the program's folder): true with its path in found, else false
 * with why naming the folder.
 */
bool rl_dlss_find(const char *dir, char *found, size_t found_size, char *why, size_t why_size);

/**
 * @brief Frees d's feature and parameters (the device must be idle for it); NGX itself stays up until the process
 * exits. NULL does nothing.
 */
void rl_dlss_free(rl_dlss *d);

#endif
