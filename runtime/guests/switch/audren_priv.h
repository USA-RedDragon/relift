/**
 * @file audren_priv.h
 * @brief State of the audio renderer shared by audren.c (the service and the
 * update parser) and audren_dsp.c (one rendered frame).
 */
#ifndef RELIFT_AUDREN_PRIV_H
#define RELIFT_AUDREN_PRIV_H

#include "hos_audio.h"

/** @brief Wave buffers per voice. */
#define AR_WAVEBUFS 4
/** @brief Most mix buffers per mix, and the side of a mix's routing matrix. */
#define AR_MIXBUFS 24
/** @brief Most channels per voice. */
#define AR_VOICE_CH 6
/** @brief Mix id that means "no mix". */
#define AR_UNUSED_MIX 0x7FFFFFFF

/** @brief Sample formats of a voice. */
enum { AR_FMT_INVALID = 0, AR_FMT_PCM16 = 2, AR_FMT_PCMF32 = 5, AR_FMT_ADPCM = 6 };
/** @brief Memory pool states as the guest sees them. */
enum { POOL_INVALID, POOL_NEW, POOL_REQUEST_DETACH, POOL_DETACHED, POOL_REQUEST_ATTACH, POOL_ATTACHED, POOL_RELEASED };
/** @brief Server-side voice play states. */
enum { VP_STARTED, VP_STOPPING, VP_STOPPED, VP_PAUSED };
/** @brief Sink types. */
enum { SINK_INVALID, SINK_DEVICE, SINK_CIRCULAR };

/** @brief A wave buffer the guest queued on a voice. */
typedef struct ar_wave {
    uint64_t addr, size;
    uint32_t start, end;
    bool loop, eos;
    int32_t loop_count;
    uint64_t ctx_addr, ctx_size;
    uint32_t loop_first, loop_last;
    /** @brief True once the render side owns it (Ryujinx IsSendToAudioProcessor). */
    bool sent_to_dsp;
    /** @brief True while the renderer may play it. */
    bool valid;
    /** @brief True after the buffer was consumed, until the guest clears its sent flag. */
    bool await_clear;
} ar_wave;

/** @brief A voice: the parameters from the last update and the playback state. */
typedef struct ar_voice {
    bool in_use, is_new;
    uint8_t play;
    uint32_t format, rate, nch;
    float pitch, volume;
    int32_t mix_id;
    uint32_t res_id[AR_VOICE_CH];
    uint64_t ds_addr, ds_size;
    ar_wave wb[AR_WAVEBUFS];
    uint32_t wb_index, wb_consumed, offset, loop_cnt;
    uint64_t played;
    float frac;
    uint16_t adpcm_pred;
    int16_t adpcm_h0, adpcm_h1;
    float prev[AR_VOICE_CH];
} ar_voice;

/** @brief A voice channel resource: the volume of each mix buffer. */
typedef struct ar_chres {
    float mix[AR_MIXBUFS];
    bool used;
} ar_chres;

/** @brief A mix: its buffers and its routing to the destination mix. */
typedef struct ar_mix {
    bool used;
    float volume;
    uint32_t rate, buf_count;
    int32_t dest_id;
    float matrix[AR_MIXBUFS * AR_MIXBUFS];
    float *buf;
} ar_mix;

/** @brief A sink: a host device or a circular buffer in guest memory. */
typedef struct ar_sink {
    uint8_t type;
    bool used;
    uint32_t input_count;
    uint8_t inputs[AR_VOICE_CH];
    uint64_t addr;
    uint32_t size, last_write;
} ar_sink;

/** @brief A memory pool the guest attached. */
typedef struct ar_pool {
    uint64_t addr, size;
    bool attached;
} ar_pool;

/** @brief An audio renderer. */
typedef struct audren {
    pthread_mutex_t lock;
    int32_t revision;
    uint32_t rate, sample_count, mix_buf_count, submix_count, voice_count, sink_count, effect_count, perf_frames;
    bool started;
    int32_t time_limit;
    float voice_drop;
    uint64_t elapsed_frames;
    ar_voice *voices;
    ar_chres *res;
    ar_pool *pools;
    uint32_t pool_count;
    ar_mix *mixes;
    uint32_t mix_count;
    ar_sink *sinks;
    hos_revent *event;
    void *tick;
    rl_audio_out *host;
    float *tmp;
} audren;

/** @brief Renders one frame of the renderer's current state, with ar->lock held. */
void audren_render_frame(audren *ar);

#endif
