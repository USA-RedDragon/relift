/**
 * @file gbamain.c
 * @brief The program of a lifted Game Boy Advance ROM: the machine, the CPU
 * and the headless run with its frame count, video memory hash and register
 * report.
 */
#include "gbamain.h"
#include "cpu.h"

#include <relift/settings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct host {
    const rl_gba_program *p;
    rl_gba_run run;
    uint32_t frames;
    uint32_t stop_frames;
    bool stop;
    bool stats;
} host;

static uint64_t fnv(uint64_t h, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

/** @brief FNV-1a of palette, video RAM, OAM and DISPCNT: the frame's inputs while no picture unit draws it. */
static uint64_t video_hash(rl_gba *g)
{
    uint64_t h = 0xCBF29CE484222325ull;
    static const rl_gba_region rs[] = {RL_GBA_PALETTE, RL_GBA_VRAM, RL_GBA_OAM};
    for (size_t i = 0; i < sizeof rs / sizeof rs[0]; i++) {
        size_t n = 0;
        const uint8_t *m = rl_gba_mem(g, rs[i], &n);
        h = fnv(h, m, n);
    }
    uint8_t d[2];
    uint32_t v = rl_gba_read(g, 0x04000000u, 2);
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)(v >> 8);
    return fnv(h, d, 2);
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
    printf("frames %u hash %016llx\n", H->frames, (unsigned long long)video_hash(H->run.g));
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
    if (H->stats)
        print_stats(H);
}

static void on_vblank(void *user, unsigned line, uint64_t when)
{
    (void)line;
    (void)when;
    host *H = user;
    H->frames++;
    if (H->stop_frames && H->frames >= H->stop_frames) {
        H->stop = true;
        rl_sched_break(H->run.k);
    }
}

static void on_check(void *user, rl_ctx *c)
{
    host *H = user;
    if (!H->stop)
        return;
    report(H, c);
    rl_process_exit(0);
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
    rl_settings_declare_runtime();
    rl_settings_args(argc, argv);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--headless"))
            continue;
        if (!strcmp(a, "--frames"))
            H.stop_frames = (uint32_t)strtoul(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(a, "--stats"))
            H.stats = true;
        else if (!strncmp(a, "--relift-", 9))
            i += strchr(a, '=') ? 0 : 1;
        else {
            fprintf(stderr, "relift-gba: unknown option %s\n", a);
            return 2;
        }
    }
    static const rl_guest_desc guest = {.name = "gba", .page_size = 0x1000, .address_bits = 32, .tick_hz = RL_GBA_HZ};
    if (rl_core_init(&guest, rl_arm32_cpu()) != 0)
        return 1;
    if (rl_units_register(p->units, p->unit_count) != 0) {
        fprintf(stderr, "relift-gba: the lifted units do not match this runtime\n");
        return 1;
    }
    rl_gba_cart cart = {.rom = p->rom, .rom_len = p->rom_len};
    rl_gba *g = rl_gba_create(&cart);
    if (!g || rl_gba_run_init(&H.run, g) != 0) {
        fprintf(stderr, "relift-gba: cannot create the machine\n");
        return 1;
    }
    rl_gba_set_vblank_hook(g, on_vblank, &H);
    H.run.check = on_check;
    H.run.check_user = &H;
    int r = rl_gba_run_start(&H.run);
    if (H.stats)
        print_stats(&H);
    rl_gba_run_free(&H.run);
    rl_gba_destroy(g);
    return r == RL_OK ? 0 : 1;
}
