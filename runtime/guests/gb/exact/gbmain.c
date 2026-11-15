/**
 * @file gbmain.c
 * @brief The program of a lifted Game Boy ROM: the machine, the window or
 * headless run, the joypad, battery saves next to the ROM, frame dumps, the
 * corpus case grading and the lockstep check.
 */
#define _POSIX_C_SOURCE 200809L
#include "gbmain.h"
#include "exact_int.h"
#include "sm83.h"

#include <relift/audio.h>
#include <relift/ports.h>
#include <relift/settings.h>
#include <relift/window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const rl_port_decl rl_gb_ports[];
extern const uint32_t rl_gb_ports_count;
extern const rl_port_decl rl_gbc_ports[];
extern const uint32_t rl_gbc_ports_count;

#define FRAME_NS 16742706ull
#define SAVE_EVERY 600u
#define AUDIO_CHUNK 2048u
#define AUDIO_AHEAD (3u * RLGBD_AUDIO_RATE / 60u)

static const struct {
    const char *name;
    uint32_t code;
    bool dmg;
} g_models[] = {
    {"dmg", 0, true},    {"cgb", 1, false},  {"cgbe", 2, false}, {"agb", 3, false},  {"mgb", 4, true},
    {"cgbb", 5, false},  {"dmg0", 6, true},  {"sgb", 7, true},   {"sgb2", 8, true},  {"cgb0", 9, false},
};

static const char *const g_buttons[8] = {"a", "b", "select", "start", "right", "left", "up", "down"};

typedef struct host {
    const rl_gb_program *p;
    rlgbd *h;
    rlgbd *ref;
    rl_window *win;
    uint32_t frames;
    uint32_t stop_frames;
    bool in_case;
    bool show;
    bool reported;
    int case_ret;
    int verdict;
    bool quit;
    const char *png;
    const char *png_dir;
    uint32_t png_every;
    const char *save;
    uint8_t *saved;
    size_t saved_len;
    bool stats;
    const char *interp_log;
    const char *learn;
    bool live;
    bool audio;
    rl_audio_out *out;
    const char *wav;
    int16_t *wav_pcm;
    size_t wav_frames;
    size_t wav_cap;
    uint64_t audio_hash;
    uint64_t audio_frames;
    int audio_peak;
    rl_control_ref pad[8];
    uint64_t next_ns;
    uint64_t checked;
    int diverged;
} host;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *b = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long len = ftell(f);
        if (len >= 0 && fseek(f, 0, SEEK_SET) == 0) {
            b = malloc((size_t)len + 1);
            if (b && fread(b, 1, (size_t)len, f) != (size_t)len) {
                free(b);
                b = NULL;
            }
            if (b) {
                b[len] = 0;
                *n = (size_t)len;
            }
        }
    }
    fclose(f);
    return b;
}

static void save_now(host *H)
{
    if (!H->save || !*H->save)
        return;
    size_t n = rlgbd_save_size(H->h);
    if (!n)
        return;
    uint8_t *b = malloc(n);
    if (!b)
        return;
    n = rlgbd_save_bytes(H->h, b, n);
    if (H->saved && H->saved_len == n && memcmp(H->saved, b, n) == 0) {
        free(b);
        return;
    }
    size_t plen = strlen(H->save) + 8;
    char *tmp = malloc(plen);
    FILE *f = tmp ? (snprintf(tmp, plen, "%s.tmp", H->save), fopen(tmp, "wb")) : NULL;
    bool ok = f && fwrite(b, 1, n, f) == n;
    if (f && fclose(f) != 0)
        ok = false;
    if (ok && rename(tmp, H->save) == 0) {
        free(H->saved);
        H->saved = b;
        H->saved_len = n;
        b = NULL;
    } else {
        rl_log(RL_LOG_WARN, "gb", "could not write the save %s", H->save);
    }
    free(tmp);
    free(b);
}

static void load_save(host *H)
{
    if (!H->save || !*H->save || !rlgbd_save_size(H->h))
        return;
    size_t n = 0;
    uint8_t *b = read_file(H->save, &n);
    if (!b)
        return;
    if (rlgbd_load_save(H->h, b, n) == 0) {
        rl_log(RL_LOG_INFO, "gb", "loaded the save %s (%zu bytes)", H->save, n);
        H->saved = b;
        H->saved_len = n;
    } else {
        rl_log(RL_LOG_WARN, "gb", "the save %s does not fit this cartridge; not loaded", H->save);
        free(b);
    }
}

static void audio_keep(host *H, const int16_t *pcm, uint32_t frames)
{
    for (uint32_t i = 0; i < frames * 2; i++) {
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
    memcpy(H->wav_pcm + H->wav_frames * 2, pcm, (size_t)frames * 4);
    H->wav_frames += frames;
}

static void audio_drain(host *H)
{
    if (!H->audio)
        return;
    float in[AUDIO_CHUNK * 2];
    int16_t pcm[AUDIO_CHUNK * 2];
    size_t n;
    while ((n = rlgbd_audio_take(H->h, in, AUDIO_CHUNK)) > 0) {
        for (size_t i = 0; i < n * 2; i++) {
            float v = in[i] * 32767.0f;
            pcm[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32767.0f ? -32767 : v);
        }
        audio_keep(H, pcm, (uint32_t)n);
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
    le32(h + 24, RLGBD_AUDIO_RATE);
    le32(h + 28, RLGBD_AUDIO_RATE * 4);
    le32(h + 40, bytes);
    FILE *f = fopen(H->wav, "wb");
    bool ok = f && fwrite(h, 1, 44, f) == 44 && (!bytes || fwrite(H->wav_pcm, 1, bytes, f) == bytes);
    if (f && fclose(f) != 0)
        ok = false;
    return ok;
}

static void audio_pace(host *H)
{
    while (!__atomic_load_n(&H->quit, __ATOMIC_ACQUIRE) && rl_audio_queued(H->out) > AUDIO_AHEAD) {
        struct timespec d = {0, 1000000L};
        nanosleep(&d, NULL);
    }
}

static void on_frame(host *H)
{
    audio_drain(H);
    if (H->png_dir && H->png_every && H->frames % H->png_every == 0) {
        char path[4096];
        snprintf(path, sizeof path, "%s/frame%06u.png", H->png_dir, H->frames);
        if (rlgbd_write_png(H->h, path) != 0)
            rl_log(RL_LOG_WARN, "gb", "could not write %s", path);
    }
    if (H->win)
        rl_window_present_rgb(H->win, rlgbd_frame_rgb(H->h), 160, 144);
    if (H->live && !H->in_case) {
        uint8_t mask = 0;
        for (int i = 0; i < 8; i++)
            if (rl_ports_read(H->pad[i]) > 0.5f)
                mask |= (uint8_t)(1u << i);
        rlgbd_set_buttons(H->h, mask);
    }
    if (H->frames % SAVE_EVERY == 0)
        save_now(H);
    if (H->stop_frames && !H->in_case && H->frames >= H->stop_frames)
        H->quit = true;
    if (H->win && H->out) {
        audio_pace(H);
    } else if (H->win) {
        uint64_t t = now_ns();
        if (!H->next_ns || t > H->next_ns + 4 * FRAME_NS)
            H->next_ns = t;
        H->next_ns += FRAME_NS;
        if (H->next_ns > t) {
            struct timespec d = {(time_t)((H->next_ns - t) / 1000000000ull), (long)((H->next_ns - t) % 1000000000ull)};
            nanosleep(&d, NULL);
        }
    }
}

static int check_step(host *H, rl_ctx *c, uint16_t pc)
{
    rlgbd_regs want, got;
    rlgbd_get_regs(H->ref, &want);
    rl_gbx_regs(c, pc, &got);
    uint64_t a = rlgbd_now(H->ref), b = rlgbd_now(H->h);
    bool same = want.a == got.a && want.f == got.f && want.b == got.b && want.c == got.c && want.d == got.d &&
                want.e == got.e && want.h == got.h && want.l == got.l && want.pc == got.pc && want.sp == got.sp &&
                want.ime == got.ime;
    if (!same || a != b) {
        fprintf(stderr,
                "relift-gb: check: diverged after %llu steps at pc %04x/%04x a %02x/%02x f %02x/%02x bc %02x%02x/%02x%02x de "
                "%02x%02x/%02x%02x hl %02x%02x/%02x%02x sp %04x/%04x ime %u/%u dots %llu/%llu (reference/lifted)\n",
                (unsigned long long)H->checked, want.pc, got.pc, want.a, got.a, want.f, got.f, want.b, want.c, got.b, got.c, want.d,
                want.e, got.d, got.e, want.h, want.l, got.h, got.l, want.sp, got.sp, want.ime, got.ime, (unsigned long long)a,
                (unsigned long long)b);
        H->diverged = 1;
        return 1;
    }
    if (H->in_case && rlgbd_case_poll(H->ref, want.pc) == RLGBD_CASE_NEED_REGS) {
        rlgbd_case_regs(H->ref, &want);
        rlgbd_case_poll(H->ref, want.pc);
    }
    rlgbd_step_native(H->ref);
    H->checked++;
    return 0;
}

static int poll(void *user, rl_ctx *c, uint16_t pc)
{
    host *H = user;
    if (H->ref && check_step(H, c, pc))
        return 70;
    if (H->in_case) {
        int v = rlgbd_case_poll(H->h, pc);
        if (v == RLGBD_CASE_NEED_REGS) {
            rlgbd_regs r;
            rl_gbx_regs(c, pc, &r);
            rlgbd_case_regs(H->h, &r);
            v = rlgbd_case_poll(H->h, pc);
        }
        if (v != RLGBD_CASE_CONTINUE) {
            H->verdict = v;
            return 64 + v;
        }
    }
    uint32_t f = H->in_case ? rlgbd_frames(H->h) : rlgbd_frame_poll(H->h);
    if (f != H->frames) {
        H->frames = f;
        on_frame(H);
    }
    return __atomic_load_n(&H->quit, __ATOMIC_ACQUIRE) ? 64 : 0;
}

static void on_window(void *user, rl_window *w, int event)
{
    (void)w;
    host *H = user;
    if (event == RL_WINDOW_CLOSE)
        __atomic_store_n(&H->quit, true, __ATOMIC_RELEASE);
}

static void print_stats(void)
{
    const rl_gbx_stats *s = rl_gbx_stats_get();
    fprintf(stderr,
            "relift-gb stats: lifted %llu (nop sleds %llu), traps %llu, steps %llu, irqs %llu, native halt-bug %llu oam-bug %llu fetch %llu "
            "bank0 %llu dma %llu, redirects ret %llu bank %llu trap %llu native %llu depth %llu, interpreter rom %llu entries %llu "
            "instructions, ram %llu entries %llu instructions, max depth %llu\n",
            (unsigned long long)s->lifted, (unsigned long long)s->sled, (unsigned long long)s->traps, (unsigned long long)s->steps, (unsigned long long)s->irqs,
            (unsigned long long)s->native[RL_GBX_HALT_BUG], (unsigned long long)s->native[RL_GBX_OAM_BUG],
            (unsigned long long)s->native[RL_GBX_FETCH], (unsigned long long)s->native[RL_GBX_BANK0],
            (unsigned long long)s->native[RL_GBX_DMA],
            (unsigned long long)s->redirects[RL_GBX_RET], (unsigned long long)s->redirects[RL_GBX_BANK],
            (unsigned long long)s->redirects[RL_GBX_TRAP], (unsigned long long)s->redirects[RL_GBX_NATIVE],
            (unsigned long long)s->redirects[RL_GBX_DEPTH], (unsigned long long)s->interp_entries[RL_GBX_ROM],
            (unsigned long long)s->interp[RL_GBX_ROM], (unsigned long long)s->interp_entries[RL_GBX_RAM],
            (unsigned long long)s->interp[RL_GBX_RAM], (unsigned long long)s->max_depth);
}

static int verdict_report(host *H, int code)
{
    int ret = 0;
    if (H->diverged) {
        ret = 70;
    } else if (H->in_case && H->verdict == RLGBD_CASE_CONTINUE) {
        printf("relift-gb: fail: the run stopped before a verdict (status %d)\n", code);
        ret = 1;
    } else if (H->in_case) {
        const char *word = H->verdict == RLGBD_CASE_PASS ? "pass" : H->verdict == RLGBD_CASE_UNSUPPORTED ? "unsupported" : "fail";
        printf("relift-gb: %s: %s\n", word, rlgbd_case_detail(H->h));
        ret = H->verdict == RLGBD_CASE_PASS ? 0 : H->verdict == RLGBD_CASE_UNSUPPORTED ? 3 : 1;
    }
    fflush(stdout);
    return ret;
}

static void hold_frame(host *H)
{
    while (!__atomic_load_n(&H->quit, __ATOMIC_ACQUIRE)) {
        rl_window_present_rgb(H->win, rlgbd_frame_rgb(H->h), 160, 144);
        struct timespec d = {0, 50000000L};
        nanosleep(&d, NULL);
    }
}

static int write_interp_log(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    size_t n = 0;
    const rl_gbx_seen *s = rl_gbx_seen_get(&n);
    for (size_t i = 0; i < n; i++) {
        if (!s[i].count)
            continue;
        unsigned kind = (unsigned)(s[i].key >> 32), bank = (unsigned)(s[i].key >> 16 & 0xFFFF), pc = (unsigned)(s[i].key & 0xFFFF);
        if (kind == RL_GBX_SEEN_RAMOP)
            fprintf(f, "ramop %04x %03x %llu\n", pc, bank, (unsigned long long)s[i].count);
        else if (kind == RL_GBX_SEEN_TARGET)
            fprintf(f, "target %03x:%04x %llu\n", bank, pc, (unsigned long long)s[i].count);
        else if (kind == RL_GBX_SEEN_INSN)
            fprintf(f, "insn %03x:%04x %llu %02x%02x%02x %u\n", bank, pc, (unsigned long long)s[i].count, s[i].bytes[0],
                    s[i].bytes[1], s[i].bytes[2], s[i].varied);
        else
            fprintf(f, "entry %03x:%04x %llu %u %llu\n", bank, pc, (unsigned long long)s[i].count, kind,
                    (unsigned long long)s[i].insns);
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int line_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static bool add_line(char ***v, size_t *n, size_t *cap, const char *line)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
        char **t = realloc(*v, nc * sizeof *t);
        if (!t)
            return false;
        *v = t;
        *cap = nc;
    }
    char *d = strdup(line);
    if (!d)
        return false;
    (*v)[(*n)++] = d;
    return true;
}

/**
 * @brief Adds what the interpreter ran to the learned file at path, keeping
 * the lines it holds: ROM locations where it was entered or went to
 * (BBB:AAAA, bank and address in hex) and RAM instructions (ram AAAA KKK,
 * the address and the RAM table key in hex). Rewrites it only when a line
 * was added.
 */
static int write_learned(const char *path)
{
    size_t n = 0, cap = 0, have;
    char **v = NULL;
    size_t len = 0;
    char *old = (char *)read_file(path, &len);
    bool ok = true;
    for (char *s = old; ok && s && *s;) {
        char *nl = strchr(s, '\n');
        if (nl)
            *nl = 0;
        unsigned b, a, k;
        char buf[32];
        if (sscanf(s, "%x:%x", &b, &a) == 2 && a < 0x8000 && b < 0x10000)
            snprintf(buf, sizeof buf, "%03x:%04x", b, a);
        else if (sscanf(s, "ram %x %x", &a, &k) == 2 && a >= 0x8000 && a < 0x10000 && k < 0x200)
            snprintf(buf, sizeof buf, "ram %04x %03x", a, k);
        else
            buf[0] = 0;
        if (buf[0])
            ok = add_line(&v, &n, &cap, buf);
        s = nl ? nl + 1 : NULL;
    }
    free(old);
    have = n;
    size_t slots = 0;
    const rl_gbx_seen *seen = rl_gbx_seen_get(&slots);
    for (size_t i = 0; ok && i < slots; i++) {
        unsigned kind = (unsigned)(seen[i].key >> 32), mid = (unsigned)(seen[i].key >> 16 & 0xFFFF),
                 pc = (unsigned)(seen[i].key & 0xFFFF);
        char buf[32];
        if (!seen[i].count || kind == RL_GBX_SEEN_INSN)
            continue;
        if (kind == RL_GBX_SEEN_RAMOP)
            snprintf(buf, sizeof buf, "ram %04x %03x", pc, mid);
        else if (pc < 0x8000)
            snprintf(buf, sizeof buf, "%03x:%04x", mid, pc);
        else
            continue;
        ok = add_line(&v, &n, &cap, buf);
    }
    if (n)
        qsort(v, n, sizeof *v, line_cmp);
    size_t u = 0;
    for (size_t i = 0; i < n; i++) {
        if (u && !strcmp(v[u - 1], v[i]))
            free(v[i]);
        else
            v[u++] = v[i];
    }
    if (ok && u != have) {
        size_t plen = strlen(path) + 8;
        char *tmp = malloc(plen);
        FILE *f = tmp ? (snprintf(tmp, plen, "%s.tmp", path), fopen(tmp, "w")) : NULL;
        ok = f != NULL;
        for (size_t i = 0; ok && i < u; i++)
            ok = fprintf(f, "%s\n", v[i]) > 0;
        if (f && fclose(f) != 0)
            ok = false;
        ok = ok && rename(tmp, path) == 0;
        free(tmp);
    }
    for (size_t i = 0; i < u; i++)
        free(v[i]);
    free(v);
    return ok ? 0 : -1;
}

typedef struct run_args {
    host *H;
    rl_ctx *c;
    uint8_t *mem;
    int ret;
} run_args;

static int run_body(void *arg)
{
    run_args *a = arg;
    host *H = a->H;
    const rl_window_backend *b = rl_window_backend_current();
    if (b) {
        H->win = b->open(H->p->title && *H->p->title ? H->p->title : "relift", 160 * 4, 144 * 4);
        H->live = H->win != NULL;
    }
    if (H->audio && H->win) {
        H->out = rl_audio_open(RLGBD_AUDIO_RATE, 2);
        if (H->out)
            rl_audio_pause(H->out, false);
    }
    rlgbd_regs r;
    rlgbd_get_regs(H->h, &r);
    a->c->a = r.a;
    a->c->f = r.f;
    a->c->b = r.b;
    a->c->c = r.c;
    a->c->d = r.d;
    a->c->e = r.e;
    a->c->h = r.h;
    a->c->l = r.l;
    a->c->sp = r.sp;
    int code = rl_gbx_run(a->c, a->mem, r.pc);
    audio_drain(H);
    if (H->show && H->win && H->in_case && !H->diverged) {
        H->case_ret = verdict_report(H, code);
        H->reported = true;
        hold_frame(H);
    }
    rl_audio_close(H->out);
    H->out = NULL;
    if (H->win) {
        b->close(H->win);
        H->win = NULL;
    }
    return code;
}

static const char *arg_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "relift-gb: %s needs a value\n", argv[*i]);
        exit(2);
    }
    return argv[++*i];
}

static char *append(char *spec, const char *line)
{
    size_t a = spec ? strlen(spec) : 0, b = strlen(line);
    char *s = realloc(spec, a + b + 2);
    if (!s)
        abort();
    memcpy(s + a, line, b);
    s[a + b] = '\n';
    s[a + b + 1] = 0;
    return s;
}

static void usage(FILE *out)
{
    fputs("usage: game [options]\n"
          "  --model NAME        hardware revision: dmg, mgb, sgb, cgb, agb and others\n"
          "  --headless          open no window\n"
          "  --frames N          stop after N frames; print the frame hash and serial output\n"
          "  --input SPEC        scripted joypad, frame:buttons,...\n"
          "  --png FILE          write the last frame as a PNG\n"
          "  --png-dir DIR       write frames as PNG files into DIR\n"
          "  --png-every N       with --png-dir, one frame in N\n"
          "  --wav FILE          record the sound as 44100 Hz stereo WAV\n"
          "  --no-audio          open no sound output\n"
          "  --case FILE         grade the run by a corpus case spec\n"
          "  --show              with --case, run in a window and keep the last frame up after the verdict\n"
          "  --save FILE         battery save file (default: next to the ROM)\n"
          "  --no-save           do not read or write a battery save\n"
          "  --stats             print run statistics\n"
          "  --check             run in lockstep with the device library's CPU\n"
          "  --relift-set K=V    set a run-time setting; --relift-settings FILE reads a file\n"
          "  --help              print this text\n",
          out);
}

int rl_gb_main(const rl_gb_program *p, int argc, char **argv)
{
    host H;
    memset(&H, 0, sizeof H);
    H.p = p;
    H.save = p->save;
    H.learn = p->learn && *p->learn ? p->learn : NULL;
    const char *model = p->model, *case_file = NULL, *input = NULL;
    bool headless = false, check = false, no_audio = false;
    rl_gb_declare_settings();
    bool color = p->rom_len > 0x143 && (p->rom[0x143] & 0x80);
    rl_ports_declare(color ? rl_gbc_ports : rl_gb_ports, color ? rl_gbc_ports_count : rl_gb_ports_count);
    rl_settings_args(argc, argv);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--model"))
            model = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--headless"))
            headless = true;
        else if (!strcmp(a, "--frames"))
            H.stop_frames = (uint32_t)strtoul(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--case"))
            case_file = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--show"))
            H.show = true;
        else if (!strcmp(a, "--input"))
            input = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--png"))
            H.png = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--png-dir"))
            H.png_dir = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--png-every"))
            H.png_every = (uint32_t)strtoul(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--wav"))
            H.wav = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--no-audio"))
            no_audio = true;
        else if (!strcmp(a, "--save"))
            H.save = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--no-save"))
            H.save = NULL;
        else if (!strcmp(a, "--stats"))
            H.stats = true;
        else if (!strcmp(a, "--interp-log"))
            H.interp_log = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--learn"))
            H.learn = arg_value(argc, argv, &i);
        else if (!strcmp(a, "--no-learn"))
            H.learn = NULL;
        else if (!strcmp(a, "--check"))
            check = true;
        else if (!strncmp(a, "--relift-", 9))
            i += strchr(a, '=') ? 0 : 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "relift-gb: unknown option %s\n", a);
            return 2;
        }
    }
    if (H.png_dir && !H.png_every)
        H.png_every = 1;
    int hw = -1;
    bool dmg = false;
    for (size_t i = 0; i < sizeof g_models / sizeof g_models[0]; i++)
        if (model && !strcmp(model, g_models[i].name)) {
            hw = (int)g_models[i].code;
            dmg = g_models[i].dmg;
        }
    if (hw < 0) {
        fprintf(stderr, "relift-gb: unknown model %s\n", model ? model : "(none)");
        return 2;
    }
    static const rl_guest_desc guest = {.name = "gb", .page_size = 0x1000, .address_bits = 16, .tick_hz = 4194304};
    if (rl_core_init(&guest, rl_sm83_cpu()) != 0)
        return 1;
    char *spec = NULL;
    if (case_file) {
        size_t n = 0;
        spec = (char *)read_file(case_file, &n);
        if (!spec) {
            fprintf(stderr, "relift-gb: cannot read %s\n", case_file);
            return 2;
        }
        headless = !H.show;
        H.save = NULL;
    } else if (H.stop_frames) {
        char line[64];
        snprintf(line, sizeof line, "protocol=run\nframes=%u", H.stop_frames);
        spec = append(NULL, line);
        if (input) {
            spec = append(spec, "input=");
            spec[strlen(spec) - 1] = 0;
            spec = append(spec, input);
        }
    }
    uint32_t flags = 0;
    if (spec && strstr(spec, "protocol=sram"))
        flags |= RLGBD_NEW_BOOT_RESIDUE;
    if (spec && strstr(spec, "cart=lazy_sram_cs"))
        flags |= RLGBD_NEW_LAZY_SRAM_CS;
    H.h = rlgbd_new_with((uint32_t)hw, p->rom, p->rom_len, flags);
    if (!H.h) {
        fprintf(stderr, "relift-gb: the device library refused the cartridge\n");
        return 1;
    }
    H.audio = !no_audio && !case_file;
    if (H.audio)
        rlgbd_audio_enable(H.h, 1);
    if (check)
        H.ref = rlgbd_new_with((uint32_t)hw, p->rom, p->rom_len, flags);
    bool correction = rl_setting_bool("video.gb_color_correction");
    rlgbd_set_color_correction(H.h, correction);
    if (H.ref)
        rlgbd_set_color_correction(H.ref, correction);
    if (spec) {
        if (rlgbd_case_open(H.h, spec) != 0) {
            fprintf(stderr, "relift-gb: case: %s\n", rlgbd_case_detail(H.h));
            return 2;
        }
        if (H.ref)
            rlgbd_case_open(H.ref, spec);
        H.in_case = true;
    }
    bool mbc1 = p->rom_len > 0x147 && p->rom[0x147] >= 1 && p->rom[0x147] <= 3;
    if (rl_gbx_init(H.h, p->rom, p->rom_len, dmg, mbc1) != 0)
        return 1;
    rl_gbx_log_interpreted(H.interp_log != NULL);
    rl_gbx_log_entries(H.learn != NULL);
    for (uint32_t i = 0; i < p->unit_count; i++)
        if (rl_gbx_add_unit(p->units[i].unit, p->units[i].bank) != 0) {
            fprintf(stderr, "relift-gb: a lifted unit was refused\n");
            return 1;
        }
    for (uint32_t i = 0; i < p->ram_count; i++)
        if (rl_gbx_add_ram(p->ram[i].pc, p->ram[i].op, p->ram[i].fn) != 0) {
            fprintf(stderr, "relift-gb: a RAM instruction was refused\n");
            return 1;
        }
    for (uint32_t i = 0; i < p->sled_count; i++)
        if (rl_gbx_add_sled(p->sleds[i].bank, p->sleds[i].start, p->sleds[i].end) != 0) {
            fprintf(stderr, "relift-gb: a nop sled was refused\n");
            return 1;
        }
    if (!H.in_case)
        load_save(&H);
    for (int i = 0; i < 8; i++)
        H.pad[i] = rl_ports_find("p1", g_buttons[i]);
    if (headless)
        rl_settings_set("video", "headless");
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return 1;
    static uint8_t mem[0x10000];
    rl_gbx_set_poll(poll, &H);
    rl_window_set_events(on_window, &H);
    run_args ra = {.H = &H, .c = t->ctx, .mem = mem};
    int code = rl_window_main(run_body, &ra);
    rl_window_set_events(NULL, NULL);
    int ret = H.reported ? H.case_ret : verdict_report(&H, code);
    if (H.ref)
        fprintf(stderr, "relift-gb: check: %llu steps matched\n", (unsigned long long)H.checked);
    if (H.audio && (H.wav || H.stop_frames))
        fprintf(stderr, "relift-gb: audio: %llu frames at %u Hz, peak %d, hash %016llx\n", (unsigned long long)H.audio_frames,
                RLGBD_AUDIO_RATE, H.audio_peak, (unsigned long long)H.audio_hash);
    if (H.wav && !wav_write(&H)) {
        fprintf(stderr, "relift-gb: could not write %s\n", H.wav);
        ret = ret ? ret : 1;
    }
    if (H.png && rlgbd_write_png(H.h, H.png) != 0) {
        fprintf(stderr, "relift-gb: could not write %s\n", H.png);
        ret = ret ? ret : 1;
    }
    save_now(&H);
    if (H.stats)
        print_stats();
    if (H.interp_log && write_interp_log(H.interp_log) != 0) {
        fprintf(stderr, "relift-gb: could not write %s\n", H.interp_log);
        ret = ret ? ret : 1;
    }
    if (H.learn && write_learned(H.learn) != 0)
        fprintf(stderr, "relift-gb: could not write %s\n", H.learn);
    fflush(stdout);
    free(spec);
    free(H.wav_pcm);
    free(H.saved);
    rlgbd_free(H.ref);
    rlgbd_free(H.h);
    return ret;
}
