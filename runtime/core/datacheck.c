/**
 * @file datacheck.c
 * @brief The game data check of mobile apps (relift/datacheck.h) and the SHA-256 it hashes with (FIPS 180-4).
 */
#include <relift/datacheck.h>

#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
    size_t fill;
} sha256;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void block(sha256 *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

static void sha_init(sha256 *s)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->fill = 0;
}

static void sha_add(sha256 *s, const uint8_t *p, size_t n)
{
    s->len += n;
    while (n) {
        size_t take = 64 - s->fill < n ? 64 - s->fill : n;
        memcpy(s->buf + s->fill, p, take);
        s->fill += take;
        p += take;
        n -= take;
        if (s->fill == 64) {
            block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void sha_hex(sha256 *s, char hex[65])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha_add(s, &pad, 1);
    pad = 0;
    while (s->fill != 56)
        sha_add(s, &pad, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++)
        l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_add(s, l, 8);
    for (int i = 0; i < 8; i++)
        snprintf(hex + 8 * i, 9, "%08x", s->h[i]);
}

void rl_data_sha256(const void *data, size_t n, char hex[65])
{
    sha256 s;
    sha_init(&s);
    sha_add(&s, data, n);
    sha_hex(&s, hex);
}

int rl_data_sha256_file(const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    sha256 s;
    sha_init(&s);
    static _Thread_local uint8_t buf[1 << 16];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0)
        sha_add(&s, buf, got);
    int bad = ferror(f);
    fclose(f);
    if (bad)
        return -1;
    sha_hex(&s, hex);
    return 0;
}

static void note_bad(rl_data_report *r, const char *name, size_t nlen, const char *why)
{
    if (!r->bad++) {
        size_t n = nlen < sizeof r->first - 1 ? nlen : sizeof r->first - 1;
        memcpy(r->first, name, n);
        r->first[n] = 0;
        snprintf(r->why, sizeof r->why, "%s", why);
    }
}

int rl_data_check(const char *manifest, size_t len, const char *romfs, const char *dlc, bool hash, rl_data_report *out)
{
    memset(out, 0, sizeof *out);
    const char *p = manifest, *end = manifest + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        size_t n = (size_t)(le - p);
        if (n && p[n - 1] == '\r')
            n--;
        if (n == 0 || p[0] == '#') {
            p = le + 1;
            continue;
        }
        char line[1024];
        if (n >= sizeof line)
            return -1;
        memcpy(line, p, n);
        line[n] = 0;
        p = le + 1;
        char *sp1 = strchr(line, ' ');
        char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        if (!sp1 || !sp2 || sp2 - sp1 - 1 != 64)
            return -1;
        *sp1 = *sp2 = 0;
        char *stop;
        unsigned long long size = strtoull(line, &stop, 10);
        if (*stop)
            return -1;
        const char *hexw = sp1 + 1, *name = sp2 + 1;
        const char *root = NULL, *rel = NULL;
        if (!strncmp(name, "romfs/", 6)) {
            root = romfs;
            rel = name + 6;
        } else if (!strncmp(name, "dlc/", 4)) {
            root = dlc;
            rel = name + 4;
        } else {
            return -1;
        }
        out->files++;
        out->bytes += size;
        char path[4096];
        struct stat st;
        if (!root || snprintf(path, sizeof path, "%s/%s", root, rel) >= (int)sizeof path || stat(path, &st) != 0) {
            note_bad(out, name, strlen(name), "missing");
            continue;
        }
        if ((unsigned long long)st.st_size != size) {
            char why[96];
            snprintf(why, sizeof why, "is %llu bytes, expected %llu", (unsigned long long)st.st_size, size);
            note_bad(out, name, strlen(name), why);
            continue;
        }
        if (hash) {
            char got[65];
            if (rl_data_sha256_file(path, got) != 0 || strcmp(got, hexw) != 0)
                note_bad(out, name, strlen(name), "contents differ");
        }
    }
    return out->bad ? 1 : 0;
}
