/**
 * @file spirvval.h
 * @brief A check of every SPIR-V module the runtime makes, for builds with
 * RL_CHECKS: when RELIFT_SPIRV_VAL names a validator command (the test runner
 * sets it to spirv-val when that is on the path), each module is piped to it
 * before the device sees it, and an invalid one stops the process with the
 * validator's message, so a SPIR-V patcher cannot emit invalid code unnoticed.
 */
#ifndef RELIFT_CORE_SPIRVVAL_H
#define RELIFT_CORE_SPIRVVAL_H

#include <stddef.h>
#include <stdint.h>

/** @brief The environment variable that names the validator command, which reads the module on its standard input. */
#define RL_SPIRV_VAL_ENV "RELIFT_SPIRV_VAL"

/**
 * @brief Pipes the n words at w to the validator RELIFT_SPIRV_VAL names and aborts with what's name when it rejects
 * them. Does nothing without RL_CHECKS, without the variable, or on hosts that cannot start a process.
 */
void rl_spirv_check(const uint32_t *w, size_t n, const char *what);

#endif
