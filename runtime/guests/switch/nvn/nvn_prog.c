/**
 * @file nvn_prog.c
 * @brief NVN programs: which compiled shader (GPU code address and control
 * section) each program holds, for the pipeline to find its translation by,
 * and the NVN1 program format.
 *
 * An NVN2 program is its 0x80-byte shader program header (graphics) and its
 * SM86 code. An NVN1 program starts with a 0x30-byte code header (the word
 * 0x12345678 and 44 zero bytes), then a 0x50-byte SPH of version 3
 * (graphics) and the Maxwell code in 32-byte bundles; its bytes for the key
 * are the SPH and the code through the last bundle control reaches, as the
 * build's scan of the game's data takes them (relift-switch-shaders).
 *
 * A GLSLC control section (magic 0x98761234) gives the bytes of constant
 * data the compiler placed in the code area (+0x3c) and their offset from
 * the code's start (+0x40); the driver binds them as the stage's constant
 * bank c[1]; code addresses are kept as guest addresses.
 */
#include "nvn_priv.h"

#include <stdlib.h>


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
    c->x[0] = nvn_obj_publish(nvn_obj_new(c->x[0], NVN_PROGRAM, sizeof(nvn_program))) != NULL;
    return nvn_ret(c);
}

/** @brief nvnProgramFinalize(NVNprogram *). */
uint64_t nvnh_nvnProgramFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_PROGRAM));
    return nvn_ret(c);
}

static uint64_t set_stages(rl_ctx *c, uint8_t *mem, nvn_program *p, uint64_t prog, int32_t n);

/** @brief Gives program prog the n stages at guest address shaders (NVNshaderData each); sets x0 to true. */
static uint64_t set_shaders(rl_ctx *c, uint8_t *mem, uint64_t prog, int32_t n, uint64_t shaders)
{
    nvn_program *p = (nvn_program *)nvn_obj_get(prog, NVN_PROGRAM);
    if (!p || n < 0 || (uint32_t)n > NVN_PROGRAM_STAGES)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnProgramSetShaders(0x%llx, %d): not a program or too many stages",
                                        (unsigned long long)prog, n);
    memcpy(p->stages, mem + shaders, (size_t)n * sizeof(shader_data));
    return set_stages(c, mem, p, prog, n);
}

/** @brief Notes the n stages already in p->stages; sets x0 to true. */
static uint64_t set_stages(rl_ctx *c, uint8_t *mem, nvn_program *p, uint64_t prog, int32_t n)
{
    c->x[0] = prog;
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
        nvn_warm_program(mem, code, p->control[i]);
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

/** @brief nvnProgramSetShaders(NVNprogram *, int count, const NVNshaderData *): NVNboolean. */
uint64_t nvnh_nvnProgramSetShaders(rl_ctx *c, uint8_t *mem)
{
    return set_shaders(c, mem, c->x[0], (int32_t)c->x[1], c->x[2]);
}

/**
 * @brief nvnProgramSetShadersExt(NVNprogram *, int count, const NVNshaderDataExt *):
 * NVNboolean. Each entry is NVNshaderData plus a pointer to 24 more bytes
 * the driver copies into its per-stage state; entries without them are
 * plain stages, and stages with them are not implemented and stop.
 */
uint64_t nvnh_nvnProgramSetShadersExt(rl_ctx *c, uint8_t *mem)
{
    uint64_t prog = c->x[0], ext = c->x[2];
    int32_t n = (int32_t)c->x[1];
    if (n < 0 || (uint32_t)n > NVN_PROGRAM_STAGES)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnProgramSetShadersExt(0x%llx, %d): too many stages",
                                        (unsigned long long)prog, n);
    nvn_program *p = (nvn_program *)nvn_obj_get(prog, NVN_PROGRAM);
    if (!p)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnProgramSetShadersExt(0x%llx): not a program",
                                        (unsigned long long)prog);
    shader_data plain[NVN_PROGRAM_STAGES];
    for (int32_t i = 0; i < n; i++) {
        uint64_t e = ext + 24 * (uint64_t)i, more = rl_ld64(mem, e + 16);
        if (more)
            return (uint64_t)rl_guest_fatal(c, "nvn",
                                            "nvnProgramSetShadersExt(0x%llx): stage %d has extended data "
                                            "%016llx %016llx %016llx (not implemented)",
                                            (unsigned long long)prog, i, (unsigned long long)rl_ld64(mem, more),
                                            (unsigned long long)rl_ld64(mem, more + 8),
                                            (unsigned long long)rl_ld64(mem, more + 16));
        plain[i] = (shader_data){ rl_ld64(mem, e), rl_ld64(mem, e + 8) };
    }
    memcpy(p->stages, plain, (size_t)n * sizeof *plain);
    return set_stages(c, mem, p, prog, n);
}

/**
 * @brief nvnProgramInitializeFromBuilder(NVNprogram *, const NVNprogramBuilder *):
 * NVNboolean. As the driver: an initialized program given the builder's
 * shaders. Ray tracing modules (asked of drivers that have their count
 * getter; older ones have none) and subroutine linkage are not implemented
 * and stop.
 */
uint64_t nvnh_nvnProgramInitializeFromBuilder(rl_ctx *c, uint8_t *mem)
{
    uint64_t prog = c->x[0], builder = c->x[1], count = 0, shaders = 0, modules = 0, links = 0;
    int rc = nvn_driver_call(c, mem, "nvnProgramBuilderGetShaderCount", &builder, 1, &count);
    if (rc == RL_OK)
        rc = nvn_driver_call(c, mem, "nvnProgramBuilderGetShaders", &builder, 1, &shaders);
    if (rc == RL_OK)
        rc = nvn_driver_call(c, mem, "nvnProgramBuilderGetSubroutineLinkageCount", &builder, 1, &links);
    uint64_t get_modules = 0;
    if (rc == RL_OK)
        rc = nvn_driver_lookup(c, mem, "nvnProgramBuilderGetRayTracingModuleCount", &get_modules);
    if (rc == RL_OK && get_modules)
        rc = nvn_guest_call(c, mem, get_modules, &builder, 1, &modules);
    if (rc != RL_OK)
        return (uint64_t)rc;
    if ((int32_t)modules || (int32_t)links)
        return (uint64_t)rl_guest_fatal(c, "nvn",
                                        "nvnProgramInitializeFromBuilder(0x%llx): %d ray tracing modules and %d "
                                        "subroutine linkages are not implemented",
                                        (unsigned long long)prog, (int32_t)modules, (int32_t)links);
    if (!nvn_obj_publish(nvn_obj_new(prog, NVN_PROGRAM, sizeof(nvn_program)))) {
        c->x[0] = 0;
        return nvn_ret(c);
    }
    return set_shaders(c, mem, prog, (int32_t)count, shaders);
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

/** @brief The first word of an NVN1 program's code header (its other 44 bytes are zero). */
#define V1_CODE_MAGIC 0x12345678u
#define V1_CODE_HEADER 0x30u
#define V1_SPH 0x50u
/** @brief Bytes of a Maxwell bundle: a scheduling word and three instructions. */
#define V1_BUNDLE 32u
/** @brief Most instructions the reachability walk visits (as the build's walk). */
#define V1_MAX_VISITS (1u << 20)
/** @brief Most bytes of Maxwell code the walk reads past the header. */
#define V1_MAX_CODE (4u << 20)

bool nvn_v1_sph(const uint8_t *b, int *stage)
{
    uint32_t w;
    memcpy(&w, b, 4);
    uint32_t kind = w & 0x1f, version = (w >> 5) & 0x1f, s = (w >> 10) & 0xf;
    if ((kind != 1 && kind != 2) || version != 3 || s < 1 || s > 5)
        return false;
    static const int map[] = { -1, 0, 3, 4, 2, 1 };
    if (stage)
        *stage = map[s];
    return true;
}

/** @brief Maxwell control-flow instructions, by their opcode bits (the instruction's top 12 bits; SYNC's top 13). */
enum v1_flow { V1_NONE, V1_BRANCH, V1_PUSH, V1_END, V1_INDIRECT, V1_INDIRECT_CALL };

static enum v1_flow v1_flow(uint64_t w, bool *bra_or_exit)
{
    *bra_or_exit = false;
    if (w >> 51 == 0x1e1f)
        return V1_END;
    switch (w >> 52) {
    case 0xe24:
        *bra_or_exit = true;
        return V1_BRANCH;
    case 0xe26:
    case 0xe29:
    case 0xe2a:
    case 0xe2b:
        return V1_PUSH;
    case 0xe30:
        *bra_or_exit = true;
        return V1_END;
    case 0xe32:
    case 0xe33:
    case 0xe34:
    case 0xe35:
        return V1_END;
    case 0xe20:
    case 0xe21:
    case 0xe25:
    case 0xe28:
    case 0xe31:
        return V1_INDIRECT;
    case 0xe22:
        return V1_INDIRECT_CALL;
    default:
        return V1_NONE;
    }
}

/** @brief Whether w runs on every thread: predicate PT, and condition code T for BRA and EXIT. */
static bool v1_always(uint64_t w, bool bra_or_exit) { return ((w >> 16) & 0xf) == 7 && (!bra_or_exit || (w & 0x1f) == 0xf); }

/**
 * @brief Bytes of the Maxwell code at c (its first bundle at c, n bytes
 * readable): through the last bundle control reaches from the first
 * instruction, following branches, pushes (SSY, PBK, PCNT, CAL) and
 * fall-through, stopping after an unconditional BRA, EXIT, BRK, CONT, SYNC,
 * RET, KIL or indirect jump. 0 when control leaves the bytes or a branch
 * target is not an instruction.
 */
static uint64_t v1_code_extent(const uint8_t *c, uint64_t n)
{
    uint64_t words = n / V1_BUNDLE * 3, end = 0;
    uint8_t *seen = calloc((size_t)(n / 8 + 1), 1);
    uint64_t *todo = malloc(sizeof *todo * 1024);
    uint32_t ntodo = 0, cap = 1024, visits = 0;
    if (!seen || !todo) {
        free(seen);
        free(todo);
        return 0;
    }
    todo[ntodo++] = 8;
    bool ok = true;
    while (ok && ntodo) {
        uint64_t at = todo[--ntodo];
        for (;;) {
            if (seen[at / 8])
                break;
            seen[at / 8] = 1;
            uint64_t k = at / V1_BUNDLE * 3 + (at % V1_BUNDLE) / 8 - 1;
            if (k >= words || ++visits > V1_MAX_VISITS) {
                ok = false;
                break;
            }
            uint64_t w;
            memcpy(&w, c + at, 8);
            if (at + 8 > end)
                end = at + 8;
            uint64_t next = (at + 8) % V1_BUNDLE ? at + 8 : at + 16;
            bool bra_or_exit;
            enum v1_flow f = v1_flow(w, &bra_or_exit);
            bool always = v1_always(w, bra_or_exit);
            if (f == V1_BRANCH || f == V1_PUSH) {
                int64_t imm = (int64_t)(((w >> 20) & 0xffffff) << 40) >> 40;
                int64_t t = (int64_t)at + 8 + imm;
                if (t < 0 || (t & 7)) {
                    ok = false;
                    break;
                }
                uint64_t tt = (uint64_t)t % V1_BUNDLE ? (uint64_t)t : (uint64_t)t + 8;
                if (ntodo == cap) {
                    uint64_t *more = realloc(todo, sizeof *todo * cap * 2);
                    if (!more) {
                        ok = false;
                        break;
                    }
                    todo = more;
                    cap *= 2;
                }
                todo[ntodo++] = tt;
                if (f == V1_BRANCH && always)
                    break;
            } else if ((f == V1_END || f == V1_INDIRECT) && always) {
                break;
            }
            at = next;
        }
    }
    free(seen);
    free(todo);
    return ok ? (end + V1_BUNDLE - 1) / V1_BUNDLE * V1_BUNDLE : 0;
}

bool nvn_v1_program(const uint8_t *mem, uint64_t code, uint64_t *start, uint64_t *len)
{
    uint32_t magic;
    memcpy(&magic, mem + code, 4);
    if (magic != V1_CODE_MAGIC)
        return false;
    uint64_t at = code + V1_CODE_HEADER;
    uint64_t sph = nvn_v1_sph(mem + at, NULL) ? V1_SPH : 0;
    uint64_t n = v1_code_extent(mem + at + sph, V1_MAX_CODE);
    if (!n)
        return false;
    *start = at;
    *len = sph + n;
    return true;
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
