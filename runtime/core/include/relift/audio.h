/**
 * @file audio.h
 * @brief Host audio output and the Opus decoder, in core terms.
 *
 * A guest layer feeds interleaved 16-bit PCM to rl_audio_out and decodes Opus
 * packets with rl_opus. Both are backed by the host layer in runtime/host
 * when the runtime is built with AUDIO=real (SDL3 and libopus). Otherwise a
 * null output (which drains in real time, so timing still works) and a
 * decoder that returns silence of the right length are used. Setting
 * RELIFT_AUDIO=null selects the null output at run time.
 *
 * Every stream keeps statistics (frames written against playing time,
 * underruns, frames dropped, queue depth). RELIFT_AUDIO_STATS=1 logs them
 * every 10 s of wall time and at close. RELIFT_AUDIO_WAV=<path> also records
 * what is handed to the host as 16-bit PCM WAV (stream 0 to the path, stream
 * n to <path>.n), whichever backend plays it; the header is kept valid while
 * the run goes on.
 */
#ifndef RELIFT_AUDIO_H
#define RELIFT_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include <relift/abi_core.h>

typedef struct rl_audio_backend rl_audio_backend;

/** @brief An open host output stream. A backend embeds this as the first member of its stream. */
typedef struct rl_audio_out {
    /** @brief The backend that opened the stream (set by the backend). */
    const rl_audio_backend *b;
} rl_audio_out;

/** @brief Operations of an output backend. */
struct rl_audio_backend {
    /** @brief Backend name for logs. */
    const char *name;
    /** @brief Opens a stream of interleaved 16-bit frames; NULL on failure. */
    rl_audio_out *(*open)(uint32_t rate, uint32_t channels);
    /** @brief Closes a stream. */
    void (*close)(rl_audio_out *o);
    /** @brief Queues frames for playback. */
    void (*write)(rl_audio_out *o, const int16_t *pcm, uint32_t frames);
    /** @brief Frames queued and not yet played. */
    uint32_t (*queued)(rl_audio_out *o);
    /** @brief Pauses or resumes playback. */
    void (*pause)(rl_audio_out *o, bool paused);
};

/** @brief The null backend: plays nothing and drains at the stream's rate. */
RL_RUNTIME_API const rl_audio_backend *rl_audio_null_backend(void);
/** @brief Sets the backend used by rl_audio_open; NULL selects the default. */
RL_RUNTIME_API void rl_audio_set_backend(const rl_audio_backend *b);
/** @brief The backend in use (the host one when built in, unless RELIFT_AUDIO=null). */
RL_RUNTIME_API const rl_audio_backend *rl_audio_backend_current(void);
/**
 * @brief Opens an output stream on the current backend, paused. A host backend
 * that fails falls back to the null output. One that has not opened within
 * RELIFT_AUDIO_OPEN_MS (default 3000) is replaced by the null output for now
 * and attached when its open completes; frames queued always come from the
 * null output's timing in that case. NULL only when the null output fails.
 */
RL_RUNTIME_API rl_audio_out *rl_audio_open(uint32_t rate, uint32_t channels);
/** @brief Closes a stream; NULL is allowed. */
RL_RUNTIME_API void rl_audio_close(rl_audio_out *o);
/** @brief Queues frames. */
RL_RUNTIME_API void rl_audio_write(rl_audio_out *o, const int16_t *pcm, uint32_t frames);
/** @brief Frames queued and not yet played. */
RL_RUNTIME_API uint32_t rl_audio_queued(rl_audio_out *o);
/** @brief Pauses or resumes playback. */
RL_RUNTIME_API void rl_audio_pause(rl_audio_out *o, bool paused);

/** @brief Records that a caller discarded frames instead of queueing them (shown in the stream's statistics). */
RL_RUNTIME_API void rl_audio_count_drop(rl_audio_out *o, uint32_t frames);

/** @brief Opus decoder errors, the values libopus uses. */
enum {
    RL_OPUS_OK = 0,
    RL_OPUS_BAD_ARG = -1,
    RL_OPUS_BUFFER_TOO_SMALL = -2,
    RL_OPUS_INTERNAL_ERROR = -3,
    RL_OPUS_INVALID_PACKET = -4
};

typedef struct rl_opus_backend rl_opus_backend;

/** @brief An Opus decoder, single stream or multistream. A backend embeds this as the first member of its decoder. */
typedef struct rl_opus {
    /** @brief The backend that created the decoder (set by rl_opus_open). */
    const rl_opus_backend *b;
} rl_opus;

/** @brief Operations of an Opus backend. */
struct rl_opus_backend {
    /** @brief Backend name for logs. */
    const char *name;
    /** @brief Creates a decoder; streams 0 means a plain (one or two channel) decoder. NULL on failure. */
    rl_opus *(*open)(uint32_t rate, uint32_t channels, uint32_t streams, uint32_t coupled, const uint8_t *mapping);
    /** @brief Frees a decoder. */
    void (*close)(rl_opus *d);
    /** @brief Decodes one packet to interleaved 16-bit frames; returns the frames or a negative RL_OPUS_ error. */
    int32_t (*decode)(rl_opus *d, const uint8_t *pkt, uint32_t len, int16_t *pcm, uint32_t max_frames);
    /** @brief Resets decoder state. */
    void (*reset)(rl_opus *d);
};

/** @brief Sets the Opus backend; NULL selects the default (host libopus when built in, else the stub). */
RL_RUNTIME_API void rl_opus_set_backend(const rl_opus_backend *b);
/** @brief The Opus backend in use. */
RL_RUNTIME_API const rl_opus_backend *rl_opus_backend_current(void);
/** @brief The stub backend: decodes every packet to silence of the right length. */
RL_RUNTIME_API const rl_opus_backend *rl_opus_stub_backend(void);
/** @brief Creates a decoder on the current backend. */
RL_RUNTIME_API rl_opus *rl_opus_open(uint32_t rate, uint32_t channels, uint32_t streams, uint32_t coupled,
                                     const uint8_t *mapping);
/** @brief Frees a decoder; NULL is allowed. */
RL_RUNTIME_API void rl_opus_close(rl_opus *d);
/** @brief Decodes one packet; returns frames per channel or a negative RL_OPUS_ error. */
RL_RUNTIME_API int32_t rl_opus_decode(rl_opus *d, const uint8_t *pkt, uint32_t len, int16_t *pcm, uint32_t max_frames);
/** @brief Resets a decoder. */
RL_RUNTIME_API void rl_opus_reset(rl_opus *d);
/** @brief Frames per channel in a packet at rate (parsed from the packet), or a negative RL_OPUS_ error. */
RL_RUNTIME_API int32_t rl_opus_packet_frames(const uint8_t *pkt, uint32_t len, uint32_t rate);

/** @brief Backends the host layer provides when built in (see runtime/host). */
RL_RUNTIME_API const rl_audio_backend *rl_host_audio_backend(void);
/** @brief The host Opus backend. */
RL_RUNTIME_API const rl_opus_backend *rl_host_opus_backend(void);

#endif
