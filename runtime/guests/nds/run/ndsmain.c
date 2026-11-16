/**
 * @file ndsmain.c
 * @brief The program of a lifted Nintendo DS cartridge: the machine, both
 * CPUs, the headless run and its report.
 */
#include "cpu.h"

#include <relift/settings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Most --peek options. */
#define PEEKS 16u

typedef struct host {
    rl_nds_run run;
    bool stats;
    uint32_t peek[PEEKS];
    uint32_t peeks;
} host;

static host H;

static void print_cpu(const rl_nds_arm *k)
{
    const rl_nds_arm_stats *s = &k->stats;
    fprintf(stderr, "relift-nds: %s tick %llu, %s, swis %llu (%llu unknown), irqs %llu, variants %llu, unfound %llu, compares %llu\n",
            k->id == RL_NDS_ARM9 ? "arm9" : "arm7", (unsigned long long)rl_sched_now(k->k),
            k->stopped == 1 ? "halted" : k->stopped == 2 ? "stopped" : "running", (unsigned long long)s->swis,
            (unsigned long long)s->unknown_swis, (unsigned long long)s->irqs, (unsigned long long)s->variants,
            (unsigned long long)s->unfound, (unsigned long long)s->compares);
    const rl_ctx *c = k->c;
    fprintf(stderr, "relift-nds: %s pc %08x (last sync), cpsr %08x, r0 %08x r1 %08x r2 %08x r3 %08x r4 %08x r5 %08x sp %08x lr %08x\n",
            k->id == RL_NDS_ARM9 ? "arm9" : "arm7", c->pc, rl_arm32_cpsr(c), c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5],
            c->r[13], c->r[14]);
}

/** @brief The end of a run: the peeked words on stdout, the counters on stderr. */
static void report(void)
{
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

static void on_exit_code(uint32_t code)
{
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
        if (!strcmp(a, "--headless"))
            continue;
        if (!strcmp(a, "--max-ticks"))
            *max_ticks = strtoull(arg_value(argc, argv, &i), NULL, 0);
        else if (!strcmp(a, "--peek")) {
            uint32_t at = (uint32_t)strtoul(arg_value(argc, argv, &i), NULL, 0);
            if (H.peeks < PEEKS)
                H.peek[H.peeks++] = at;
        } else if (!strcmp(a, "--stats"))
            H.stats = true;
        else if (!strncmp(a, "--relift-", 9))
            i += strchr(a, '=') ? 0 : 1;
        else {
            fprintf(stderr, "relift-nds: unknown option %s\n", a);
            return 2;
        }
    }
    return 0;
}

int rl_nds_main(const rl_nds_program *p, int argc, char **argv)
{
    memset(&H, 0, sizeof H);
    uint64_t max_ticks = 0;
    rl_settings_args(argc, argv);
    if (options((int)p->arg_count, (char **)p->args, 0, &max_ticks) != 0 || options(argc, argv, 1, &max_ticks) != 0)
        return 2;
    static const rl_guest_desc guest = {.name = "nds", .page_size = 0x1000, .address_bits = 32, .tick_hz = RL_NDS_TICK_HZ};
    if (rl_core_init(&guest, rl_arm32_cpu()) != 0)
        return 1;
    if (rl_nds_run_init(&H.run, p) != 0) {
        fprintf(stderr, "relift-nds: cannot start %s\n", p->title && *p->title ? p->title : "the cartridge");
        return 1;
    }
    H.run.max_ticks = max_ticks;
    rl_set_exit_hook(on_exit_code);
    int r = rl_nds_run_start(&H.run);
    rl_set_exit_hook(NULL);
    report();
    rl_nds_run_free(&H.run);
    return r;
}
