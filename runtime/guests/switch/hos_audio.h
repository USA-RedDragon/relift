/**
 * @file hos_audio.h
 * @brief Audio services of the Switch guest: audren:u (the audio renderer),
 * audout:u (PCM output) and hwopus (the Opus decoder), and the timer that
 * paces them.
 *
 * Behavior and data layouts follow Ryujinx (MIT, Ryujinx.Audio and
 * Ryujinx.Horizon/Sdk/Audio) and libnx (ISC) audren.h, audout.c, hwopus.c.
 * Host output and Opus decoding come from <relift/audio.h>.
 */
#ifndef RELIFT_SWITCH_AUDIO_H
#define RELIFT_SWITCH_AUDIO_H

#include "ipc.h"

#include <relift/audio.h>

/** @brief Audio service results (module 153), as in Ryujinx's ResultCode. */
#define AUDIO_RESULT(n) RL_MAKE_RESULT(153, (n))
#define AUDIO_DEVICE_NOT_FOUND AUDIO_RESULT(1)
#define AUDIO_OPERATION_FAILED AUDIO_RESULT(2)
#define AUDIO_UNSUPPORTED_SAMPLE_RATE AUDIO_RESULT(3)
#define AUDIO_WORK_BUFFER_TOO_SMALL AUDIO_RESULT(4)
#define AUDIO_BUFFER_RING_FULL AUDIO_RESULT(8)
#define AUDIO_UNSUPPORTED_CHANNELS AUDIO_RESULT(10)
#define AUDIO_INVALID_UPDATE_INFO AUDIO_RESULT(41)
#define AUDIO_INVALID_ADDRESS_INFO AUDIO_RESULT(42)
#define AUDIO_INVALID_MIX_SORTING AUDIO_RESULT(43)
#define AUDIO_UNSUPPORTED_OPERATION AUDIO_RESULT(513)

/** @brief Codec results (module 111), as in Ryujinx's CodecResult. */
#define CODEC_INVALID_LENGTH RL_MAKE_RESULT(111, 3)
#define CODEC_OPUS_BAD_ARG RL_MAKE_RESULT(111, 130)
#define CODEC_OPUS_INVALID_PACKET RL_MAKE_RESULT(111, 133)
#define CODEC_INVALID_NUMBER_OF_STREAMS RL_MAKE_RESULT(111, 1000)
#define CODEC_INVALID_SAMPLE_RATE RL_MAKE_RESULT(111, 1001)
#define CODEC_INVALID_CHANNEL_COUNT RL_MAKE_RESULT(111, 1002)

/** @brief The audio renderer's frame period: 5 ms, 200 Hz. */
#define AUDIO_FRAME_NS 5000000ull

/** @brief Registers audren:u, audout:u and hwopus with sm:. */
RL_RUNTIME_API void rl_hos_audio_register(void);

/** @brief A timer callback; runs on the audio timer thread without hos.lock, once per frame. */
typedef void audio_tick_fn(void *ctx);
/** @brief Adds a callback called every AUDIO_FRAME_NS; starts the timer thread when it is the first. Returns a handle or NULL. */
void *audio_tick_add(audio_tick_fn *fn, void *ctx);
/** @brief Removes a callback and waits until it is not running. */
void audio_tick_remove(void *handle);
/** @brief Number of callbacks registered (tests). */
uint32_t audio_tick_count(void);

/** @brief Factories of the services. */
rl_ipc_object *audren_manager_new(void);
rl_ipc_object *audout_manager_new(void);
rl_ipc_object *hwopus_manager_new(void);

/** @brief Little-endian field access into guest-layout byte arrays. */
static inline uint32_t le32(const uint8_t *p)
{
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    return v;
}
static inline uint64_t le64(const uint8_t *p)
{
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    return v;
}
static inline float lef32(const uint8_t *p)
{
    float v;
    __builtin_memcpy(&v, p, 4);
    return v;
}
static inline void put32(uint8_t *p, uint32_t v) { __builtin_memcpy(p, &v, 4); }
static inline void put64(uint8_t *p, uint64_t v) { __builtin_memcpy(p, &v, 8); }

/** @brief Reads size bytes at guest address addr into dst; false when the range is not user-readable. */
bool audio_guest_read(uint64_t addr, void *dst, uint64_t size);
/** @brief Writes size bytes at guest address addr; false when the range is not user-writable. */
bool audio_guest_write(uint64_t addr, const void *src, uint64_t size);

#endif
