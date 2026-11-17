/**
 * @file nvn_prog.c
 * @brief NVN2 programs: which compiled shader (GPU code address and control
 * section) each program holds, for the pipeline to find its translation by.
 */
#include "nvn_priv.h"

/** @brief Most shader stages a program holds. */
#define PROGRAM_STAGES 6u

/** @brief NVNshaderData as the guest lays it out: the GPU address of the code and a pointer to the control section. */
typedef struct shader_data {
    uint64_t data;
    uint64_t control;
} shader_data;

typedef struct nvn_program {
    nvn_obj o;
    shader_data stages[PROGRAM_STAGES];
    uint32_t n;
    uint64_t sets;
} nvn_program;

/** @brief nvnProgramInitialize(NVNprogram *, NVNdevice *): NVNboolean. */
uint64_t nvnh_nvnProgramInitialize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = nvn_obj_new(c->x[0], NVN_PROGRAM, sizeof(nvn_program)) != NULL;
    return nvn_ret(c);
}

/** @brief nvnProgramFinalize(NVNprogram *). */
uint64_t nvnh_nvnProgramFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_PROGRAM));
    return nvn_ret(c);
}

/** @brief nvnProgramSetShaders(NVNprogram *, int count, const NVNshaderData *): NVNboolean. */
uint64_t nvnh_nvnProgramSetShaders(rl_ctx *c, uint8_t *mem)
{
    nvn_program *p = (nvn_program *)nvn_obj_get(c->x[0], NVN_PROGRAM);
    int32_t n = (int32_t)c->x[1];
    if (!p || n < 0 || (uint32_t)n > PROGRAM_STAGES)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnProgramSetShaders(0x%llx, %d): not a program or too many stages",
                                        (unsigned long long)c->x[0], n);
    memcpy(p->stages, mem + c->x[2], (size_t)n * sizeof(shader_data));
    p->n = (uint32_t)n;
    if (p->sets++ == 0 && rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "nvnProgramSetShaders(0x%llx): %d stages, first code 0x%llx control 0x%llx",
               (unsigned long long)c->x[0], n, (unsigned long long)p->stages[0].data,
               (unsigned long long)p->stages[0].control);
    c->x[0] = 1;
    return nvn_ret(c);
}

/** @brief nvnProgramSetDebugLabel(NVNprogram *, const char *). */
uint64_t nvnh_nvnProgramSetDebugLabel(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return nvn_ret(c);
}

/** @brief nvnProgramSetSampleShading(NVNprogram *, NVNsampleShadingMode). */
uint64_t nvnh_nvnProgramSetSampleShading(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return nvn_ret(c);
}
