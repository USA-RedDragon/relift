/**
 * @file relift/arm64/fp16.h
 * @brief IEEE binary16 arithmetic on uint16_t bit patterns for the IR's
 * half-precision operations (docs/abi.md section 10.3).
 *
 * Round to nearest even (rl_fp16_round_int takes a mode), no FPSR flags, no
 * flush to zero. A NaN result is the first NaN operand quieted (sign and
 * payload kept, the top fraction bit set), else the default NaN 0x7e00; the
 * IR leaves Arm's NaN selection to the frontend, so this only needs to be
 * deterministic. Every result is computed exactly in integers and rounded
 * once, so it does not depend on the host FPU.
 */
#ifndef RELIFT_ARM64_FP16_H
#define RELIFT_ARM64_FP16_H

#include <relift/arm64/abi.h>

/** @brief The default NaN of the binary16 helpers. */
#define RL_FP16_DEFAULT_NAN 0x7e00u

/** @brief a + b. */
RL_RUNTIME_API uint16_t rl_fp16_add(uint16_t a, uint16_t b);
/** @brief a - b. */
RL_RUNTIME_API uint16_t rl_fp16_sub(uint16_t a, uint16_t b);
/** @brief a * b. */
RL_RUNTIME_API uint16_t rl_fp16_mul(uint16_t a, uint16_t b);
/** @brief a / b. */
RL_RUNTIME_API uint16_t rl_fp16_div(uint16_t a, uint16_t b);
/** @brief The square root of a; -0 gives -0, other negative values the default NaN. */
RL_RUNTIME_API uint16_t rl_fp16_sqrt(uint16_t a);
/** @brief a * b + c with one rounding. NaN operands are taken in the order a, b, c. */
RL_RUNTIME_API uint16_t rl_fp16_muladd(uint16_t a, uint16_t b, uint16_t c);
/** @brief a rounded to an integral value in rmode (RL_RM_NEAREST, RL_RM_POSINF, RL_RM_NEGINF, RL_RM_ZERO, RL_RM_AWAY; other values round to nearest). A zero result keeps the sign of a. */
RL_RUNTIME_API uint16_t rl_fp16_round_int(uint16_t a, int rmode);
/** @brief The binary32 bits of h (exact; a NaN keeps its sign, payload and quietness). */
RL_RUNTIME_API uint32_t rl_fp16_to_f32(uint16_t h);
/** @brief The binary64 bits of h (exact; a NaN keeps its sign, payload and quietness). */
RL_RUNTIME_API uint64_t rl_fp16_to_f64(uint16_t h);
/** @brief Binary32 bits rounded once to binary16; a NaN is quieted and keeps its sign and the top 10 payload bits. */
RL_RUNTIME_API uint16_t rl_fp16_from_f32(uint32_t f);
/** @brief Binary64 bits rounded once to binary16; a NaN is quieted and keeps its sign and the top 10 payload bits. */
RL_RUNTIME_API uint16_t rl_fp16_from_f64(uint64_t d);
/** @brief The integer x of bits bits (32 or 64; the rest of x is ignored), signed unless is_unsigned, rounded once to binary16. */
RL_RUNTIME_API uint16_t rl_fp16_from_int(uint64_t x, unsigned bits, int is_unsigned);
/** @brief h rounded to an integer in rmode (as rl_fp16_round_int) and saturated to bits bits (32 or 64), signed unless is_unsigned; a NaN gives 0. The result is zero-extended from bits bits. */
RL_RUNTIME_API uint64_t rl_fp16_to_int(uint16_t h, unsigned bits, int is_unsigned, int rmode);

#endif
