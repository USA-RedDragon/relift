/**
 * @file audio_sdl3.c
 * @brief Host audio output through SDL3 (zlib license). Built with AUDIO=real.
 */
#include <relift/audio.h>

#include "core.h"

#include <SDL3/SDL.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
#include <relift/ios.h>
#endif

#include <stdlib.h>

typedef struct sdl_out {
    rl_audio_out head;
    SDL_AudioStream *stream;
    uint32_t frame_bytes;
    /**
     * @brief Frames kept queued ahead of the guest's (rl_audio_lead_visible):
     * the device's buffer and one 5 ms guest mixing period. A stream that is
     * paused while empty, or runs dry while playing, gets this much silence,
     * so playback starts with it.
     */
    uint32_t lead;
    /** @brief Frames put into the stream in all, the lead included. */
    uint64_t put;
    rl_audio_clock clock;
    /** @brief Steers the stream's rate onto the system clock; used only under the stream's lock. */
    rl_audio_drift drift;
    /** @brief Input frames the device has asked for in all, and the rate applied last. */
    uint64_t asked;
    float applied;
    /** @brief The device asked for more than the stream held since the last write (set on the device's thread). */
    bool dry;
} sdl_out;

static const rl_audio_backend g_sdl;

static void SDLCALL sdl_asked(void *user, SDL_AudioStream *s, int additional, int total)
{
    sdl_out *o = user;
    if (additional > 0)
        __atomic_store_n(&o->dry, true, __ATOMIC_RELAXED);
    float r = (float)rl_audio_drift_ratio(&o->drift, o->asked, rl_clock_ns());
    o->asked += (uint32_t)total / o->frame_bytes;
    if (r != o->applied && SDL_SetAudioStreamFrequencyRatio(s, r))
        o->applied = r;
}

static rl_audio_out *sdl_open(uint32_t rate, uint32_t channels)
{
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        rl_log(RL_LOG_WARN, "audio", "SDL audio init failed: %s", SDL_GetError());
        return NULL;
    }
    SDL_AudioSpec spec = { .format = SDL_AUDIO_S16, .channels = (int)channels, .freq = (int)rate };
    SDL_AudioStream *s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!s) {
        rl_log(RL_LOG_WARN, "audio", "SDL could not open the default output: %s", SDL_GetError());
        return NULL;
    }
    sdl_out *o = calloc(1, sizeof *o);
    if (!o) {
        SDL_DestroyAudioStream(s);
        return NULL;
    }
    o->head.b = &g_sdl;
    o->stream = s;
    o->frame_bytes = channels * (uint32_t)sizeof(int16_t);
    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(s);
    SDL_AudioSpec have = { 0 };
    int device_frames = 0;
    if (!SDL_GetAudioDeviceFormat(dev, &have, &device_frames) || device_frames <= 0)
        device_frames = 1024;
    if (have.freq > 0 && (uint32_t)have.freq != rate)
        device_frames = (int)((uint64_t)device_frames * rate / (uint32_t)have.freq + 1);
    o->lead = (uint32_t)device_frames + rate / 200;
    o->clock = (rl_audio_clock){ .rate = rate, .device = (uint32_t)device_frames };
    o->drift = (rl_audio_drift){ .rate = rate };
    o->applied = 1.0f;
    SDL_SetAudioStreamGetCallback(s, sdl_asked, o);
    const char *name = SDL_GetAudioDeviceName(dev);
    rl_log(RL_LOG_INFO, "audio", "SDL %s output %s, %u Hz, %u channels, device buffer %d frames", SDL_GetCurrentAudioDriver(),
           name ? name : "?", rate, channels, device_frames);
#if defined(__APPLE__) && TARGET_OS_IPHONE
    rl_ios_log_audio_route();
#endif
    return &o->head;
}

static void sdl_close(rl_audio_out *h)
{
    sdl_out *o = (sdl_out *)h;
    SDL_DestroyAudioStream(o->stream);
    free(o);
}

static uint32_t sdl_held(rl_audio_out *h)
{
    sdl_out *o = (sdl_out *)h;
    int bytes = SDL_GetAudioStreamQueued(o->stream);
    return bytes > 0 ? (uint32_t)bytes / o->frame_bytes : 0;
}

/** @brief Queues the lead as silence, ahead of the guest's frames. */
static void put_lead(sdl_out *o)
{
    void *zero = calloc(o->lead, o->frame_bytes);
    if (zero) {
        SDL_PutAudioStreamData(o->stream, zero, (int)(o->lead * o->frame_bytes));
        o->put += o->lead;
    }
    free(zero);
}

/**
 * Guests play on the system clock, so a queue that ran dry (the guest was
 * late, or the device runs faster than the drift steering reaches) does not
 * refill by itself: the lead goes back in first.
 */
static void sdl_write(rl_audio_out *h, const int16_t *pcm, uint32_t frames)
{
    sdl_out *o = (sdl_out *)h;
    if (__atomic_exchange_n(&o->dry, false, __ATOMIC_RELAXED) && o->clock.running)
        put_lead(o);
    SDL_PutAudioStreamData(o->stream, pcm, (int)(frames * o->frame_bytes));
    o->put += frames;
}

/** @brief Statistics: a queue the device found short counts as empty, though the resampler may keep a few frames. */
static uint32_t sdl_held_stats(rl_audio_out *h)
{
    sdl_out *o = (sdl_out *)h;
    return __atomic_load_n(&o->dry, __ATOMIC_RELAXED) ? 0 : sdl_held(h);
}

static uint32_t sdl_queued(rl_audio_out *h)
{
    sdl_out *o = (sdl_out *)h;
    uint64_t played = rl_audio_clock_played(&o->clock, o->put - sdl_held(h), rl_clock_ns());
    uint64_t q = o->put - played;
    return rl_audio_lead_visible(q > UINT32_MAX ? UINT32_MAX : (uint32_t)q, o->lead);
}

static void sdl_pause(rl_audio_out *h, bool paused)
{
    sdl_out *o = (sdl_out *)h;
    rl_audio_clock_run(&o->clock, !paused, o->put - sdl_held(h), rl_clock_ns());
    if (paused) {
        SDL_PauseAudioStreamDevice(o->stream);
        SDL_LockAudioStream(o->stream);
        rl_audio_drift_pause(&o->drift);
        SDL_UnlockAudioStream(o->stream);
        if (SDL_GetAudioStreamQueued(o->stream) == 0) {
            put_lead(o);
            __atomic_store_n(&o->dry, false, __ATOMIC_RELAXED);
        }
    } else {
        SDL_ResumeAudioStreamDevice(o->stream);
    }
}

static const rl_audio_backend g_sdl = {
    .name = "sdl3",
    .open = sdl_open,
    .close = sdl_close,
    .write = sdl_write,
    .queued = sdl_queued,
    .pause = sdl_pause,
    .held = sdl_held_stats,
};

const rl_audio_backend *rl_host_audio_backend(void) { return &g_sdl; }
