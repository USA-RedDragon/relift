/**
 * @file fp_slow.h
 * @brief The single-instruction FP slow path: lifted FP code assumes FPCR is
 * zero, and when it is not it stores the context and calls rl_fp_slow for
 * the one instruction.
 */
#ifndef RELIFT_A64_FP_SLOW_H
#define RELIFT_A64_FP_SLOW_H

#include <relift/a64/abi.h>

/**
 * @brief Executes one A64 FP or AdvSIMD FP instruction with the context's
 * current FPCR, exactly as the architecture defines it.
 *
 * The context holds every guest register on entry. The registers, NZCV (for
 * compares) and the cumulative FPSR bits are updated in place. Returns
 * RL_OK, after which the caller continues at pc + 4. An encoding that has no
 * slow path is reported with rl_guest_fatal and gives RL_PENDING.
 *
 * Covered: scalar single and double data processing (one, two and three
 * sources), FCVT with half, FCMP/FCMPE, FCCMP/FCCMPE, FCSEL, FMOV immediate,
 * float to integer and fixed point, integer and fixed point to float, FMOV
 * between general and FP registers, and the AdvSIMD forms FADD, FSUB, FMUL,
 * FDIV, FMLA, FMLS, FABD, FMAX, FMIN, FMAXNM, FMINNM, their pairwise forms,
 * FABS, FNEG, FSQRT, FRINT*, FCVT[NPMZA][SU] and SCVTF/UCVTF (2S, 4S, 2D).
 */
RL_RUNTIME_API int rl_fp_slow(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn);

#endif
