/**
 * @file smsmain.c
 * @brief The program of a lifted Master System or Game Gear ROM: the window
 * or headless run, the scripted input, frame hashes, PNG and raw frame
 * dumps.
 */
#include "smsmain.h"

#include "audio.h"
#include "core.h"

#include <relift/audio.h>
#include <relift/settings.h>
#include <relift/window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_NS 16688154ull
#define AUDIO_CHUNK 1024u
#define AUDIO_AHEAD (3u * SMS_AUDIO_RATE / 60u)

typedef struct host {
    const rl_sms_program *p;
    uint64_t frames;
    uint64_t stop_frames;
    bool stats;
    rl_window *win;
    const char *png;
    const char *dump_ram;
    const char *png_dir;
    uint64_t png_every;
    const char *raw_dir;
    uint64_t raw_from, raw_to;
    FILE *frame_log;
    FILE *state_log;
    FILE *insn_log;
    uint64_t insn_count;
    uint8_t *inputs;
    size_t input_count;
    uint64_t next_ns;
    bool quit;
    uint16_t px[SMS_VDP_W * SMS_VDP_H];
    uint8_t rgb[SMS_VDP_W * SMS_VDP_H * 3];
    uint32_t w, h;
    uint32_t last_crc;
    int code;
    bool audio;
    bool audio_ready;
    sms_audio mix;
    rl_audio_out *out;
    const char *wav;
    int16_t *wav_pcm;
    size_t wav_frames, wav_cap;
    uint64_t audio_frames;
    uint64_t audio_hash;
    int audio_peak;
} host;

static void audio_keep(host *H, const int16_t *pcm, size_t frames)
{
    for (size_t i = 0; i < frames * 2; i++) {
        uint16_t v = (uint16_t)pcm[i];
        H->audio_hash = (H->audio_hash ^ (v & 0xFFu)) * 0x100000001b3ull;
        H->audio_hash = (H->audio_hash ^ (v >> 8)) * 0x100000001b3ull;
        int a = pcm[i] < 0 ? -(int)pcm[i] : pcm[i];
        if (a > H->audio_peak)
            H->audio_peak = a;
    }
    H->audio_frames += frames;
    if (!H->wav)
        return;
    if (H->wav_frames + frames > H->wav_cap) {
        size_t cap = H->wav_cap ? H->wav_cap * 2 : 65536;
        while (cap < H->wav_frames + frames)
            cap *= 2;
        int16_t *n = realloc(H->wav_pcm, cap * 4);
        if (!n) {
            H->wav = NULL;
            return;
        }
        H->wav_pcm = n;
        H->wav_cap = cap;
    }
    memcpy(H->wav_pcm + H->wav_frames * 2, pcm, frames * 4);
    H->wav_frames += frames;
}

static void audio_drain(host *H, sms *m)
{
    if (!H->audio)
        return;
    if (!H->audio_ready) {
        sms_audio_init(&H->mix, m, SMS_AUDIO_RATE);
        H->audio_ready = true;
    }
    int16_t pcm[AUDIO_CHUNK * 2];
    size_t n;
    while ((n = sms_audio_take(&H->mix, m, m->cycles, pcm, AUDIO_CHUNK)) > 0) {
        audio_keep(H, pcm, n);
        if (H->out)
            rl_audio_write(H->out, pcm, (uint32_t)n);
    }
}

static void le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static bool wav_write(const host *H)
{
    uint32_t bytes = (uint32_t)(H->wav_frames * 4);
    uint8_t h[44] = "RIFFxxxxWAVEfmt \x10\0\0\0\1\0\2\0xxxxxxxx\4\0\x10\0data";
    le32(h + 4, 36 + bytes);
    le32(h + 24, SMS_AUDIO_RATE);
    le32(h + 28, SMS_AUDIO_RATE * 4);
    le32(h + 40, bytes);
    FILE *f = fopen(H->wav, "wb");
    bool ok = f && fwrite(h, 1, 44, f) == 44 && (!bytes || fwrite(H->wav_pcm, 1, bytes, f) == bytes);
    if (f && fclose(f) != 0)
        ok = false;
    return ok;
}

static uint32_t crc32(uint32_t c, const uint8_t *b, size_t n)
{
    c = ~c;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t fnv32(const uint8_t *b, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t h[8];
    be32(h, n);
    memcpy(h + 4, type, 4);
    uint32_t c = crc32(crc32(0, h + 4, 4), data, n);
    uint8_t t[4];
    be32(t, c);
    return fwrite(h, 1, 8, f) == 8 && (n == 0 || fwrite(data, 1, n, f) == n) && fwrite(t, 1, 4, f) == 4;
}

/** @brief Writes w by h RGB pixels as a PNG with stored (uncompressed) deflate blocks. */
static bool write_png(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h)
{
    size_t row = 1 + (size_t)w * 3, raw_n = row * h;
    uint8_t *raw = malloc(raw_n);
    size_t blocks = (raw_n + 65534) / 65535;
    uint8_t *z = malloc(2 + blocks * 5 + raw_n + 4);
    if (!raw || !z) {
        free(raw);
        free(z);
        return false;
    }
    for (uint32_t y = 0; y < h; y++) {
        raw[y * row] = 0;
        memcpy(raw + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    for (size_t off = 0; off < raw_n; off += 65535) {
        uint16_t n = (uint16_t)(raw_n - off < 65535 ? raw_n - off : 65535);
        z[o++] = off + n == raw_n;
        z[o++] = (uint8_t)n;
        z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n;
        z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + off, n);
        o += n;
    }
    be32(z + o, (b << 16) | a);
    o += 4;
    uint8_t ihdr[13] = {0};
    be32(ihdr, w);
    be32(ihdr + 4, h);
    ihdr[8] = 8;
    ihdr[9] = 2;
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
              chunk(f, "IDAT", z, (uint32_t)o) && chunk(f, "IEND", NULL, 0);
    if (f && fclose(f) != 0)
        ok = false;
    free(raw);
    free(z);
    return ok;
}

static void to_rgb(host *H, const sms *m)
{
    for (uint32_t i = 0; i < H->w * H->h; i++)
        sms_rgb(m, H->px[i], H->rgb + 3 * i);
}

static int on_frame(void *user, sms *m, uint64_t frame)
{
    host *H = user;
    H->frames = frame + 1;
    audio_drain(H, m);
    sms_screen(m, &H->w, &H->h);
    sms_picture(m, H->px);
    size_t n = (size_t)H->w * H->h;
    H->last_crc = crc32(0, (const uint8_t *)H->px, n * 2);
    if (H->frame_log)
        fprintf(H->frame_log, "%llu\t%08x\t%u\t%u\n", (unsigned long long)frame, H->last_crc, H->w, H->h);
    if (H->state_log) {
        uint8_t cram[64];
        for (unsigned i = 0; i < 32; i++) {
            cram[2 * i] = (uint8_t)m->vdp.cram[i];
            cram[2 * i + 1] = (uint8_t)(m->vdp.cram[i] >> 8);
        }
        fprintf(H->state_log, "%llu\t%08x\t%08x\t%08x\t%u\t%u\n", (unsigned long long)frame, fnv32(m->ram, sizeof m->ram),
                fnv32(m->vdp.vram, sizeof m->vdp.vram), fnv32(cram, sizeof cram), m->vdp.vcounter, m->vdp.hcounter);
    }
    if (H->raw_dir && frame >= H->raw_from && frame <= H->raw_to) {
        char path[4096];
        snprintf(path, sizeof path, "%s/f%06llu.bin", H->raw_dir, (unsigned long long)frame);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(H->px, 2, n, f);
            fclose(f);
        }
    }
    if (H->png_dir && H->frames % H->png_every == 0) {
        char path[4096];
        snprintf(path, sizeof path, "%s/frame%06llu.png", H->png_dir, (unsigned long long)H->frames);
        to_rgb(H, m);
        if (!write_png(path, H->rgb, H->w, H->h))
            rl_log(RL_LOG_WARN, "sms", "could not write %s", path);
    }
    if (H->win) {
        to_rgb(H, m);
        rl_window_present_rgb(H->win, H->rgb, H->w, H->h);
        if (H->out) {
            while (!__atomic_load_n(&H->quit, __ATOMIC_ACQUIRE) && rl_audio_queued(H->out) > AUDIO_AHEAD)
                rl_sleep_ns(1000000ull);
        } else {
            uint64_t t = rl_clock_ns();
            if (!H->next_ns || t > H->next_ns + 4 * FRAME_NS)
                H->next_ns = t;
            H->next_ns += FRAME_NS;
            if (H->next_ns > t)
                rl_sleep_ns(H->next_ns - t);
        }
    }
    if (H->stop_frames && H->frames >= H->stop_frames) {
        to_rgb(H, m);
        printf("frames %llu crc %08x ram %08x vram %08x\n", (unsigned long long)H->frames, H->last_crc,
               fnv32(m->ram, sizeof m->ram), fnv32(m->vdp.vram, sizeof m->vdp.vram));
        fflush(stdout);
        if (H->png && !write_png(H->png, H->rgb, H->w, H->h))
            fprintf(stderr, "relift-sms: could not write %s\n", H->png);
        if (H->dump_ram) {
            FILE *f = fopen(H->dump_ram, "wb");
            if (!f || fwrite(m->ram, 1, sizeof m->ram, f) != sizeof m->ram)
                fprintf(stderr, "relift-sms: could not write %s\n", H->dump_ram);
            if (f)
                fclose(f);
        }
        return 1;
    }
    return __atomic_load_n(&H->quit, __ATOMIC_ACQUIRE) ? 1 : 0;
}

static void on_window(void *user, rl_window *w, int event)
{
    (void)w;
    host *H = user;
    if (event == RL_WINDOW_CLOSE)
        __atomic_store_n(&H->quit, true, __ATOMIC_RELEASE);
}

static int run_body(void *arg)
{
    host *H = arg;
    const rl_window_backend *b = rl_window_backend_current();
    if (b && b->present_rgb) {
        uint32_t w = H->p->system == SMS_SYSTEM_GG ? 160 : 256, h = H->p->system == SMS_SYSTEM_GG ? 144 : 192;
        H->win = b->open(H->p->title && *H->p->title ? H->p->title : "relift", w * 3, h * 3);
    }
    if (H->audio && H->win) {
        H->out = rl_audio_open(SMS_AUDIO_RATE, 2);
        if (H->out)
            rl_audio_pause(H->out, false);
    }
    H->code = rl_sms_run(H->p, H->inputs, H->input_count, on_frame, H);
    rl_audio_close(H->out);
    H->out = NULL;
    if (H->win)
        b->close(H->win);
    if (H->stats) {
        const rl_sms_stats *s = rl_sms_stats_get();
        fprintf(stderr,
                "relift-sms: frames %llu, instructions %llu, irqs %llu, nmis %llu, calls %llu, redirects %llu, "
                "dispatches %llu, halts %llu\n",
                (unsigned long long)H->frames, (unsigned long long)s->insns, (unsigned long long)s->irqs,
                (unsigned long long)s->nmis, (unsigned long long)s->calls, (unsigned long long)s->redirects,
                (unsigned long long)s->dispatches, (unsigned long long)s->halts);
    }
    return H->code;
}

static const char *arg_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "relift-sms: %s needs a value\n", argv[*i]);
        exit(2);
    }
    return argv[++*i];
}

/** @brief Reads an input script: lines of a frame number and a hex mask; frames with no line keep the mask before. */
static int read_inputs(host *H, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned long long frame;
        unsigned mask;
        if (sscanf(line, "%llu %x", &frame, &mask) != 2 || frame > 10000000ull)
            continue;
        if (frame >= H->input_count) {
            size_t n = (size_t)frame + 1;
            uint8_t *x = realloc(H->inputs, n);
            if (!x) {
                fclose(f);
                return -1;
            }
            uint8_t fill = H->input_count ? x[H->input_count - 1] : 0;
            memset(x + H->input_count, fill, n - H->input_count);
            H->inputs = x;
            H->input_count = n;
        }
        H->inputs[frame] = (uint8_t)mask;
    }
    fclose(f);
    return 0;
}

int rl_sms_main(const rl_sms_program *p, int argc, char **argv)
{
    static host H;
    memset(&H, 0, sizeof H);
    H.p = p;
    H.audio = true;
    H.audio_hash = 0xcbf29ce484222325ull;
    bool headless = false;
    const char *log = NULL;
    rl_settings_args(argc, argv);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--headless"))
            headless = true;
        else if (!strcmp(a, "--frames"))
            H.stop_frames = strtoull(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--input")) {
            const char *path = arg_value(argc, argv, &i);
            if (read_inputs(&H, path) != 0) {
                fprintf(stderr, "relift-sms: cannot read %s\n", path);
                return 2;
            }
        } else if (!strcmp(a, "--frame-log"))
            log = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--state-log")) {
            const char *path = arg_value(argc, argv, &i);
            if (!(H.state_log = fopen(path, "w"))) {
                fprintf(stderr, "relift-sms: cannot write %s\n", path);
                return 2;
            }
        } else if (!strcmp(a, "--insn-log")) {
            const char *path = arg_value(argc, argv, &i);
            if (!(H.insn_log = fopen(path, "w"))) {
                fprintf(stderr, "relift-sms: cannot write %s\n", path);
                return 2;
            }
        } else if (!strcmp(a, "--insn-count"))
            H.insn_count = strtoull(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--png"))
            H.png = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--dump-ram"))
            H.dump_ram = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--png-dir"))
            H.png_dir = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--png-every"))
            H.png_every = strtoull(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--raw-dir"))
            H.raw_dir = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--raw-from"))
            H.raw_from = strtoull(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--raw-to"))
            H.raw_to = strtoull(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--wav"))
            H.wav = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--no-audio"))
            H.audio = false;
        else if (!strcmp(a, "--stats"))
            H.stats = true;
        else if (!strncmp(a, "--relift-", 9))
            i += strchr(a, '=') ? 0 : 1;
        else {
            fprintf(stderr, "relift-sms: unknown option %s\n", a);
            return 2;
        }
    }
    if (H.png_dir && !H.png_every)
        H.png_every = 1;
    if (log && !(H.frame_log = fopen(log, "w"))) {
        fprintf(stderr, "relift-sms: cannot write %s\n", log);
        return 2;
    }
    if (H.insn_log)
        rl_sms_trace(H.insn_log, H.insn_count ? H.insn_count : UINT64_MAX);
    if (headless)
        rl_settings_set("video", "headless");
    rl_window_set_events(on_window, &H);
    int r = rl_window_main(run_body, &H);
    rl_window_set_events(NULL, NULL);
    if (H.audio && (H.wav || H.stop_frames))
        printf("audio frames %llu peak %d hash %016llx\n", (unsigned long long)H.audio_frames, H.audio_peak,
               (unsigned long long)H.audio_hash);
    if (H.wav && !wav_write(&H)) {
        fprintf(stderr, "relift-sms: could not write %s\n", H.wav);
        r = r ? r : 1;
    }
    free(H.wav_pcm);
    if (H.frame_log)
        fclose(H.frame_log);
    if (H.state_log)
        fclose(H.state_log);
    if (H.insn_log)
        fclose(H.insn_log);
    free(H.inputs);
    return r;
}
