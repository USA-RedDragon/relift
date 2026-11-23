/**
 * @file rlx_run.c
 * @brief Drives librelift_gpu_xlate through rlx.h only, as the runtime does.
 *
 * rlx_run tables <programs> <device.json> <dir> [threads]: writes the tables for the device, printing the progress
 * calls, the time and what the tables hold.
 * rlx_run one <device.json> <program.bin> <out.spv>: translates one program and writes its SPIR-V words.
 * rlx_run stop <programs> <device.json> <dir>: stops at the first progress call; exits 0 when that failed the run.
 */
#define _POSIX_C_SOURCE 199309L
#include "rlx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __ANDROID__
#include <malloc.h>
#endif

/** @brief The bytes of the file at path in *out (malloc'd), or NULL. */
static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len > 0 ? (size_t)len : 1);
    if (b && len > 0 && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *n = len > 0 ? (size_t)len : 0;
    return b;
}

/** @brief Seconds on a monotonic clock. */
static double now(void)
{
    struct timespec t;
#ifdef _WIN32
    timespec_get(&t, TIME_UTC);
#else
    clock_gettime(CLOCK_MONOTONIC, &t);
#endif
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/** @brief The peak resident memory in KiB where the system tells it (Linux, Android), else 0. */
static long peak_kib(void)
{
    long kib = 0;
#ifdef __linux__
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "VmHWM:", 6))
            kib = strtol(line + 6, NULL, 10);
    if (f)
        fclose(f);
#endif
    return kib;
}

/** @brief The progress calls a run saw and whether to stop at the first. */
typedef struct seen {
    uint32_t calls, last_done, total;
    int stop;
} seen;

static int on_progress(void *user, uint32_t done, uint32_t total)
{
    seen *s = user;
    s->calls++;
    if (done < s->last_done || total == 0) {
        fprintf(stderr, "progress went back: %u after %u of %u\n", done, s->last_done, total);
        exit(3);
    }
    s->last_done = done;
    s->total = total;
    return !s->stop;
}

int main(int argc, char **argv)
{
    char why[1024] = "";
#ifdef __ANDROID__
    /* Scudo shares two thread caches by default; translator threads then spend most of their time on its locks. */
    if (getenv("RLX_TSDS"))
        mallopt(M_TSDS_COUNT_MAX, atoi(getenv("RLX_TSDS")));
#endif
    if (rlx_abi() != RLX_ABI) {
        fprintf(stderr, "library ABI %u, header %u\n", rlx_abi(), RLX_ABI);
        return 2;
    }
    if (argc >= 5 && (!strcmp(argv[1], "tables") || !strcmp(argv[1], "stop"))) {
        size_t dn;
        uint8_t *dev = slurp(argv[3], &dn);
        if (!dev) {
            fprintf(stderr, "cannot read %s\n", argv[3]);
            return 2;
        }
        double t0 = now();
        rlx *x = rlx_new(argv[2], dev, dn, why, sizeof why);
        if (!x) {
            fprintf(stderr, "rlx_new: %s\n", why);
            return 1;
        }
        double t1 = now();
        seen s = { .stop = !strcmp(argv[1], "stop") };
        rlx_built b = { 0 };
        uint32_t threads = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 10) : 0;
        int rc = rlx_write_tables(x, argv[4], threads, on_progress, &s, &b, why, sizeof why);
        double t2 = now();
        rlx_free(x);
        free(dev);
        if (s.stop) {
            printf("stopped: rc %d (%s), %u progress call(s)\n", rc, why, s.calls);
            return rc == -1 && s.calls == 1 ? 0 : 1;
        }
        if (rc) {
            fprintf(stderr, "rlx_write_tables: %s\n", why);
            return 1;
        }
        printf("stamp %s: %u of %u programs translated, %u refused; open %.2f s, translate %.2f s, %u progress "
               "calls, last %u of %u, peak %ld MiB\n",
               rlx_stamp(), b.translated, b.programs, b.refused, t1 - t0, t2 - t1, s.calls, s.last_done, s.total,
               peak_kib() / 1024);
        return s.last_done == b.programs ? 0 : 1;
    }
    if (argc == 5 && !strcmp(argv[1], "one")) {
        size_t dn, cn;
        uint8_t *dev = slurp(argv[2], &dn), *code = slurp(argv[3], &cn);
        if (!dev || !code) {
            fprintf(stderr, "cannot read the inputs\n");
            return 2;
        }
        rlx *x = rlx_new(NULL, dev, dn, why, sizeof why);
        if (!x) {
            fprintf(stderr, "rlx_new: %s\n", why);
            return 1;
        }
        uint32_t *w = NULL;
        size_t n = 0;
        if (rlx_translate_one(x, code, cn, &w, &n, why, sizeof why)) {
            fprintf(stderr, "rlx_translate_one: %s\n", why);
            return 1;
        }
        FILE *f = fopen(argv[4], "wb");
        if (!f || fwrite(w, 4, n, f) != n) {
            fprintf(stderr, "cannot write %s\n", argv[4]);
            return 2;
        }
        fclose(f);
        printf("%zu words\n", n);
        rlx_words_free(w, n);
        rlx_free(x);
        free(dev);
        free(code);
        return 0;
    }
    fprintf(stderr, "usage: rlx_run tables|stop <programs> <device.json> <dir> [threads] | one <device.json> "
                    "<program.bin> <out.spv>\n");
    return 2;
}
