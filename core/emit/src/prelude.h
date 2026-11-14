#ifndef RELIFT_LIFTED_PRELUDE_H
#define RELIFT_LIFTED_PRELUDE_H

#include <stdint.h>
#include <string.h>

typedef __int128 rl_e_s128;

static inline int64_t rl_e_sx64(uint64_t v, unsigned w)
{
    return w >= 64 ? (int64_t)v : (int64_t)(v << (64 - w)) >> (64 - w);
}

static inline rl_e_s128 rl_e_sx128(rl_u128 v, unsigned w)
{
    return w >= 128 ? (rl_e_s128)v : (rl_e_s128)(v << (128 - w)) >> (128 - w);
}

static inline uint64_t rl_e_m64(unsigned w) { return w >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << w) - 1; }

static inline rl_u128 rl_e_m128(unsigned w) { return w >= 128 ? ~(rl_u128)0 : ((rl_u128)1 << w) - 1; }

static inline uint64_t rl_e_clz64(uint64_t x, unsigned w) { return x ? (uint64_t)__builtin_clzll(x) - (64 - w) : w; }

static inline uint64_t rl_e_ctz64(uint64_t x, unsigned w) { return x ? (uint64_t)__builtin_ctzll(x) : w; }

static inline rl_u128 rl_e_clz128(rl_u128 x, unsigned w)
{
    uint64_t hi = (uint64_t)(x >> 64), lo = (uint64_t)x;
    if (!x)
        return w;
    return (hi ? (rl_u128)__builtin_clzll(hi) : 64 + (rl_u128)__builtin_clzll(lo)) - (128 - w);
}

static inline rl_u128 rl_e_ctz128(rl_u128 x, unsigned w)
{
    uint64_t hi = (uint64_t)(x >> 64), lo = (uint64_t)x;
    if (!x)
        return w;
    return lo ? (rl_u128)__builtin_ctzll(lo) : 64 + (rl_u128)__builtin_ctzll(hi);
}

static inline rl_u128 rl_e_popcount128(rl_u128 x)
{
    return (rl_u128)(__builtin_popcountll((uint64_t)x) + __builtin_popcountll((uint64_t)(x >> 64)));
}

static inline uint64_t rl_e_rbit64(uint64_t x)
{
    uint64_t r = 0;
    for (int i = 0; i < 64; i++)
        r |= ((x >> i) & 1) << (63 - i);
    return r;
}

static inline rl_u128 rl_e_rbit128(rl_u128 x)
{
    return ((rl_u128)rl_e_rbit64((uint64_t)x) << 64) | rl_e_rbit64((uint64_t)(x >> 64));
}

static inline rl_u128 rl_e_bswap128(rl_u128 x)
{
    return ((rl_u128)__builtin_bswap64((uint64_t)x) << 64) | __builtin_bswap64((uint64_t)(x >> 64));
}

static inline uint64_t rl_e_sdiv64(uint64_t a, uint64_t b, unsigned w)
{
    int64_t x = rl_e_sx64(a, w), y = rl_e_sx64(b, w);
    if (y == 0)
        return 0;
    if (y == -1)
        return 0 - (uint64_t)x;
    return (uint64_t)(x / y);
}

static inline uint64_t rl_e_srem64(uint64_t a, uint64_t b, unsigned w)
{
    int64_t x = rl_e_sx64(a, w), y = rl_e_sx64(b, w);
    if (y == 0)
        return a;
    if (y == -1)
        return 0;
    return (uint64_t)(x % y);
}

static inline rl_u128 rl_e_sdiv128(rl_u128 a, rl_u128 b, unsigned w)
{
    rl_e_s128 x = rl_e_sx128(a, w), y = rl_e_sx128(b, w);
    if (y == 0)
        return 0;
    if (y == -1)
        return 0 - (rl_u128)x;
    return (rl_u128)(x / y);
}

static inline rl_u128 rl_e_srem128(rl_u128 a, rl_u128 b, unsigned w)
{
    rl_e_s128 x = rl_e_sx128(a, w), y = rl_e_sx128(b, w);
    if (y == 0)
        return a;
    if (y == -1)
        return 0;
    return (rl_u128)(x % y);
}

static inline uint64_t rl_e_ror64(uint64_t a, unsigned r, unsigned w)
{
    return r == 0 ? a : ((a >> r) | (a << (w - r))) & rl_e_m64(w);
}

static inline rl_u128 rl_e_ror128(rl_u128 a, unsigned r, unsigned w)
{
    return r == 0 ? a : ((a >> r) | (a << (w - r))) & rl_e_m128(w);
}

static inline float rl_e_f32(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }

static inline double rl_e_f64(uint64_t b) { double f; memcpy(&f, &b, 8); return f; }

static inline uint32_t rl_e_b32(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

static inline uint64_t rl_e_b64(double f) { uint64_t b; memcpy(&b, &f, 8); return b; }

#endif
