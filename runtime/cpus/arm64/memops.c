/**
 * @file memops.c
 * @brief Host replacements for guest libc memory routines (docs/abi.md
 * section 7.2): the guest routine's loads and stores become the host's
 * memmove and memset on the guest window, which move wider and need no
 * lifted code.
 */
#include "arm64.h"

#include <string.h>

int rl_arm64_memmove(rl_ctx *c, uint8_t *mem)
{
    memmove(mem + c->x[0], mem + c->x[1], c->x[2]);
    return RL_OK;
}

int rl_arm64_memset(rl_ctx *c, uint8_t *mem)
{
    memset(mem + c->x[0], (int)(c->x[1] & 0xffu), c->x[2]);
    return RL_OK;
}
