/**
 * @file nvn_module.c
 * @brief Shader modules of translated programs (see nvn_module.h).
 */
#include "nvn_module.h"
#include "nvn_pipe.h"
#include "spirvval.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/** @brief What patch_spirv learns of one id: its decorations and, plus one, its storage class. */
typedef struct spv_var {
    uint32_t set, binding, location, storage, builtin;
    bool has_set, has_binding, has_location, has_builtin;
} spv_var;

bool nvn_spirv_stores(const uint32_t *w, size_t n)
{
    if (n < 5 || w[0] != 0x07230203)
        return false;
    uint32_t bound = w[3];
    uint32_t *cls = calloc(bound, sizeof *cls), *type = calloc(bound, sizeof *type), *root = calloc(bound, sizeof *root);
    uint8_t *set3 = calloc(bound, 1), *bind2 = calloc(bound, 1);
    bool stores = !cls || !type || !root || !set3 || !bind2;
    for (size_t i = 5; !stores && i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n)
            break;
        if (op == 32 && len >= 4 && w[i + 1] < bound) {
            cls[w[i + 1]] = w[i + 2] + 1u;
        } else if (op == 71 && len >= 4 && w[i + 1] < bound) {
            if (w[i + 2] == 34 && w[i + 3] == NVN_MESH_BUFFER_SET)
                set3[w[i + 1]] = 1;
            else if (w[i + 2] == 33 && w[i + 3] == NVN_MESH_BUFFER_BINDING)
                bind2[w[i + 1]] = 1;
        } else if (op >= 54 && (op < 71 || op > 75) && op != 332 && op != 5632 && op != 5633 && len >= 3 &&
                   w[i + 1] < bound && w[i + 2] < bound && cls[w[i + 1]]) {
            uint32_t id = w[i + 2];
            type[id] = w[i + 1];
            root[id] = (op == 65 || op == 66 || op == 67 || op == 70 || op == 83 || op == 124) && len >= 4 &&
                               w[i + 3] < bound && root[w[i + 3]]
                           ? root[w[i + 3]]
                           : id;
        }
        uint32_t ptr = 0;
        if ((op == 62 || op == 63 || op == 64 || op == 228) && len >= 2)
            ptr = w[i + 1];
        else if (((op >= 229 && op <= 242) || op == 5614 || op == 5615 || op == 6035) && len >= 4)
            ptr = w[i + 3];
        if (ptr && ptr < bound && type[ptr] && type[ptr] < bound) {
            uint32_t c = cls[type[ptr]] - 1u, r = root[ptr];
            bool mesh = r && r < bound && set3[r] && bind2[r];
            stores = (c == 5349 || c == 12 || c == 2) && !mesh;
        }
        i += len;
    }
    free(cls);
    free(type);
    free(root);
    free(set3);
    free(bind2);
    return stores;
}

/** @brief Reads which banks, inputs and sets words (n of them) uses, and moves its bindings for stage. */
static bool patch_spirv(uint32_t *w, size_t n, int stage, nvn_module *m)
{
    if (n < 5 || w[0] != 0x07230203)
        return false;
    uint32_t bound = w[3];
    spv_var *v = calloc(bound, sizeof *v);
    if (!v)
        return false;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n)
            break;
        if (op == 71 && len >= 4 && w[i + 1] < bound) {
            spv_var *x = &v[w[i + 1]];
            if (w[i + 2] == 34)
                x->set = w[i + 3], x->has_set = true;
            else if (w[i + 2] == 33)
                x->binding = w[i + 3], x->has_binding = true;
            else if (w[i + 2] == 30)
                x->location = w[i + 3], x->has_location = true;
            else if (w[i + 2] == 11)
                x->builtin = w[i + 3], x->has_builtin = true;
            else if (w[i + 2] == 1 && w[i + 3] == ALPHA_FUNC_SPEC)
                m->alpha_test = true;
        } else if (op == 59 && len >= 4 && w[i + 2] < bound) {
            v[w[i + 2]].storage = w[i + 3] + 1u;
            m->fixed_push |= w[i + 3] == 9;
        }
        i += len;
    }
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n)
            break;
        if (op == 71 && len >= 4 && w[i + 2] == 33 && w[i + 1] < bound) {
            spv_var *x = &v[w[i + 1]];
            if (x->set == 0) {
                m->user |= 1u << (x->binding & 31);
                w[i + 3] = USER_BINDINGS * (uint32_t)stage + x->binding;
            } else if (x->set == 1 && x->binding == NVN_SLOT_BINDING_BASE && stage != (int)NVN_STAGE_COMPUTE) {
                m->slot_array = true;
                w[i + 3] = nvn_slot_binding((uint32_t)stage);
            } else if (x->set == 1 && stage == (int)NVN_STAGE_COMPUTE && x->binding == NVN_COMPUTE_BASE_TABLE_BINDING) {
                m->cbase_table = true;
            } else if (x->set == 1 && stage == (int)NVN_STAGE_COMPUTE && x->binding >= NVN_COMPUTE_BASE_BINDING &&
                       x->binding < NVN_COMPUTE_BASE_BINDING + NVN_COMPUTE_BASE_BYTES / 8) {
                uint16_t off = (uint16_t)(8 * (x->binding - NVN_COMPUTE_BASE_BINDING));
                uint32_t j = 0;
                while (j < m->ncbase && m->cbase[j] != off)
                    j++;
                if (j == m->ncbase && j < NVN_STORAGE_SLOTS)
                    m->cbase[m->ncbase++] = off;
                if (j < NVN_STORAGE_SLOTS)
                    w[i + 3] = NVN_SLOT_BINDING_BASE + j;
                else
                    m->other_sets = true;
            } else if (x->set == 1) {
                m->driver |= 1u << (x->binding & 31);
                w[i + 3] = DRIVER_BINDINGS * (uint32_t)stage + x->binding;
            } else if (x->set == 2) {
                if (x->binding < 32)
                    m->texture_bindings |= 1u << x->binding;
                else
                    m->images = true;
            } else if (x->set == NVN_POOL_SET && (x->binding == NVN_POOL_ARRAY_BINDING || x->binding == NVN_POOL_TABLE_BINDING)) {
                /* without the pool table the layouts have no set for it */
                m->pools = true;
                if (!nvn_pools_layout())
                    m->other_sets = true;
            } else if (x->set == NVN_MESH_BUFFER_SET && x->binding == NVN_MESH_BUFFER_BINDING) {
                m->mesh_buffer = true;
            } else if (x->set == NVN_TRACE_SET && x->binding == NVN_TRACE_BINDING &&
                       (stage == (int)NVN_STAGE_COMPUTE || stage == 0 || stage == 1)) {
                /* the trace buffer: its size comes with the interface records */
            } else {
                m->other_sets = true;
            }
        }
        i += len;
    }
    m->stores_guest = nvn_spirv_stores(w, n);
    for (uint32_t id = 0; id < bound; id++) {
        if (v[id].storage == 2 && v[id].has_location && v[id].location < 32)
            m->inputs |= 1u << v[id].location;
        if (v[id].storage == 4 && v[id].has_location && v[id].location < 32)
            m->outputs |= 1u << v[id].location;
        if (v[id].storage == 4 && v[id].has_builtin) {
            m->writes_layer |= v[id].builtin == 9;
            m->writes_viewport |= v[id].builtin == 10;
        }
    }
    free(v);
    return true;
}

uint32_t *nvn_spirv_demote_inputs(const uint32_t *w, size_t n, uint32_t mask, size_t *out_n)
{
    uint32_t bound = w[3];
    uint32_t *loc = calloc(bound, sizeof *loc), *pointee = calloc(bound, sizeof *pointee);
    uint32_t *tcomp = calloc(bound, sizeof *tcomp), *tcount = calloc(bound, sizeof *tcount);
    uint8_t *tfloat = calloc(bound, 1);
    uint8_t *hasloc = calloc(bound, 1), *demote = calloc(bound, 1);
    uint8_t *chained = calloc(bound, 1), *privtype = calloc(bound, 1);
    uint32_t *privof = calloc(bound, sizeof *privof);
    uint32_t *o = malloc((n + 16 * 32 + 16 + 4 * (size_t)bound) * sizeof *o);
    if (!loc || !pointee || !tcomp || !tcount || !tfloat || !hasloc || !demote || !chained || !privtype || !privof ||
        !o) {
        free(chained);
        free(privtype);
        free(privof);
        free(tcomp);
        free(tcount);
        free(tfloat);
        free(loc);
        free(pointee);
        free(hasloc);
        free(demote);
        free(o);
        return NULL;
    }
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n)
            break;
        if (op == 71 && len >= 4 && w[i + 2] == 30 && w[i + 1] < bound)
            loc[w[i + 1]] = w[i + 3], hasloc[w[i + 1]] = 1;
        i += len;
    }
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n)
            break;
        if (op == 59 && len >= 4 && w[i + 3] == 1 && w[i + 2] < bound && hasloc[w[i + 2]] && loc[w[i + 2]] < 32 &&
            (mask >> loc[w[i + 2]] & 1))
            demote[w[i + 2]] = 1;
        if ((op == 65 || op == 66) && len >= 4 && w[i + 1] < bound && w[i + 2] < bound && w[i + 3] < bound &&
            (demote[w[i + 3]] || chained[w[i + 3]]))
            chained[w[i + 2]] = 1, privtype[w[i + 1]] = 1;
        i += len;
    }
    memcpy(o, w, 5 * sizeof *o);
    size_t at = 5;
    uint32_t next = bound;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n)
            break;
        if (op == 32 && len >= 4 && w[i + 1] < bound)
            pointee[w[i + 1]] = w[i + 3];
        if (op == 22 && len >= 3 && w[i + 1] < bound && w[i + 2] == 32)
            tfloat[w[i + 1]] = 1;
        if (op == 23 && len >= 4 && w[i + 1] < bound && w[i + 2] < bound && tfloat[w[i + 2]])
            tcomp[w[i + 1]] = w[i + 2], tcount[w[i + 1]] = w[i + 3];
        if (op == 71 && len >= 3 && w[i + 1] < bound && demote[w[i + 1]]) {
        } else if (op == 32 && len >= 4 && w[i + 1] < bound && privtype[w[i + 1]]) {
            memcpy(o + at, w + i, len * sizeof *o);
            at += len;
            privof[w[i + 1]] = next++;
            o[at++] = 4u << 16 | 32;
            o[at++] = privof[w[i + 1]];
            o[at++] = 6;
            o[at++] = w[i + 3];
        } else if ((op == 65 || op == 66) && len >= 4 && w[i + 2] < bound && chained[w[i + 2]] && w[i + 1] < bound &&
                   privof[w[i + 1]]) {
            memcpy(o + at, w + i, len * sizeof *o);
            o[at + 1] = privof[w[i + 1]];
            at += len;
        } else if (op == 59 && len >= 4 && w[i + 2] < bound && demote[w[i + 2]]) {
            uint32_t ptr = next++, init = next++, ty = pointee[w[i + 1]];
            o[at++] = 4u << 16 | 32;
            o[at++] = ptr;
            o[at++] = 6;
            o[at++] = ty;
            if (ty < bound && tfloat[ty]) {
                o[at++] = 4u << 16 | 43;
                o[at++] = ty;
                o[at++] = init;
                o[at++] = UNWRITTEN_VARYING[0];
            } else if (ty < bound && tcount[ty] >= 2 && tcount[ty] <= 4) {
                uint32_t comps[4], ct = tcomp[ty];
                for (uint32_t k = 0; k < tcount[ty]; k++) {
                    comps[k] = next++;
                    o[at++] = 4u << 16 | 43;
                    o[at++] = ct;
                    o[at++] = comps[k];
                    o[at++] = UNWRITTEN_VARYING[k];
                }
                o[at++] = (3u + tcount[ty]) << 16 | 44;
                o[at++] = ty;
                o[at++] = init;
                for (uint32_t k = 0; k < tcount[ty]; k++)
                    o[at++] = comps[k];
            } else {
                o[at++] = 3u << 16 | 46;
                o[at++] = ty;
                o[at++] = init;
            }
            o[at++] = 5u << 16 | 59;
            o[at++] = ptr;
            o[at++] = w[i + 2];
            o[at++] = 6;
            o[at++] = init;
        } else {
            memcpy(o + at, w + i, len * sizeof *o);
            at += len;
        }
        i += len;
    }
    o[3] = next;
    *out_n = at;
    free(tcomp);
    free(tcount);
    free(tfloat);
    free(loc);
    free(pointee);
    free(hasloc);
    free(demote);
    free(chained);
    free(privtype);
    free(privof);
    return o;
}

/**
 * @brief The fragment module of frag to draw with after vertex program vert: frag itself when the vertex program writes
 * every location frag reads, else a variant of frag with the unwritten inputs demoted (nvn_spirv_demote_inputs). 0 when it
 * cannot be made.
 */
const nvn_module *nvn_module_last_stage(nvn_module *const mods[5])
{
    return mods[2] ? mods[2] : mods[4] ? mods[4] : mods[0];
}

VkShaderModule nvn_module_fragment_for(const nvn_module *vert, nvn_module *frag)
{
    uint32_t missing = frag->inputs & ~vert->outputs;
    if (!missing)
        return frag->m;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < frag->nvariants; i++)
        if (frag->variants[i].mask == missing) {
            VkShaderModule have = frag->variants[i].m;
            pthread_mutex_unlock(&g_lock);
            return have;
        }
    bool full = frag->nvariants == 8;
    pthread_mutex_unlock(&g_lock);
    size_t n, dn;
    uint32_t *w = full ? NULL : nvn_shader_spirv(frag->key, &n);
    if (!w)
        return NULL;
    nvn_module scratch;
    memset(&scratch, 0, sizeof scratch);
    uint32_t *dw = patch_spirv(w, n, frag->stage, &scratch) ? nvn_spirv_demote_inputs(w, n, missing, &dn) : NULL;
    free(w);
    if (!dw)
        return NULL;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = dn * 4,
                                    .pCode = dw };
    VkShaderModule m = NULL;
    rl_spirv_check(dw, dn, "a fragment program with demoted inputs");
    if (VK(vkCreateShaderModule)(g.dev, &ci, NULL, &m) != VK_SUCCESS)
        m = NULL;
    free(dw);
    if (m) {
        pthread_mutex_lock(&g_lock);
        frag->variants[frag->nvariants].mask = missing;
        frag->variants[frag->nvariants++].m = m;
        pthread_mutex_unlock(&g_lock);
    }
    return m;
}


bool nvn_spirv_float_inputs(const uint32_t *w, size_t n)
{
    if (n < 5)
        return false;
    uint32_t bound = w[3];
    uint8_t *f32 = calloc(bound, 1), *vec = calloc(bound, 1), *isloc = calloc(bound, 1);
    uint32_t *pointee = calloc(bound, sizeof *pointee);
    bool ok = f32 && vec && isloc && pointee;
    for (size_t i = 5; ok && i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n)
            break;
        if (op == 71 && len >= 4 && w[i + 2] == 30 && w[i + 1] < bound)
            isloc[w[i + 1]] = 1;
        else if (op == 22 && len >= 3 && w[i + 1] < bound && w[i + 2] == 32)
            f32[w[i + 1]] = 1;
        else if (op == 23 && len >= 4 && w[i + 1] < bound && w[i + 2] < bound && f32[w[i + 2]] && w[i + 3] <= 4)
            vec[w[i + 1]] = 1;
        else if (op == 32 && len >= 4 && w[i + 1] < bound)
            pointee[w[i + 1]] = w[i + 3];
        else if (op == 59 && len >= 4 && w[i + 3] == 1 && w[i + 2] < bound && isloc[w[i + 2]] && w[i + 1] < bound) {
            uint32_t t = pointee[w[i + 1]];
            ok = t < bound && (f32[t] || vec[t]);
        }
        i += len;
    }
    free(f32);
    free(vec);
    free(isloc);
    free(pointee);
    return ok;
}

uint32_t *nvn_spirv_add_outputs(const uint32_t *w, size_t n, uint32_t mask, size_t *out_n)
{
    if (n < 5 || !mask)
        return NULL;
    uint32_t f = 0, v4 = 0, nloc = 0;
    size_t first_type = 0, first_fn = 0, entry = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n)
            return NULL;
        if (op == 15 && !entry)
            entry = i;
        if (!first_type && ((op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == 59))
            first_type = i;
        if (op == 54 && !first_fn)
            first_fn = i;
        if (op == 22 && len >= 3 && w[i + 2] == 32)
            f = w[i + 1];
        i += len;
    }
    for (size_t i = 5; f && i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == 23 && len >= 4 && w[i + 2] == f && w[i + 3] == 4)
            v4 = w[i + 1];
        i += len;
    }
    if (!entry || !first_type || !first_fn)
        return NULL;
    uint32_t locs[32];
    for (uint32_t l = 0; l < 32; l++)
        if (mask >> l & 1)
            locs[nloc++] = l;
    uint32_t *o = malloc((n + 32 + 8 * (size_t)nloc) * sizeof *o);
    if (!o)
        return NULL;
    uint32_t next = w[3];
    uint32_t nf = f ? 0 : next++, nv4 = v4 ? 0 : next++;
    f = f ? f : nf;
    v4 = v4 ? v4 : nv4;
    uint32_t ptr = next++, comps[4], init, var0;
    for (uint32_t k = 0; k < 4; k++)
        comps[k] = next++;
    init = next++;
    var0 = next;
    next += nloc;
    memcpy(o, w, 5 * sizeof *o);
    size_t at = 5;
    for (size_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16;
        if (i == first_type)
            for (uint32_t k = 0; k < nloc; k++) {
                o[at++] = 4u << 16 | 71;
                o[at++] = var0 + k;
                o[at++] = 30;
                o[at++] = locs[k];
            }
        if (i == first_fn) {
            if (nf) {
                o[at++] = 3u << 16 | 22;
                o[at++] = nf;
                o[at++] = 32;
            }
            if (nv4) {
                o[at++] = 4u << 16 | 23;
                o[at++] = nv4;
                o[at++] = f;
                o[at++] = 4;
            }
            o[at++] = 4u << 16 | 32;
            o[at++] = ptr;
            o[at++] = 3;
            o[at++] = v4;
            for (uint32_t k = 0; k < 4; k++) {
                o[at++] = 4u << 16 | 43;
                o[at++] = f;
                o[at++] = comps[k];
                o[at++] = UNWRITTEN_VARYING[k];
            }
            o[at++] = 7u << 16 | 44;
            o[at++] = v4;
            o[at++] = init;
            for (uint32_t k = 0; k < 4; k++)
                o[at++] = comps[k];
            for (uint32_t k = 0; k < nloc; k++) {
                o[at++] = 5u << 16 | 59;
                o[at++] = ptr;
                o[at++] = var0 + k;
                o[at++] = 3;
                o[at++] = init;
            }
        }
        memcpy(o + at, w + i, len * sizeof *o);
        if (i == entry) {
            o[at] = (len + nloc) << 16 | 15;
            for (uint32_t k = 0; k < nloc; k++)
                o[at + len + k] = var0 + k;
            at += nloc;
        }
        at += len;
        i += len;
    }
    o[3] = next;
    *out_n = at;
    return o;
}

/** @brief Whether every input of frag is a 32-bit float scalar or vector, which a vertex variant's added outputs can feed. */
static bool float_inputs(nvn_module *frag)
{
    pthread_mutex_lock(&g_lock);
    uint8_t known = frag->float_inputs;
    pthread_mutex_unlock(&g_lock);
    if (!known) {
        size_t n;
        uint32_t *w = nvn_shader_spirv(frag->key, &n);
        known = w && nvn_spirv_float_inputs(w, n) ? 2 : 1;
        free(w);
        pthread_mutex_lock(&g_lock);
        frag->float_inputs = known;
        pthread_mutex_unlock(&g_lock);
    }
    return known == 2;
}

VkShaderModule nvn_module_vertex_for(nvn_module *vert, nvn_module *frag)
{
    uint32_t missing = frag->inputs & ~vert->outputs;
    if (!missing)
        return vert->m;
    if (vert->stage != (int)NVN_STAGE_VERTEX || !float_inputs(frag))
        return NULL;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < vert->nvariants; i++)
        if (vert->variants[i].mask == missing) {
            VkShaderModule have = vert->variants[i].m;
            pthread_mutex_unlock(&g_lock);
            return have;
        }
    bool full = vert->nvariants == 8;
    pthread_mutex_unlock(&g_lock);
    size_t n, dn;
    uint32_t *w = full ? NULL : nvn_shader_spirv(vert->key, &n);
    if (!w)
        return NULL;
    nvn_module scratch;
    memset(&scratch, 0, sizeof scratch);
    uint32_t *dw = patch_spirv(w, n, vert->stage, &scratch) ? nvn_spirv_add_outputs(w, n, missing, &dn) : NULL;
    free(w);
    if (!dw)
        return NULL;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = dn * 4,
                                    .pCode = dw };
    VkShaderModule m = NULL;
    rl_spirv_check(dw, dn, "a vertex program with outputs added for its fragment program");
    if (VK(vkCreateShaderModule)(g.dev, &ci, NULL, &m) != VK_SUCCESS)
        m = NULL;
    free(dw);
    if (m) {
        pthread_mutex_lock(&g_lock);
        if (vert->nvariants < 8) {
            vert->variants[vert->nvariants].mask = missing;
            vert->variants[vert->nvariants++].m = m;
        }
        pthread_mutex_unlock(&g_lock);
    }
    return m;
}

bool nvn_module_link(nvn_module *const mods[5], VkShaderModule out[5])
{
    if (!mods[1])
        return true;
    if (mods[0] && !mods[2] && !mods[3] && !mods[4]) {
        VkShaderModule v = nvn_module_vertex_for(mods[0], mods[1]);
        if (v) {
            out[0] = v;
            out[1] = mods[1]->m;
            return true;
        }
    }
    return (out[1] = nvn_module_fragment_for(nvn_module_last_stage(mods), mods[1])) != NULL;
}

bool nvn_spirv_stage(const uint32_t *w, size_t n, int *stage)
{
    static const int map[] = { 0, 3, 4, 2, 1, 5 };
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n)
            break;
        if (op == 15 && len >= 2 && (w[i + 1] < sizeof map / sizeof map[0] || w[i + 1] == 5364 || w[i + 1] == 5365)) {
            *stage = w[i + 1] == 5364 ? (int)NVN_STAGE_TASK : w[i + 1] == 5365 ? (int)NVN_STAGE_MESH : map[w[i + 1]];
            return true;
        }
        i += len;
    }
    return false;
}

/** @brief The modules made so far, in an open-addressed table by key. */
static struct {
    nvn_module **slot;
    uint32_t cap, n;
} g_mods;

static uint32_t key_hash(const char *key)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 64; i++)
        h = (h ^ (uint8_t)key[i]) * 16777619u;
    return h;
}

/** @brief The module with key made for stage (any stage when it is negative) in the table, or NULL; g_lock held. */
static nvn_module *find_locked(const char *key, int stage)
{
    if (!g_mods.cap)
        return NULL;
    for (uint32_t i = key_hash(key) & (g_mods.cap - 1);; i = (i + 1) & (g_mods.cap - 1)) {
        if (!g_mods.slot[i])
            return NULL;
        if (!memcmp(g_mods.slot[i]->key, key, 64) && (stage < 0 || g_mods.slot[i]->stage == stage))
            return g_mods.slot[i];
    }
}

/** @brief Puts m in the table, growing it; false when memory runs out; g_lock held. */
static bool insert_locked(nvn_module *m)
{
    if ((g_mods.n + 1) * 2 > g_mods.cap) {
        uint32_t cap = g_mods.cap ? g_mods.cap * 2 : 1024;
        nvn_module **slot = calloc(cap, sizeof *slot);
        if (!slot)
            return false;
        for (uint32_t i = 0; i < g_mods.cap; i++)
            if (g_mods.slot[i]) {
                uint32_t j = key_hash(g_mods.slot[i]->key) & (cap - 1);
                while (slot[j])
                    j = (j + 1) & (cap - 1);
                slot[j] = g_mods.slot[i];
            }
        free(g_mods.slot);
        g_mods.slot = slot;
        g_mods.cap = cap;
    }
    uint32_t j = key_hash(m->key) & (g_mods.cap - 1);
    while (g_mods.slot[j])
        j = (j + 1) & (g_mods.cap - 1);
    g_mods.slot[j] = m;
    g_mods.n++;
    return true;
}

static void why_set(char *why, size_t n, const char *fmt, ...) RL_PRINTF(3, 4);

/** @brief Writes the printf-style reason into why (n bytes) when there is room for one. */
static void why_set(char *why, size_t n, const char *fmt, ...)
{
    if (!why || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

nvn_module *nvn_module_get(const char key[65], int stage, char *why, size_t why_n)
{
    pthread_mutex_lock(&g_lock);
    nvn_module *have = find_locked(key, stage);
    pthread_mutex_unlock(&g_lock);
    if (have)
        return have;
    char stamp_why[512];
    if (!nvn_shader_stamp_check(stamp_why, sizeof stamp_why)) {
        why_set(why, why_n, "%s", stamp_why);
        return NULL;
    }
    size_t n;
    uint32_t *w = nvn_shader_spirv(key, &n);
    if (!w) {
        char refused[600];
        if (nvn_shader_refusal(key, refused, sizeof refused))
            why_set(why, why_n, "the translator refused program %s for this device: %s", key, refused);
        else
            why_set(why, why_n, "program %s is not in the build's shader table", key);
        return NULL;
    }
    nvn_module *m = calloc(1, sizeof *m);
    if (!m) {
        free(w);
        why_set(why, why_n, "out of memory making module %s", key);
        return NULL;
    }
    memcpy(m->key, key, 65);
    if (stage < 0 && !nvn_spirv_stage(w, n, &stage)) {
        free(w);
        free(m);
        why_set(why, why_n, "translation %s has no entry point", key);
        return NULL;
    }
    m->stage = stage;
    m->nrecs = nvn_shader_iface(key, NULL, 0);
    m->recs = calloc(m->nrecs ? m->nrecs : 1, sizeof *m->recs);
    if (!m->recs) {
        free(w);
        free(m);
        why_set(why, why_n, "out of memory reading the interface of %s", key);
        return NULL;
    }
    nvn_shader_iface(key, m->recs, m->nrecs);
    memset(m->xfb_select, 0xff, sizeof m->xfb_select);
    for (uint32_t i = 0; i < m->nrecs; i++) {
        m->global_store |= m->recs[i].kind == NVN_IFACE_GLOBAL_STORE;
        m->marks_pages |= m->recs[i].kind == NVN_IFACE_GLOBAL_STORE && m->recs[i].bank == 1;
        m->register_handles |= m->recs[i].kind == NVN_IFACE_REGISTER_HANDLES;
        if (m->recs[i].kind == NVN_IFACE_TRACE)
            m->trace_words = m->recs[i].bank;
        if (m->recs[i].kind == NVN_IFACE_XFB_MEMORY) {
            m->xfb_record = m->recs[i].bank;
            m->xfb_slots = m->recs[i].offset;
        }
        if (m->recs[i].kind == NVN_IFACE_XFB_SELECT && m->recs[i].bank < 4 && m->recs[i].offset + 4 <= 512)
            memcpy(&m->xfb_select[m->recs[i].bank][m->recs[i].offset], &m->recs[i].spec, 4);
        if (m->recs[i].kind == NVN_IFACE_XFB && m->recs[i].bank < 4) {
            m->xfb_mask |= 1u << m->recs[i].bank;
            m->xfb_stride[m->recs[i].bank] = m->recs[i].offset;
        }
        if (m->recs[i].kind == NVN_IFACE_STORAGE_SLOT && m->recs[i].bank < NVN_STORAGE_SLOTS)
            m->storage_slots |= 1u << m->recs[i].bank;
        if (m->recs[i].kind == NVN_IFACE_IMAGE_BINDING && !nvn_texset_declares_image(m->recs[i].bank)) {
            uint32_t b = m->recs[i].bank;
            free(w);
            free(m->recs);
            free(m);
            why_set(why, why_n,
                    "translation %s uses storage image array %u, which the texture set does not declare (its layout "
                    "follows the arrays the shader table's programs use, and this program is not in that table)",
                    key, b);
            return NULL;
        }
    }
    if (!patch_spirv(w, n, stage, m)) {
        free(w);
        free(m->recs);
        free(m);
        why_set(why, why_n, "translation %s is not SPIR-V", key);
        return NULL;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = n * 4,
                                    .pCode = w };
    rl_spirv_check(w, n, key);
    VkResult vr = VK(vkCreateShaderModule)(g.dev, &ci, NULL, &m->m);
    free(w);
    if (vr != VK_SUCCESS) {
        free(m->recs);
        free(m);
        why_set(why, why_n, "vkCreateShaderModule failed for %s (%d)", key, (int)vr);
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    have = find_locked(key, stage);
    bool ok = have || insert_locked(m);
    pthread_mutex_unlock(&g_lock);
    if (have) {
        VK(vkDestroyShaderModule)(g.dev, m->m, NULL);
        free(m->recs);
        free(m);
        return have;
    }
    if (!ok) {
        VK(vkDestroyShaderModule)(g.dev, m->m, NULL);
        free(m->recs);
        free(m);
        why_set(why, why_n, "out of memory making module %s", key);
        return NULL;
    }
    return m;
}

void nvn_module_default_spec(const nvn_module *m, uint32_t *n, VkSpecializationMapEntry *map, uint32_t *vals)
{
    for (uint32_t i = 0; i < m->nrecs; i++) {
        const nvn_iface_rec *h = &m->recs[i];
        if (h->kind != NVN_IFACE_TEXTURE)
            continue;
        for (uint32_t j = 0; j < m->nrecs; j++) {
            const nvn_iface_rec *c = &m->recs[j];
            if ((c->kind != NVN_IFACE_TEXTURE_KIND && c->kind != NVN_IFACE_TEXTURE_DIM) || c->bank != h->bank ||
                c->offset != h->offset || *n == NVN_MAX_SPECS)
                continue;
            map[*n] = (VkSpecializationMapEntry){ c->spec, 4 * *n, 4 };
            vals[(*n)++] = c->kind != NVN_IFACE_TEXTURE_KIND             ? 1u
                           : c->spec >= NVN_FETCH_KIND_SPEC && c->spec < NVN_FETCH_KIND_SPEC + 0x100u ? NVN_KIND_AT_RUN_TIME
                                                                                                       : 0u;
        }
    }
}
