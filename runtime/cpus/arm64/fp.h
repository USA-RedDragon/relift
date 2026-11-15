/**
 * @file fp.h
 * @brief Width-generic entries of the exact FP layer (fp.c) for the FPCR
 * slow paths (fp_slow.c): every Arm FP operation the slow paths need, for
 * 16, 32 and 64-bit formats, with the given FPCR, ORing the cumulative
 * flags into *fpsr (NULL drops them). Private to the A64 layer.
 */
#ifndef RELIFT_ARM64_FP_H
#define RELIFT_ARM64_FP_H

#include <stdint.h>

/** @brief FPAdd. */
uint64_t rl_fpn_add(unsigned bits, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPSub. */
uint64_t rl_fpn_sub(unsigned bits, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPMul. */
uint64_t rl_fpn_mul(unsigned bits, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPDiv. */
uint64_t rl_fpn_div(unsigned bits, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPMulAdd: addend + a * b with one rounding. */
uint64_t rl_fpn_muladd(unsigned bits, uint64_t addend, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPSqrt. */
uint64_t rl_fpn_sqrt(unsigned bits, uint64_t a, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPRoundInt with rmode from enum rl_rmode; exact for FRINTX. */
uint64_t rl_fpn_round_int(unsigned bits, uint64_t a, uint32_t rmode, int exact, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPRoundIntN (FRINT32Z, FRINT32X, FRINT64Z, FRINT64X) to an n-bit (32 or 64) signed range. */
uint64_t rl_fpn_round_int_n(unsigned bits, uint64_t a, uint32_t rmode, unsigned n, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPRecipEstimate (FRECPE). */
uint64_t rl_fpn_recip_est(unsigned bits, uint64_t a, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPRSqrtEstimate (FRSQRTE). */
uint64_t rl_fpn_rsqrt_est(unsigned bits, uint64_t a, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPRecipStepFused (FRECPS): 2 - a * b. */
uint64_t rl_fpn_recip_step(unsigned bits, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPRSqrtStepFused (FRSQRTS): (3 - a * b) / 2. */
uint64_t rl_fpn_rsqrt_step(unsigned bits, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPMax, FPMin (num 0) or FPMaxNum, FPMinNum (num 1). */
uint64_t rl_fpn_maxmin(unsigned bits, uint64_t a, uint64_t b, int is_max, int num, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPCompare: NZCV in bits 31 to 28; signal_nans for FCMPE. */
uint32_t rl_fpn_compare(unsigned bits, uint64_t a, uint64_t b, int signal_nans, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPCompareEQ (kind 0), FPCompareGE (1), FPCompareGT (2). */
int rl_fpn_compare_bool(unsigned bits, uint64_t a, uint64_t b, int kind, uint32_t fpcr, uint32_t *fpsr);

#endif
