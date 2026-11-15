/**
 * @file gbamain.c
 * @brief The program of a lifted Game Boy Advance ROM: the machine, the CPU,
 * the window or headless run, frame hashes and PNG dumps, and the backup
 * file.
 */
#define _POSIX_C_SOURCE 200809L
#include "gbamain.h"
#include "cpu.h"

#include <relift/ports.h>
#include <relift/settings.h>
#include <relift/window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const rl_port_decl rl_gba_ports[];
extern const uint32_t rl_gba_ports_count;

#define FRAME_NS 16742706ull
#define W 240u
#define H_PX 160u

typedef struct host {
    const rl_gba_program *p;
    rl_gba_run run;
    uint32_t frames;
    uint32_t stop_frames;
    bool stop;
    bool stats;
    bool live;
    bool quit;
    rl_window *win;
    const char *png;
    const char *png_dir;
    uint32_t png_every;
    uint64_t next_ns;
    char save[4096];
} host;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void save_now(host *H)
{
    if (H->save[0] && rl_gba_save_write(H->run.g, H->save) < 0)
        rl_log(RL_LOG_WARN, "gba", "could not write the save %s", H->save);
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

/** @brief Writes a frame as an RGB PNG with stored (uncompressed) deflate blocks. */
static bool write_png(const char *path, const uint16_t *pixels)
{
    enum { ROW = 1 + W * 3, RAW = ROW * H_PX };
    static uint8_t raw[RAW];
    static uint8_t rgb[W * H_PX * 3];
    rl_gba_frame_rgb(pixels, rgb);
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

static void print_stats(host *H)
{
    const rl_gba_run_stats *s = &H->run.stats;
    const rl_arm32_stats *a = rl_arm32_get_stats();
    fprintf(stderr,
            "relift-gba: frames %u, cycles %llu, swis %llu (%llu unknown), irqs %llu (%llu in the BIOS), returns %llu, "
            "redirects %llu, misses %llu\n",
            H->frames, (unsigned long long)rl_sched_cycles(H->run.k), (unsigned long long)s->swis,
            (unsigned long long)s->unknown_swis, (unsigned long long)s->irqs, (unsigned long long)s->bios_irqs,
            (unsigned long long)a->returns, (unsigned long long)a->redirects, (unsigned long long)a->misses);
}

static void report(host *H, rl_ctx *c)
{
    char buf[512];
    rl_arm32_cpu()->describe(c, buf, sizeof buf);
    printf("frames %u hash %016llx\n", H->frames, (unsigned long long)rl_gba_frame_hash(H->run.g));
    printf("regs %s\n", buf);
    static const struct {
        const char *name;
        rl_gba_region r;
    } rs[] = {{"ewram", RL_GBA_EWRAM}, {"iwram", RL_GBA_IWRAM}, {"palette", RL_GBA_PALETTE}, {"vram", RL_GBA_VRAM}, {"oam", RL_GBA_OAM}};
    printf("mem");
    for (size_t i = 0; i < sizeof rs / sizeof rs[0]; i++) {
        size_t n = 0;
        const uint8_t *m = rl_gba_mem(H->run.g, rs[i].r, &n);
        printf(" %s=%016llx", rs[i].name, (unsigned long long)fnv(0xCBF29CE484222325ull, m, n));
    }
    printf("\n");
    fflush(stdout);
}

static void on_frame(void *user, const uint16_t *pixels, uint64_t when)
{
    (void)when;
    host *H = user;
    H->frames++;
    if (H->png_dir && H->frames % H->png_every == 0) {
        char path[4096];
        snprintf(path, sizeof path, "%s/frame%06u.png", H->png_dir, H->frames);
        if (!write_png(path, pixels))
            rl_log(RL_LOG_WARN, "gba", "could not write %s", path);
    }
    if (H->save[0] && rl_gba_save_poll(H->run.g, H->save, RL_GBA_HZ) < 0)
        rl_log(RL_LOG_WARN, "gba", "could not write the save %s", H->save);
    if (H->win) {
        rl_gba_present(H->win, pixels);
        uint64_t t = now_ns();
        if (!H->next_ns || t > H->next_ns + 4 * FRAME_NS)
            H->next_ns = t;
        H->next_ns += FRAME_NS;
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
    const uint16_t *px = rl_gba_frame(H->run.g);
    if (H->png && px && !write_png(H->png, px))
        fprintf(stderr, "relift-gba: could not write %s\n", H->png);
    save_now(H);
    if (H->stats)
        print_stats(H);
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
    if (b && b->present_rgb) {
        H->win = b->open(H->p->title && *H->p->title ? H->p->title : "relift", W * 3, H_PX * 3);
        H->live = H->win != NULL;
    }
    int r = rl_gba_run_start(&H->run);
    finish(H);
    if (H->win)
        b->close(H->win);
    return r == RL_OK ? 0 : 1;
}

static const char *arg_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "relift-gba: %s needs a value\n", argv[*i]);
        exit(2);
    }
    return argv[++*i];
}

int rl_gba_main(const rl_gba_program *p, int argc, char **argv)
{
    static host H;
    memset(&H, 0, sizeof H);
    H.p = p;
    const char *save = NULL;
    bool no_save = false, headless = false;
    rl_gba_declare_settings();
    rl_settings_args(argc, argv);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--headless"))
            headless = true;
        else if (!strcmp(a, "--frames"))
            H.stop_frames = (uint32_t)strtoul(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--png"))
            H.png = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--png-dir"))
            H.png_dir = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--png-every"))
            H.png_every = (uint32_t)strtoul(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--save"))
            save = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--no-save"))
            no_save = true;
        else if (!strcmp(a, "--stats"))
            H.stats = true;
        else if (!strncmp(a, "--relift-", 9))
            i += strchr(a, '=') ? 0 : 1;
        else {
            fprintf(stderr, "relift-gba: unknown option %s\n", a);
            return 2;
        }
    }
    if (H.png_dir && !H.png_every)
        H.png_every = 1;
    static const rl_guest_desc guest = {.name = "gba", .page_size = 0x1000, .address_bits = 32, .tick_hz = RL_GBA_HZ};
    if (rl_core_init(&guest, rl_arm32_cpu()) != 0)
        return 1;
    if (rl_units_register(p->units, p->unit_count) != 0) {
        fprintf(stderr, "relift-gba: the lifted units do not match this runtime\n");
        return 1;
    }
    rl_ports_declare(rl_gba_ports, rl_gba_ports_count);
    rl_ports_bind_defaults();
    rl_gba_save_info si = rl_gba_save_detect(p->rom, p->rom_len);
    rl_gba_cart cart = {.rom = p->rom, .rom_len = p->rom_len, .save = si.kind, .flash_id = si.flash_id};
    rl_gba *g = rl_gba_create(&cart);
    if (!g || rl_gba_run_init(&H.run, g) != 0) {
        fprintf(stderr, "relift-gba: cannot create the machine\n");
        return 1;
    }
    if (!no_save && si.kind != RL_GBA_SAVE_NONE) {
        if (save)
            snprintf(H.save, sizeof H.save, "%s", save);
        else if (p->rom_path && *p->rom_path && !rl_gba_save_path(H.save, sizeof H.save, p->rom_path))
            H.save[0] = 0;
    }
    if (H.save[0] && rl_gba_save_read(g, H.save) < 0)
        rl_log(RL_LOG_WARN, "gba", "the save %s does not fit this cartridge; starting without it", H.save);
    rl_gba_set_frame_hook(g, on_frame, &H);
    H.run.check = on_check;
    H.run.check_user = &H;
    if (headless)
        rl_settings_set("video", "headless");
    rl_window_set_events(on_window, &H);
    int r = rl_window_main(run_body, &H);
    rl_window_set_events(NULL, NULL);
    rl_gba_run_free(&H.run);
    rl_gba_destroy(g);
    return r;
}
