/**
 * @file nvn_prog.c
 * @brief NVN2 programs: which compiled shader (GPU code address and control
 * section) each program holds, for the pipeline to find its translation by.
 *
 * A GLSLC control section (magic 0x98761234) gives the bytes of constant
 * data the compiler placed in the code area (+0x3c) and their offset from
 * the code's start (+0x40); the driver binds them as the stage's constant
 * bank c[1]; code addresses are kept as guest addresses.
 */
#include "nvn_priv.h"


/** @brief NVNshaderData as the guest lays it out: the GPU address of the code and a pointer to the control section. */
typedef struct shader_data {
    uint64_t data;
    uint64_t control;
} shader_data;

#define CONTROL_MAGIC 0x98761234u

typedef struct nvn_program {
    nvn_obj o;
    shader_data stages[NVN_PROGRAM_STAGES];
    uint64_t code[NVN_PROGRAM_STAGES];
    nvn_consts consts[NVN_PROGRAM_STAGES];
    /** @brief Guest address of each stage's GLSLC control section, 0 for none. */
    uint64_t control[NVN_PROGRAM_STAGES];
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
    if (!p || n < 0 || (uint32_t)n > NVN_PROGRAM_STAGES)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnProgramSetShaders(0x%llx, %d): not a program or too many stages",
                                        (unsigned long long)c->x[0], n);
    memcpy(p->stages, mem + c->x[2], (size_t)n * sizeof(shader_data));
    p->n = (uint32_t)n;
    for (uint32_t i = 0; i < p->n; i++) {
        char key[65];
        uint64_t code = nvn_gpu_guest(p->stages[i].data);
        p->code[i] = code;
        p->consts[i] = (nvn_consts){ 0, 0 };
        uint64_t ctl = p->stages[i].control;
        p->control[i] = ctl && rl_ld32(mem, ctl) == CONTROL_MAGIC ? ctl : 0;
        if (p->control[i] && rl_ld32(mem, ctl + 0x3c))
            p->consts[i] = (nvn_consts){ code + rl_ld32(mem, ctl + 0x40), rl_ld32(mem, ctl + 0x3c) };
        if (!nvn_shader_note(mem, code, key))
            return (uint64_t)rl_guest_fatal(c, "nvn", "nvnProgramSetShaders(0x%llx): stage %u at 0x%llx has no end",
                                            (unsigned long long)c->x[0], i, (unsigned long long)p->stages[i].data);
        if (rl_cfg()->trace_os)
            rl_log(RL_LOG_INFO, "nvn", "program 0x%llx stage %u: %s", (unsigned long long)c->x[0], i, key);
    }
    if (p->sets++ == 0 && rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "nvnProgramSetShaders(0x%llx): %d stages, first code 0x%llx control 0x%llx",
               (unsigned long long)c->x[0], n, (unsigned long long)p->stages[0].data,
               (unsigned long long)p->stages[0].control);
    c->x[0] = 1;
    return nvn_ret(c);
}

const uint64_t *nvn_program_code(uint64_t prog, uint32_t *n, const nvn_consts **consts, const uint64_t **control)
{
    nvn_program *p = (nvn_program *)nvn_obj_get(prog, NVN_PROGRAM);
    *n = p ? p->n : 0;
    if (consts)
        *consts = p ? p->consts : NULL;
    if (control)
        *control = p ? p->control : NULL;
    return p ? p->code : NULL;
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
