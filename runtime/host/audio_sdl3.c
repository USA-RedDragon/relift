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

/** @brief Silence queued when playback starts on an empty stream (60 ms at 48 kHz), so scheduling jitter of the producer does not reach the device as a gap. */
#define PRIME_FRAMES 2880u

typedef struct sdl_out {
    rl_audio_out head;
    SDL_AudioStream *stream;
    uint32_t frame_bytes;
} sdl_out;

static const rl_audio_backend g_sdl;

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
    const char *name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(s));
    rl_log(RL_LOG_INFO, "audio", "SDL %s output %s, %u Hz, %u channels", SDL_GetCurrentAudioDriver(), name ? name : "?",
           rate, channels);
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

static void sdl_write(rl_audio_out *h, const int16_t *pcm, uint32_t frames)
{
    sdl_out *o = (sdl_out *)h;
    SDL_PutAudioStreamData(o->stream, pcm, (int)(frames * o->frame_bytes));
}

static uint32_t sdl_queued(rl_audio_out *h)
{
    sdl_out *o = (sdl_out *)h;
    int bytes = SDL_GetAudioStreamQueued(o->stream);
    return bytes > 0 ? (uint32_t)bytes / o->frame_bytes : 0;
}

static void sdl_pause(rl_audio_out *h, bool paused)
{
    sdl_out *o = (sdl_out *)h;
    if (paused)
        SDL_PauseAudioStreamDevice(o->stream);
    else {
        if (SDL_GetAudioStreamQueued(o->stream) == 0) {
            int bytes = (int)(PRIME_FRAMES * o->frame_bytes);
            void *zero = calloc(1, (size_t)bytes);
            if (zero)
                SDL_PutAudioStreamData(o->stream, zero, bytes);
            free(zero);
        }
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
};

const rl_audio_backend *rl_host_audio_backend(void) { return &g_sdl; }
