/**
 * @file opus_libopus.c
 * @brief Opus decoding through libopus (BSD license). Built with AUDIO=real.
 */
#include <relift/audio.h>

#include <opus/opus.h>
#include <opus/opus_multistream.h>

#include <stdlib.h>

typedef struct lo_dec {
    rl_opus head;
    OpusDecoder *single;
    OpusMSDecoder *multi;
    uint32_t rate;
} lo_dec;

static rl_opus *lo_open(uint32_t rate, uint32_t channels, uint32_t streams, uint32_t coupled, const uint8_t *mapping)
{
    int err = 0;
    lo_dec *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->rate = rate;
    if (streams == 0)
        d->single = opus_decoder_create((opus_int32)rate, (int)channels, &err);
    else
        d->multi = opus_multistream_decoder_create((opus_int32)rate, (int)channels, (int)streams, (int)coupled, mapping, &err);
    if (err != OPUS_OK || (!d->single && !d->multi)) {
        free(d);
        return NULL;
    }
    return &d->head;
}

static void lo_close(rl_opus *h)
{
    lo_dec *d = (lo_dec *)h;
    if (d->single)
        opus_decoder_destroy(d->single);
    if (d->multi)
        opus_multistream_decoder_destroy(d->multi);
    free(d);
}

static int32_t lo_decode(rl_opus *h, const uint8_t *pkt, uint32_t len, int16_t *pcm, uint32_t max_frames)
{
    lo_dec *d = (lo_dec *)h;
    int n = d->single ? opus_decode(d->single, pkt, (opus_int32)len, pcm, (int)max_frames, 0)
                      : opus_multistream_decode(d->multi, pkt, (opus_int32)len, pcm, (int)max_frames, 0);
    return n;
}

static void lo_reset(rl_opus *h)
{
    lo_dec *d = (lo_dec *)h;
    if (d->single)
        opus_decoder_ctl(d->single, OPUS_RESET_STATE);
    else
        opus_multistream_decoder_ctl(d->multi, OPUS_RESET_STATE);
}

static const rl_opus_backend g_lo = {
    .name = "libopus",
    .open = lo_open,
    .close = lo_close,
    .decode = lo_decode,
    .reset = lo_reset,
};

const rl_opus_backend *rl_host_opus_backend(void) { return &g_lo; }
