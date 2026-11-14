/**
 * @file srv_mm.c
 * @brief The multimedia clock request service mm:u. Codecs ask for a
 * minimum clock rate per module; the runtime records the requests and
 * reports the highest active one as the actual rate.
 *
 * Command ids and argument layouts follow Ryujinx (MIT) Horizon MmNv
 * Request: 0-3 by module (old interface), 4-7 by request id.
 */
#include "services.h"

#include <stdlib.h>

#define MM_MAX_REQUESTS 32

typedef struct mm_request {
    uint32_t id;
    uint32_t module;
    uint32_t rate;
    bool used;
} mm_request;

typedef struct mm_state {
    mm_request req[MM_MAX_REQUESTS];
    uint32_t next_id;
} mm_state;

#define ST(r) ((mm_state *)(r)->self->state)

static mm_request *by_id(mm_state *s, uint32_t id)
{
    for (uint32_t i = 0; i < MM_MAX_REQUESTS; i++)
        if (s->req[i].used && s->req[i].id == id)
            return &s->req[i];
    return NULL;
}

static mm_request *by_module(mm_state *s, uint32_t module)
{
    for (uint32_t i = 0; i < MM_MAX_REQUESTS; i++)
        if (s->req[i].used && s->req[i].module == module)
            return &s->req[i];
    return NULL;
}

static mm_request *add(mm_state *s, uint32_t module)
{
    for (uint32_t i = 0; i < MM_MAX_REQUESTS; i++)
        if (!s->req[i].used) {
            s->req[i] = (mm_request){ s->next_id++, module, 0, true };
            return &s->req[i];
        }
    return NULL;
}

static uint32_t rate_of(mm_state *s, uint32_t module)
{
    uint32_t rate = 0;
    for (uint32_t i = 0; i < MM_MAX_REQUESTS; i++)
        if (s->req[i].used && s->req[i].module == module && s->req[i].rate > rate)
            rate = s->req[i].rate;
    return rate;
}

static void set_rate(mm_request *q, uint32_t rate) { q->rate = (int32_t)rate <= 0 ? 0 : rate; }

static rl_result initialize_old(rl_ipc_req *r)
{
    uint32_t in[3];
    if (!rl_srv_in(r, in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    return add(ST(r), in[0]) ? KR_SUCCESS : KR_OUT_OF_RESOURCE;
}

static rl_result finalize_old(rl_ipc_req *r)
{
    uint32_t module;
    if (!rl_srv_in(r, &module, sizeof module))
        return SF_RESULT_INVALID_HEADER;
    mm_request *q = by_module(ST(r), module);
    if (q)
        q->used = false;
    return KR_SUCCESS;
}

static rl_result set_and_wait_old(rl_ipc_req *r)
{
    uint32_t in[3];
    if (!rl_srv_in(r, in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    mm_request *q = by_module(ST(r), in[0]);
    if (!q)
        q = add(ST(r), in[0]);
    if (!q)
        return KR_OUT_OF_RESOURCE;
    set_rate(q, in[1]);
    return KR_SUCCESS;
}

static rl_result get_old(rl_ipc_req *r)
{
    uint32_t module;
    if (!rl_srv_in(r, &module, sizeof module))
        return SF_RESULT_INVALID_HEADER;
    *(uint32_t *)rl_ipc_out(r, 4) = rate_of(ST(r), module);
    return KR_SUCCESS;
}

static rl_result initialize(rl_ipc_req *r)
{
    uint32_t in[3];
    if (!rl_srv_in(r, in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    mm_request *q = add(ST(r), in[0]);
    if (!q)
        return KR_OUT_OF_RESOURCE;
    *(uint32_t *)rl_ipc_out(r, 4) = q->id;
    return KR_SUCCESS;
}

static rl_result finalize(rl_ipc_req *r)
{
    uint32_t id;
    if (!rl_srv_in(r, &id, sizeof id))
        return SF_RESULT_INVALID_HEADER;
    mm_request *q = by_id(ST(r), id);
    if (q)
        q->used = false;
    return KR_SUCCESS;
}

static rl_result set_and_wait(rl_ipc_req *r)
{
    uint32_t in[3];
    if (!rl_srv_in(r, in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    mm_request *q = by_id(ST(r), in[0]);
    if (q)
        set_rate(q, in[1]);
    return KR_SUCCESS;
}

static rl_result get(rl_ipc_req *r)
{
    uint32_t id;
    if (!rl_srv_in(r, &id, sizeof id))
        return SF_RESULT_INVALID_HEADER;
    mm_request *q = by_id(ST(r), id);
    *(uint32_t *)rl_ipc_out(r, 4) = q ? rate_of(ST(r), q->module) : 0;
    return KR_SUCCESS;
}

static const rl_ipc_cmd mm_cmds[] = {
    { 0, "InitializeOld", initialize_old }, { 1, "FinalizeOld", finalize_old }, { 2, "SetAndWaitOld", set_and_wait_old },
    { 3, "GetOld", get_old },               { 4, "Initialize", initialize },    { 5, "Finalize", finalize },
    { 6, "SetAndWait", set_and_wait },      { 7, "Get", get },
};

static const rl_ipc_class mm_class = { "nn::mmnv::IRequest", mm_cmds, sizeof mm_cmds / sizeof mm_cmds[0], 0, free };

static rl_ipc_object *mm_factory(void)
{
    mm_state *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->next_id = 1;
    rl_ipc_object *o = rl_srv_object(&mm_class, s);
    if (!o)
        free(s);
    return o;
}

void rl_srv_install_mm(void) { rl_ipc_register_service("mm:u", mm_factory); }
