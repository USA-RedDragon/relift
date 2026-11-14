/**
 * @file test_crypto.c
 * @brief The A64 crypto instruction helpers: FIPS-197 AES round states on
 * AESE/AESMC and AESD/AESIMC sequences, whole-message known answers for
 * SHA-1, SHA-256, SHA-512, SM3 and SM4 driven through the instruction
 * helpers, random differential runs of every host path against its portable
 * path, and an optional vector file.
 *
 * Vector file: when RL_CRYPTO_VECTORS names a file, every line of the form
 * `name d n m extra -> result` (hex, at most 32 digits each, `0x` optional;
 * `#` starts a comment) is checked against both the dispatching function and
 * the portable path. Operands map to the instruction's registers: d is the
 * old Vd, n is Vn (Sn for SHA1C/P/M and SHA1H), m is Vm, extra is Va for
 * SM3SS1 and imm2 for SM3TT*; AESMC and AESIMC read n; PMULL reads the low
 * 64 bits of n and m. Unused operands may be any value.
 */
#include "runner.h"

#include <relift/a64/crypto.h>

#include <ctype.h>
#include <stdlib.h>

/** @brief The vector of 16 bytes given as 32 hex digits, byte 0 first (FIPS-197 order). */
static rl_u128 bytes_hex(const char *s) {
    rl_u128 r = 0;
    for (unsigned i = 0; i < 16; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        r |= (rl_u128)v << (8 * i);
    }
    return r;
}

/** @brief Word i of x. */
static uint32_t w32(rl_u128 x, unsigned i) {
    return (uint32_t)(x >> (32 * i));
}

static rl_u128 v32(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (rl_u128)a | ((rl_u128)b << 32) | ((rl_u128)c << 64) | ((rl_u128)d << 96);
}

static rl_u128 v64(uint64_t lo, uint64_t hi) {
    return (rl_u128)lo | ((rl_u128)hi << 64);
}

static rl_u128 add32(rl_u128 a, rl_u128 b) {
    return v32(w32(a, 0) + w32(b, 0), w32(a, 1) + w32(b, 1), w32(a, 2) + w32(b, 2), w32(a, 3) + w32(b, 3));
}

static rl_u128 add64(rl_u128 a, rl_u128 b) {
    return v64((uint64_t)a + (uint64_t)b, (uint64_t)(a >> 64) + (uint64_t)(b >> 64));
}

/** @brief EXT Vd.16B, x, y, #8: the high half of x then the low half of y. */
static rl_u128 ext8(rl_u128 x, rl_u128 y) {
    return (x >> 64) | (y << 64);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static uint32_t rol32(uint32_t x, unsigned s) {
    return s ? (x << s) | (x >> (32 - s)) : x;
}

/* ---- AES ---------------------------------------------------------------- */

/** @brief The AES S-box entry of b: SubBytes of a vector of equal bytes ignores ShiftRows. */
static uint8_t sbox_of(uint8_t b) {
    rl_u128 x = 0;
    for (unsigned i = 0; i < 16; i++)
        x |= (rl_u128)b << (8 * i);
    return (uint8_t)rl_aese(x, 0);
}

/** @brief The AES-128 key schedule (FIPS-197 section 5.2): rk[0..10]. */
static void aes128_keys(rl_u128 key, rl_u128 rk[11]) {
    uint32_t w[44];
    for (unsigned i = 0; i < 4; i++)
        w[i] = w32(key, i); /* byte 0 of the word in bits 7:0 */
    uint8_t rcon = 1;
    for (unsigned i = 4; i < 44; i++) {
        uint32_t t = w[i - 1];
        if (i % 4 == 0) {
            t = (t >> 8) | (t << 24); /* RotWord on bytes stored low first */
            t = (uint32_t)sbox_of((uint8_t)t) | ((uint32_t)sbox_of((uint8_t)(t >> 8)) << 8) |
                ((uint32_t)sbox_of((uint8_t)(t >> 16)) << 16) | ((uint32_t)sbox_of((uint8_t)(t >> 24)) << 24);
            t ^= rcon;
            rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80) ? 0x1b : 0));
        }
        w[i] = w[i - 4] ^ t;
    }
    for (unsigned r = 0; r < 11; r++)
        rk[r] = v32(w[4 * r], w[4 * r + 1], w[4 * r + 2], w[4 * r + 3]);
}

static rl_u128 aes128_encrypt(rl_u128 pt, const rl_u128 rk[11]) {
    rl_u128 s = pt;
    for (unsigned r = 0; r < 9; r++)
        s = rl_aesmc(rl_aese(s, rk[r]));
    return rl_aese(s, rk[9]) ^ rk[10];
}

/** @brief The equivalent inverse cipher on AESD and AESIMC (round keys 1 to 9 through InvMixColumns). */
static rl_u128 aes128_decrypt(rl_u128 ct, const rl_u128 rk[11]) {
    rl_u128 s = rl_aesd(ct, rk[10]);
    for (unsigned r = 9; r >= 1; r--)
        s = rl_aesd(rl_aesimc(s), rl_aesimc(rk[r]));
    return s ^ rk[0];
}

TEST(crypto_aes_fips197_round_states)
{
    /* FIPS-197 Appendix B: round 1 of the cipher example. */
    rl_u128 start = bytes_hex("193de3bea0f4e22b9ac68d2ae9f84808");
    CHECK(rl_aese(start, 0) == bytes_hex("d4bf5d30e0b452aeb84111f11e2798e5"));
    CHECK(rl_aese_portable(start, 0) == bytes_hex("d4bf5d30e0b452aeb84111f11e2798e5"));
    CHECK(rl_aesmc(bytes_hex("d4bf5d30e0b452aeb84111f11e2798e5")) == bytes_hex("046681e5e0cb199a48f8d37a2806264c"));
    CHECK(rl_aesmc_portable(bytes_hex("d4bf5d30e0b452aeb84111f11e2798e5")) ==
          bytes_hex("046681e5e0cb199a48f8d37a2806264c"));
    /* Appendix C.1, AES-128: round 1 start, s_row and m_col, round 2 start. */
    rl_u128 pt = bytes_hex("00112233445566778899aabbccddeeff");
    rl_u128 key = bytes_hex("000102030405060708090a0b0c0d0e0f");
    CHECK((pt ^ key) == bytes_hex("00102030405060708090a0b0c0d0e0f0"));
    rl_u128 s = rl_aese(pt, key);
    CHECK(s == bytes_hex("6353e08c0960e104cd70b751bacad0e7"));
    s = rl_aesmc(s);
    CHECK(s == bytes_hex("5f72641557f5bc92f7be3b291db9f91a"));
    rl_u128 rk[11];
    aes128_keys(key, rk);
    CHECK(rk[1] == bytes_hex("d6aa74fdd2af72fadaa678f1d6ab76fe"));
    CHECK(rk[10] == bytes_hex("13111d7fe3944a17f307a78b4d2b30c5"));
    CHECK((s ^ rk[1]) == bytes_hex("89d810e8855ace682d1843d8cb128fe4"));
    rl_u128 ct = aes128_encrypt(pt, rk);
    CHECK(ct == bytes_hex("69c4e0d86a7b0430d8cdb78070b4c55a"));
    /* The inverse cipher: AESD/AESIMC, and the inverse round states. */
    CHECK(aes128_decrypt(ct, rk) == pt);
    CHECK(rl_aesd(bytes_hex("6353e08c0960e104cd70b751bacad0e7"), 0) == bytes_hex("00102030405060708090a0b0c0d0e0f0"));
    CHECK(rl_aesimc(bytes_hex("5f72641557f5bc92f7be3b291db9f91a")) == bytes_hex("6353e08c0960e104cd70b751bacad0e7"));
    CHECK(rl_aesimc_portable(rl_aesmc_portable(ct)) == ct);
    CHECK(rl_aesd_portable(rl_aese_portable(ct, key), 0) == (ct ^ key));
    /* Appendix B whole cipher. */
    aes128_keys(bytes_hex("2b7e151628aed2a6abf7158809cf4f3c"), rk);
    CHECK(rk[1] == bytes_hex("a0fafe1788542cb123a339392a6c7605"));
    CHECK(aes128_encrypt(bytes_hex("3243f6a8885a308d313198a2e0370734"), rk) == bytes_hex("3925841d02dc09fbdc118597196a0b32"));
}

TEST(crypto_pmull_known)
{
    CHECK(rl_pmull64(3, 3) == 5);
    CHECK(rl_pmull64_portable(3, 3) == 5);
    rl_u128 all = ((rl_u128)0x5555555555555555ull << 64) | 0x5555555555555555ull;
    CHECK(rl_pmull64(~0ull, ~0ull) == all);
    CHECK(rl_pmull64_portable(~0ull, ~0ull) == all);
    CHECK(rl_pmull64(1ull << 63, 1ull << 63) == (rl_u128)1 << 126);
    CHECK(rl_pmull64(0x87, 0) == 0);
}

static const uint32_t k256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};
static const uint32_t iv256[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};
static const uint64_t k512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};
static const uint64_t iv512[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

/* ---- Whole-message known answers ---------------------------------------- */

/** @brief The one padded block of "abc" for SHA-1, SHA-256 and SM3 (64 bytes). */
static void pad64_abc(uint8_t b[64]) {
    memset(b, 0, 64);
    b[0] = 'a';
    b[1] = 'b';
    b[2] = 'c';
    b[3] = 0x80;
    b[63] = 24;
}

TEST(crypto_sha1_abc)
{
    uint8_t b[64];
    pad64_abc(b);
    rl_u128 w[4];
    for (unsigned i = 0; i < 4; i++)
        w[i] = v32(be32(b + 16 * i), be32(b + 16 * i + 4), be32(b + 16 * i + 8), be32(b + 16 * i + 12));
    static const uint32_t k[4] = {0x5a827999u, 0x6ed9eba1u, 0x8f1bbcdcu, 0xca62c1d6u};
    const uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    rl_u128 abcd = v32(h[0], h[1], h[2], h[3]);
    uint32_t e0 = h[4];
    for (unsigned g = 0; g < 20; g++) {
        rl_u128 kv = v32(k[g / 5], k[g / 5], k[g / 5], k[g / 5]);
        rl_u128 wk = add32(w[g % 4], kv);
        uint32_t e1 = rl_sha1h(w32(abcd, 0));
        if (g < 5)
            abcd = rl_sha1c(abcd, e0, wk);
        else if (g < 10 || g >= 15)
            abcd = rl_sha1p(abcd, e0, wk);
        else
            abcd = rl_sha1m(abcd, e0, wk);
        e0 = e1;
        if (g < 16)
            w[g % 4] = rl_sha1su1(rl_sha1su0(w[g % 4], w[(g + 1) % 4], w[(g + 2) % 4]), w[(g + 3) % 4]);
    }
    CHECK_EQ(w32(abcd, 0) + h[0], 0xa9993e36u);
    CHECK_EQ(w32(abcd, 1) + h[1], 0x4706816au);
    CHECK_EQ(w32(abcd, 2) + h[2], 0xba3e2571u);
    CHECK_EQ(w32(abcd, 3) + h[3], 0x7850c26cu);
    CHECK_EQ(e0 + h[4], 0x9cd0d89du);
}

TEST(crypto_sha256_abc)
{
    uint8_t b[64];
    pad64_abc(b);
    rl_u128 w[4];
    for (unsigned i = 0; i < 4; i++)
        w[i] = v32(be32(b + 16 * i), be32(b + 16 * i + 4), be32(b + 16 * i + 8), be32(b + 16 * i + 12));
    rl_u128 abcd = v32(iv256[0], iv256[1], iv256[2], iv256[3]);
    rl_u128 efgh = v32(iv256[4], iv256[5], iv256[6], iv256[7]);
    for (unsigned g = 0; g < 16; g++) {
        rl_u128 wk = add32(w[g % 4], v32(k256[4 * g], k256[4 * g + 1], k256[4 * g + 2], k256[4 * g + 3]));
        rl_u128 abcd0 = abcd;
        abcd = rl_sha256h(abcd, efgh, wk);
        efgh = rl_sha256h2(efgh, abcd0, wk);
        if (g < 12)
            w[g % 4] = rl_sha256su1(rl_sha256su0(w[g % 4], w[(g + 1) % 4]), w[(g + 2) % 4], w[(g + 3) % 4]);
    }
    static const uint32_t want[8] = {0xba7816bfu, 0x8f01cfeau, 0x414140deu, 0x5dae2223u,
                                     0xb00361a3u, 0x96177a9cu, 0xb410ff61u, 0xf20015adu};
    for (unsigned i = 0; i < 4; i++) {
        CHECK_EQ(w32(abcd, i) + iv256[i], want[i]);
        CHECK_EQ(w32(efgh, i) + iv256[4 + i], want[4 + i]);
    }
}

TEST(crypto_sha512_abc)
{
    uint8_t b[128];
    memset(b, 0, sizeof b);
    b[0] = 'a';
    b[1] = 'b';
    b[2] = 'c';
    b[3] = 0x80;
    b[127] = 24;
    rl_u128 w[8];
    for (unsigned i = 0; i < 8; i++)
        w[i] = v64(be64(b + 16 * i), be64(b + 16 * i + 8));
    rl_u128 ab = v64(iv512[0], iv512[1]), cd = v64(iv512[2], iv512[3]);
    rl_u128 ef = v64(iv512[4], iv512[5]), gh = v64(iv512[6], iv512[7]);
    /*
     * Two rounds per step. SHA512H takes (g + K1 + W1, h + K0 + W0), (f, g)
     * and (d, e) and returns the two T1 values; SHA512H2 takes them with
     * (c, -) and (a, b) and returns the new (a, b).
     */
    for (unsigned g = 0; g < 40; g++) {
        rl_u128 kw = add64(w[g % 8], v64(k512[2 * g], k512[2 * g + 1]));
        rl_u128 hin = add64(gh, (kw >> 64) | (kw << 64));
        rl_u128 t1 = rl_sha512h(hin, ext8(ef, gh), ext8(cd, ef));
        rl_u128 ab2 = rl_sha512h2(t1, cd, ab);
        gh = ef;
        ef = add64(cd, t1);
        cd = ab;
        ab = ab2;
        if (g < 32)
            w[g % 8] = rl_sha512su1(rl_sha512su0(w[g % 8], w[(g + 1) % 8]), w[(g + 7) % 8],
                                    ext8(w[(g + 4) % 8], w[(g + 5) % 8]));
    }
    static const uint64_t want[8] = {0xddaf35a193617abaull, 0xcc417349ae204131ull, 0x12e6fa4e89a97ea2ull,
                                     0x0a9eeee64b55d39aull, 0x2192992a274fc1a8ull, 0x36ba3c23a3feebbdull,
                                     0x454d4423643ce80eull, 0x2a9ac94fa54ca49full};
    rl_u128 st[4] = {ab, cd, ef, gh};
    for (unsigned i = 0; i < 4; i++) {
        CHECK_EQ((uint64_t)st[i] + iv512[2 * i], want[2 * i]);
        CHECK_EQ((uint64_t)(st[i] >> 64) + iv512[2 * i + 1], want[2 * i + 1]);
    }
}

TEST(crypto_sm3_abc)
{
    uint8_t b[64];
    pad64_abc(b);
    uint32_t w[68], wp[64];
    for (unsigned i = 0; i < 16; i++)
        w[i] = be32(b + 4 * i);
    /* W16..W67 four at a time with SM3PARTW1 and SM3PARTW2. */
    for (unsigned j = 16; j < 68; j += 4) {
        rl_u128 p1 = rl_sm3partw1(v32(w[j - 16], w[j - 15], w[j - 14], w[j - 13]),
                                  v32(w[j - 9], w[j - 8], w[j - 7], w[j - 6]),
                                  v32(w[j - 4], w[j - 3], w[j - 2], w[j - 1]));
        rl_u128 r = rl_sm3partw2(p1, v32(w[j - 6], w[j - 5], w[j - 4], w[j - 3]),
                                 v32(w[j - 13], w[j - 12], w[j - 11], w[j - 10]));
        for (unsigned i = 0; i < 4; i++)
            w[j + i] = w32(r, i);
    }
    for (unsigned j = 0; j < 64; j++)
        wp[j] = w[j] ^ w[j + 4];
    static const uint32_t iv[8] = {0x7380166fu, 0x4914b2b9u, 0x172442d7u, 0xda8a0600u,
                                   0xa96f30bcu, 0x163138aau, 0xe38dee4du, 0xb0fb0e4eu};
    /* State words (D, C, B, A) and (H, G, F, E), as SM3TT1x and SM3TT2x keep them. */
    rl_u128 abcd = v32(iv[3], iv[2], iv[1], iv[0]);
    rl_u128 efgh = v32(iv[7], iv[6], iv[5], iv[4]);
    for (unsigned j = 0; j < 64; j++) {
        uint32_t t = rol32(j < 16 ? 0x79cc4519u : 0x7a879d8au, j % 32);
        rl_u128 ss1 = rl_sm3ss1(abcd, efgh, (rl_u128)t << 96);
        unsigned g = j & ~3u, i = j & 3u;
        rl_u128 vwp = v32(wp[g], wp[g + 1], wp[g + 2], wp[g + 3]);
        rl_u128 vw = v32(w[g], w[g + 1], w[g + 2], w[g + 3]);
        if (j < 16) {
            abcd = rl_sm3tt1a(abcd, ss1, vwp, i);
            efgh = rl_sm3tt2a(efgh, ss1, vw, i);
        } else {
            abcd = rl_sm3tt1b(abcd, ss1, vwp, i);
            efgh = rl_sm3tt2b(efgh, ss1, vw, i);
        }
    }
    static const uint32_t want[8] = {0x66c7f0f4u, 0x62eeedd9u, 0xd1f2d46bu, 0xdc10e4e2u,
                                     0x4167c487u, 0x5cf2f7a2u, 0x297da02bu, 0x8f4ba8e0u};
    for (unsigned i = 0; i < 4; i++) {
        CHECK_EQ(w32(abcd, 3 - i) ^ iv[i], want[i]);
        CHECK_EQ(w32(efgh, 3 - i) ^ iv[4 + i], want[4 + i]);
    }
}

TEST(crypto_sm4_standard_example)
{
    /* GB/T 32907-2016 example 1: key = plaintext = 0123456789abcdeffedcba9876543210. */
    static const uint32_t fk[4] = {0xa3b1bac6u, 0x56aa3350u, 0x677d9197u, 0xb27022dcu};
    const uint32_t mk[4] = {0x01234567u, 0x89abcdefu, 0xfedcba98u, 0x76543210u};
    rl_u128 k = v32(mk[0] ^ fk[0], mk[1] ^ fk[1], mk[2] ^ fk[2], mk[3] ^ fk[3]);
    rl_u128 rk[8];
    for (unsigned g = 0; g < 8; g++) {
        uint32_t ck[4];
        for (unsigned i = 0; i < 4; i++) {
            uint32_t c = 0;
            for (unsigned j = 0; j < 4; j++)
                c = (c << 8) | (uint8_t)((4 * (4 * g + i) + j) * 7);
            ck[i] = c;
        }
        k = rl_sm4ekey(k, v32(ck[0], ck[1], ck[2], ck[3]));
        rk[g] = k;
    }
    CHECK_EQ(w32(rk[0], 0), 0xf12186f9u);
    CHECK_EQ(w32(rk[7], 3), 0x9124a012u);
    rl_u128 s = v32(mk[0], mk[1], mk[2], mk[3]);
    for (unsigned g = 0; g < 8; g++)
        s = rl_sm4e(s, rk[g]);
    CHECK_EQ(w32(s, 3), 0x681edf34u);
    CHECK_EQ(w32(s, 2), 0xd206965eu);
    CHECK_EQ(w32(s, 1), 0x86b3e94fu);
    CHECK_EQ(w32(s, 0), 0x536e4246u);
}

/* ---- Host paths against portable paths ---------------------------------- */

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint64_t rnd64(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

/** @brief A random vector, sometimes with zero or all-one words to reach the edges. */
static rl_u128 rnd128(void) {
    rl_u128 r = v64(rnd64(), rnd64());
    switch (rnd64() % 8) {
    case 0:
        r &= ~(rl_u128)0xffffffff;
        break;
    case 1:
        r |= (rl_u128)0xffffffff << 64;
        break;
    default:
        break;
    }
    return r;
}

/** @brief One differential case: the operands and the instruction to run. */
typedef struct cr_fn {
    const char *name;
    unsigned feature;
    rl_u128 (*fast)(rl_u128 d, rl_u128 n, rl_u128 m, rl_u128 x);
    rl_u128 (*slow)(rl_u128 d, rl_u128 n, rl_u128 m, rl_u128 x);
} cr_fn;

#define U64(x) ((uint64_t)(x))
#define U32(x) ((uint32_t)(x))
#define CR2(nm, F, expr_fast, expr_slow)                                                \
    static rl_u128 f_##nm(rl_u128 d, rl_u128 n, rl_u128 m, rl_u128 x) {                 \
        (void)d, (void)n, (void)m, (void)x;                                             \
        return expr_fast;                                                               \
    }                                                                                   \
    static rl_u128 s_##nm(rl_u128 d, rl_u128 n, rl_u128 m, rl_u128 x) {                 \
        (void)d, (void)n, (void)m, (void)x;                                             \
        return expr_slow;                                                               \
    }
#define CR(nm, F, args) CR2(nm, F, rl_##nm args, rl_##nm##_portable args)

CR(aese, RL_CRYPTO_HOST_AES, (d, n))
CR(aesd, RL_CRYPTO_HOST_AES, (d, n))
CR(aesmc, RL_CRYPTO_HOST_AES, (n))
CR(aesimc, RL_CRYPTO_HOST_AES, (n))
CR(pmull64, RL_CRYPTO_HOST_PMULL, (U64(n), U64(m)))
CR(sha1c, RL_CRYPTO_HOST_SHA1, (d, U32(n), m))
CR(sha1p, RL_CRYPTO_HOST_SHA1, (d, U32(n), m))
CR(sha1m, RL_CRYPTO_HOST_SHA1, (d, U32(n), m))
CR(sha1h, RL_CRYPTO_HOST_SHA1, (U32(n)))
CR(sha1su0, RL_CRYPTO_HOST_SHA1, (d, n, m))
CR(sha1su1, RL_CRYPTO_HOST_SHA1, (d, n))
CR(sha256h, RL_CRYPTO_HOST_SHA256, (d, n, m))
CR(sha256h2, RL_CRYPTO_HOST_SHA256, (d, n, m))
CR(sha256su0, RL_CRYPTO_HOST_SHA256, (d, n))
CR(sha256su1, RL_CRYPTO_HOST_SHA256, (d, n, m))
CR(sha512h, RL_CRYPTO_HOST_SHA512, (d, n, m))
CR(sha512h2, RL_CRYPTO_HOST_SHA512, (d, n, m))
CR(sha512su0, RL_CRYPTO_HOST_SHA512, (d, n))
CR(sha512su1, RL_CRYPTO_HOST_SHA512, (d, n, m))
CR(sm3partw1, RL_CRYPTO_HOST_SM3, (d, n, m))
CR(sm3partw2, RL_CRYPTO_HOST_SM3, (d, n, m))
CR(sm3ss1, RL_CRYPTO_HOST_SM3, (n, m, x))
CR(sm3tt1a, RL_CRYPTO_HOST_SM3, (d, n, m, U32(x) & 3))
CR(sm3tt1b, RL_CRYPTO_HOST_SM3, (d, n, m, U32(x) & 3))
CR(sm3tt2a, RL_CRYPTO_HOST_SM3, (d, n, m, U32(x) & 3))
CR(sm3tt2b, RL_CRYPTO_HOST_SM3, (d, n, m, U32(x) & 3))
CR(sm4e, RL_CRYPTO_HOST_SM4, (d, n))
CR(sm4ekey, RL_CRYPTO_HOST_SM4, (n, m))

/* SHA1H has no x86-64 instruction; it is a rotate and stays portable there. */
#if defined(__x86_64__)
#define SHA1H_HOST 0u
#else
#define SHA1H_HOST RL_CRYPTO_HOST_SHA1
#endif

#define E(nm, F) {#nm, F, f_##nm, s_##nm}
static const cr_fn fns[] = {
    E(aese, RL_CRYPTO_HOST_AES),         E(aesd, RL_CRYPTO_HOST_AES),
    E(aesmc, RL_CRYPTO_HOST_AES),        E(aesimc, RL_CRYPTO_HOST_AES),
    E(pmull64, RL_CRYPTO_HOST_PMULL),    E(sha1c, RL_CRYPTO_HOST_SHA1),
    E(sha1p, RL_CRYPTO_HOST_SHA1),       E(sha1m, RL_CRYPTO_HOST_SHA1),
    E(sha1h, SHA1H_HOST),       E(sha1su0, RL_CRYPTO_HOST_SHA1),
    E(sha1su1, RL_CRYPTO_HOST_SHA1),     E(sha256h, RL_CRYPTO_HOST_SHA256),
    E(sha256h2, RL_CRYPTO_HOST_SHA256),  E(sha256su0, RL_CRYPTO_HOST_SHA256),
    E(sha256su1, RL_CRYPTO_HOST_SHA256), E(sha512h, RL_CRYPTO_HOST_SHA512),
    E(sha512h2, RL_CRYPTO_HOST_SHA512),  E(sha512su0, RL_CRYPTO_HOST_SHA512),
    E(sha512su1, RL_CRYPTO_HOST_SHA512), E(sm3partw1, RL_CRYPTO_HOST_SM3),
    E(sm3partw2, RL_CRYPTO_HOST_SM3),    E(sm3ss1, RL_CRYPTO_HOST_SM3),
    E(sm3tt1a, RL_CRYPTO_HOST_SM3),      E(sm3tt1b, RL_CRYPTO_HOST_SM3),
    E(sm3tt2a, RL_CRYPTO_HOST_SM3),      E(sm3tt2b, RL_CRYPTO_HOST_SM3),
    E(sm4e, RL_CRYPTO_HOST_SM4),         E(sm4ekey, RL_CRYPTO_HOST_SM4),
};
#define NFNS (sizeof fns / sizeof fns[0])

/** @brief Random cases per function. */
#define CR_RANDOM 20000

TEST(crypto_host_paths_match_portable)
{
    unsigned f = rl_crypto_host_features();
    unsigned ran = 0;
    unsigned long long cases = 0;
    printf("  host crypto features 0x%x:", f);
    for (unsigned i = 0; i < NFNS; i++) {
        if (!(f & fns[i].feature))
            continue;
        ran++;
        printf(" %s", fns[i].name);
        for (unsigned k = 0; k < CR_RANDOM; k++) {
            rl_u128 d = rnd128(), n = rnd128(), m = rnd128(), x = rnd128();
            rl_u128 a = fns[i].fast(d, n, m, x), b = fns[i].slow(d, n, m, x);
            if (a != b) {
                printf("\n  %s mismatch: host %016llx%016llx portable %016llx%016llx\n", fns[i].name,
                       (unsigned long long)(a >> 64), (unsigned long long)a, (unsigned long long)(b >> 64),
                       (unsigned long long)b);
                fflush(stdout);
            }
            CHECK(a == b);
            cases++;
        }
    }
    printf("\n  %u of %u functions on host paths, %llu random cases matched\n", ran, (unsigned)NFNS, cases);
    fflush(stdout);
}

/* ---- Vector file -------------------------------------------------------- */

/** @brief Parses one hex operand of at most 32 digits; returns the end or NULL. */
static const char *hex128(const char *s, rl_u128 *out) {
    while (*s == ' ' || *s == '\t')
        s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    rl_u128 v = 0;
    unsigned nd = 0;
    while (isxdigit((unsigned char)*s)) {
        int c = *s++;
        unsigned dv = (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        v = (v << 4) | dv;
        if (++nd > 32)
            return NULL;
    }
    if (!nd)
        return NULL;
    *out = v;
    return s;
}

TEST(crypto_vector_file)
{
    const char *path = getenv("RL_CRYPTO_VECTORS");
    if (!path || !*path) {
        printf("  RL_CRYPTO_VECTORS unset, skipped\n");
        fflush(stdout);
        return;
    }
    FILE *fp = fopen(path, "r");
    CHECK(fp != NULL);
    char line[512];
    unsigned long long n_ok = 0, n_bad = 0, lineno = 0;
    while (fgets(line, sizeof line, fp)) {
        lineno++;
        char *h = strchr(line, '#');
        if (h)
            *h = 0;
        char name[32];
        int used = 0;
        if (sscanf(line, " %31s%n", name, &used) != 1)
            continue;
        const cr_fn *fn = NULL;
        for (unsigned i = 0; i < NFNS; i++)
            if (!strcmp(name, fns[i].name))
                fn = &fns[i];
        if (!fn) {
            printf("  line %llu: unknown function %s\n", lineno, name);
            n_bad++;
            continue;
        }
        rl_u128 op[4], want;
        const char *p = line + used;
        for (unsigned i = 0; i < 4 && p; i++)
            p = hex128(p, &op[i]);
        if (p) {
            while (*p == ' ' || *p == '\t')
                p++;
            p = (p[0] == '-' && p[1] == '>') ? hex128(p + 2, &want) : NULL;
        }
        if (!p) {
            printf("  line %llu: bad syntax\n", lineno);
            n_bad++;
            continue;
        }
        rl_u128 a = fn->fast(op[0], op[1], op[2], op[3]);
        rl_u128 b = fn->slow(op[0], op[1], op[2], op[3]);
        if (a != want || b != want) {
            printf("  line %llu: %s got %016llx%016llx (portable %016llx%016llx)\n", lineno, name,
                   (unsigned long long)(a >> 64), (unsigned long long)a, (unsigned long long)(b >> 64),
                   (unsigned long long)b);
            n_bad++;
        } else {
            n_ok++;
        }
    }
    fclose(fp);
    printf("  %s: %llu vectors matched, %llu failed\n", path, n_ok, n_bad);
    fflush(stdout);
    CHECK_EQ(n_bad, 0);
    CHECK(n_ok > 0);
}
