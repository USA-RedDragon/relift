/**
 * @file softfloat.h
 * @brief IEEE 754 binary32 and binary64 arithmetic in integer code, with the
 * four rounding modes of FCR31 and the five exception flags. Tininess is
 * detected after rounding and the underflow flag needs an inexact result, as
 * on x86 with underflow masked. A NaN operand gives some NaN with no flag:
 * the VR4300 rules (fpu.c) look at NaN operands before an operation runs and
 * replace a NaN result with the default one.
 *
 * Values travel as their bit patterns in a uint64_t (binary32 in the low
 * word); dbl selects binary64.
 */
#ifndef RELIFT_MIPS_SOFTFLOAT_H
#define RELIFT_MIPS_SOFTFLOAT_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Exception flags, in FCR31's order (bit 0 inexact). */
enum {
    RL_SF_I = 1u << 0,
    RL_SF_U = 1u << 1,
    RL_SF_O = 1u << 2,
    RL_SF_Z = 1u << 3,
    RL_SF_V = 1u << 4,
};

/** @brief Rounding modes, in FCR31's encoding. */
enum {
    RL_SF_RN = 0,
    RL_SF_RZ = 1,
    RL_SF_RP = 2,
    RL_SF_RM = 3,
};

/** @brief The class of a value. */
typedef enum rl_sf_class {
    RL_SF_ZERO,
    RL_SF_SUBNORMAL,
    RL_SF_NORMAL,
    RL_SF_INF,
    RL_SF_NAN,
} rl_sf_class;

/** @brief The class of the value with bits x. */
rl_sf_class rl_sf_classify(bool dbl, uint64_t x);
/** @brief Whether x has its sign bit set. */
bool rl_sf_sign(bool dbl, uint64_t x);
/** @brief The top bit of the fraction (set: a quiet NaN in IEEE 754-2008, a signalling one on the VR4300). */
bool rl_sf_frac_msb(bool dbl, uint64_t x);

/** @brief a + b rounded by rm; flags gets the exceptions raised. */
uint64_t rl_sf_add(bool dbl, uint64_t a, uint64_t b, unsigned rm, unsigned *flags);
/** @brief a - b. */
uint64_t rl_sf_sub(bool dbl, uint64_t a, uint64_t b, unsigned rm, unsigned *flags);
/** @brief a * b. */
uint64_t rl_sf_mul(bool dbl, uint64_t a, uint64_t b, unsigned rm, unsigned *flags);
/** @brief a / b. */
uint64_t rl_sf_div(bool dbl, uint64_t a, uint64_t b, unsigned rm, unsigned *flags);
/** @brief The square root of a. */
uint64_t rl_sf_sqrt(bool dbl, uint64_t a, unsigned rm, unsigned *flags);
/** @brief a converted to the other format (to_dbl: binary32 to binary64). */
uint64_t rl_sf_convert(bool to_dbl, uint64_t a, unsigned rm, unsigned *flags);
/** @brief The integer v as a float. */
uint64_t rl_sf_from_int(bool dbl, int64_t v, unsigned rm, unsigned *flags);
/**
 * @brief a rounded by rm to an integer. a must be finite with |a| < 2^63
 * (the caller checks the range it needs); sets the inexact flag when a was
 * not an integer.
 */
int64_t rl_sf_to_int(bool dbl, uint64_t a, unsigned rm, unsigned *flags);

/** @brief The result of a comparison. */
typedef enum rl_sf_order {
    RL_SF_LESS,
    RL_SF_EQUAL,
    RL_SF_GREATER,
    RL_SF_UNORDERED,
} rl_sf_order;

/** @brief How a compares with b (signed zeros are equal, a NaN is unordered). */
rl_sf_order rl_sf_compare(bool dbl, uint64_t a, uint64_t b);

#endif
