/**
 * @file test_nvn_shading.c
 * @brief A program's sample shading mode takes effect at its next nvnProgramSetShaders, as the driver's does.
 */
#include "nvn/nvn_priv.h"
#include "tc.h"

#define RET 0x7000000ull

static uint64_t call(rl_ctx *c, rl_fn_t *fn, uint64_t x0, uint64_t x1, uint64_t x2)
{
    c->x[0] = x0;
    c->x[1] = x1;
    c->x[2] = x2;
    c->x[30] = RET;
    return fn(c, t_mem());
}

TEST(nvn_sample_shading_takes_effect_at_the_next_set_shaders)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t prog = t_heap(c, 0x1000);
    CHECK_EQ(call(c, nvnh_nvnProgramInitialize, prog, 0, 0), RET);
    CHECK_EQ(nvn_program_sample_shading(prog), 0u);
    call(c, nvnh_nvnProgramSetSampleShading, prog, 1, 0);
    CHECK_EQ(nvn_program_sample_shading(prog), 0u);
    call(c, nvnh_nvnProgramSetShaders, prog, 0, 0);
    CHECK_EQ(nvn_program_sample_shading(prog), 1u);
    call(c, nvnh_nvnProgramSetSampleShading, prog, 2, 0);
    CHECK_EQ(nvn_program_sample_shading(prog), 1u);
    call(c, nvnh_nvnProgramSetShaders, prog, 0, 0);
    CHECK_EQ(nvn_program_sample_shading(prog), 2u);
    call(c, nvnh_nvnProgramSetSampleShading, prog, 0, 0);
    call(c, nvnh_nvnProgramSetShaders, prog, 0, 0);
    CHECK_EQ(nvn_program_sample_shading(prog), 0u);
    call(c, nvnh_nvnProgramFinalize, prog, 0, 0);
}
