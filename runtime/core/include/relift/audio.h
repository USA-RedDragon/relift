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
    /** @brief Frames queued in the host, the lead (rl_audio_lead_visible) included; NULL when it equals queued. Statistics only. */
    uint32_t (*held)(rl_audio_out *o);
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

/**
 * @brief The frames a guest sees queued when a host output keeps lead frames
 * ahead of it. A host device takes frames in its own buffer size (1024
 * frames or more) while the console's mixer takes 5 ms at a time, so a guest
 * that keeps only a few milliseconds queued, as the console allows, would run
 * the host queue dry at every device pull. A backend that reports its queue
 * less the lead makes the guest keep the lead queued on top of its own
 * frames; the guest's buffers are released that much earlier, as on a device
 * with a longer output latency, and the rate it sees is unchanged.
 */
RL_RUNTIME_API uint32_t rl_audio_lead_visible(uint32_t queued, uint32_t lead);

/**
 * @brief The play position of a host output between device pulls. A host
 * takes frames from its queue a whole device buffer at a time (1024 frames
 * or more), so the queue drops in steps, while the frames play one by one
 * over the buffer's length. A guest that releases its buffers as the queue
 * drops gets them back in bursts a device buffer apart and appends in
 * bursts. The position advances with the clock at the stream's rate and
 * stays within one device buffer behind the frames the host has taken.
 */
typedef struct rl_audio_clock {
    uint64_t played;
    uint64_t last_ns;
    /** @brief Nanoseconds times rate not yet counted as a frame. */
    uint64_t carry;
    uint32_t rate;
    /** @brief Frames of one device buffer. */
    uint32_t device;
    bool running;
} rl_audio_clock;

/** @brief Frames played at now_ns, when the host has taken consumed frames from its queue in all. */
RL_RUNTIME_API uint64_t rl_audio_clock_played(rl_audio_clock *c, uint64_t consumed, uint64_t now_ns);
/** @brief Starts or stops the clock at now_ns (playback resumed or paused). */
RL_RUNTIME_API void rl_audio_clock_run(rl_audio_clock *c, bool running, uint64_t consumed, uint64_t now_ns);

/** @brief Furthest a steered stream's rate moves from its nominal rate (500 ppm, under one cent of pitch). */
#define RL_AUDIO_DRIFT_BOUND 0.0005

/**
 * @brief Steers a host stream's playback rate onto the system clock. Guests
 * make their frames on the system clock, as on the console, where the audio
 * and the clock share one crystal; a host device runs on its own crystal, a
 * little fast or slow, and would slowly drain or fill the queue. The frames
 * the device has asked the stream for (whether or not the queue had them)
 * are compared with the frames the system clock says it should have asked
 * for; the smoothed difference, measured from where it settled after the
 * start, and the rate offset learned from it set the ratio at which the
 * stream plays its input, bounded by RL_AUDIO_DRIFT_BOUND, so the queue
 * returns to its level at the start. A device further off than the bound
 * keeps drifting at the rest.
 */
typedef struct rl_audio_drift {
    uint32_t rate;
    uint64_t last_ns;
    /** @brief Playing time counted so far. */
    uint64_t run_ns;
    /** @brief Frames the device should have asked for by its request at last_ns. */
    double expected;
    /** @brief Frames asked for less expected, smoothed. */
    double error;
    /** @brief The smoothed error once settled after the start. */
    double base;
    /** @brief The device's rate offset learned so far, as a fraction of the rate. */
    double learned;
    double ratio;
    bool started;
    bool based;
    bool paused;
} rl_audio_drift;

/**
 * @brief The ratio to play the stream's input at (above 1 plays it faster).
 * Call it as the device asks for frames, at now_ns, with the input frames it
 * asked for before this request in all: measured always at a request, the
 * device's steps do not show as drift. A gap of over 250 ms between requests
 * (a pause) is left out of the measurement.
 */
RL_RUNTIME_API double rl_audio_drift_ratio(rl_audio_drift *d, uint64_t asked, uint64_t now_ns);
/** @brief Leaves the time until the next request out of the measurement (the stream was paused). */
RL_RUNTIME_API void rl_audio_drift_pause(rl_audio_drift *d);

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
