/**
 * @file fpu_cases.c
 * @brief Runs COP1 instructions through the FPU slow path, one case per
 * line, for relift-check's differential run against ares
 * (`relift-check --oracle ares lift --fp-corpus --c-slow-path FILE`).
 *
 * Input lines: `insn status fcr31 f0 ... f31`, in hex. Output lines:
 * `exc fcr31 f0 ... f31`, in hex, where exc is 15 when the instruction
 * raised the floating-point exception and 0 otherwise.
 */
#include "fpu.h"

#include <inttypes.h>
#include <stdio.h>

int main(void)
{
    unsigned long long insn, status, fcr;
    while (scanf("%llx %llx %llx", &insn, &status, &fcr) == 3) {
        uint64_t f[32];
        for (int i = 0; i < 32; i++) {
            unsigned long long v;
            if (scanf("%llx", &v) != 1)
                return 2;
            f[i] = v;
        }
        uint32_t fcr31 = (uint32_t)fcr;
        bool raised = rl_mips_fpu_eval((uint32_t)insn, (uint32_t)status, &fcr31, f);
        printf("%x %x", raised ? 15u : 0u, fcr31);
        for (int i = 0; i < 32; i++)
            printf(" %" PRIx64, f[i]);
        printf("\n");
    }
    return 0;
}
