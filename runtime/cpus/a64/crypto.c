/**
 * @file crypto.c
 * @brief A64 cryptographic extension instructions (relift/a64/crypto.h):
 * portable C paths transcribed from the Arm pseudocode (the execute functions
 * of AESE, AESD, AESMC, AESIMC, PMULL, SHA1*, SHA256*, SHA512*, SM3* and SM4*
 * and their helpers AESShiftRows, AESSubBytes, AESMixColumns, SHAchoose,
 * SHAmajority, SHAparity, SHA256hash and Sbox), and host paths chosen at run
 * time.
 *
 * The S-box tables are computed from the field arithmetic the standards
 * define (FIPS-197 section 5.1.1 for AES, GB/T 32907-2016 for SM4) by the same
 * construction as core/ir/src/crypto.rs, and printed here as constants.
 *
 * Host paths:
 * - arm64: the ARMv8 crypto intrinsics of arm_neon.h, which are the guest
 *   instructions themselves. Each host function carries a target attribute,
 *   so the file builds without -march flags; features come from getauxval
 *   (Linux, Android) or sysctlbyname (macOS, iOS).
 * - x86-64: AES-NI, PCLMULQDQ and SHA-NI, detected with cpuid. The mapping
 *   of each instruction is in the comment above its host function and in
 *   docs/abi.md section 10.2. SHA-512, SM3 and SM4 have no x86-64 path.
 */
#include <relift/a64/crypto.h>

#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#if defined(__linux__)
#include <sys/auxv.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

static const uint8_t aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};
static const uint8_t aes_inv_sbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};
static const uint8_t sm4_sbox[256] = {
    0xd6, 0x90, 0xe9, 0xfe, 0xcc, 0xe1, 0x3d, 0xb7, 0x16, 0xb6, 0x14, 0xc2, 0x28, 0xfb, 0x2c, 0x05,
    0x2b, 0x67, 0x9a, 0x76, 0x2a, 0xbe, 0x04, 0xc3, 0xaa, 0x44, 0x13, 0x26, 0x49, 0x86, 0x06, 0x99,
    0x9c, 0x42, 0x50, 0xf4, 0x91, 0xef, 0x98, 0x7a, 0x33, 0x54, 0x0b, 0x43, 0xed, 0xcf, 0xac, 0x62,
    0xe4, 0xb3, 0x1c, 0xa9, 0xc9, 0x08, 0xe8, 0x95, 0x80, 0xdf, 0x94, 0xfa, 0x75, 0x8f, 0x3f, 0xa6,
    0x47, 0x07, 0xa7, 0xfc, 0xf3, 0x73, 0x17, 0xba, 0x83, 0x59, 0x3c, 0x19, 0xe6, 0x85, 0x4f, 0xa8,
    0x68, 0x6b, 0x81, 0xb2, 0x71, 0x64, 0xda, 0x8b, 0xf8, 0xeb, 0x0f, 0x4b, 0x70, 0x56, 0x9d, 0x35,
    0x1e, 0x24, 0x0e, 0x5e, 0x63, 0x58, 0xd1, 0xa2, 0x25, 0x22, 0x7c, 0x3b, 0x01, 0x21, 0x78, 0x87,
    0xd4, 0x00, 0x46, 0x57, 0x9f, 0xd3, 0x27, 0x52, 0x4c, 0x36, 0x02, 0xe7, 0xa0, 0xc4, 0xc8, 0x9e,
    0xea, 0xbf, 0x8a, 0xd2, 0x40, 0xc7, 0x38, 0xb5, 0xa3, 0xf7, 0xf2, 0xce, 0xf9, 0x61, 0x15, 0xa1,
    0xe0, 0xae, 0x5d, 0xa4, 0x9b, 0x34, 0x1a, 0x55, 0xad, 0x93, 0x32, 0x30, 0xf5, 0x8c, 0xb1, 0xe3,
    0x1d, 0xf6, 0xe2, 0x2e, 0x82, 0x66, 0xca, 0x60, 0xc0, 0x29, 0x23, 0xab, 0x0d, 0x53, 0x4e, 0x6f,
    0xd5, 0xdb, 0x37, 0x45, 0xde, 0xfd, 0x8e, 0x2f, 0x03, 0xff, 0x6a, 0x72, 0x6d, 0x6c, 0x5b, 0x51,
    0x8d, 0x1b, 0xaf, 0x92, 0xbb, 0xdd, 0xbc, 0x7f, 0x11, 0xd9, 0x5c, 0x41, 0x1f, 0x10, 0x5a, 0xd8,
    0x0a, 0xc1, 0x31, 0x88, 0xa5, 0xcd, 0x7b, 0xbd, 0x2d, 0x74, 0xd0, 0x12, 0xb8, 0xe5, 0xb4, 0xb0,
    0x89, 0x69, 0x97, 0x4a, 0x0c, 0x96, 0x77, 0x7e, 0x65, 0xb9, 0xf1, 0x09, 0xc5, 0x6e, 0xc6, 0x84,
    0x18, 0xf0, 0x7d, 0xec, 0x3a, 0xdc, 0x4d, 0x20, 0x79, 0xee, 0x5f, 0x3e, 0xd7, 0xcb, 0x39, 0x48,
};

/** @brief Word e (0 to 3) of a vector. */
static inline uint32_t w32(rl_u128 x, unsigned e) {
    return (uint32_t)(x >> (32 * e));
}

/** @brief The vector with words w0 (bits 31:0) to w3 (bits 127:96). */
static inline rl_u128 v32(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
    return (rl_u128)w0 | ((rl_u128)w1 << 32) | ((rl_u128)w2 << 64) | ((rl_u128)w3 << 96);
}

/** @brief The vector with doublewords lo (bits 63:0) and hi (bits 127:64). */
static inline rl_u128 v64(uint64_t lo, uint64_t hi) {
    return (rl_u128)lo | ((rl_u128)hi << 64);
}

static inline uint32_t rol32(uint32_t x, unsigned s) {
    return (x << s) | (x >> (32 - s));
}

static inline uint32_t ror32(uint32_t x, unsigned s) {
    return (x >> s) | (x << (32 - s));
}

static inline uint64_t ror64(uint64_t x, unsigned s) {
    return (x >> s) | (x << (64 - s));
}

/* ---- AES ---------------------------------------------------------------- */

/** @brief Byte i (0 to 15) of a vector. */
static inline uint8_t byte_of(rl_u128 x, unsigned i) {
    return (uint8_t)(x >> (8 * i));
}

/** @brief Each byte of x through the table t. */
static rl_u128 sub_bytes(rl_u128 x, const uint8_t t[256]) {
    rl_u128 r = 0;
    for (unsigned i = 0; i < 16; i++)
        r |= (rl_u128)t[byte_of(x, i)] << (8 * i);
    return r;
}

/**
 * @brief AESShiftRows (inverse when inv): byte 4c+r is row r of column c;
 * row r rotates left by r columns (right when inv).
 */
static rl_u128 shift_rows(rl_u128 x, int inv) {
    rl_u128 r = 0;
    for (unsigned c = 0; c < 4; c++)
        for (unsigned row = 0; row < 4; row++) {
            unsigned src = inv ? (c + 4 - row) % 4 : (c + row) % 4;
            r |= (rl_u128)byte_of(x, 4 * src + row) << (8 * (4 * c + row));
        }
    return r;
}

/** @brief a * b in GF(2^8) modulo x^8 + x^4 + x^3 + x + 1. */
static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1)
            r ^= a;
        b >>= 1;
        a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
    }
    return r;
}

/** @brief AESMixColumns with coefficients k (2, 3, 1, 1) or AESInvMixColumns (14, 11, 13, 9). */
static rl_u128 mix_columns(rl_u128 x, const uint8_t k[4]) {
    rl_u128 r = 0;
    for (unsigned c = 0; c < 4; c++)
        for (unsigned row = 0; row < 4; row++) {
            uint8_t v = 0;
            for (unsigned j = 0; j < 4; j++)
                v ^= gf_mul(byte_of(x, 4 * c + (row + j) % 4), k[j]);
            r |= (rl_u128)v << (8 * (4 * c + row));
        }
    return r;
}

static const uint8_t mc_k[4] = {2, 3, 1, 1};
static const uint8_t imc_k[4] = {14, 11, 13, 9};

rl_u128 rl_aese_portable(rl_u128 d, rl_u128 n) {
    return sub_bytes(shift_rows(d ^ n, 0), aes_sbox);
}

rl_u128 rl_aesd_portable(rl_u128 d, rl_u128 n) {
    return sub_bytes(shift_rows(d ^ n, 1), aes_inv_sbox);
}

rl_u128 rl_aesmc_portable(rl_u128 x) {
    return mix_columns(x, mc_k);
}

rl_u128 rl_aesimc_portable(rl_u128 x) {
    return mix_columns(x, imc_k);
}

/* ---- PMULL -------------------------------------------------------------- */

rl_u128 rl_pmull64_portable(uint64_t a, uint64_t b) {
    rl_u128 r = 0;
    for (unsigned i = 0; i < 64; i++)
        if ((a >> i) & 1)
            r ^= (rl_u128)b << i;
    return r;
}

/* ---- SHA-1 -------------------------------------------------------------- */

/** @brief The SHA-1 round functions of SHA1C, SHA1P and SHA1M. */
enum { F_CHOOSE, F_PARITY, F_MAJORITY };

static uint32_t sha_choose(uint32_t x, uint32_t y, uint32_t z) {
    return ((y ^ z) & x) ^ z;
}

static uint32_t sha_parity(uint32_t x, uint32_t y, uint32_t z) {
    return x ^ y ^ z;
}

static uint32_t sha_majority(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) | ((x | y) & z);
}

/**
 * @brief The four rounds of SHA1C, SHA1P and SHA1M: x = A, B, C, D in words
 * 0 to 3, y = E, w = W + K per round.
 */
static rl_u128 sha1_rounds(rl_u128 d, uint32_t e, rl_u128 m, int f) {
    uint32_t x[4] = {w32(d, 0), w32(d, 1), w32(d, 2), w32(d, 3)};
    uint32_t y = e;
    for (unsigned i = 0; i < 4; i++) {
        uint32_t t = f == F_CHOOSE   ? sha_choose(x[1], x[2], x[3])
                     : f == F_PARITY ? sha_parity(x[1], x[2], x[3])
                                     : sha_majority(x[1], x[2], x[3]);
        y = y + rol32(x[0], 5) + t + w32(m, i);
        x[1] = rol32(x[1], 30);
        /* (y : x) = ROL(y : x, 32) */
        uint32_t top = x[3];
        x[3] = x[2];
        x[2] = x[1];
        x[1] = x[0];
        x[0] = y;
        y = top;
    }
    return v32(x[0], x[1], x[2], x[3]);
}

rl_u128 rl_sha1c_portable(rl_u128 d, uint32_t e, rl_u128 m) {
    return sha1_rounds(d, e, m, F_CHOOSE);
}

rl_u128 rl_sha1p_portable(rl_u128 d, uint32_t e, rl_u128 m) {
    return sha1_rounds(d, e, m, F_PARITY);
}

rl_u128 rl_sha1m_portable(rl_u128 d, uint32_t e, rl_u128 m) {
    return sha1_rounds(d, e, m, F_MAJORITY);
}

uint32_t rl_sha1h_portable(uint32_t n) {
    return rol32(n, 30);
}

rl_u128 rl_sha1su0_portable(rl_u128 d, rl_u128 n, rl_u128 m) {
    rl_u128 r = (d >> 64) | (n << 64);
    return r ^ d ^ m;
}

rl_u128 rl_sha1su1_portable(rl_u128 d, rl_u128 n) {
    rl_u128 t = d ^ (n >> 32);
    uint32_t t0 = w32(t, 0);
    return v32(rol32(t0, 1), rol32(w32(t, 1), 1), rol32(w32(t, 2), 1),
               rol32(w32(t, 3), 1) ^ rol32(t0, 2));
}

/* ---- SHA-256 ------------------------------------------------------------ */

static uint32_t sha256_sigma0_big(uint32_t x) {
    return ror32(x, 2) ^ ror32(x, 13) ^ ror32(x, 22);
}

static uint32_t sha256_sigma1_big(uint32_t x) {
    return ror32(x, 6) ^ ror32(x, 11) ^ ror32(x, 25);
}

/**
 * @brief SHA256hash: x = A, B, C, D and y = E, F, G, H in words 0 to 3,
 * w = W + K; returns the new x when part1, else the new y.
 */
static rl_u128 sha256_hash(rl_u128 xi, rl_u128 yi, rl_u128 w, int part1) {
    uint32_t x[4] = {w32(xi, 0), w32(xi, 1), w32(xi, 2), w32(xi, 3)};
    uint32_t y[4] = {w32(yi, 0), w32(yi, 1), w32(yi, 2), w32(yi, 3)};
    for (unsigned e = 0; e < 4; e++) {
        uint32_t chs = sha_choose(y[0], y[1], y[2]);
        uint32_t maj = sha_majority(x[0], x[1], x[2]);
        uint32_t t = y[3] + sha256_sigma1_big(y[0]) + chs + w32(w, e);
        x[3] = t + x[3];
        y[3] = t + sha256_sigma0_big(x[0]) + maj;
        /* (y : x) = ROL(y : x, 32) */
        uint32_t x3 = x[3], y3 = y[3];
        x[3] = x[2];
        x[2] = x[1];
        x[1] = x[0];
        x[0] = y3;
        y[3] = y[2];
        y[2] = y[1];
        y[1] = y[0];
        y[0] = x3;
    }
    return part1 ? v32(x[0], x[1], x[2], x[3]) : v32(y[0], y[1], y[2], y[3]);
}

rl_u128 rl_sha256h_portable(rl_u128 d, rl_u128 n, rl_u128 m) {
    return sha256_hash(d, n, m, 1);
}

rl_u128 rl_sha256h2_portable(rl_u128 d, rl_u128 n, rl_u128 m) {
    return sha256_hash(n, d, m, 0);
}

rl_u128 rl_sha256su0_portable(rl_u128 d, rl_u128 n) {
    rl_u128 t = (d >> 32) | (n << 96);
    uint32_t r[4];
    for (unsigned e = 0; e < 4; e++) {
        uint32_t elt = w32(t, e);
        elt = ror32(elt, 7) ^ ror32(elt, 18) ^ (elt >> 3);
        r[e] = elt + w32(d, e);
    }
    return v32(r[0], r[1], r[2], r[3]);
}

rl_u128 rl_sha256su1_portable(rl_u128 d, rl_u128 n, rl_u128 m) {
    rl_u128 t0 = (n >> 32) | (m << 96);
    uint32_t r[4];
    for (unsigned e = 0; e < 4; e++) {
        uint32_t elt = e < 2 ? w32(m, e + 2) : r[e - 2];
        elt = ror32(elt, 17) ^ ror32(elt, 19) ^ (elt >> 10);
        r[e] = elt + w32(d, e) + w32(t0, e);
    }
    return v32(r[0], r[1], r[2], r[3]);
}

/* ---- SHA-512 ------------------------------------------------------------ */

static uint64_t sha512_sigma1_big(uint64_t x) {
    return ror64(x, 14) ^ ror64(x, 18) ^ ror64(x, 41);
}

static uint64_t sha512_sigma0_big(uint64_t x) {
    return ror64(x, 28) ^ ror64(x, 34) ^ ror64(x, 39);
}

rl_u128 rl_sha512h_portable(rl_u128 d, rl_u128 n, rl_u128 m) {
    uint64_t xlo = (uint64_t)n, xhi = (uint64_t)(n >> 64);
    uint64_t ylo = (uint64_t)m, yhi = (uint64_t)(m >> 64);
    uint64_t wlo = (uint64_t)d, whi = (uint64_t)(d >> 64);
    uint64_t hi = ((yhi & xlo) ^ (~yhi & xhi)) + sha512_sigma1_big(yhi) + whi;
    uint64_t tmp = hi + ylo;
    uint64_t lo = ((tmp & yhi) ^ (~tmp & xlo)) + sha512_sigma1_big(tmp) + wlo;
    return v64(lo, hi);
}

rl_u128 rl_sha512h2_portable(rl_u128 d, rl_u128 n, rl_u128 m) {
    uint64_t xlo = (uint64_t)n;
    uint64_t ylo = (uint64_t)m, yhi = (uint64_t)(m >> 64);
    uint64_t wlo = (uint64_t)d, whi = (uint64_t)(d >> 64);
    uint64_t hi = ((xlo & yhi) ^ (xlo & ylo) ^ (yhi & ylo)) + sha512_sigma0_big(ylo) + whi;
    uint64_t lo = ((hi & ylo) ^ (hi & yhi) ^ (yhi & ylo)) + sha512_sigma0_big(hi) + wlo;
    return v64(lo, hi);
}

static uint64_t sha512_sigma0_small(uint64_t x) {
    return ror64(x, 1) ^ ror64(x, 8) ^ (x >> 7);
}

static uint64_t sha512_sigma1_small(uint64_t x) {
    return ror64(x, 19) ^ ror64(x, 61) ^ (x >> 6);
}

rl_u128 rl_sha512su0_portable(rl_u128 d, rl_u128 n) {
    uint64_t wlo = (uint64_t)d, whi = (uint64_t)(d >> 64);
    return v64(wlo + sha512_sigma0_small(whi), whi + sha512_sigma0_small((uint64_t)n));
}

rl_u128 rl_sha512su1_portable(rl_u128 d, rl_u128 n, rl_u128 m) {
    uint64_t hi = (uint64_t)(d >> 64) + sha512_sigma1_small((uint64_t)(n >> 64)) + (uint64_t)(m >> 64);
    uint64_t lo = (uint64_t)d + sha512_sigma1_small((uint64_t)n) + (uint64_t)m;
    return v64(lo, hi);
}

/* ---- SM3 ---------------------------------------------------------------- */

/** @brief The SM3 permutation P1. */
static uint32_t sm3_p1(uint32_t x) {
    return x ^ rol32(x, 15) ^ rol32(x, 23);
}

/** @brief The SM3 permutation P0. */
static uint32_t sm3_p0(uint32_t x) {
    return x ^ rol32(x, 9) ^ rol32(x, 17);
}

rl_u128 rl_sm3partw1_portable(rl_u128 d, rl_u128 n, rl_u128 m) {
    rl_u128 dn = d ^ n;
    uint32_t r[4];
    for (unsigned i = 0; i < 3; i++)
        r[i] = w32(dn, i) ^ rol32(w32(m, i + 1), 15);
    for (unsigned i = 0; i < 4; i++) {
        if (i == 3)
            r[3] = w32(dn, 3) ^ rol32(r[0], 15);
        r[i] = sm3_p1(r[i]);
    }
    return v32(r[0], r[1], r[2], r[3]);
}

rl_u128 rl_sm3partw2_portable(rl_u128 d, rl_u128 n, rl_u128 m) {
    rl_u128 tmp = n ^ v32(rol32(w32(m, 0), 7), rol32(w32(m, 1), 7), rol32(w32(m, 2), 7),
                          rol32(w32(m, 3), 7));
    rl_u128 r = d ^ tmp;
    uint32_t tmp2 = sm3_p1(rol32(w32(tmp, 0), 15));
    return r ^ ((rl_u128)tmp2 << 96);
}

rl_u128 rl_sm3ss1_portable(rl_u128 n, rl_u128 m, rl_u128 a) {
    uint32_t v = rol32(rol32(w32(n, 3), 12) + w32(m, 3) + w32(a, 3), 7);
    return (rl_u128)v << 96;
}

/** @brief The SM3 round functions of SM3TT1A/B and SM3TT2A/B. */
enum { TT1A, TT1B, TT2A, TT2B };

static rl_u128 sm3_tt(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2, int kind) {
    uint32_t d0 = w32(d, 0), d1 = w32(d, 1), d2 = w32(d, 2), d3 = w32(d, 3);
    uint32_t wj = w32(m, imm2 & 3);
    uint32_t f, tt, r3, r1;
    switch (kind) {
    case TT1A:
    case TT1B:
        f = kind == TT1A ? d1 ^ d3 ^ d2 : (d3 & d1) | (d3 & d2) | (d1 & d2);
        tt = f + d0 + (w32(n, 3) ^ rol32(d3, 12)) + wj;
        r3 = tt;
        r1 = rol32(d2, 9);
        break;
    default:
        f = kind == TT2A ? d1 ^ d3 ^ d2 : (d3 & d2) | (~d3 & d1);
        tt = f + d0 + w32(n, 3) + wj;
        r3 = sm3_p0(tt);
        r1 = rol32(d2, 19);
        break;
    }
    return v32(d1, r1, d3, r3);
}

rl_u128 rl_sm3tt1a_portable(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2) {
    return sm3_tt(d, n, m, imm2, TT1A);
}

rl_u128 rl_sm3tt1b_portable(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2) {
    return sm3_tt(d, n, m, imm2, TT1B);
}

rl_u128 rl_sm3tt2a_portable(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2) {
    return sm3_tt(d, n, m, imm2, TT2A);
}

rl_u128 rl_sm3tt2b_portable(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2) {
    return sm3_tt(d, n, m, imm2, TT2B);
}

/* ---- SM4 ---------------------------------------------------------------- */

/** @brief Four SM4 rounds on rr with the words of k; key selects the key schedule's linear map. */
static rl_u128 sm4_rounds(rl_u128 rr, rl_u128 k, int key) {
    uint32_t r[4] = {w32(rr, 0), w32(rr, 1), w32(rr, 2), w32(rr, 3)};
    for (unsigned idx = 0; idx < 4; idx++) {
        uint32_t iv = r[3] ^ r[2] ^ r[1] ^ w32(k, idx);
        iv = (uint32_t)sm4_sbox[iv & 0xff] | ((uint32_t)sm4_sbox[(iv >> 8) & 0xff] << 8) |
             ((uint32_t)sm4_sbox[(iv >> 16) & 0xff] << 16) | ((uint32_t)sm4_sbox[iv >> 24] << 24);
        if (key)
            iv = iv ^ rol32(iv, 13) ^ rol32(iv, 23);
        else
            iv = iv ^ rol32(iv, 2) ^ rol32(iv, 10) ^ rol32(iv, 18) ^ rol32(iv, 24);
        iv ^= r[0];
        r[0] = r[1];
        r[1] = r[2];
        r[2] = r[3];
        r[3] = iv;
    }
    return v32(r[0], r[1], r[2], r[3]);
}

rl_u128 rl_sm4e_portable(rl_u128 d, rl_u128 n) {
    return sm4_rounds(d, n, 0);
}

rl_u128 rl_sm4ekey_portable(rl_u128 n, rl_u128 m) {
    return sm4_rounds(n, m, 1);
}

/* ---- Host paths --------------------------------------------------------- */

#if defined(__x86_64__)

#define X86_TARGET __attribute__((target("aes,pclmul,sha,ssse3,sse4.1")))

static inline __m128i to_m(rl_u128 x) {
    __m128i v;
    memcpy(&v, &x, sizeof v);
    return v;
}

static inline rl_u128 from_m(__m128i v) {
    rl_u128 x;
    memcpy(&x, &v, sizeof x);
    return x;
}

/*
 * AES on AES-NI. Both ISAs keep byte i of the FIPS-197 state (column i / 4,
 * row i % 4) in byte i of the register, so only the order of the steps
 * differs:
 * - Arm ARM (AESE, AESD, AESMC, AESIMC pseudocode): AESE = SubBytes(
 *   ShiftRows(state XOR key)), AESD = InvSubBytes(InvShiftRows(state XOR
 *   key)), AESMC = MixColumns, AESIMC = InvMixColumns.
 * - Intel AES-NI white paper (S. Gueron, "Intel Advanced Encryption Standard
 *   (AES) New Instructions Set", rev. 3.01, 2012): AESENC = ShiftRows,
 *   SubBytes, MixColumns, then XOR the round key; AESENCLAST omits
 *   MixColumns; AESDEC and AESDECLAST are the inverses in the same shape;
 *   AESIMC = InvMixColumns.
 * So AESE(d, k) = AESENCLAST(d ^ k, 0), AESD(d, k) = AESDECLAST(d ^ k, 0),
 * AESIMC(x) = AESIMC(x), and AESMC(x) = AESENC(AESDECLAST(x, 0), 0): the
 * InvShiftRows and InvSubBytes of AESDECLAST are undone by the ShiftRows and
 * SubBytes of AESENC (the two steps commute, as one permutes bytes and the
 * other maps each byte), leaving MixColumns.
 */
X86_TARGET static rl_u128 x86_aese(rl_u128 d, rl_u128 n) {
    return from_m(_mm_aesenclast_si128(to_m(d ^ n), _mm_setzero_si128()));
}

X86_TARGET static rl_u128 x86_aesd(rl_u128 d, rl_u128 n) {
    return from_m(_mm_aesdeclast_si128(to_m(d ^ n), _mm_setzero_si128()));
}

X86_TARGET static rl_u128 x86_aesmc(rl_u128 x) {
    __m128i z = _mm_setzero_si128();
    return from_m(_mm_aesenc_si128(_mm_aesdeclast_si128(to_m(x), z), z));
}

X86_TARGET static rl_u128 x86_aesimc(rl_u128 x) {
    return from_m(_mm_aesimc_si128(to_m(x)));
}

/* PMULL 1Q is PCLMULQDQ of the two low quadwords. */
X86_TARGET static rl_u128 x86_pmull64(uint64_t a, uint64_t b) {
    return from_m(_mm_clmulepi64_si128(_mm_cvtsi64_si128((long long)a),
                                       _mm_cvtsi64_si128((long long)b), 0x00));
}

/*
 * SHA-1 on SHA-NI (Intel SDM, SHA1RNDS4, SHA1MSG1, SHA1MSG2). Intel keeps A
 * and W0 in the top word where Arm keeps them in word 0, so every operand is
 * reversed by word (shuffle 0x1b). SHA1RNDS4 adds the round constant of its
 * function selector itself and takes E already added to W0; Arm's Vm holds
 * W + K, so the constant is subtracted from each word (exact modulo 2^32) and
 * E is added to word 0. Arm's choose, parity and majority functions are the
 * selectors 0, 1 (or 3) and 2. The schedule mappings, checked term by term
 * against both pseudocodes: SHA1SU0(d, n, m) = rev(SHA1MSG1(rev d, rev n))
 * ^ m and SHA1SU1(d, n) = rev(SHA1MSG2(rev d, rev n)).
 */
#define SHA1_ROUNDS(name, sel, k)                                                       \
    X86_TARGET static rl_u128 name(rl_u128 d, uint32_t e, rl_u128 m) {                  \
        __m128i abcd = _mm_shuffle_epi32(to_m(d), 0x1b);                                \
        __m128i wk = _mm_shuffle_epi32(to_m(m), 0x1b);                                  \
        wk = _mm_sub_epi32(wk, _mm_set1_epi32((int)(k)));                               \
        wk = _mm_add_epi32(wk, _mm_set_epi32((int)e, 0, 0, 0));                         \
        return from_m(_mm_shuffle_epi32(_mm_sha1rnds4_epu32(abcd, wk, sel), 0x1b));     \
    }
SHA1_ROUNDS(x86_sha1c, 0, 0x5a827999u)
SHA1_ROUNDS(x86_sha1p, 1, 0x6ed9eba1u)
SHA1_ROUNDS(x86_sha1m, 2, 0x8f1bbcdcu)

X86_TARGET static rl_u128 x86_sha1su0(rl_u128 d, rl_u128 n, rl_u128 m) {
    __m128i r = _mm_sha1msg1_epu32(_mm_shuffle_epi32(to_m(d), 0x1b), _mm_shuffle_epi32(to_m(n), 0x1b));
    return from_m(_mm_shuffle_epi32(r, 0x1b)) ^ m;
}

X86_TARGET static rl_u128 x86_sha1su1(rl_u128 d, rl_u128 n) {
    __m128i r = _mm_sha1msg2_epu32(_mm_shuffle_epi32(to_m(d), 0x1b), _mm_shuffle_epi32(to_m(n), 0x1b));
    return from_m(_mm_shuffle_epi32(r, 0x1b));
}

/*
 * SHA-256 on SHA-NI (Intel SDM, SHA256RNDS2, SHA256MSG1, SHA256MSG2).
 * SHA256RNDS2(cdgh, abef, wk) does two rounds with W + K from words 0 and 1
 * of wk and returns the new ABEF; the old ABEF is the new CDGH. Intel packs
 * the state as ABEF = (F, E, B, A) and CDGH = (H, G, D, C) in words 0 to 3,
 * Arm as ABCD = (A, B, C, D) and EFGH = (E, F, G, H). Four Arm rounds are
 * two SHA256RNDS2 with the state repacked before and after; SHA256H returns
 * the new ABCD and SHA256H2 the new EFGH. The schedule instructions keep W0
 * in word 0 as Arm does: SHA256SU0(d, n) = SHA256MSG1(d, n) and
 * SHA256SU1(d, n, m) = SHA256MSG2(d + (n1, n2, n3, m0), m), checked term by
 * term against both pseudocodes.
 */
X86_TARGET static void x86_sha256_4rounds(rl_u128 *abcd_io, rl_u128 *efgh_io, rl_u128 wk) {
    __m128i t0 = _mm_shuffle_epi32(to_m(*abcd_io), 0xb1); /* B A D C */
    __m128i t1 = _mm_shuffle_epi32(to_m(*efgh_io), 0xb1); /* F E H G */
    __m128i abef = _mm_unpacklo_epi64(t1, t0);            /* F E B A */
    __m128i cdgh = _mm_unpackhi_epi64(t1, t0);            /* H G D C */
    __m128i w = to_m(wk);
    __m128i abef1 = _mm_sha256rnds2_epu32(cdgh, abef, w);
    __m128i abef2 = _mm_sha256rnds2_epu32(abef, abef1, _mm_shuffle_epi32(w, 0x0e));
    t0 = _mm_shuffle_epi32(abef2, 0x1b); /* A B E F */
    t1 = _mm_shuffle_epi32(abef1, 0x1b); /* C D G H */
    *abcd_io = from_m(_mm_unpacklo_epi64(t0, t1));
    *efgh_io = from_m(_mm_unpackhi_epi64(t0, t1));
}

X86_TARGET static rl_u128 x86_sha256h(rl_u128 d, rl_u128 n, rl_u128 m) {
    x86_sha256_4rounds(&d, &n, m);
    return d;
}

X86_TARGET static rl_u128 x86_sha256h2(rl_u128 d, rl_u128 n, rl_u128 m) {
    x86_sha256_4rounds(&n, &d, m);
    return d;
}

X86_TARGET static rl_u128 x86_sha256su0(rl_u128 d, rl_u128 n) {
    return from_m(_mm_sha256msg1_epu32(to_m(d), to_m(n)));
}

X86_TARGET static rl_u128 x86_sha256su1(rl_u128 d, rl_u128 n, rl_u128 m) {
    __m128i t0 = _mm_alignr_epi8(to_m(m), to_m(n), 4);
    return from_m(_mm_sha256msg2_epu32(_mm_add_epi32(to_m(d), t0), to_m(m)));
}

static unsigned detect(void) {
    unsigned a, b, c, d, f = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d))
        return 0;
    int sse = (c & bit_SSSE3) && (c & bit_SSE4_1);
    if (sse && (c & bit_AES))
        f |= RL_CRYPTO_HOST_AES;
    if (sse && (c & bit_PCLMUL))
        f |= RL_CRYPTO_HOST_PMULL;
    if (sse && __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & bit_SHA))
        f |= RL_CRYPTO_HOST_SHA1 | RL_CRYPTO_HOST_SHA256;
    return f;
}

#define HOST_AES(call) return x86_##call
#define HOST_PMULL(call) return x86_##call
#define HOST_SHA1(call) return x86_##call
#define HOST_SHA1H(call) (void)0
#define HOST_SHA256(call) return x86_##call
#define HOST_SHA512(call) (void)0
#define HOST_SM3(call) (void)0
#define HOST_SM4(call) (void)0

#elif defined(__aarch64__)

#define A_AES __attribute__((target("aes")))
#define A_SHA2 __attribute__((target("sha2")))
#define A_SHA3 __attribute__((target("sha3")))
#define A_SM4 __attribute__((target("sm4")))

static inline uint8x16_t to_u8(rl_u128 x) {
    uint8x16_t v;
    memcpy(&v, &x, sizeof v);
    return v;
}

static inline uint32x4_t to_u32(rl_u128 x) {
    uint32x4_t v;
    memcpy(&v, &x, sizeof v);
    return v;
}

static inline uint64x2_t to_u64(rl_u128 x) {
    uint64x2_t v;
    memcpy(&v, &x, sizeof v);
    return v;
}

#define FROM_V(v)                                                                       \
    __extension__({                                                                     \
        rl_u128 r_;                                                                     \
        __typeof__(v) v_ = (v);                                                         \
        memcpy(&r_, &v_, sizeof r_);                                                    \
        r_;                                                                             \
    })

/* The ARMv8 intrinsics are the guest instructions; vaeseq_u8 includes the XOR with the key. */
A_AES static rl_u128 arm_aese(rl_u128 d, rl_u128 n) {
    return FROM_V(vaeseq_u8(to_u8(d), to_u8(n)));
}

A_AES static rl_u128 arm_aesd(rl_u128 d, rl_u128 n) {
    return FROM_V(vaesdq_u8(to_u8(d), to_u8(n)));
}

A_AES static rl_u128 arm_aesmc(rl_u128 x) {
    return FROM_V(vaesmcq_u8(to_u8(x)));
}

A_AES static rl_u128 arm_aesimc(rl_u128 x) {
    return FROM_V(vaesimcq_u8(to_u8(x)));
}

A_AES static rl_u128 arm_pmull64(uint64_t a, uint64_t b) {
    return FROM_V(vmull_p64((poly64_t)a, (poly64_t)b));
}

A_SHA2 static rl_u128 arm_sha1c(rl_u128 d, uint32_t e, rl_u128 m) {
    return FROM_V(vsha1cq_u32(to_u32(d), e, to_u32(m)));
}

A_SHA2 static rl_u128 arm_sha1p(rl_u128 d, uint32_t e, rl_u128 m) {
    return FROM_V(vsha1pq_u32(to_u32(d), e, to_u32(m)));
}

A_SHA2 static rl_u128 arm_sha1m(rl_u128 d, uint32_t e, rl_u128 m) {
    return FROM_V(vsha1mq_u32(to_u32(d), e, to_u32(m)));
}

A_SHA2 static uint32_t arm_sha1h(uint32_t n) {
    return vsha1h_u32(n);
}

A_SHA2 static rl_u128 arm_sha1su0(rl_u128 d, rl_u128 n, rl_u128 m) {
    return FROM_V(vsha1su0q_u32(to_u32(d), to_u32(n), to_u32(m)));
}

A_SHA2 static rl_u128 arm_sha1su1(rl_u128 d, rl_u128 n) {
    return FROM_V(vsha1su1q_u32(to_u32(d), to_u32(n)));
}

A_SHA2 static rl_u128 arm_sha256h(rl_u128 d, rl_u128 n, rl_u128 m) {
    return FROM_V(vsha256hq_u32(to_u32(d), to_u32(n), to_u32(m)));
}

A_SHA2 static rl_u128 arm_sha256h2(rl_u128 d, rl_u128 n, rl_u128 m) {
    return FROM_V(vsha256h2q_u32(to_u32(d), to_u32(n), to_u32(m)));
}

A_SHA2 static rl_u128 arm_sha256su0(rl_u128 d, rl_u128 n) {
    return FROM_V(vsha256su0q_u32(to_u32(d), to_u32(n)));
}

A_SHA2 static rl_u128 arm_sha256su1(rl_u128 d, rl_u128 n, rl_u128 m) {
    return FROM_V(vsha256su1q_u32(to_u32(d), to_u32(n), to_u32(m)));
}

A_SHA3 static rl_u128 arm_sha512h(rl_u128 d, rl_u128 n, rl_u128 m) {
    return FROM_V(vsha512hq_u64(to_u64(d), to_u64(n), to_u64(m)));
}

A_SHA3 static rl_u128 arm_sha512h2(rl_u128 d, rl_u128 n, rl_u128 m) {
    return FROM_V(vsha512h2q_u64(to_u64(d), to_u64(n), to_u64(m)));
}

A_SHA3 static rl_u128 arm_sha512su0(rl_u128 d, rl_u128 n) {
    return FROM_V(vsha512su0q_u64(to_u64(d), to_u64(n)));
}

A_SHA3 static rl_u128 arm_sha512su1(rl_u128 d, rl_u128 n, rl_u128 m) {
    return FROM_V(vsha512su1q_u64(to_u64(d), to_u64(n), to_u64(m)));
}

A_SM4 static rl_u128 arm_sm3partw1(rl_u128 d, rl_u128 n, rl_u128 m) {
    return FROM_V(vsm3partw1q_u32(to_u32(d), to_u32(n), to_u32(m)));
}

A_SM4 static rl_u128 arm_sm3partw2(rl_u128 d, rl_u128 n, rl_u128 m) {
    return FROM_V(vsm3partw2q_u32(to_u32(d), to_u32(n), to_u32(m)));
}

A_SM4 static rl_u128 arm_sm3ss1(rl_u128 n, rl_u128 m, rl_u128 a) {
    return FROM_V(vsm3ss1q_u32(to_u32(n), to_u32(m), to_u32(a)));
}

/* The SM3TT intrinsics take the lane index as a constant. */
#define SM3TT(name, intr)                                                               \
    A_SM4 static rl_u128 name(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2) {         \
        uint32x4_t a = to_u32(d), b = to_u32(n), c = to_u32(m);                         \
        switch (imm2 & 3) {                                                             \
        case 0:                                                                         \
            return FROM_V(intr(a, b, c, 0));                                            \
        case 1:                                                                         \
            return FROM_V(intr(a, b, c, 1));                                            \
        case 2:                                                                         \
            return FROM_V(intr(a, b, c, 2));                                            \
        default:                                                                        \
            return FROM_V(intr(a, b, c, 3));                                            \
        }                                                                               \
    }
SM3TT(arm_sm3tt1a, vsm3tt1aq_u32)
SM3TT(arm_sm3tt1b, vsm3tt1bq_u32)
SM3TT(arm_sm3tt2a, vsm3tt2aq_u32)
SM3TT(arm_sm3tt2b, vsm3tt2bq_u32)

A_SM4 static rl_u128 arm_sm4e(rl_u128 d, rl_u128 n) {
    return FROM_V(vsm4eq_u32(to_u32(d), to_u32(n)));
}

A_SM4 static rl_u128 arm_sm4ekey(rl_u128 n, rl_u128 m) {
    return FROM_V(vsm4ekeyq_u32(to_u32(n), to_u32(m)));
}

#if defined(__linux__)
/* Linux arm64 AT_HWCAP bits (arch/arm64/include/uapi/asm/hwcap.h). */
#define HW_AES (1ul << 3)
#define HW_PMULL (1ul << 4)
#define HW_SHA1 (1ul << 5)
#define HW_SHA2 (1ul << 6)
#define HW_SM3 (1ul << 18)
#define HW_SM4 (1ul << 19)
#define HW_SHA512 (1ul << 21)

static unsigned detect(void) {
    unsigned long hw = getauxval(AT_HWCAP);
    unsigned f = 0;
    if (hw & HW_AES)
        f |= RL_CRYPTO_HOST_AES;
    if (hw & HW_PMULL)
        f |= RL_CRYPTO_HOST_PMULL;
    if (hw & HW_SHA1)
        f |= RL_CRYPTO_HOST_SHA1;
    if (hw & HW_SHA2)
        f |= RL_CRYPTO_HOST_SHA256;
    if (hw & HW_SHA512)
        f |= RL_CRYPTO_HOST_SHA512;
    if (hw & HW_SM3)
        f |= RL_CRYPTO_HOST_SM3;
    if (hw & HW_SM4)
        f |= RL_CRYPTO_HOST_SM4;
    return f;
}
#elif defined(__APPLE__)
static int has(const char *name) {
    int v = 0;
    size_t len = sizeof v;
    return sysctlbyname(name, &v, &len, NULL, 0) == 0 && v != 0;
}

static unsigned detect(void) {
    unsigned f = 0;
    if (has("hw.optional.arm.FEAT_AES"))
        f |= RL_CRYPTO_HOST_AES;
    if (has("hw.optional.arm.FEAT_PMULL"))
        f |= RL_CRYPTO_HOST_PMULL;
    if (has("hw.optional.arm.FEAT_SHA1"))
        f |= RL_CRYPTO_HOST_SHA1;
    if (has("hw.optional.arm.FEAT_SHA256"))
        f |= RL_CRYPTO_HOST_SHA256;
    if (has("hw.optional.arm.FEAT_SHA512"))
        f |= RL_CRYPTO_HOST_SHA512;
    /* Apple cores so far have no SM3 or SM4; the names are asked for anyway. */
    if (has("hw.optional.arm.FEAT_SM3"))
        f |= RL_CRYPTO_HOST_SM3;
    if (has("hw.optional.arm.FEAT_SM4"))
        f |= RL_CRYPTO_HOST_SM4;
    return f;
}
#else
static unsigned detect(void) {
    return 0;
}
#endif

#define HOST_AES(call) return arm_##call
#define HOST_PMULL(call) return arm_##call
#define HOST_SHA1(call) return arm_##call
#define HOST_SHA1H(call) return arm_##call
#define HOST_SHA256(call) return arm_##call
#define HOST_SHA512(call) return arm_##call
#define HOST_SM3(call) return arm_##call
#define HOST_SM4(call) return arm_##call

#else

static unsigned detect(void) {
    return 0;
}

#define HOST_AES(call) (void)0
#define HOST_PMULL(call) (void)0
#define HOST_SHA1(call) (void)0
#define HOST_SHA1H(call) (void)0
#define HOST_SHA256(call) (void)0
#define HOST_SHA512(call) (void)0
#define HOST_SM3(call) (void)0
#define HOST_SM4(call) (void)0

#endif

/* ---- Dispatch ----------------------------------------------------------- */

/** @brief Set in the cache once detection ran. */
#define DETECTED (1u << 31)

static unsigned features_cache;

/** @brief The RL_CRYPTO_HOST_* bits; racing first calls detect twice and store the same value. */
static inline unsigned features(void) {
    unsigned f = __atomic_load_n(&features_cache, __ATOMIC_RELAXED);
    if (RL_UNLIKELY(!f)) {
        f = detect() | DETECTED;
        __atomic_store_n(&features_cache, f, __ATOMIC_RELAXED);
    }
    return f;
}

unsigned rl_crypto_host_features(void) {
    return features() & ~DETECTED;
}

/** @brief Defines rl_<name>: the host path when feature bit F is set, else the portable path. */
#define DISPATCH(F, HOST, ret, name, params, args)                                      \
    ret rl_##name params {                                                              \
        if (features() & (F)) {                                                         \
            HOST(name args);                                                            \
        }                                                                               \
        return rl_##name##_portable args;                                               \
    }

#define V rl_u128
DISPATCH(RL_CRYPTO_HOST_AES, HOST_AES, V, aese, (V d, V n), (d, n))
DISPATCH(RL_CRYPTO_HOST_AES, HOST_AES, V, aesd, (V d, V n), (d, n))
DISPATCH(RL_CRYPTO_HOST_AES, HOST_AES, V, aesmc, (V x), (x))
DISPATCH(RL_CRYPTO_HOST_AES, HOST_AES, V, aesimc, (V x), (x))
DISPATCH(RL_CRYPTO_HOST_PMULL, HOST_PMULL, V, pmull64, (uint64_t a, uint64_t b), (a, b))
DISPATCH(RL_CRYPTO_HOST_SHA1, HOST_SHA1, V, sha1c, (V d, uint32_t e, V m), (d, e, m))
DISPATCH(RL_CRYPTO_HOST_SHA1, HOST_SHA1, V, sha1p, (V d, uint32_t e, V m), (d, e, m))
DISPATCH(RL_CRYPTO_HOST_SHA1, HOST_SHA1, V, sha1m, (V d, uint32_t e, V m), (d, e, m))
DISPATCH(RL_CRYPTO_HOST_SHA1, HOST_SHA1H, uint32_t, sha1h, (uint32_t n), (n))
DISPATCH(RL_CRYPTO_HOST_SHA1, HOST_SHA1, V, sha1su0, (V d, V n, V m), (d, n, m))
DISPATCH(RL_CRYPTO_HOST_SHA1, HOST_SHA1, V, sha1su1, (V d, V n), (d, n))
DISPATCH(RL_CRYPTO_HOST_SHA256, HOST_SHA256, V, sha256h, (V d, V n, V m), (d, n, m))
DISPATCH(RL_CRYPTO_HOST_SHA256, HOST_SHA256, V, sha256h2, (V d, V n, V m), (d, n, m))
DISPATCH(RL_CRYPTO_HOST_SHA256, HOST_SHA256, V, sha256su0, (V d, V n), (d, n))
DISPATCH(RL_CRYPTO_HOST_SHA256, HOST_SHA256, V, sha256su1, (V d, V n, V m), (d, n, m))
DISPATCH(RL_CRYPTO_HOST_SHA512, HOST_SHA512, V, sha512h, (V d, V n, V m), (d, n, m))
DISPATCH(RL_CRYPTO_HOST_SHA512, HOST_SHA512, V, sha512h2, (V d, V n, V m), (d, n, m))
DISPATCH(RL_CRYPTO_HOST_SHA512, HOST_SHA512, V, sha512su0, (V d, V n), (d, n))
DISPATCH(RL_CRYPTO_HOST_SHA512, HOST_SHA512, V, sha512su1, (V d, V n, V m), (d, n, m))
DISPATCH(RL_CRYPTO_HOST_SM3, HOST_SM3, V, sm3partw1, (V d, V n, V m), (d, n, m))
DISPATCH(RL_CRYPTO_HOST_SM3, HOST_SM3, V, sm3partw2, (V d, V n, V m), (d, n, m))
DISPATCH(RL_CRYPTO_HOST_SM3, HOST_SM3, V, sm3ss1, (V n, V m, V a), (n, m, a))
DISPATCH(RL_CRYPTO_HOST_SM3, HOST_SM3, V, sm3tt1a, (V d, V n, V m, unsigned i), (d, n, m, i))
DISPATCH(RL_CRYPTO_HOST_SM3, HOST_SM3, V, sm3tt1b, (V d, V n, V m, unsigned i), (d, n, m, i))
DISPATCH(RL_CRYPTO_HOST_SM3, HOST_SM3, V, sm3tt2a, (V d, V n, V m, unsigned i), (d, n, m, i))
DISPATCH(RL_CRYPTO_HOST_SM3, HOST_SM3, V, sm3tt2b, (V d, V n, V m, unsigned i), (d, n, m, i))
DISPATCH(RL_CRYPTO_HOST_SM4, HOST_SM4, V, sm4e, (V d, V n), (d, n))
DISPATCH(RL_CRYPTO_HOST_SM4, HOST_SM4, V, sm4ekey, (V n, V m), (n, m))
#undef V
