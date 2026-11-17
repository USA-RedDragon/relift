/**
 * @file pipe.h
 * @brief The RSP's issue pipeline at run time: the model of relift-cpu-rsp's
 * pipe module over the words in IMEM, for the transfers whose target lifted
 * code cannot see (rsp.c rl_rsp_pipe).
 */
#ifndef RELIFT_RSP_PIPE_H
#define RELIFT_RSP_PIPE_H

#include <stdbool.h>
#include <stdint.h>

/** @brief The pipeline between two words (relift_cpu_rsp::pipe::State). */
typedef struct rl_rsp_pipe_state {
    /** @brief Scalar registers not ready in the last two cycles, newest first. */
    uint32_t r[2];
    /** @brief Vector registers not ready in the last three cycles. */
    uint32_t v[3];
    /** @brief The groups of the last two cycles loaded. */
    bool load[2];
    /** @brief The next group issues alone. */
    bool single;
    /** @brief The previous word starts a pair with the next one. */
    bool pend;
} rl_rsp_pipe_state;

/** @brief The state State::pack gives as a, b and d. */
rl_rsp_pipe_state rl_rsp_pipe_unpack(uint64_t a, uint64_t b, uint64_t d);

/** @brief The stall after the delay slot of a taken branch to IMEM word t. */
void rl_rsp_pipe_taken(rl_rsp_pipe_state *s, uint32_t t);

/**
 * @brief What the cycles from IMEM word w on differ by when the pipeline is
 * x instead of e (pipe::delta); *met tells whether the two met.
 */
int64_t rl_rsp_pipe_delta(rl_rsp_pipe_state x, rl_rsp_pipe_state e, uint32_t w, const uint8_t *imem, bool *met);

#endif
