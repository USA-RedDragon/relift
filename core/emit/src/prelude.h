#ifndef RELIFT_LIFTED_PRELUDE_H
#define RELIFT_LIFTED_PRELUDE_H

#include <stdint.h>
#include <string.h>

typedef __int128 rl_e_s128;

#define RL_E_SYNC(store, call, reload)                                                                                 \
    do {                                                                                                               \
        if (RL_UNLIKELY(budget <= 0)) {                                                                                \
            store                                                                                                      \
            rc = (call);                                                                                               \
            if (RL_UNLIKELY(rc))                                                                                       \
                return RL_PENDING;                                                                                     \
            reload                                                                                                     \
        }                                                                                                              \
    } while (0)

/*
 * A 64-bit register's context slot, read into a general register on its
 * own. The caller wrote the slots one by one; a plain read lets clang merge
 * two adjacent slots that feed a paired guest store into one 16-byte load,
 * which the host cannot forward from the two 8-byte stores still in flight
 * and stalls on.
 */
static inline uint64_t rl_e_ld64(const uint64_t *p)
{
    uint64_t v = *p;
    __asm__("" : "+r"(v));
    return v;
}

/*
 * rl_e_vq: the type of 128-bit registers and of most 128-bit values. On
 * arm64 a vector of two 64-bit lanes, which keeps them in NEON registers;
 * elsewhere rl_u128, which measured smaller on x86-64, where no vector
 * register survives a call. RL_E_VQ builds one from its halves and
 * rl_e_laneN reads lane i of N bits.
 */
#if defined(__aarch64__)
typedef uint64_t rl_e_vq __attribute__((vector_size(16)));
typedef uint32_t rl_e_vs __attribute__((vector_size(16)));
typedef uint16_t rl_e_vh __attribute__((vector_size(16)));
typedef uint8_t rl_e_vb __attribute__((vector_size(16)));

#define RL_E_VQ(lo, hi) ((rl_e_vq){(uint64_t)(lo), (uint64_t)(hi)})
#define rl_e_lane8(x, i) (((rl_e_vb)(x))[i])
#define rl_e_lane16(x, i) (((rl_e_vh)(x))[i])
#define rl_e_lane32(x, i) (((rl_e_vs)(x))[i])
#define rl_e_lane64(x, i) ((x)[i])

static inline rl_e_vq rl_e_u2v(rl_u128 u)
{
    rl_e_vq v;
    memcpy(&v, &u, 16);
    return v;
}

static inline rl_u128 rl_e_v2u(rl_e_vq v)
{
    rl_u128 u;
    memcpy(&u, &v, 16);
    return u;
}
#else
typedef rl_u128 rl_e_vq;

#define RL_E_VQ(lo, hi) (((rl_u128)(uint64_t)(hi) << 64) | (uint64_t)(lo))
#define rl_e_lane8(x, i) ((uint8_t)((x) >> (8 * (i))))
#define rl_e_lane16(x, i) ((uint16_t)((x) >> (16 * (i))))
#define rl_e_lane32(x, i) ((uint32_t)((x) >> (32 * (i))))
#define rl_e_lane64(x, i) ((uint64_t)((x) >> (64 * (i))))

static inline rl_e_vq rl_e_u2v(rl_u128 u) { return u; }

static inline rl_u128 rl_e_v2u(rl_e_vq v) { return v; }
#endif

/*
 * The carry (cf) and overflow (vf) of x + y and x - y, as A64 sets C and V:
 * a subtract carries when it does not borrow.
 */
static inline int rl_e_cf_add64(uint64_t x, uint64_t y) { return (uint64_t)(x + y) < x; }
static inline int rl_e_cf_sub64(uint64_t x, uint64_t y) { return x >= y; }
static inline int rl_e_vf_add64(uint64_t x, uint64_t y)
{
    int64_t t;
    return __builtin_add_overflow((int64_t)x, (int64_t)y, &t);
}
static inline int rl_e_vf_sub64(uint64_t x, uint64_t y)
{
    int64_t t;
    return __builtin_sub_overflow((int64_t)x, (int64_t)y, &t);
}
static inline int rl_e_cf_add32(uint32_t x, uint32_t y) { return (uint32_t)(x + y) < x; }
static inline int rl_e_cf_sub32(uint32_t x, uint32_t y) { return x >= y; }
static inline int rl_e_vf_add32(uint32_t x, uint32_t y)
{
    int32_t t;
    return __builtin_add_overflow((int32_t)x, (int32_t)y, &t);
}
static inline int rl_e_vf_sub32(uint32_t x, uint32_t y)
{
    int32_t t;
    return __builtin_sub_overflow((int32_t)x, (int32_t)y, &t);
}

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
