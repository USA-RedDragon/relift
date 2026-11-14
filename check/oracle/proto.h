/*
 * The oracle protocol, shared by the native harness (harness.c) and the
 * slow-path engine (slow.c); check/src/oracle.rs is the other side and
 * check/oracle/README.md describes it.
 */
#ifndef RELIFT_CHECK_PROTO_H
#define RELIFT_CHECK_PROTO_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The layout; keep in step with check/src/emu.rs. */
#define CODE 0x10010000000ull
#define CODE_MAP (CODE - 0x100000ull)
#define CODE_SIZE 0x200000ull
#define DATA 0x10020000000ull
#define DATA_SIZE 0x4000u

#define REQ_MAGIC 0x514f4c52u /* "RLOQ" */
#define RES_MAGIC 0x524f4c52u /* "RLOR" */
#define MAX_WORDS 64u

/* Result status: ran, faulted, refused, no slow path. */
enum { ST_DONE = 0, ST_FAULT = 1, ST_REFUSED = 2, ST_UNSUPPORTED = 3 };
enum { MEM_SEED = 0, MEM_BYTES = 1 };

typedef struct {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint32_t nzcv;
    uint32_t fpcr;
    uint32_t fpsr;
    uint32_t pad;
    uint64_t pad2;
    unsigned __int128 v[32];
} gstate;
_Static_assert(sizeof(gstate) == 800, "the protocol's state is 800 bytes");

static void proto_die(const char *m)
{
    fprintf(stderr, "oracle: %s: %s\n", m, strerror(errno));
    exit(1);
}

static int proto_read_all(void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        ssize_t r = read(0, b, n);
        if (r == 0)
            return 0;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            proto_die("read");
        }
        b += r;
        n -= (size_t)r;
    }
    return 1;
}

static uint8_t proto_obuf[1 << 16];
static size_t proto_olen;

static void proto_flush(void)
{
    size_t o = 0;
    while (o < proto_olen) {
        ssize_t r = write(1, proto_obuf + o, proto_olen - o);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            proto_die("write");
        }
        o += (size_t)r;
    }
    proto_olen = 0;
}

static void proto_put(const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        if (proto_olen == sizeof proto_obuf)
            proto_flush();
        size_t k = sizeof proto_obuf - proto_olen;
        if (k > n)
            k = n;
        memcpy(proto_obuf + proto_olen, b, k);
        proto_olen += k;
        b += k;
        n -= k;
    }
}

static void proto_put32(uint32_t v) { proto_put(&v, 4); }

/* The data window's fill from a seed: splitmix64; keep in step with
 * oracle.rs. */
static void proto_fill(uint8_t *w, uint64_t seed)
{
    uint64_t s = seed;
    for (unsigned i = 0; i < DATA_SIZE; i += 8) {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z ^= z >> 31;
        memcpy(w + i, &z, 8);
    }
}

/* Reads one request; 0 at the end of input. */
static int proto_read(uint32_t *words, unsigned *n, gstate *in, uint8_t *window)
{
    uint32_t hdr[2];
    if (!proto_read_all(hdr, sizeof hdr))
        return 0;
    if (hdr[0] != REQ_MAGIC || hdr[1] == 0 || hdr[1] > MAX_WORDS) {
        fprintf(stderr, "oracle: bad request header\n");
        exit(1);
    }
    *n = hdr[1];
    uint32_t kind = 0;
    if (!proto_read_all(words, 4 * *n) || !proto_read_all(in, sizeof *in) || !proto_read_all(&kind, 4))
        proto_die("short request");
    if (kind == MEM_SEED) {
        uint64_t seed;
        if (!proto_read_all(&seed, 8))
            proto_die("short request");
        proto_fill(window, seed);
    } else if (!proto_read_all(window, DATA_SIZE)) {
        proto_die("short request");
    }
    return 1;
}

/* Writes one result: the state and the bytes of `after` that differ from
 * `before`, as ranges. Each result leaves at once; the client pipelines. */
static void proto_write(uint32_t status, uint32_t sig, uint64_t addr, const gstate *out, const uint8_t *after,
                        const uint8_t *before)
{
    proto_put32(RES_MAGIC);
    proto_put32(status);
    proto_put32(sig);
    proto_put32(0);
    proto_put(&addr, 8);
    proto_put(out, sizeof *out);
    uint32_t nr = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1)
            proto_put32(nr);
        for (unsigned i = 0; i < DATA_SIZE;) {
            if (after[i] == before[i]) {
                i++;
                continue;
            }
            unsigned j = i;
            while (j < DATA_SIZE && after[j] != before[j])
                j++;
            if (pass == 0) {
                nr++;
            } else {
                proto_put32(i);
                proto_put32(j - i);
                proto_put(after + i, j - i);
            }
            i = j;
        }
    }
    proto_flush();
}

#endif
