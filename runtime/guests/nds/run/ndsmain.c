/**
 * @file ndsmain.c
 * @brief The program of a lifted Nintendo DS cartridge: the machine, both
 * CPUs, the window (both screens in the video.nds_layout layout and the
 * sound controller's output) or the headless run, its report, frame hashes and PNG dumps
 * of both screens (the top one above the bottom one, 256 x 384).
 */
#include "cpu.h"
#include "play.h"

#include <relift/audio.h>
#include <relift/settings.h>
#include <relift/window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** @brief Most --peek options and --png-frames entries. */
#define PEEKS 16u
#define PNG_FRAMES 64u
/** @brief Width and height of a dumped frame: both screens stacked. */
#define W RL_NDS_SCREEN_W
#define H_PX (2u * RL_NDS_SCREEN_H)
/** @brief A frame's length in nanoseconds (59.8261 Hz), the pace without audio. */
#define FRAME_NS 16715279ull
/** @brief Frames of sound queued in the host before a frame waits (three frames' worth). */
#define AUDIO_AHEAD (3u * RL_NDS_AUDIO_RATE / 60u)
#define AUDIO_CHUNK 1024u

typedef struct host {
    rl_nds_run run;
    rl_event save_ev;
    rl_event max_ev;
    char save[4096];
    bool stats;
    bool no_save;
    uint32_t peek[PEEKS];
    uint32_t peeks;
    /** @brief --png: the last frame at the end of the run. */
    const char *png;
    /** @brief --png-dir with --png-every or --png-frames: frames written as DIR/frameNNNNNN.png. */
    const char *png_dir;
    uint32_t png_every;
    uint32_t png_frames[PNG_FRAMES];
    uint32_t png_frame_count;
    /** @brief --frames: stop after this many frames (0: never). */
    uint32_t stop_frames;
    /** @brief --until-debug: the run ends when the debug port prints a line that starts with it. */
    const char *stop_debug;
    /** @brief Frames the frame hook saw. */
    uint32_t frames;
    /** @brief --frame-log: one line per frame on stdout (number, ARM9 tick, frame hash, the --peek words). */
    bool frame_log;
    /** @brief The window, its layout and picture, the host's audio and the pace without audio. */
    rl_window *win;
    rl_nds_layout layout;
    uint8_t rgb[2 * RL_NDS_SCREEN_W * RL_NDS_SCREEN_H * 3];
    rl_audio_out *out;
    uint64_t next_ns;
    /** @brief The window was closed. */
    bool quit;
    bool headless;
} host;

static host H;

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

/** @brief Writes a frame (rl_nds_frame) as an RGB PNG with stored (uncompressed) deflate blocks. */
static bool write_png(const char *path, const uint32_t *pixels)
{
    enum { ROW = 1 + W * 3, RAW = ROW * H_PX };
    static uint8_t raw[RAW];
    static uint8_t rgb[W * H_PX * 3];
    rl_nds_frame_rgb(pixels, rgb);
    for (uint32_t y = 0; y < H_PX; y++) {
        raw[y * ROW] = 0;
        memcpy(raw + y * ROW + 1, rgb + y * W * 3, W * 3);
    }
    size_t blocks = (RAW + 65534) / 65535;
    size_t zn = 2 + blocks * 5 + RAW + 4;
    uint8_t *z = malloc(zn);
    if (!z)
        return false;
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < RAW; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    for (size_t off = 0; off < RAW; off += 65535) {
        uint16_t n = (uint16_t)(RAW - off < 65535 ? RAW - off : 65535);
        z[o++] = off + n == RAW;
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
    be32(ihdr, W);
    be32(ihdr + 4, H_PX);
    ihdr[8] = 8;
    ihdr[9] = 2;
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
              chunk(f, "IDAT", z, (uint32_t)o) && chunk(f, "IEND", NULL, 0);
    if (f && fclose(f) != 0)
        ok = false;
    free(z);
    return ok;
}

/** @brief True when frame n is in --png-frames. */
static bool listed(uint32_t n)
{
    for (uint32_t i = 0; i < H.png_frame_count; i++)
        if (H.png_frames[i] == n)
            return true;
    return false;
}

/** @brief Hands the sound made so far to the host, then waits while three frames of it are queued. */
static void audio_pace(void)
{
    int16_t pcm[AUDIO_CHUNK * 2];
    uint32_t n;
    while ((n = rl_nds_audio_take(H.run.m, pcm, AUDIO_CHUNK)) > 0)
        rl_audio_write(H.out, pcm, n);
    while (!__atomic_load_n(&H.quit, __ATOMIC_ACQUIRE) && rl_audio_queued(H.out) > AUDIO_AHEAD)
        rl_sleep_ns(1000000ull);
}

/** @brief Shows the frame and keeps the run at the console's pace: by the audio queue, else by the clock. */
static void present(const uint32_t *pixels)
{
    uint32_t w, h;
    rl_nds_layout_size(H.layout, &w, &h);
    rl_nds_layout_rgb(H.layout, pixels, H.rgb);
    rl_window_present_rgb(H.win, H.rgb, w, h);
    if (H.out) {
        audio_pace();
        return;
    }
    uint64_t t = rl_clock_ns();
    if (!H.next_ns || t > H.next_ns + 4 * FRAME_NS)
        H.next_ns = t;
    H.next_ns += FRAME_NS;
    if (H.next_ns > t)
        rl_sleep_ns(H.next_ns - t);
}

/** @brief The frame hook: the window, PNG dumps and the --frames stop. */
static void on_frame(void *user, const uint32_t *pixels, uint64_t when)
{
    (void)user;
    H.frames++;
    if (H.win)
        present(pixels);
    if (__atomic_load_n(&H.quit, __ATOMIC_ACQUIRE))
        rl_process_exit(0);
    if (H.frame_log) {
        printf("F %u tick=%llu h=%016llx", H.frames, (unsigned long long)when, (unsigned long long)rl_nds_frame_hash(pixels));
        for (uint32_t i = 0; i < H.peeks; i++)
            printf(" %08x", rl_nds_read(H.run.m, RL_NDS_ARM9, H.peek[i], 4));
        putchar('\n');
    }
    if ((H.png_every && H.frames % H.png_every == 0) || listed(H.frames)) {
        char path[4096];
        snprintf(path, sizeof path, "%s/frame%06u.png", H.png_dir ? H.png_dir : ".", H.frames);
        if (!write_png(path, pixels))
            fprintf(stderr, "relift-nds: could not write %s\n", path);
    }
    if (H.stop_frames && H.frames >= H.stop_frames)
        rl_process_exit(0);
}

/** @brief The debug port hook: the --until-debug stop. */
static void on_debug(void *user, const char *line)
{
    (void)user;
    if (!strncmp(line, H.stop_debug, strlen(H.stop_debug))) {
        fflush(stdout);
        rl_process_exit(0);
    }
}

/** @brief The end of a run: --png of the last frame. */
static void finish(void)
{
    const uint32_t *px = H.run.m ? rl_nds_frame(H.run.m) : NULL;
    if (H.png && px && !write_png(H.png, px))
        fprintf(stderr, "relift-nds: could not write %s\n", H.png);
}

static void print_cpu(const rl_nds_arm *k)
{
    const rl_nds_arm_stats *s = &k->stats;
    fprintf(stderr, "relift-nds: %s tick %llu, %s, swis %llu (%llu unknown), irqs %llu, variants %llu, unfound %llu, colds %llu, compares %llu\n",
            k->id == RL_NDS_ARM9 ? "arm9" : "arm7", (unsigned long long)rl_sched_now(k->k),
            k->stopped == 1 ? "halted" : k->stopped == 2 ? "stopped" : "running", (unsigned long long)s->swis,
            (unsigned long long)s->unknown_swis, (unsigned long long)s->irqs, (unsigned long long)s->variants,
            (unsigned long long)s->unfound, (unsigned long long)s->colds, (unsigned long long)s->compares);
    const rl_ctx *c = k->c;
    fprintf(stderr, "relift-nds: %s pc %08x (last sync), cpsr %08x, r0 %08x r1 %08x r2 %08x r3 %08x r4 %08x r5 %08x sp %08x lr %08x\n",
            k->id == RL_NDS_ARM9 ? "arm9" : "arm7", c->pc, rl_arm32_cpsr(c), c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5],
            c->r[13], c->r[14]);
}

/** @brief The end of a run: the frame count and hash and the peeked words on stdout, the counters on stderr. */
static void report(void)
{
    finish();
    const uint32_t *px = rl_nds_frame(H.run.m);
    printf("frames %llu hash %016llx\n", (unsigned long long)rl_nds_frames(H.run.m), (unsigned long long)(px ? rl_nds_frame_hash(px) : 0));
    for (uint32_t i = 0; i < H.peeks; i++)
        printf("peek 0x%08x = 0x%08x\n", H.peek[i], rl_nds_read(H.run.m, RL_NDS_ARM9, H.peek[i], 4));
    fflush(stdout);
    if (!H.stats)
        return;
    print_cpu(&H.run.cpu[RL_NDS_ARM9]);
    print_cpu(&H.run.cpu[RL_NDS_ARM7]);
    const rl_arm32_stats *a = rl_arm32_get_stats();
    const rl_sched_stats *s = rl_sched_get_stats(H.run.s);
    fprintf(stderr, "relift-nds: returns %llu, redirects %llu, misses %llu, orders %llu (%llu waited), switches %llu\n",
            (unsigned long long)a->returns, (unsigned long long)a->redirects, (unsigned long long)a->misses,
            (unsigned long long)s->orders, (unsigned long long)s->order_waits, (unsigned long long)s->switches);
}

/** @brief Writes the save now when it changed. */
static void save_now(void)
{
    if (H.save[0] && rl_nds_save_dirty(H.run.m) && rl_nds_save_write(H.run.m, H.save) < 0)
        rl_log(RL_LOG_WARN, "nds", "could not write the save %s", H.save);
}

/**
 * @brief The --max-ticks bound as an event, so that it ends a run whose
 * CPUs sit in halts and never reach a sync point's tick check.
 */
static void max_tick(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    (void)e;
    fprintf(stderr, "relift-nds: the run passed tick %llu (--max-ticks)\n", (unsigned long long)when);
    rl_process_exit(RL_EXIT_SOFTWARE);
}

/** @brief The program turned the console off: the run ends with status 0. */
static void power_off(void *user, uint64_t when)
{
    (void)user;
    fprintf(stderr, "relift-nds: the program turned the console off at tick %llu\n", (unsigned long long)when);
    rl_process_exit(0);
}

/** @brief Once per emulated second: writes a save that has been quiet for a second. */
static void save_tick(rl_sched *s, rl_event *e, uint64_t when)
{
    if (H.save[0] && rl_nds_save_poll(H.run.m, H.save, RL_NDS_TICK_HZ) < 0)
        rl_log(RL_LOG_WARN, "nds", "could not write the save %s", H.save);
    rl_sched_post(s, e, when + RL_NDS_TICK_HZ);
}

/** @brief The clock's start: the rtc.time setting ("YYYY-MM-DD HH:MM:SS") or the host's local time. */
static void start_clock(rl_nds *m)
{
    rl_nds_time t = {2000, 1, 1, 0, 0, 0};
    unsigned y, mo, d, h, mi, s;
    const char *pin = rl_setting_str("rtc.time");
    if (*pin && sscanf(pin, "%u-%u-%u %u:%u:%u", &y, &mo, &d, &h, &mi, &s) == 6) {
        t = (rl_nds_time){(uint16_t)y, (uint8_t)mo, (uint8_t)d, (uint8_t)h, (uint8_t)mi, (uint8_t)s};
    } else {
        time_t now = time(NULL);
        const struct tm *lt = localtime(&now);
        if (lt)
            t = (rl_nds_time){(uint16_t)(lt->tm_year + 1900), (uint8_t)(lt->tm_mon + 1), (uint8_t)lt->tm_mday,
                              (uint8_t)lt->tm_hour, (uint8_t)lt->tm_min, (uint8_t)lt->tm_sec};
    }
    rl_nds_set_time(m, &t);
}

static void on_exit_code(uint32_t code)
{
    save_now();
    report();
    fflush(stderr);
    _Exit((int)code);
}

static const char *arg_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "relift-nds: %s needs a value\n", argv[*i]);
        exit(2);
    }
    return argv[++*i];
}

/** @brief Reads the options of argv from index first on; returns 0, or 2 for an unknown one. */
static int options(int argc, char **argv, int first, uint64_t *max_ticks)
{
    for (int i = first; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--headless")) {
            H.headless = true;
            continue;
        }
        if (!strcmp(a, "--no-save")) {
            H.no_save = true;
            continue;
        }
        if (!strcmp(a, "--save")) {
            snprintf(H.save, sizeof H.save, "%s", arg_value(argc, argv, &i));
            continue;
        }
        if (!strcmp(a, "--max-ticks"))
            *max_ticks = strtoull(arg_value(argc, argv, &i), NULL, 0);
        else if (!strcmp(a, "--peek")) {
            uint32_t at = (uint32_t)strtoul(arg_value(argc, argv, &i), NULL, 0);
            if (H.peeks < PEEKS)
                H.peek[H.peeks++] = at;
        } else if (!strcmp(a, "--stats"))
            H.stats = true;
        else if (!strcmp(a, "--frame-log"))
            H.frame_log = true;
        else if (!strcmp(a, "--until-debug"))
            H.stop_debug = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--frames"))
            H.stop_frames = (uint32_t)strtoul(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--png"))
            H.png = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--png-dir"))
            H.png_dir = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--png-every"))
            H.png_every = (uint32_t)strtoul(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--png-frames")) {
            for (const char *v = arg_value(argc, argv, &i); *v && H.png_frame_count < PNG_FRAMES;) {
                char *end;
                unsigned long n = strtoul(v, &end, 10);
                if (end == v) {
                    v++;
                    continue;
                }
                H.png_frames[H.png_frame_count++] = (uint32_t)n;
                v = end;
            }
        }
        else if (!strncmp(a, "--relift-", 9))
            i += strchr(a, '=') ? 0 : 1;
        else {
            fprintf(stderr, "relift-nds: unknown option %s\n", a);
            return 2;
        }
    }
    return 0;
}

static void on_window(void *user, rl_window *w, int event)
{
    (void)user;
    (void)w;
    if (event == RL_WINDOW_CLOSE)
        __atomic_store_n(&H.quit, true, __ATOMIC_RELEASE);
}

/** @brief The run, on its own thread when a window backend runs the event loop on the caller's. */
static int run_body(void *arg)
{
    const rl_nds_program *p = arg;
    const rl_window_backend *b = rl_window_backend_current();
    if (b && b->present_rgb) {
        uint32_t w, h;
        rl_nds_layout_size(H.layout, &w, &h);
        H.win = b->open(p->title && *p->title ? p->title : "relift", w * 2, h * 2);
    }
    if (H.win) {
        rl_window_set_fit(H.win, !rl_setting_bool("video.nds_integer_scale"));
        H.out = rl_audio_open(RL_NDS_AUDIO_RATE, 2);
        if (H.out)
            rl_audio_pause(H.out, false);
    }
    if (H.win || H.png_every || H.png_frame_count || H.stop_frames || H.frame_log || getenv("RELIFT_INPUT_SCRIPT"))
        rl_nds_set_frame_hook(H.run.m, on_frame, NULL);
    int r = rl_nds_run_start(&H.run);
    rl_audio_close(H.out);
    H.out = NULL;
    if (H.win)
        b->close(H.win);
    H.win = NULL;
    return r;
}

int rl_nds_main(const rl_nds_program *p, int argc, char **argv)
{
    memset(&H, 0, sizeof H);
    uint64_t max_ticks = 0;
    rl_nds_declare_settings();
    rl_settings_args(argc, argv);
    if (options((int)p->arg_count, (char **)p->args, 0, &max_ticks) != 0 || options(argc, argv, 1, &max_ticks) != 0)
        return 2;
    if (H.png_dir && !H.png_every && !H.png_frame_count)
        H.png_every = 1;
    static const rl_guest_desc guest = {.name = "nds", .page_size = 0x1000, .address_bits = 32, .tick_hz = RL_NDS_TICK_HZ};
    if (rl_core_init(&guest, rl_arm32_cpu()) != 0)
        return 1;
    if (rl_nds_run_init(&H.run, p) != 0) {
        fprintf(stderr, "relift-nds: cannot start %s\n", p->title && *p->title ? p->title : "the cartridge");
        return 1;
    }
    H.run.max_ticks = max_ticks;
    H.layout = rl_nds_layout_setting();
    if (H.stop_debug)
        rl_nds_set_debug_hook(H.run.m, on_debug, NULL);
    start_clock(H.run.m);
    if (rl_nds_save_set(H.run.m, rl_setting_str("save.type")) != 0)
        rl_log(RL_LOG_WARN, "nds", "unknown save.type %s; the chip is chosen from the game's commands", rl_setting_str("save.type"));
    if (!H.no_save) {
        const char *name = p->title;
        if (!H.save[0] && name && *name && !rl_nds_save_path(H.save, sizeof H.save, name))
            H.save[0] = 0;
        if (H.save[0] && rl_nds_save_read(H.run.m, H.save) < 0)
            rl_log(RL_LOG_WARN, "nds", "the save %s does not fit the backup chip; starting without it", H.save);
    } else {
        H.save[0] = 0;
    }
    H.save_ev = (rl_event){.fire = save_tick};
    rl_sched_post(H.run.s, &H.save_ev, RL_NDS_TICK_HZ);
    if (max_ticks) {
        H.max_ev = (rl_event){.fire = max_tick};
        rl_sched_post(H.run.s, &H.max_ev, max_ticks + 1);
    }
    rl_nds_set_power_off_hook(H.run.m, power_off, NULL);
    rl_set_exit_hook(on_exit_code);
    if (H.headless)
        rl_settings_set("video", "headless");
    rl_window_set_events(on_window, NULL);
    int r = rl_window_main(run_body, (void *)p);
    rl_window_set_events(NULL, NULL);
    rl_set_exit_hook(NULL);
    save_now();
    report();
    rl_nds_run_free(&H.run);
    return r;
}
