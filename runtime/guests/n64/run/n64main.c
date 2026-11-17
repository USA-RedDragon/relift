/**
 * @file n64main.c
 * @brief The program of a lifted Nintendo 64 ROM: the machine, the CPU, the
 * window or headless run, framebuffer hashes and PNG dumps.
 */
#define _POSIX_C_SOURCE 200809L
#include "n64main.h"
#include "cpu.h"

#include "hostlib.h"

#include <relift/audio.h>
#include <relift/ports.h>
#include <relift/settings.h>
#include <relift/window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AUDIO_CHUNK 2048u
#define MAX_W 640u
#define MAX_H 576u

typedef struct host {
    const rl_n64_program *p;
    rl_n64_run run;
    uint32_t frames;
    uint32_t stop_frames;
    bool stop;
    bool stats;
    bool quit;
    rl_window *win;
    const char *png;
    const char *raw_png;
    const char *png_dir;
    uint32_t png_every;
    const char *dump;
    uint64_t next_ns;
    uint64_t last_cycles;
    bool audio;
    rl_audio_out *out;
    const char *wav;
    int16_t *wav_pcm;
    size_t wav_frames;
    size_t wav_cap;
    uint64_t audio_hash;
    uint64_t audio_frames;
    int audio_peak;
    uint8_t *rgb;
    size_t cap;
    uint32_t w, h;
    bool have;
    bool framed;
    char save[4096];
} host;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t fnv(uint64_t h, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
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

static void audio_keep(host *H, const int16_t *pcm, size_t frames)
{
    for (size_t i = 0; i < frames * 2u; i++) {
        uint16_t v = (uint16_t)pcm[i];
        H->audio_hash = (H->audio_hash ^ (v & 0xFFu)) * 0x100000001B3ull;
        H->audio_hash = (H->audio_hash ^ (v >> 8)) * 0x100000001B3ull;
        int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > H->audio_peak)
            H->audio_peak = a;
    }
    H->audio_frames += frames;
    if (!H->wav)
        return;
    if (H->wav_frames + frames > H->wav_cap) {
        size_t cap = H->wav_cap ? H->wav_cap * 2u : 65536u;
        while (cap < H->wav_frames + frames)
            cap *= 2u;
        int16_t *n = realloc(H->wav_pcm, cap * 4u);
        if (!n) {
            H->wav = NULL;
            return;
        }
        H->wav_pcm = n;
        H->wav_cap = cap;
    }
    memcpy(H->wav_pcm + H->wav_frames * 2u, pcm, frames * 4u);
    H->wav_frames += frames;
}

/** @brief Hands the audio the machine made since the last call to the output, the hash and the capture. */
static void audio_drain(host *H)
{
    int16_t pcm[AUDIO_CHUNK * 2u];
    size_t n;
    while ((n = rl_n64_audio_take(H->run.m, pcm, AUDIO_CHUNK)) > 0) {
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
    uint32_t bytes = (uint32_t)(H->wav_frames * 4u);
    uint8_t h[44] = "RIFFxxxxWAVEfmt \x10\0\0\0\1\0\2\0xxxxxxxx\4\0\x10\0data";
    le32(h + 4, 36u + bytes);
    le32(h + 24, RL_N64_AUDIO_RATE);
    le32(h + 28, RL_N64_AUDIO_RATE * 4u);
    le32(h + 40, bytes);
    FILE *f = fopen(H->wav, "wb");
    bool ok = f && fwrite(h, 1, 44, f) == 44 && (!bytes || fwrite(H->wav_pcm, 1, bytes, f) == bytes);
    if (f && fclose(f) != 0)
        ok = false;
    return ok;
}

/** @brief Takes the picture VI outputs now, growing the picture buffer to its size. */
static void scan(host *H)
{
    H->have = rl_n64_scanout(H->run.m, H->rgb, H->cap, &H->w, &H->h);
    size_t need = (size_t)H->w * H->h;
    if (H->have || need <= H->cap)
        return;
    uint8_t *grown = realloc(H->rgb, need * 3u);
    if (!grown)
        return;
    H->rgb = grown;
    H->cap = need;
    H->have = rl_n64_scanout(H->run.m, H->rgb, H->cap, &H->w, &H->h);
}

/** @brief The picture of the last frame: the one the frame hook took, else the picture now. */
static void last_picture(host *H)
{
    if (!H->framed)
        scan(H);
}

static void save_now(host *H)
{
    if (H->save[0] && rl_n64_save_write(H->run.m, H->save) < 0)
        rl_log(RL_LOG_WARN, "n64", "could not write the save %s", H->save);
}

static void print_stats(host *H)
{
    const rl_n64_stats *s = rl_n64_get_stats(H->run.m);
    const rl_mips_stats *a = rl_mips_get_stats();
    static const rl_rsp_stats none;
    const rl_rsp_stats *q = H->run.rsp ? rl_rsp_get_stats(H->run.rsp) : &none;
    fprintf(stderr,
            "relift-n64: frames %u, cycles %llu, interpreted 0, pi dmas %llu, sp dmas %llu, binds %llu, checks %llu, "
            "mismatches %llu, invalidations %llu, rsp starts %llu, exceptions %llu, slow paths %llu, returns %llu, "
            "redirects %llu, misses %llu\n",
            H->frames, (unsigned long long)rl_sched_cycles(H->run.k), (unsigned long long)s->pi_dmas,
            (unsigned long long)s->sp_dmas, (unsigned long long)s->binds, (unsigned long long)s->checks,
            (unsigned long long)s->mismatches, (unsigned long long)s->invalidations,
            (unsigned long long)s->rsp_starts, (unsigned long long)a->exceptions, (unsigned long long)a->slow,
            (unsigned long long)a->returns, (unsigned long long)a->redirects, (unsigned long long)a->misses);
    fprintf(stderr,
            "relift-n64: rsp interpreted 0, cycles %llu, slices %llu, breaks %llu, binds %llu, invalidations %llu, "
            "misses %llu, dp submits %llu, pipelines unmet %llu\n",
            (unsigned long long)q->cycles, (unsigned long long)q->slices, (unsigned long long)s->rsp_breaks,
            (unsigned long long)q->binds, (unsigned long long)q->invalidations, (unsigned long long)q->misses,
            (unsigned long long)s->dp_submits, (unsigned long long)q->pipe_unmet);
    fprintf(stderr, "relift-n64: icache fills %llu, dcache fills %llu, dcache write backs %llu\n",
            (unsigned long long)a->icache_misses, (unsigned long long)a->dcache_misses,
            (unsigned long long)a->dcache_writebacks);
}

static void report(host *H, rl_ctx *c)
{
    char buf[1024];
    rl_mips_cpu()->describe(c, buf, sizeof buf);
    last_picture(H);
    uint64_t fb = H->have ? fnv(0xCBF29CE484222325ull, H->rgb, (size_t)H->w * H->h * 3) : 0;
    printf("frames %u fb %ux%u hash %016llx\n", H->frames, H->have ? H->w : 0, H->have ? H->h : 0,
           (unsigned long long)fb);
    printf("regs %s\n", buf);
    printf("vi origin=%06x width=%u control=%04x\n", rl_n64_vi_reg(H->run.m, 1), rl_n64_vi_reg(H->run.m, 2),
           rl_n64_vi_reg(H->run.m, 0));
    const uint8_t *sp = rl_n64_spmem(H->run.m);
    uint8_t *rdram = malloc(RL_N64_RDRAM_SIZE);
    if (!rdram)
        return;
    rl_n64_rdram_get_cpu(H->run.m, 0, rdram, RL_N64_RDRAM_SIZE);
    printf("mem rdram=%016llx dmem=%016llx imem=%016llx\n",
           (unsigned long long)fnv(0xCBF29CE484222325ull, rdram, RL_N64_RDRAM_SIZE),
           (unsigned long long)fnv(0xCBF29CE484222325ull, sp, 0x1000),
           (unsigned long long)fnv(0xCBF29CE484222325ull, sp + 0x1000, 0x1000));
    fflush(stdout);
    if (H->dump) {
        const struct {
            const char *ext;
            const uint8_t *p;
            size_t n;
        } parts[3] = {{"rdram", rdram, RL_N64_RDRAM_SIZE}, {"dmem", sp, 0x1000}, {"imem", sp + 0x1000, 0x1000}};
        for (int i = 0; i < 3; i++) {
            char path[4096];
            snprintf(path, sizeof path, "%s.%s", H->dump, parts[i].ext);
            FILE *f = fopen(path, "wb");
            if (!f || fwrite(parts[i].p, 1, parts[i].n, f) != parts[i].n)
                fprintf(stderr, "relift-n64: could not write %s\n", path);
            if (f)
                fclose(f);
        }
    }
    free(rdram);
}

static void on_frame(void *user, rl_n64 *m)
{
    host *H = user;
    H->frames++;
    rl_ports_script_frame(H->frames);
    if (H->save[0] && rl_n64_save_poll(m, H->save, RL_N64_HZ) < 0)
        rl_log(RL_LOG_WARN, "n64", "could not write the save %s", H->save);
    if (H->png_dir || H->png || H->win) {
        scan(H);
        H->framed = H->have;
    }
    if (H->png_dir && H->frames % H->png_every == 0) {
        char path[4096];
        snprintf(path, sizeof path, "%s/frame%06u.png", H->png_dir, H->frames);
        if (H->have && !write_png(path, H->rgb, H->w, H->h))
            rl_log(RL_LOG_WARN, "n64", "could not write %s", path);
    }
    audio_drain(H);
    uint64_t cycles = rl_sched_cycles(H->run.k);
    uint64_t span = (cycles - H->last_cycles) * 1000000000ull / RL_N64_HZ;
    H->last_cycles = cycles;
    if (H->win) {
        if (H->have)
            rl_window_present_rgb(H->win, H->rgb, H->w, H->h);
        uint64_t t = now_ns();
        if (!H->next_ns || t > H->next_ns + 4 * span)
            H->next_ns = t;
        H->next_ns += span;
        if (H->next_ns > t)
            rl_sleep_ns(H->next_ns - t);
    }
    if ((H->stop_frames && H->frames >= H->stop_frames) || __atomic_load_n(&H->quit, __ATOMIC_ACQUIRE)) {
        H->stop = true;
        rl_sched_break(H->run.k);
    }
}

static void finish(host *H)
{
    if (H->png) {
        last_picture(H);
        if (H->have && !write_png(H->png, H->rgb, H->w, H->h))
            fprintf(stderr, "relift-n64: could not write %s\n", H->png);
    }
    if (H->raw_png) {
        static uint8_t raw[MAX_W * MAX_H * 3];
        uint32_t w = 0, h = 0;
        if (rl_n64_framebuffer(H->run.m, raw, &w, &h) && !write_png(H->raw_png, raw, w, h))
            fprintf(stderr, "relift-n64: could not write %s\n", H->raw_png);
    }
    audio_drain(H);
    if (H->wav && !wav_write(H))
        fprintf(stderr, "relift-n64: could not write %s\n", H->wav);
    if (H->audio && (H->wav || H->stop_frames))
        fprintf(stderr, "relift-n64: audio: %llu frames at %u Hz, peak %d, hash %016llx\n",
                (unsigned long long)H->audio_frames, RL_N64_AUDIO_RATE, H->audio_peak,
                (unsigned long long)H->audio_hash);
    free(H->wav_pcm);
    H->wav_pcm = NULL;
    save_now(H);
    if (H->stats)
        print_stats(H);
#ifdef RL_N64_TRACE
    rl_n64_trace_write();
#endif
}

static void on_check(void *user, rl_ctx *c)
{
    host *H = user;
    if (!H->stop)
        return;
    if (H->stop_frames)
        report(H, c);
    finish(H);
    rl_process_exit(0);
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
    rl_thread_bind(H->run.t);
    const rl_window_backend *b = rl_window_backend_current();
    if (b && b->present_rgb)
        H->win = b->open(H->p->title && *H->p->title ? H->p->title : "relift", 640, 480);
    if (H->audio && H->win) {
        H->out = rl_audio_open(RL_N64_AUDIO_RATE, 2);
        if (H->out)
            rl_audio_pause(H->out, false);
    }
    int r = rl_n64_run_start(&H->run);
    finish(H);
    rl_audio_close(H->out);
    H->out = NULL;
    if (H->win)
        b->close(H->win);
    return r == RL_OK ? 0 : 1;
}

static const char *arg_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "relift-n64: %s needs a value\n", argv[*i]);
        exit(2);
    }
    return argv[++*i];
}

int rl_n64_main(const rl_n64_program *p, int argc, char **argv)
{
    static host H;
    memset(&H, 0, sizeof H);
    H.p = p;
    const char *save = NULL;
    bool no_save = false, headless = false, no_audio = false;
    rl_n64_declare_settings(p->picture);
    rl_settings_args(argc, argv);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--headless"))
            headless = true;
        else if (!strcmp(a, "--frames"))
            H.stop_frames = (uint32_t)strtoul(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--png"))
            H.png = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--raw-png"))
            H.raw_png = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--wav"))
            H.wav = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--no-audio"))
            no_audio = true;
        else if (!strcmp(a, "--png-dir"))
            H.png_dir = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--png-every"))
            H.png_every = (uint32_t)strtoul(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--dump"))
            H.dump = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--save"))
            save = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--no-save"))
            no_save = true;
        else if (!strcmp(a, "--stats"))
            H.stats = true;
        else if (!strncmp(a, "--relift-", 9))
            i += strchr(a, '=') ? 0 : 1;
        else {
            fprintf(stderr, "relift-n64: unknown option %s\n", a);
            return 2;
        }
    }
    H.audio = !no_audio;
    if (H.png_dir && !H.png_every)
        H.png_every = 1;
    static const rl_guest_desc guest = {.name = "n64", .page_size = 0x1000, .address_bits = 32, .tick_hz = RL_N64_HZ};
    if (rl_core_init(&guest, rl_mips_cpu()) != 0)
        return 1;
    rl_ports_declare(rl_n64_ports, rl_n64_ports_count);
    rl_ports_bind_defaults();
    rl_n64 *m = rl_n64_create(p->rom, p->rom_len);
    if (!m || rl_n64_code_init(m, p->units, p->unit_count, p->code, p->code_count) != 0) {
        fprintf(stderr, "relift-n64: the lifted units do not match this runtime\n");
        return 1;
    }
    if (rl_n64_run_init(&H.run, m, p->cic, p->entry) != 0) {
        fprintf(stderr, "relift-n64: cannot create the machine\n");
        return 1;
    }
    H.run.dump = H.dump;
#ifdef RL_N64_TRACE
    const char *trace = getenv("RELIFT_N64_TRACE");
    if (trace && *trace)
        rl_n64_trace_calls(trace);
#endif
    char why[256];
    if (rl_n64_run_rsp(&H.run, p->rsp, p->rsp_count, why, sizeof why) != 0) {
        fprintf(stderr, "relift-n64: %s\n", why);
        return 1;
    }
    rl_n64_pad_use_ports(m, true);
    if (!no_save) {
        if (save)
            snprintf(H.save, sizeof H.save, "%s", save);
        else if (p->rom_path && *p->rom_path && !rl_n64_save_path(H.save, sizeof H.save, p->rom_path))
            H.save[0] = 0;
    }
    if (H.save[0] && rl_n64_save_load(m, H.save) < 0)
        rl_log(RL_LOG_WARN, "n64", "a save of %s does not fit this cartridge; it is ignored", H.save);
    rl_n64_set_frame_hook(m, on_frame, &H);
    void *rdp = NULL;
    if (p->renderer) {
        char rdp_why[160];
        rdp = p->renderer->create(m, rl_hostlib_vulkan_gipa(), rdp_why, sizeof rdp_why);
        if (rdp)
            rl_n64_set_rdp(m, p->renderer->ops, rdp);
        else
            rl_log(RL_LOG_WARN, "n64", "the RDP draws nothing: %s", rdp_why);
    }
    H.run.check = on_check;
    H.run.check_user = &H;
    if (headless)
        rl_settings_set("video", "headless");
    rl_window_set_events(on_window, &H);
    int r = rl_window_main(run_body, &H);
    rl_window_set_events(NULL, NULL);
    rl_n64_run_free(&H.run);
    rl_n64_destroy(m);
    free(H.rgb);
    if (rdp)
        p->renderer->destroy(rdp);
    return r;
}
