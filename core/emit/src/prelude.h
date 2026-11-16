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

/* A doubleword of word-layout RAM: its two host-order words swapped into one value. */
static inline uint64_t rl_e_rot32(uint64_t x) { return x << 32 | x >> 32; }

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

/*
 * Lane operations (relift_ir::lane, docs/abi.md section 16.1) on rl_e_vq
 * values read as 8, 16, 32 or 64-bit lanes, lane 0 the least significant.
 * rl_lane_KINDn is kind KIND on n-bit lanes; the bitwise kinds have no n.
 *
 * With clang's vector extensions (RL_LANE_VEC) every helper is a
 * target-independent vector expression, so the compiler picks the NEON,
 * SSE2, SSSE3, SSE4.1 or wasm SIMD128 instruction that the build's target
 * flags allow. Defining RL_LANE_SCALAR before this header, or a compiler
 * without the extensions, gives the portable per-lane loops instead. Both
 * assume a little-endian host, as the rest of the prelude does.
 */
#if !defined(RL_LANE_SCALAR) && defined(__clang__) && defined(__has_builtin)
#if __has_builtin(__builtin_elementwise_add_sat) && __has_builtin(__builtin_elementwise_min) &&                       \
    __has_builtin(__builtin_convertvector) && __has_builtin(__builtin_shufflevector)
#define RL_LANE_VEC 1
#endif
#endif

#define RL_L_ID(...) __VA_ARGS__

#if defined(RL_LANE_VEC)
typedef uint8_t rl_l_u8 __attribute__((vector_size(16)));
typedef int8_t rl_l_s8 __attribute__((vector_size(16)));
typedef uint16_t rl_l_u16 __attribute__((vector_size(16)));
typedef int16_t rl_l_s16 __attribute__((vector_size(16)));
typedef uint32_t rl_l_u32 __attribute__((vector_size(16)));
typedef int32_t rl_l_s32 __attribute__((vector_size(16)));
typedef uint64_t rl_l_u64 __attribute__((vector_size(16)));
typedef int64_t rl_l_s64 __attribute__((vector_size(16)));
/* Twice the lanes of the type named by the suffix's lane width, 32 bytes. */
typedef uint16_t rl_l_u16x2 __attribute__((vector_size(32)));
typedef int16_t rl_l_s16x2 __attribute__((vector_size(32)));
typedef uint32_t rl_l_u32x2 __attribute__((vector_size(32)));
typedef int32_t rl_l_s32x2 __attribute__((vector_size(32)));
typedef uint64_t rl_l_u64x2 __attribute__((vector_size(32)));
typedef int64_t rl_l_s64x2 __attribute__((vector_size(32)));
/* Half the lanes, 8 bytes. */
typedef uint8_t rl_l_u8h __attribute__((vector_size(8)));
typedef int8_t rl_l_s8h __attribute__((vector_size(8)));
typedef uint16_t rl_l_u16h __attribute__((vector_size(8)));
typedef int16_t rl_l_s16h __attribute__((vector_size(8)));
typedef uint32_t rl_l_u32h __attribute__((vector_size(8)));
typedef int32_t rl_l_s32h __attribute__((vector_size(8)));

static inline rl_l_u8 rl_l_in(rl_e_vq v)
{
    rl_l_u8 r;
    memcpy(&r, &v, 16);
    return r;
}

static inline rl_e_vq rl_l_out(rl_l_u8 v)
{
    rl_e_vq r;
    memcpy(&r, &v, 16);
    return r;
}

#define RL_L_lo8 0, 1, 2, 3, 4, 5, 6, 7
#define RL_L_hi8 8, 9, 10, 11, 12, 13, 14, 15
#define RL_L_lo16 0, 1, 2, 3
#define RL_L_hi16 4, 5, 6, 7
#define RL_L_lo32 0, 1
#define RL_L_hi32 2, 3
#define RL_L_ALL16 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
#define RL_L_ALL32 0, 1, 2, 3, 4, 5, 6, 7
#define RL_L_ALL64 0, 1, 2, 3

#define RL_L_ARGS1(n)                                                                                                  \
    rl_l_u##n a = (rl_l_u##n)rl_l_in(a_);                                                                              \
    rl_l_s##n sa = (rl_l_s##n)a;                                                                                       \
    (void)sa
#define RL_L_ARGS2(n)                                                                                                  \
    RL_L_ARGS1(n);                                                                                                     \
    rl_l_u##n b = (rl_l_u##n)rl_l_in(b_);                                                                              \
    rl_l_s##n sb = (rl_l_s##n)b;                                                                                       \
    (void)sb

#define RL_LANE1(name, n, ve, se)                                                                                      \
    static inline rl_e_vq rl_lane_##name##n(rl_e_vq a_)                                                                \
    {                                                                                                                  \
        RL_L_ARGS1(n);                                                                                                 \
        return rl_l_out((rl_l_u8)(ve));                                                                                \
    }
#define RL_LANE2(name, n, ve, se)                                                                                      \
    static inline rl_e_vq rl_lane_##name##n(rl_e_vq a_, rl_e_vq b_)                                                    \
    {                                                                                                                  \
        RL_L_ARGS2(n);                                                                                                 \
        return rl_l_out((rl_l_u8)(ve));                                                                                \
    }
#define RL_LANEI(name, n, ve, se)                                                                                      \
    static inline rl_e_vq rl_lane_##name##n(rl_e_vq a_, unsigned k)                                                    \
    {                                                                                                                  \
        RL_L_ARGS1(n);                                                                                                 \
        return rl_l_out((rl_l_u8)(ve));                                                                                \
    }

#define RL_L_MIN(x, y) __builtin_elementwise_min(x, y)
#define RL_L_MAX(x, y) __builtin_elementwise_max(x, y)
#define RL_L_MASK(n, c) ((rl_l_u##n)(c))

static inline rl_e_vq rl_lane_insert8(rl_e_vq a_, unsigned k, uint8_t s)
{
    rl_l_u8 a = rl_l_in(a_);
    a[k] = s;
    return rl_l_out(a);
}

static inline rl_e_vq rl_lane_insert16(rl_e_vq a_, unsigned k, uint16_t s)
{
    rl_l_u16 a = (rl_l_u16)rl_l_in(a_);
    a[k] = s;
    return rl_l_out((rl_l_u8)a);
}

static inline rl_e_vq rl_lane_insert32(rl_e_vq a_, unsigned k, uint32_t s)
{
    rl_l_u32 a = (rl_l_u32)rl_l_in(a_);
    a[k] = s;
    return rl_l_out((rl_l_u8)a);
}

static inline rl_e_vq rl_lane_insert64(rl_e_vq a_, unsigned k, uint64_t s)
{
    rl_l_u64 a = (rl_l_u64)rl_l_in(a_);
    a[k] = s;
    return rl_l_out((rl_l_u8)a);
}

static inline rl_e_vq rl_lane_splat8(uint8_t s) { return rl_l_out((rl_l_u8){} + s); }
static inline rl_e_vq rl_lane_splat16(uint16_t s) { return rl_l_out((rl_l_u8)((rl_l_u16){} + s)); }
static inline rl_e_vq rl_lane_splat32(uint32_t s) { return rl_l_out((rl_l_u8)((rl_l_u32){} + s)); }
static inline rl_e_vq rl_lane_splat64(uint64_t s) { return rl_l_out((rl_l_u8)((rl_l_u64){} + s)); }

static inline rl_e_vq rl_lane_select(rl_e_vq m_, rl_e_vq a_, rl_e_vq b_)
{
    rl_l_u8 m = rl_l_in(m_), a = rl_l_in(a_), b = rl_l_in(b_);
    return rl_l_out((m & a) | (~m & b));
}

/*
 * Widening: the low or high half of a (lanes of n bits) extended to lanes
 * of w = 2n bits, sign (s) or zero (u).
 */
#define RL_L_WIDEN(n, w)                                                                                               \
    static inline rl_e_vq rl_lane_widen_lo_s##n(rl_e_vq a_)                                                            \
    {                                                                                                                  \
        RL_L_ARGS1(n);                                                                                                 \
        rl_l_s##n##h h = __builtin_shufflevector(sa, sa, RL_L_lo##n);                                                  \
        return rl_l_out((rl_l_u8)__builtin_convertvector(h, rl_l_s##w));                                               \
    }                                                                                                                  \
    static inline rl_e_vq rl_lane_widen_hi_s##n(rl_e_vq a_)                                                            \
    {                                                                                                                  \
        RL_L_ARGS1(n);                                                                                                 \
        rl_l_s##n##h h = __builtin_shufflevector(sa, sa, RL_L_hi##n);                                                  \
        return rl_l_out((rl_l_u8)__builtin_convertvector(h, rl_l_s##w));                                               \
    }                                                                                                                  \
    static inline rl_e_vq rl_lane_widen_lo_u##n(rl_e_vq a_)                                                            \
    {                                                                                                                  \
        RL_L_ARGS1(n);                                                                                                 \
        rl_l_u##n##h h = __builtin_shufflevector(a, a, RL_L_lo##n);                                                    \
        return rl_l_out((rl_l_u8)__builtin_convertvector(h, rl_l_u##w));                                               \
    }                                                                                                                  \
    static inline rl_e_vq rl_lane_widen_hi_u##n(rl_e_vq a_)                                                            \
    {                                                                                                                  \
        RL_L_ARGS1(n);                                                                                                 \
        rl_l_u##n##h h = __builtin_shufflevector(a, a, RL_L_hi##n);                                                    \
        return rl_l_out((rl_l_u8)__builtin_convertvector(h, rl_l_u##w));                                               \
    }

/*
 * Widening multiplies: the full products of the low or high halves, signed
 * x signed (ss), unsigned x unsigned (uu) or signed a x unsigned b (su).
 */
#define RL_L_MULWIDE(half, n, w)                                                                                       \
    static inline rl_e_vq rl_lane_mul_wide_##half##_ss##n(rl_e_vq a_, rl_e_vq b_)                                    \
    {                                                                                                                  \
        RL_L_ARGS2(n);                                                                                                 \
        rl_l_s##w x = __builtin_convertvector(__builtin_shufflevector(sa, sa, RL_L_##half##n), rl_l_s##w);           \
        rl_l_s##w y = __builtin_convertvector(__builtin_shufflevector(sb, sb, RL_L_##half##n), rl_l_s##w);           \
        return rl_l_out((rl_l_u8)(x * y));                                                                             \
    }                                                                                                                  \
    static inline rl_e_vq rl_lane_mul_wide_##half##_uu##n(rl_e_vq a_, rl_e_vq b_)                                    \
    {                                                                                                                  \
        RL_L_ARGS2(n);                                                                                                 \
        rl_l_u##w x = __builtin_convertvector(__builtin_shufflevector(a, a, RL_L_##half##n), rl_l_u##w);             \
        rl_l_u##w y = __builtin_convertvector(__builtin_shufflevector(b, b, RL_L_##half##n), rl_l_u##w);             \
        return rl_l_out((rl_l_u8)(x * y));                                                                             \
    }                                                                                                                  \
    static inline rl_e_vq rl_lane_mul_wide_##half##_su##n(rl_e_vq a_, rl_e_vq b_)                                    \
    {                                                                                                                  \
        RL_L_ARGS2(n);                                                                                                 \
        rl_l_s##w x = __builtin_convertvector(__builtin_shufflevector(sa, sa, RL_L_##half##n), rl_l_s##w);           \
        rl_l_s##w y = (rl_l_s##w)__builtin_convertvector(__builtin_shufflevector(b, b, RL_L_##half##n), rl_l_u##w);  \
        return rl_l_out((rl_l_u8)(x * y));                                                                             \
    }

/*
 * The high n bits of the 2n-bit products of all lanes, computed in the
 * 32-byte type of 2n-bit lanes (x2); the signed x unsigned one is below.
 */
#define RL_L_MULHI(n, w)                                                                                               \
    static inline rl_e_vq rl_lane_mul_hi_s##n(rl_e_vq a_, rl_e_vq b_)                                                \
    {                                                                                                                  \
        RL_L_ARGS2(n);                                                                                                 \
        rl_l_s##w##x2 p = __builtin_convertvector(sa, rl_l_s##w##x2) * __builtin_convertvector(sb, rl_l_s##w##x2);   \
        return rl_l_out((rl_l_u8)__builtin_convertvector(p >> n, rl_l_s##n));                                         \
    }                                                                                                                  \
    static inline rl_e_vq rl_lane_mul_hi_u##n(rl_e_vq a_, rl_e_vq b_)                                                \
    {                                                                                                                  \
        RL_L_ARGS2(n);                                                                                                 \
        rl_l_u##w##x2 p = __builtin_convertvector(a, rl_l_u##w##x2) * __builtin_convertvector(b, rl_l_u##w##x2);     \
        return rl_l_out((rl_l_u8)__builtin_convertvector(p >> n, rl_l_u##n));                                         \
    }

/*
 * Narrowing: the lanes of a then b (n bits each) in one 32-byte value,
 * clamped as the kind says and truncated to n/2 bits (h).
 */
#define RL_L_NARROW(n, h, smin, smax, umax)                                                                            \
    static inline rl_e_vq rl_lane_narrow_ss##n(rl_e_vq a_, rl_e_vq b_)                                               \
    {                                                                                                                  \
        RL_L_ARGS2(n);                                                                                                 \
        rl_l_s##n##x2 c = __builtin_shufflevector(sa, sb, RL_L_ALL##n);                                                \
        c = RL_L_MAX(RL_L_MIN(c, (rl_l_s##n##x2){} + (smax)), (rl_l_s##n##x2){} + (smin));                             \
        return rl_l_out((rl_l_u8)__builtin_convertvector(c, rl_l_s##h));                                              \
    }                                                                                                                  \
    static inline rl_e_vq rl_lane_narrow_uu##n(rl_e_vq a_, rl_e_vq b_)                                               \
    {                                                                                                                  \
        RL_L_ARGS2(n);                                                                                                 \
        rl_l_u##n##x2 c = __builtin_shufflevector(a, b, RL_L_ALL##n);                                                  \
        c = RL_L_MIN(c, (rl_l_u##n##x2){} + (umax));                                                                   \
        return rl_l_out((rl_l_u8)__builtin_convertvector(c, rl_l_u##h));                                              \
    }                                                                                                                  \
    static inline rl_e_vq rl_lane_narrow_su##n(rl_e_vq a_, rl_e_vq b_)                                               \
    {                                                                                                                  \
        RL_L_ARGS2(n);                                                                                                 \
        rl_l_s##n##x2 c = __builtin_shufflevector(sa, sb, RL_L_ALL##n);                                                \
        c = RL_L_MAX(RL_L_MIN(c, (rl_l_s##n##x2){} + (umax)), (rl_l_s##n##x2){});                                      \
        return rl_l_out((rl_l_u8)__builtin_convertvector(c, rl_l_u##h));                                              \
    }
#define RL_L_TRUNC(n, h)                                                                                               \
    static inline rl_e_vq rl_lane_narrow_trunc##n(rl_e_vq a_, rl_e_vq b_)                                            \
    {                                                                                                                  \
        RL_L_ARGS2(n);                                                                                                 \
        return rl_l_out((rl_l_u8)__builtin_convertvector(__builtin_shufflevector(a, b, RL_L_ALL##n), rl_l_u##h));    \
    }

/*
 * Byte permutations. RL_LANE_PERM1 takes byte I[i] of a, or 0 where I[i] is
 * 16. RL_LANE_PERM2 takes byte J[i] of a (0 to 15) or of b (16 to 31), or 0
 * where J[i] is -1, and O is i where J[i] picks a byte and 16 elsewhere.
 * SSSE3 has no two-source byte shuffle; two pshufb whose patterns zero the
 * bytes of the other source and an or do it, the zeros included.
 */
#define RL_LANE_PERM1(a, I) rl_l_out(__builtin_shufflevector(rl_l_in(a), (rl_l_u8){}, RL_L_ID I))
#if defined(__SSSE3__)
typedef char rl_l_c8 __attribute__((vector_size(16)));

static inline rl_e_vq rl_lane_perm2_(rl_e_vq a, rl_e_vq b, rl_l_u8 j)
{
    rl_l_u8 k = j ^ 0x10;
    rl_l_c8 ja = (rl_l_c8)(j | ((j & 0x10) << 3)), jb = (rl_l_c8)(k | ((k & 0x10) << 3));
    return rl_l_out((rl_l_u8)(__builtin_ia32_pshufb128((rl_l_c8)rl_l_in(a), ja) |
                              __builtin_ia32_pshufb128((rl_l_c8)rl_l_in(b), jb)));
}

#define RL_LANE_PERM2(a, b, J, O) rl_lane_perm2_(a, b, (rl_l_u8)(rl_l_s8){RL_L_ID J})
#else
#define RL_LANE_PERM2(a, b, J, O)                                                                                      \
    rl_l_out(__builtin_shufflevector(__builtin_shufflevector(rl_l_in(a), rl_l_in(b), RL_L_ID J), (rl_l_u8){},         \
                                     RL_L_ID O))
#endif
#else
typedef union {
    rl_e_vq v;
    uint8_t u8[16];
    int8_t s8[16];
    uint16_t u16[8];
    int16_t s16[8];
    uint32_t u32[4];
    int32_t s32[4];
    uint64_t u64[2];
    int64_t s64[2];
} rl_l_x;

static inline int64_t rl_l_clamp(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }

#define RL_L_MIN(x, y) ((x) < (y) ? (x) : (y))
#define RL_L_MAX(x, y) ((x) > (y) ? (x) : (y))
#define RL_L_MASK(n, c) (0 - (uint64_t)(c))

#define RL_L_LOOP(n, se)                                                                                               \
    for (unsigned i = 0; i < 128 / n; i++) {                                                                           \
        uint64_t x = a.u##n[i], y = b.u##n[i];                                                                         \
        int64_t sx = a.s##n[i], sy = b.s##n[i];                                                                        \
        (void)x;                                                                                                       \
        (void)y;                                                                                                       \
        (void)sx;                                                                                                      \
        (void)sy;                                                                                                      \
        r.u##n[i] = (uint##n##_t)(se);                                                                                 \
    }                                                                                                                  \
    return r.v

#define RL_LANE1(name, n, ve, se)                                                                                      \
    static inline rl_e_vq rl_lane_##name##n(rl_e_vq a_)                                                                \
    {                                                                                                                  \
        rl_l_x a, b, r;                                                                                                \
        a.v = a_;                                                                                                      \
        b.v = a_;                                                                                                      \
        RL_L_LOOP(n, se);                                                                                              \
    }
#define RL_LANE2(name, n, ve, se)                                                                                      \
    static inline rl_e_vq rl_lane_##name##n(rl_e_vq a_, rl_e_vq b_)                                                    \
    {                                                                                                                  \
        rl_l_x a, b, r;                                                                                                \
        a.v = a_;                                                                                                      \
        b.v = b_;                                                                                                      \
        RL_L_LOOP(n, se);                                                                                              \
    }
#define RL_LANEI(name, n, ve, se)                                                                                      \
    static inline rl_e_vq rl_lane_##name##n(rl_e_vq a_, unsigned k)                                                    \
    {                                                                                                                  \
        rl_l_x a, b, r;                                                                                                \
        a.v = a_;                                                                                                      \
        b.v = a_;                                                                                                      \
        RL_L_LOOP(n, se);                                                                                              \
    }

#define RL_L_INSERT(n)                                                                                                 \
    static inline rl_e_vq rl_lane_insert##n(rl_e_vq a_, unsigned k, uint##n##_t s)                                     \
    {                                                                                                                  \
        rl_l_x a;                                                                                                      \
        a.v = a_;                                                                                                      \
        a.u##n[k] = s;                                                                                                 \
        return a.v;                                                                                                    \
    }                                                                                                                  \
    static inline rl_e_vq rl_lane_splat##n(uint##n##_t s)                                                              \
    {                                                                                                                  \
        rl_l_x r;                                                                                                      \
        for (unsigned i = 0; i < 128 / n; i++)                                                                         \
            r.u##n[i] = s;                                                                                             \
        return r.v;                                                                                                    \
    }
RL_L_INSERT(8)
RL_L_INSERT(16)
RL_L_INSERT(32)
RL_L_INSERT(64)

static inline rl_e_vq rl_lane_select(rl_e_vq m_, rl_e_vq a_, rl_e_vq b_)
{
    rl_l_x m, a, b, r;
    m.v = m_;
    a.v = a_;
    b.v = b_;
    for (unsigned i = 0; i < 2; i++)
        r.u64[i] = (m.u64[i] & a.u64[i]) | (~m.u64[i] & b.u64[i]);
    return r.v;
}

#define RL_L_WIDEN1(name, n, w, f, base)                                                                               \
    static inline rl_e_vq rl_lane_##name##n(rl_e_vq a_)                                                                \
    {                                                                                                                  \
        rl_l_x a, r;                                                                                                   \
        a.v = a_;                                                                                                      \
        for (unsigned i = 0; i < 64 / n; i++)                                                                          \
            r.f##w[i] = a.f##n[(base) + i];                                                                            \
        return r.v;                                                                                                    \
    }
#define RL_L_WIDEN(n, w)                                                                                               \
    RL_L_WIDEN1(widen_lo_s, n, w, s, 0)                                                                                \
    RL_L_WIDEN1(widen_hi_s, n, w, s, 64 / n)                                                                           \
    RL_L_WIDEN1(widen_lo_u, n, w, u, 0)                                                                                \
    RL_L_WIDEN1(widen_hi_u, n, w, u, 64 / n)

#define RL_L_MULWIDE1(name, n, w, fa, fb, base)                                                                        \
    static inline rl_e_vq rl_lane_##name##n(rl_e_vq a_, rl_e_vq b_)                                                    \
    {                                                                                                                  \
        rl_l_x a, b, r;                                                                                                \
        a.v = a_;                                                                                                      \
        b.v = b_;                                                                                                      \
        for (unsigned i = 0; i < 64 / n; i++)                                                                          \
            r.u##w[i] = (uint##w##_t)((int64_t)a.fa##n[(base) + i] * (int64_t)b.fb##n[(base) + i]);                    \
        return r.v;                                                                                                    \
    }
#define RL_L_MULWIDE(half, n, w)                                                                                       \
    RL_L_MULWIDE1(mul_wide_##half##_ss, n, w, s, s, RL_L_BASE_##half(n))                                               \
    RL_L_MULWIDE1(mul_wide_##half##_uu, n, w, u, u, RL_L_BASE_##half(n))                                               \
    RL_L_MULWIDE1(mul_wide_##half##_su, n, w, s, u, RL_L_BASE_##half(n))
#define RL_L_BASE_lo(n) 0
#define RL_L_BASE_hi(n) (64 / n)

#define RL_L_MULHI(n, w)                                                                                               \
    RL_LANE2(mul_hi_s, n, , (uint64_t)((sx * sy) >> n))                                                                \
    RL_LANE2(mul_hi_u, n, , (x * y) >> n)

#define RL_L_NARROW1(name, n, h, f)                                                                                    \
    static inline rl_e_vq rl_lane_##name##n(rl_e_vq a_, rl_e_vq b_)                                                    \
    {                                                                                                                  \
        rl_l_x a, b, r;                                                                                                \
        a.v = a_;                                                                                                      \
        b.v = b_;                                                                                                      \
        for (unsigned i = 0; i < 128 / n; i++) {                                                                       \
            r.u##h[i] = (uint##h##_t)f(a, n, i);                                                                       \
            r.u##h[128 / n + i] = (uint##h##_t)f(b, n, i);                                                             \
        }                                                                                                              \
        return r.v;                                                                                                    \
    }
#define RL_L_N_SS(v, n, i) rl_l_clamp(v.s##n[i], -(INT64_C(1) << (n / 2 - 1)), (INT64_C(1) << (n / 2 - 1)) - 1)
#define RL_L_N_UU(v, n, i) (v.u##n[i] > (UINT64_C(1) << n / 2) - 1 ? (UINT64_C(1) << n / 2) - 1 : v.u##n[i])
#define RL_L_N_SU(v, n, i) rl_l_clamp(v.s##n[i], 0, (INT64_C(1) << n / 2) - 1)
#define RL_L_N_TR(v, n, i) v.u##n[i]
#define RL_L_NARROW(n, h, smin, smax, umax)                                                                            \
    RL_L_NARROW1(narrow_ss, n, h, RL_L_N_SS)                                                                           \
    RL_L_NARROW1(narrow_uu, n, h, RL_L_N_UU)                                                                           \
    RL_L_NARROW1(narrow_su, n, h, RL_L_N_SU)
#define RL_L_TRUNC(n, h) RL_L_NARROW1(narrow_trunc, n, h, RL_L_N_TR)

static inline rl_e_vq rl_lane_perm_(rl_e_vq a_, rl_e_vq b_, const int8_t *j)
{
    rl_l_x a, b, r;
    a.v = a_;
    b.v = b_;
    for (unsigned i = 0; i < 16; i++)
        r.u8[i] = j[i] < 0 ? 0 : j[i] < 16 ? a.u8[j[i]] : b.u8[j[i] - 16];
    return r.v;
}

#define RL_LANE_PERM1(a, I) rl_lane_perm_(a, RL_E_VQ(0, 0), (const int8_t[16]){RL_L_ID I})
#define RL_LANE_PERM2(a, b, J, O) rl_lane_perm_(a, b, (const int8_t[16]){RL_L_ID J})
#endif

/*
 * Each kind once: the vector expression on a, b (unsigned lanes) and sa,
 * sb (signed lanes), and the per-lane expression on x, y (unsigned) and
 * sx, sy (signed), both widened to 64 bits; k is the immediate.
 */
#define RL_L_COMMON(n)                                                                                                 \
    RL_LANE2(add, n, a + b, x + y)                                                                                     \
    RL_LANE2(sub, n, a - b, x - y)                                                                                     \
    RL_LANE1(neg, n, -a, 0 - x)                                                                                        \
    RL_LANE2(eq, n, a == b, RL_L_MASK(n, x == y))                                                                      \
    RL_LANE2(ne, n, a != b, RL_L_MASK(n, x != y))                                                                      \
    RL_LANEI(shli, n, a << k, x << k)                                                                                  \
    RL_LANEI(lshri, n, a >> k, x >> k)                                                                                 \
    RL_LANEI(ashri, n, sa >> k, (uint64_t)(sx >> k))                                                                   \
    RL_LANE2(shl, n, (a << (b & (n - 1))) & RL_L_MASK(n, b < n), y >= n ? 0 : x << y)                                  \
    RL_LANE2(lshr, n, (a >> (b & (n - 1))) & RL_L_MASK(n, b < n), y >= n ? 0 : x >> y)                                 \
    RL_LANE2(ashr, n, sa >> (rl_l_s##n)RL_L_MIN(b, (rl_l_u##n){} + (n - 1)), (uint64_t)(sx >> RL_L_MIN(y, n - 1)))    \
    RL_LANEI(broadcast, n, (rl_l_u##n){} + a[k], a.u##n[k])
#define RL_L_SMALL(n)                                                                                                  \
    RL_LANE2(min_s, n, RL_L_MIN(sa, sb), RL_L_MIN(sx, sy))                                                             \
    RL_LANE2(min_u, n, RL_L_MIN(a, b), RL_L_MIN(x, y))                                                                 \
    RL_LANE2(max_s, n, RL_L_MAX(sa, sb), RL_L_MAX(sx, sy))                                                             \
    RL_LANE2(max_u, n, RL_L_MAX(a, b), RL_L_MAX(x, y))                                                                 \
    RL_LANE1(abs, n, (rl_l_u##n)RL_L_MAX(sa, -sa), sx < 0 ? 0 - x : x)                                                 \
    RL_LANE2(slt, n, sa < sb, RL_L_MASK(n, sx < sy))                                                                   \
    RL_LANE2(sle, n, sa <= sb, RL_L_MASK(n, sx <= sy))                                                                 \
    RL_LANE2(ult, n, a < b, RL_L_MASK(n, x < y))                                                                       \
    RL_LANE2(ule, n, a <= b, RL_L_MASK(n, x <= y))
#define RL_L_SAT(n)                                                                                                    \
    RL_LANE2(add_sat_s, n, __builtin_elementwise_add_sat(sa, sb),                                                     \
             rl_l_clamp(sx + sy, -(INT64_C(1) << (n - 1)), (INT64_C(1) << (n - 1)) - 1))                               \
    RL_LANE2(add_sat_u, n, __builtin_elementwise_add_sat(a, b), RL_L_MIN(x + y, (UINT64_C(1) << n) - 1))               \
    RL_LANE2(sub_sat_s, n, __builtin_elementwise_sub_sat(sa, sb),                                                     \
             rl_l_clamp(sx - sy, -(INT64_C(1) << (n - 1)), (INT64_C(1) << (n - 1)) - 1))                               \
    RL_LANE2(sub_sat_u, n, __builtin_elementwise_sub_sat(a, b), x < y ? 0 : x - y)

RL_L_COMMON(8)
RL_L_COMMON(16)
RL_L_COMMON(32)
RL_L_COMMON(64)
RL_L_SMALL(8)
RL_L_SMALL(16)
RL_L_SMALL(32)
RL_L_SAT(8)
RL_L_SAT(16)
RL_LANE2(mul_lo, 16, a * b, x * y)
RL_LANE2(mul_lo, 32, a * b, x * y)
RL_L_MULHI(16, 32)
RL_L_MULHI(32, 64)
RL_L_WIDEN(8, 16)
RL_L_WIDEN(16, 32)
RL_L_WIDEN(32, 64)
RL_L_MULWIDE(lo, 8, 16)
RL_L_MULWIDE(hi, 8, 16)
RL_L_MULWIDE(lo, 16, 32)
RL_L_MULWIDE(hi, 16, 32)
RL_L_MULWIDE(lo, 32, 64)
RL_L_MULWIDE(hi, 32, 64)
RL_L_NARROW(16, 8, -128, 127, 255)
RL_L_NARROW(32, 16, -32768, 32767, 65535)
RL_L_TRUNC(16, 8)
RL_L_TRUNC(32, 16)
RL_L_TRUNC(64, 32)

static inline rl_e_vq rl_lane_and(rl_e_vq a, rl_e_vq b) { return a & b; }

/*
 * The high half of signed a x unsigned b is that of unsigned a x b less b
 * in the lanes where a is negative: a reads 2^n too high there.
 */
static inline rl_e_vq rl_lane_mul_hi_su16(rl_e_vq a, rl_e_vq b)
{
    return rl_lane_sub16(rl_lane_mul_hi_u16(a, b), rl_lane_and(b, rl_lane_ashri16(a, 15)));
}

static inline rl_e_vq rl_lane_mul_hi_su32(rl_e_vq a, rl_e_vq b)
{
    return rl_lane_sub32(rl_lane_mul_hi_u32(a, b), rl_lane_and(b, rl_lane_ashri32(a, 31)));
}
static inline rl_e_vq rl_lane_or(rl_e_vq a, rl_e_vq b) { return a | b; }
static inline rl_e_vq rl_lane_xor(rl_e_vq a, rl_e_vq b) { return a ^ b; }
static inline rl_e_vq rl_lane_and_not(rl_e_vq a, rl_e_vq b) { return a & ~b; }
static inline rl_e_vq rl_lane_not(rl_e_vq a) { return ~a; }

#endif
