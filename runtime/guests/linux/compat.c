/**
 * @file compat.c
 * @brief Structure marshalling for the guest's layouts (compat.h).
 */
#include "arch.h"
#include "compat.h"

#include <string.h>

_Thread_local bool lx_ts32;
_Thread_local bool lx_rlim32;

static bool narrow(void) { return lx_arch_word == 4; }

uint32_t lx_timespec_bytes(void) { return narrow() && lx_ts32 ? 8 : 16; }

bool lx_get_timespec(uint64_t addr, l_timespec *ts)
{
    if (narrow() && lx_ts32) {
        int32_t w[2];
        if (!lx_get(w, addr, 8))
            return false;
        ts->sec = w[0];
        ts->nsec = w[1];
        return true;
    }
    if (narrow()) {
        /* i386 struct __kernel_timespec: 64-bit seconds, then a 64-bit nanoseconds word. */
        int64_t w[2];
        if (!lx_get(w, addr, 16))
            return false;
        ts->sec = w[0];
        ts->nsec = w[1];
        return true;
    }
    return lx_get(ts, addr, sizeof *ts);
}

bool lx_put_timespec(uint64_t addr, int64_t sec, int64_t nsec)
{
    if (narrow() && lx_ts32) {
        int32_t w[2] = { (int32_t)sec, (int32_t)nsec };
        return lx_put(addr, w, 8);
    }
    int64_t w[2] = { sec, nsec };
    return lx_put(addr, w, 16);
}

bool lx_put_timeval(uint64_t addr, int64_t sec, int64_t usec) { return lx_put_longs(addr, (int64_t[2]){ sec, usec }, 2); }

bool lx_get_timespec2(uint64_t addr, int64_t t[4])
{
    uint32_t sz = lx_timespec_bytes();
    l_timespec a, b;
    if (!lx_get_timespec(addr, &a) || !lx_get_timespec(addr + sz, &b))
        return false;
    t[0] = a.sec;
    t[1] = a.nsec;
    t[2] = b.sec;
    t[3] = b.nsec;
    return true;
}

bool lx_get_longs(uint64_t addr, int64_t *out, unsigned n)
{
    if (!narrow())
        return lx_get(out, addr, (uint64_t)n * 8);
    for (unsigned i = 0; i < n; i++) {
        int32_t v;
        if (!lx_get(&v, addr + 4ull * i, 4))
            return false;
        out[i] = v;
    }
    return true;
}

bool lx_put_longs(uint64_t addr, const int64_t *v, unsigned n)
{
    if (!narrow())
        return lx_put(addr, v, (uint64_t)n * 8);
    int32_t w[32];
    if (n > 32)
        return false;
    for (unsigned i = 0; i < n; i++)
        w[i] = (int32_t)v[i];
    return lx_put(addr, w, 4ull * n);
}

bool lx_put_ulongs(uint64_t addr, const uint64_t *v, unsigned n)
{
    if (!narrow())
        return lx_put(addr, v, (uint64_t)n * 8);
    uint32_t w[32];
    if (n > 32)
        return false;
    for (unsigned i = 0; i < n; i++)
        w[i] = v[i] == ~0ull ? ~0u : (uint32_t)v[i];
    return lx_put(addr, w, 4ull * n);
}

bool lx_get_ptr(uint64_t addr, uint64_t *v)
{
    if (!narrow())
        return lx_get(v, addr, 8);
    uint32_t w;
    if (!lx_get(&w, addr, 4))
        return false;
    *v = w;
    return true;
}

bool lx_put_ptr(uint64_t addr, uint64_t v)
{
    if (!narrow())
        return lx_put(addr, &v, 8);
    uint32_t w = (uint32_t)v;
    return lx_put(addr, &w, 4);
}

/** @brief struct stat64 of i386 (asm/stat.h), packed, 96 bytes. */
static void pack_stat64(uint8_t *o, const l_stat *st)
{
    memset(o, 0, 96);
    uint32_t u;
    uint64_t q;
    q = st->st_dev;
    memcpy(o + 0, &q, 8);
    u = (uint32_t)st->st_ino;
    memcpy(o + 12, &u, 4);
    memcpy(o + 16, &st->st_mode, 4);
    memcpy(o + 20, &st->st_nlink, 4);
    memcpy(o + 24, &st->st_uid, 4);
    memcpy(o + 28, &st->st_gid, 4);
    q = st->st_rdev;
    memcpy(o + 32, &q, 8);
    memcpy(o + 44, &st->st_size, 8);
    u = (uint32_t)st->st_blksize;
    memcpy(o + 52, &u, 4);
    memcpy(o + 56, &st->st_blocks, 8);
    int32_t t[6] = { (int32_t)st->st_atime_sec, (int32_t)st->st_atime_ns, (int32_t)st->st_mtime_sec,
                     (int32_t)st->st_mtime_ns,  (int32_t)st->st_ctime_sec, (int32_t)st->st_ctime_ns };
    memcpy(o + 64, t, 24);
    memcpy(o + 88, &st->st_ino, 8);
}

bool lx_put_stat(uint64_t addr, const l_stat *st)
{
    if (!narrow())
        return lx_put(addr, st, sizeof *st);
    uint8_t o[96];
    pack_stat64(o, st);
    return lx_put(addr, o, sizeof o);
}

bool lx_get_iov(uint64_t addr, uint64_t i, uint64_t e[2])
{
    if (!narrow())
        return lx_get(e, addr + 16 * i, 16);
    uint32_t w[2];
    if (!lx_get(w, addr + 8 * i, 8))
        return false;
    e[0] = w[0];
    e[1] = w[1];
    return true;
}

bool lx_put_sysinfo(uint64_t addr, uint64_t uptime, uint64_t ram, uint16_t procs)
{
    if (!narrow()) {
        uint8_t b[112] = { 0 };
        uint32_t unit = 1;
        memcpy(b, &uptime, 8);
        memcpy(b + 32, &ram, 8);
        memcpy(b + 40, &ram, 8);
        memcpy(b + 80, &procs, 2);
        memcpy(b + 104, &unit, 4);
        return lx_put(addr, b, sizeof b);
    }
    uint8_t b[64] = { 0 };
    uint32_t up = (uint32_t)uptime, r = ram > 0xffffffffull ? 0xffffffffu : (uint32_t)ram, unit = 1;
    memcpy(b, &up, 4);
    memcpy(b + 16, &r, 4);
    memcpy(b + 20, &r, 4);
    memcpy(b + 40, &procs, 2);
    memcpy(b + 52, &unit, 4);
    return lx_put(addr, b, sizeof b);
}

uint32_t lx_epoll_event_bytes(void) { return narrow() ? 12 : 16; }

bool lx_get_epoll_event(uint64_t addr, uint32_t *events, uint64_t *data)
{
    uint8_t b[16] = { 0 };
    if (!lx_get(b, addr, lx_epoll_event_bytes()))
        return false;
    memcpy(events, b, 4);
    memcpy(data, b + (narrow() ? 4 : 8), 8);
    return true;
}

bool lx_put_epoll_event(uint64_t addr, uint64_t i, uint32_t events, uint64_t data)
{
    uint8_t b[16] = { 0 };
    memcpy(b, &events, 4);
    memcpy(b + (narrow() ? 4 : 8), &data, 8);
    return lx_put(addr + i * lx_epoll_event_bytes(), b, lx_epoll_event_bytes());
}

bool lx_get_sigaction(uint64_t addr, uint64_t k[4])
{
    if (!narrow())
        return lx_get(k, addr, 32);
    uint32_t w[3];
    uint64_t mask;
    if (!lx_get(w, addr, 12) || !lx_get(&mask, addr + 12, 8))
        return false;
    k[0] = w[0];
    k[1] = w[1];
    k[2] = w[2];
    k[3] = mask;
    return true;
}

bool lx_put_sigaction(uint64_t addr, const uint64_t k[4])
{
    if (!narrow())
        return lx_put(addr, k, 32);
    uint32_t w[3] = { (uint32_t)k[0], (uint32_t)k[1], (uint32_t)k[2] };
    return lx_put(addr, w, 12) && lx_put(addr + 12, &k[3], 8);
}

bool lx_get_stack_t(uint64_t addr, uint64_t *sp, int32_t *flags, uint64_t *size)
{
    if (!narrow()) {
        uint8_t n[24];
        if (!lx_get(n, addr, sizeof n))
            return false;
        memcpy(sp, n, 8);
        memcpy(flags, n + 8, 4);
        memcpy(size, n + 16, 8);
        return true;
    }
    uint32_t w[3];
    if (!lx_get(w, addr, 12))
        return false;
    *sp = w[0];
    *flags = (int32_t)w[1];
    *size = w[2];
    return true;
}

bool lx_put_stack_t(uint64_t addr, uint64_t sp, int32_t flags, uint64_t size)
{
    if (!narrow()) {
        uint8_t o[24] = { 0 };
        memcpy(o, &sp, 8);
        memcpy(o + 8, &flags, 4);
        memcpy(o + 16, &size, 8);
        return lx_put(addr, o, sizeof o);
    }
    uint32_t w[3] = { (uint32_t)sp, (uint32_t)flags, (uint32_t)size };
    return lx_put(addr, w, 12);
}

uint32_t lx_siginfo_union_offset(void) { return narrow() ? 12 : 16; }
