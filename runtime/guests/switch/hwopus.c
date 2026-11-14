/**
 * @file hwopus.c
 * @brief hwopus: IHardwareOpusDecoderManager and IHardwareOpusDecoder.
 *
 * Commands, parameter layouts, work buffer sizes and results follow Ryujinx
 * (MIT) Ryujinx.Horizon/Sdk/Codec/Detail; replies match libnx (ISC) hwopus.c.
 * Decoding goes through rl_opus, so libopus is used when the runtime is
 * built with AUDIO=real. Each packet starts with an 8-byte big-endian header
 * (payload length, final range).
 */
#include "audio.h"

#include <stdlib.h>
#include <string.h>

static bool valid_rate(int32_t r) { return r == 8000 || r == 12000 || r == 16000 || r == 24000 || r == 48000; }
static bool valid_channels(int32_t c) { return c > 0 && c <= 2; }
static bool valid_multi_channels(int32_t c) { return c > 0 && c <= 255; }

static bool valid_streams(int32_t streams, int32_t stereo, int32_t channels)
{
    return streams > 0 && streams + stereo <= channels && stereo >= 0 && stereo <= streams;
}

static int32_t align_up(int32_t v, int32_t a) { return (v + a - 1) / a * a; }

static int32_t opus_alloc_size(int32_t ch) { return ch * 0x800 + 0x4800; }

static int32_t decoder_size(int32_t ch)
{
    if (ch < 1 || ch > 2)
        return 0;
    int32_t celt = (0x2030 + 120 * 4) * ch + 21 * 16 + 0x54;
    return (opus_alloc_size(ch) | 0x50) + 0x2160 + celt;
}

static int32_t multistream_decoder_size(int32_t streams, int32_t coupled)
{
    if (streams < 1 || coupled > streams || coupled < 0)
        return 0;
    int32_t c = decoder_size(2), m = decoder_size(1);
    return align_up(m - opus_alloc_size(1), 4) * (streams - coupled) + align_up(c - opus_alloc_size(2), 4) * coupled +
           0xb920;
}

static int32_t frame_size(int32_t rate, int32_t channels, int32_t mono48)
{
    int32_t ratio = rate ? 48000 / rate : 0;
    return align_up(ratio ? channels * mono48 / ratio : 0, 64);
}

static rl_result work_size(int32_t rate, int32_t ch, int32_t mono48, int32_t *out)
{
    *out = 0;
    if (!valid_channels(ch))
        return CODEC_INVALID_CHANNEL_COUNT;
    if (!valid_rate(rate))
        return CODEC_INVALID_SAMPLE_RATE;
    *out = decoder_size(ch) + 1536 + frame_size(rate, ch, mono48);
    return KR_SUCCESS;
}

static rl_result work_size_multi(int32_t rate, int32_t ch, int32_t streams, int32_t stereo, int32_t mono48, int32_t *out)
{
    *out = 0;
    if (!valid_multi_channels(ch))
        return CODEC_INVALID_CHANNEL_COUNT;
    if (!valid_rate(rate))
        return CODEC_INVALID_SAMPLE_RATE;
    if (!valid_streams(streams, stereo, ch))
        return CODEC_INVALID_SAMPLE_RATE;
    *out = multistream_decoder_size(streams, stereo) + align_up(streams * 1500, 64) + frame_size(rate, ch, mono48);
    return KR_SUCCESS;
}

/** @brief State of one decoder object. */
typedef struct opus_state {
    rl_opus *dec;
    uint32_t rate;
    uint32_t channels;
} opus_state;

static void opus_state_destroy(void *p)
{
    opus_state *s = p;
    rl_opus_close(s->dec);
    free(s);
}

static rl_result codec_error(int32_t e)
{
    switch (e) {
    case RL_OPUS_BAD_ARG:
        return CODEC_OPUS_BAD_ARG;
    case RL_OPUS_INVALID_PACKET:
        return CODEC_OPUS_INVALID_PACKET;
    default:
        return CODEC_INVALID_LENGTH;
    }
}

static rl_result decode(rl_ipc_req *r, bool with_perf, bool reset_in_data)
{
    opus_state *s = r->self->state;
    uint8_t reset = 0;
    if (reset_in_data && !rl_ipc_in(r, &reset, 1))
        return SF_RESULT_INVALID_HEADER;
    rl_ipc_buf in = rl_ipc_auto_in(r, 0), out = rl_ipc_auto_out(r, 0);
    if (in.size < 8)
        return CODEC_INVALID_LENGTH;
    uint8_t hdr[8];
    if (!rl_ipc_buf_read(&in, hdr, 8))
        return KR_INVALID_MEM_STATE;
    uint32_t len = (uint32_t)hdr[0] << 24 | (uint32_t)hdr[1] << 16 | (uint32_t)hdr[2] << 8 | hdr[3];
    uint64_t total = (uint64_t)len + 8;
    if (total > in.size)
        return CODEC_INVALID_LENGTH;
    uint8_t *pkt = malloc(len ? len : 1);
    if (!pkt)
        return KR_OUT_OF_MEMORY;
    rl_ipc_buf body = { in.addr + 8, len, in.attr };
    if (!rl_ipc_buf_read(&body, pkt, len)) {
        free(pkt);
        return KR_INVALID_MEM_STATE;
    }
    int32_t frames = rl_opus_packet_frames(pkt, len, s->rate);
    rl_result rc = KR_SUCCESS;
    int16_t *pcm = NULL;
    int32_t got = 0;
    if (frames < 0) {
        rc = codec_error(frames);
    } else if ((uint64_t)frames * s->channels * 2 > out.size) {
        rc = CODEC_INVALID_LENGTH;
    } else {
        pcm = calloc((size_t)frames * s->channels + 1, sizeof *pcm);
        if (!pcm) {
            free(pkt);
            return KR_OUT_OF_MEMORY;
        }
        if (reset)
            rl_opus_reset(s->dec);
        got = rl_opus_decode(s->dec, pkt, len, pcm, (uint32_t)frames);
        if (got < 0)
            rc = codec_error(got);
        else if (!rl_ipc_buf_write(&out, pcm, (uint64_t)got * s->channels * 2))
            rc = KR_INVALID_MEM_STATE;
    }
    free(pcm);
    free(pkt);
    if (rc != KR_SUCCESS)
        return rc;
    uint8_t *o = rl_ipc_out(r, with_perf ? 16 : 8);
    put32(o, (uint32_t)total);
    put32(o + 4, (uint32_t)got);
    return KR_SUCCESS;
}

static rl_result dec_old(rl_ipc_req *r) { return decode(r, false, false); }
static rl_result dec_perf(rl_ipc_req *r) { return decode(r, true, false); }
static rl_result dec_perf_reset(rl_ipc_req *r) { return decode(r, true, true); }

static rl_result dec_set_context(rl_ipc_req *r)
{
    (void)r;
    return KR_SUCCESS;
}

static const rl_ipc_cmd decoder_cmds[] = {
    { 0, "DecodeInterleavedOld", dec_old },
    { 1, "SetContext", dec_set_context },
    { 2, "DecodeInterleavedForMultiStreamOld", dec_old },
    { 3, "SetContextForMultiStream", dec_set_context },
    { 4, "DecodeInterleavedWithPerfOld", dec_perf },
    { 5, "DecodeInterleavedForMultiStreamWithPerfOld", dec_perf },
    { 6, "DecodeInterleavedWithPerfAndResetOld", dec_perf_reset },
    { 7, "DecodeInterleavedForMultiStreamWithPerfAndResetOld", dec_perf_reset },
    { 8, "DecodeInterleaved", dec_perf_reset },
    { 9, "DecodeInterleavedForMultiStream", dec_perf_reset },
};

static const rl_ipc_class decoder_class = {
    .name = "nn::codec::detail::IHardwareOpusDecoder",
    .cmds = decoder_cmds,
    .ncmds = sizeof decoder_cmds / sizeof decoder_cmds[0],
    .pointer_buffer_size = 0,
    .destroy = opus_state_destroy,
};

static rl_result push_decoder(rl_ipc_req *r, rl_opus *dec, uint32_t rate, uint32_t channels)
{
    if (!dec)
        return AUDIO_OPERATION_FAILED;
    opus_state *s = calloc(1, sizeof *s);
    rl_ipc_object *o = s ? rl_ipc_object_new(&decoder_class, s) : NULL;
    if (!o) {
        free(s);
        rl_opus_close(dec);
        return KR_OUT_OF_MEMORY;
    }
    s->dec = dec;
    s->rate = rate;
    s->channels = channels;
    return rl_ipc_push_object(r, o);
}

static rl_result mgr_open(rl_ipc_req *r)
{
    int32_t in[3];
    if (!rl_ipc_in(r, in, 12))
        return SF_RESULT_INVALID_HEADER;
    if (!valid_rate(in[0]))
        return CODEC_INVALID_SAMPLE_RATE;
    if (!valid_channels(in[1]))
        return CODEC_INVALID_CHANNEL_COUNT;
    return push_decoder(r, rl_opus_open((uint32_t)in[0], (uint32_t)in[1], 0, 0, NULL), (uint32_t)in[0], (uint32_t)in[1]);
}

static rl_result mgr_open_ex(rl_ipc_req *r)
{
    int32_t in[6];
    if (!rl_ipc_in(r, in, 20))
        return SF_RESULT_INVALID_HEADER;
    if (!valid_channels(in[1]))
        return CODEC_INVALID_CHANNEL_COUNT;
    if (!valid_rate(in[0]))
        return CODEC_INVALID_SAMPLE_RATE;
    return push_decoder(r, rl_opus_open((uint32_t)in[0], (uint32_t)in[1], 0, 0, NULL), (uint32_t)in[0], (uint32_t)in[1]);
}

static rl_result mgr_size(rl_ipc_req *r)
{
    int32_t in[2], size;
    if (!rl_ipc_in(r, in, 8))
        return SF_RESULT_INVALID_HEADER;
    rl_result rc = work_size(in[0], in[1], 1920, &size);
    put32(rl_ipc_out(r, 4), (uint32_t)size);
    return rc;
}

static rl_result mgr_size_ex(rl_ipc_req *r)
{
    int32_t in[4], size;
    if (!rl_ipc_in(r, in, 16))
        return SF_RESULT_INVALID_HEADER;
    rl_result rc = work_size(in[0], in[1], (in[2] & 1) ? 5760 : 1920, &size);
    put32(rl_ipc_out(r, 4), (uint32_t)size);
    return rc;
}

/** @brief Reads the multistream parameter (0x110 bytes, or 0x118 with flags) from the first input buffer. */
static bool read_multi(rl_ipc_req *r, bool ex, int32_t p[5], uint8_t map[256])
{
    rl_ipc_buf b = rl_ipc_auto_in(r, 0);
    uint8_t raw[0x118];
    uint32_t want = ex ? 0x118 : 0x110;
    if (b.size < want || !rl_ipc_buf_read(&b, raw, want))
        return false;
    for (int i = 0; i < 4; i++)
        p[i] = (int32_t)le32(raw + 4 * i);
    p[4] = 0;
    if (ex) {
        p[4] = (int32_t)le32(raw + 16);
        memcpy(map, raw + 24, 256);
    } else {
        memcpy(map, raw + 16, 256);
    }
    return true;
}

static rl_result mgr_open_multi_common(rl_ipc_req *r, bool ex)
{
    int32_t p[5];
    uint8_t map[256] = { 0 };
    if (!read_multi(r, ex, p, map))
        return SF_RESULT_INVALID_HEADER;
    if (!valid_rate(p[0]))
        return CODEC_INVALID_SAMPLE_RATE;
    if (!valid_multi_channels(p[1]))
        return CODEC_INVALID_CHANNEL_COUNT;
    if (!valid_streams(p[2], p[3], p[1]))
        return CODEC_INVALID_NUMBER_OF_STREAMS;
    return push_decoder(r, rl_opus_open((uint32_t)p[0], (uint32_t)p[1], (uint32_t)p[2], (uint32_t)p[3], map),
                        (uint32_t)p[0], (uint32_t)p[1]);
}

static rl_result mgr_open_multi(rl_ipc_req *r) { return mgr_open_multi_common(r, false); }
static rl_result mgr_open_multi_ex(rl_ipc_req *r) { return mgr_open_multi_common(r, true); }

static rl_result mgr_size_multi_common(rl_ipc_req *r, bool ex)
{
    int32_t p[5], size;
    uint8_t map[256];
    if (!read_multi(r, ex, p, map))
        return SF_RESULT_INVALID_HEADER;
    rl_result rc = work_size_multi(p[0], p[1], p[2], p[3], (p[4] & 1) ? 5760 : 1920, &size);
    put32(rl_ipc_out(r, 4), (uint32_t)size);
    return rc;
}

static rl_result mgr_size_multi(rl_ipc_req *r) { return mgr_size_multi_common(r, false); }
static rl_result mgr_size_multi_ex(rl_ipc_req *r) { return mgr_size_multi_common(r, true); }

static const rl_ipc_cmd manager_cmds[] = {
    { 0, "OpenHardwareOpusDecoder", mgr_open },
    { 1, "GetWorkBufferSize", mgr_size },
    { 2, "OpenHardwareOpusDecoderForMultiStream", mgr_open_multi },
    { 3, "GetWorkBufferSizeForMultiStream", mgr_size_multi },
    { 4, "OpenHardwareOpusDecoderEx", mgr_open_ex },
    { 5, "GetWorkBufferSizeEx", mgr_size_ex },
    { 6, "OpenHardwareOpusDecoderForMultiStreamEx", mgr_open_multi_ex },
    { 7, "GetWorkBufferSizeForMultiStreamEx", mgr_size_multi_ex },
    { 8, "GetWorkBufferSizeExEx", mgr_size_ex },
    { 9, "GetWorkBufferSizeForMultiStreamExEx", mgr_size_multi_ex },
};

static const rl_ipc_class manager_class = {
    .name = "nn::codec::detail::IHardwareOpusDecoderManager",
    .cmds = manager_cmds,
    .ncmds = sizeof manager_cmds / sizeof manager_cmds[0],
    .pointer_buffer_size = 0x1000,
    .destroy = NULL,
};

rl_ipc_object *hwopus_manager_new(void) { return rl_ipc_object_new(&manager_class, NULL); }
