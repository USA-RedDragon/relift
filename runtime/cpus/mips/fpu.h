/**
 * @file fpu.h
 * @brief The VR4300 FPU (COP1) rules over the soft-float primitives: every
 * arithmetic, conversion and compare operation of formats S, D, W and L with
 * the FCR31 rounding mode, cause, flag and enable bits, flush to zero (FS),
 * the unimplemented-operation exception and the FR register views.
 */
#ifndef RELIFT_MIPS_FPU_H
#define RELIFT_MIPS_FPU_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Runs the COP1 arithmetic or compare instruction insn (major opcode
 * 0x11, fmt 0x10 or above) on the register file f and *fcr31, with the FR
 * view of status. Coprocessor 1 is taken as usable. Returns true when the
 * instruction raises the floating-point exception (the destination is then
 * untouched and FCR31 holds the cause bits).
 */
bool rl_mips_fpu_eval(uint32_t insn, uint32_t status, uint32_t *fcr31, uint64_t f[32]);

/** @brief Whether insn is a COP1 arithmetic or compare instruction (what rl_mips_fpu_eval runs). */
bool rl_mips_fpu_is_arith(uint32_t insn);
/**
 * @brief The cycles a COP1 arithmetic instruction takes beyond its issue
 * cycle, as the lifted templates (from the Sail model) charge them.
 */
uint32_t rl_mips_fpu_cycles(uint32_t insn);

#endif
