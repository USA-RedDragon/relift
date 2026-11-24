/**
 * @file gpu_floor.h
 * @brief The GPU clock floor (the gpu.clock_floor setting): keeps a phone's
 * GPU clock up while a frame's work runs, so the GPU governor does not clock
 * down in the gaps between a game's frames and stretch the next one.
 *
 * Android only. On Qualcomm's own driver the device's queues get
 * VK_QCOM_queue_perf_hint at the highest clock; where a driver lacks it, an
 * ADPF performance hint session (APerformanceHint, Android 13 and later)
 * hears each frame's work span against one 60 Hz period. On every other
 * host the setting is accepted and does nothing.
 */
#ifndef RELIFT_GPU_FLOOR_H
#define RELIFT_GPU_FLOOR_H

#include "vkdev.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief The setting that turns the floor on. */
#define RL_GPU_FLOOR_KEY "gpu.clock_floor"

/** @brief The period a frame's work is held to: one 60 Hz vsync, the shortest a console game asks for. */
#define RL_GPU_FLOOR_PERIOD_NS 16666667

/** @brief How the floor is held. */
typedef enum rl_gpu_floor_kind {
    /** @brief The setting is off. */
    RL_GPU_FLOOR_OFF,
    /** @brief This host has no floor (every host but Android, or an Android without either way below). */
    RL_GPU_FLOOR_NONE,
    /** @brief VK_QCOM_queue_perf_hint at the highest clock on every queue of the core's device. */
    RL_GPU_FLOOR_QUEUE_HINT,
    /** @brief An ADPF performance hint session that hears each frame's work span. */
    RL_GPU_FLOOR_ADPF,
} rl_gpu_floor_kind;

/**
 * @brief Which floor a device gets: off when the setting is off, else the queue hint where the driver has it, else
 * ADPF where the platform has it, else none. Only Android has either.
 */
rl_gpu_floor_kind rl_gpu_floor_pick(bool on, bool android, bool queue_hint, bool adpf);

/** @brief The name of a kind, for the log. */
const char *rl_gpu_floor_name(rl_gpu_floor_kind kind);

/**
 * @brief The work span of one frame: from start (the frame's first guest submit) to end (the frame handed to the
 * presenter, after its GPU work finished); 0 when there was no start or the span is not a frame's (negative, or a
 * second or longer, as after a pause).
 */
int64_t rl_gpu_floor_span(uint64_t start_ns, uint64_t end_ns);

/** @brief Before vkCreateDevice: asks for VK_QCOM_queue_perf_hint with its feature where the floor would use it. */
void rl_gpu_floor_build(const rlvk_dev *d, rl_vkdev_build *b);

/** @brief After the device and its queues exist: starts the floor the device and platform allow, and logs it. */
void rl_gpu_floor_start(const rlvk_dev *d);

/** @brief The floor running now. */
rl_gpu_floor_kind rl_gpu_floor_active(void);

/** @brief A guest submits work: the first submit after a frame was handed over starts the next frame's span. */
void rl_gpu_floor_work(void);

/** @brief A frame was handed to the presenter: reports its span to the ADPF session, when that is the floor. */
void rl_gpu_floor_frame(void);

#endif
