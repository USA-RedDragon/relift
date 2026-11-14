/**
 * @file relift/a64/crypto.h
 * @brief A64 cryptographic extension instructions (FEAT_AES, FEAT_PMULL,
 * FEAT_SHA1, FEAT_SHA256, FEAT_SHA512, FEAT_SM3, FEAT_SM4): one function per
 * guest instruction, with the single-instruction semantics of the Arm
 * pseudocode (docs/abi.md section 10.2).
 *
 * A 128-bit vector register is an rl_u128 holding the register's bits:
 * element e of size s is bits [e*s, e*s+s). d is the old value of the
 * destination register, n and m are the Vn and Vm operands. Every function
 * uses the host's own instruction when the host has it (detected once at run
 * time) and a portable C path otherwise; rl_<name>_portable is the portable
 * path, always available, for tests.
 */
#ifndef RELIFT_A64_CRYPTO_H
#define RELIFT_A64_CRYPTO_H

#include <relift/a64/abi.h>

/** @brief rl_crypto_host_features bit: AESE, AESD, AESMC, AESIMC run on the host's instructions. */
#define RL_CRYPTO_HOST_AES (1u << 0)
/** @brief PMULL (64-bit) runs on the host's instruction. */
#define RL_CRYPTO_HOST_PMULL (1u << 1)
/** @brief SHA1C, SHA1P, SHA1M, SHA1SU0, SHA1SU1 (and SHA1H on arm64) run on the host's instructions. */
#define RL_CRYPTO_HOST_SHA1 (1u << 2)
/** @brief SHA256H, SHA256H2, SHA256SU0, SHA256SU1 run on the host's instructions. */
#define RL_CRYPTO_HOST_SHA256 (1u << 3)
/** @brief SHA512H, SHA512H2, SHA512SU0, SHA512SU1 run on the host's instructions. */
#define RL_CRYPTO_HOST_SHA512 (1u << 4)
/** @brief SM3PARTW1, SM3PARTW2, SM3SS1, SM3TT1A/B, SM3TT2A/B run on the host's instructions. */
#define RL_CRYPTO_HOST_SM3 (1u << 5)
/** @brief SM4E and SM4EKEY run on the host's instructions. */
#define RL_CRYPTO_HOST_SM4 (1u << 6)

/** @brief The RL_CRYPTO_HOST_* bits of the paths this host uses. Detected on the first call and cached. */
RL_RUNTIME_API unsigned rl_crypto_host_features(void);

/** @brief AESE: SubBytes(ShiftRows(d ^ n)). */
RL_RUNTIME_API rl_u128 rl_aese(rl_u128 d, rl_u128 n);
/** @brief AESD: InvSubBytes(InvShiftRows(d ^ n)). */
RL_RUNTIME_API rl_u128 rl_aesd(rl_u128 d, rl_u128 n);
/** @brief AESMC: MixColumns(x). */
RL_RUNTIME_API rl_u128 rl_aesmc(rl_u128 x);
/** @brief AESIMC: InvMixColumns(x). */
RL_RUNTIME_API rl_u128 rl_aesimc(rl_u128 x);
/** @brief PMULL and PMULL2 with 64-bit elements (1Q): the carry-less product of a and b. */
RL_RUNTIME_API rl_u128 rl_pmull64(uint64_t a, uint64_t b);

/** @brief SHA1C: four SHA-1 rounds with the choose function; d = ABCD, e = Sn, m = W + K. Returns the new ABCD. */
RL_RUNTIME_API rl_u128 rl_sha1c(rl_u128 d, uint32_t e, rl_u128 m);
/** @brief SHA1P: four SHA-1 rounds with the parity function. */
RL_RUNTIME_API rl_u128 rl_sha1p(rl_u128 d, uint32_t e, rl_u128 m);
/** @brief SHA1M: four SHA-1 rounds with the majority function. */
RL_RUNTIME_API rl_u128 rl_sha1m(rl_u128 d, uint32_t e, rl_u128 m);
/** @brief SHA1H: n rotated left by 30. */
RL_RUNTIME_API uint32_t rl_sha1h(uint32_t n);
/** @brief SHA1SU0: (n[63:0] : d[127:64]) ^ d ^ m. */
RL_RUNTIME_API rl_u128 rl_sha1su0(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief SHA1SU1: the last step of four SHA-1 schedule words. */
RL_RUNTIME_API rl_u128 rl_sha1su1(rl_u128 d, rl_u128 n);

/** @brief SHA256H: four SHA-256 rounds; d = ABCD, n = EFGH, m = W + K. Returns the new ABCD. */
RL_RUNTIME_API rl_u128 rl_sha256h(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief SHA256H2: four SHA-256 rounds; d = EFGH, n = ABCD, m = W + K. Returns the new EFGH. */
RL_RUNTIME_API rl_u128 rl_sha256h2(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief SHA256SU0: the first step of four SHA-256 schedule words. */
RL_RUNTIME_API rl_u128 rl_sha256su0(rl_u128 d, rl_u128 n);
/** @brief SHA256SU1: the second step of four SHA-256 schedule words. */
RL_RUNTIME_API rl_u128 rl_sha256su1(rl_u128 d, rl_u128 n, rl_u128 m);

/** @brief SHA512H: the first half of two SHA-512 rounds. */
RL_RUNTIME_API rl_u128 rl_sha512h(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief SHA512H2: the second half of two SHA-512 rounds. */
RL_RUNTIME_API rl_u128 rl_sha512h2(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief SHA512SU0: the first step of two SHA-512 schedule words. */
RL_RUNTIME_API rl_u128 rl_sha512su0(rl_u128 d, rl_u128 n);
/** @brief SHA512SU1: the second step of two SHA-512 schedule words. */
RL_RUNTIME_API rl_u128 rl_sha512su1(rl_u128 d, rl_u128 n, rl_u128 m);

/** @brief SM3PARTW1: the first part of four SM3 message expansion words. */
RL_RUNTIME_API rl_u128 rl_sm3partw1(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief SM3PARTW2: the second part of four SM3 message expansion words. */
RL_RUNTIME_API rl_u128 rl_sm3partw2(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief SM3SS1 Vd, Vn, Vm, Va: bits [127:96] = ROL(ROL(n, 12) + m + a, 7) on the top words, the rest zero. */
RL_RUNTIME_API rl_u128 rl_sm3ss1(rl_u128 n, rl_u128 m, rl_u128 a);
/** @brief SM3TT1A Vd, Vn, Vm[imm2]: one SM3 round, TT1 with the XOR function. */
RL_RUNTIME_API rl_u128 rl_sm3tt1a(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2);
/** @brief SM3TT1B: one SM3 round, TT1 with the majority function. */
RL_RUNTIME_API rl_u128 rl_sm3tt1b(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2);
/** @brief SM3TT2A: one SM3 round, TT2 with the XOR function. */
RL_RUNTIME_API rl_u128 rl_sm3tt2a(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2);
/** @brief SM3TT2B: one SM3 round, TT2 with the choose function. */
RL_RUNTIME_API rl_u128 rl_sm3tt2b(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2);
/** @brief SM4E: four SM4 rounds on the state d with the round keys n. */
RL_RUNTIME_API rl_u128 rl_sm4e(rl_u128 d, rl_u128 n);
/** @brief SM4EKEY Vd, Vn, Vm: four SM4 key schedule rounds on the keys n with the constants m. */
RL_RUNTIME_API rl_u128 rl_sm4ekey(rl_u128 n, rl_u128 m);

/** @brief Portable path of rl_aese. */
RL_RUNTIME_API rl_u128 rl_aese_portable(rl_u128 d, rl_u128 n);
/** @brief Portable path of rl_aesd. */
RL_RUNTIME_API rl_u128 rl_aesd_portable(rl_u128 d, rl_u128 n);
/** @brief Portable path of rl_aesmc. */
RL_RUNTIME_API rl_u128 rl_aesmc_portable(rl_u128 x);
/** @brief Portable path of rl_aesimc. */
RL_RUNTIME_API rl_u128 rl_aesimc_portable(rl_u128 x);
/** @brief Portable path of rl_pmull64. */
RL_RUNTIME_API rl_u128 rl_pmull64_portable(uint64_t a, uint64_t b);
/** @brief Portable path of rl_sha1c. */
RL_RUNTIME_API rl_u128 rl_sha1c_portable(rl_u128 d, uint32_t e, rl_u128 m);
/** @brief Portable path of rl_sha1p. */
RL_RUNTIME_API rl_u128 rl_sha1p_portable(rl_u128 d, uint32_t e, rl_u128 m);
/** @brief Portable path of rl_sha1m. */
RL_RUNTIME_API rl_u128 rl_sha1m_portable(rl_u128 d, uint32_t e, rl_u128 m);
/** @brief Portable path of rl_sha1h. */
RL_RUNTIME_API uint32_t rl_sha1h_portable(uint32_t n);
/** @brief Portable path of rl_sha1su0. */
RL_RUNTIME_API rl_u128 rl_sha1su0_portable(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief Portable path of rl_sha1su1. */
RL_RUNTIME_API rl_u128 rl_sha1su1_portable(rl_u128 d, rl_u128 n);
/** @brief Portable path of rl_sha256h. */
RL_RUNTIME_API rl_u128 rl_sha256h_portable(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief Portable path of rl_sha256h2. */
RL_RUNTIME_API rl_u128 rl_sha256h2_portable(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief Portable path of rl_sha256su0. */
RL_RUNTIME_API rl_u128 rl_sha256su0_portable(rl_u128 d, rl_u128 n);
/** @brief Portable path of rl_sha256su1. */
RL_RUNTIME_API rl_u128 rl_sha256su1_portable(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief Portable path of rl_sha512h. */
RL_RUNTIME_API rl_u128 rl_sha512h_portable(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief Portable path of rl_sha512h2. */
RL_RUNTIME_API rl_u128 rl_sha512h2_portable(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief Portable path of rl_sha512su0. */
RL_RUNTIME_API rl_u128 rl_sha512su0_portable(rl_u128 d, rl_u128 n);
/** @brief Portable path of rl_sha512su1. */
RL_RUNTIME_API rl_u128 rl_sha512su1_portable(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief Portable path of rl_sm3partw1. */
RL_RUNTIME_API rl_u128 rl_sm3partw1_portable(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief Portable path of rl_sm3partw2. */
RL_RUNTIME_API rl_u128 rl_sm3partw2_portable(rl_u128 d, rl_u128 n, rl_u128 m);
/** @brief Portable path of rl_sm3ss1. */
RL_RUNTIME_API rl_u128 rl_sm3ss1_portable(rl_u128 n, rl_u128 m, rl_u128 a);
/** @brief Portable path of rl_sm3tt1a. */
RL_RUNTIME_API rl_u128 rl_sm3tt1a_portable(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2);
/** @brief Portable path of rl_sm3tt1b. */
RL_RUNTIME_API rl_u128 rl_sm3tt1b_portable(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2);
/** @brief Portable path of rl_sm3tt2a. */
RL_RUNTIME_API rl_u128 rl_sm3tt2a_portable(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2);
/** @brief Portable path of rl_sm3tt2b. */
RL_RUNTIME_API rl_u128 rl_sm3tt2b_portable(rl_u128 d, rl_u128 n, rl_u128 m, unsigned imm2);
/** @brief Portable path of rl_sm4e. */
RL_RUNTIME_API rl_u128 rl_sm4e_portable(rl_u128 d, rl_u128 n);
/** @brief Portable path of rl_sm4ekey. */
RL_RUNTIME_API rl_u128 rl_sm4ekey_portable(rl_u128 n, rl_u128 m);

#endif
