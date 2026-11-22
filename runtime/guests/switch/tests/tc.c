/**
 * @file tc.c
 * @brief The test client for services. The message encoder follows the
 * layout of libnx (ISC, libnx Authors) sf/hipc.h and sf/cmif.h, written with
 * guest addresses.
 */
#include "tc.h"

static void put(uint32_t *w, uint32_t *pos, uint32_t v) { w[(*pos)++] = v; }

static void buf_desc(uint32_t *w, uint32_t *pos, uint64_t addr, uint64_t size)
{
    put(w, pos, (uint32_t)size);
    put(w, pos, (uint32_t)addr);
    put(w, pos, (uint32_t)(((addr >> 36) & 0x3FFFFF) << 2) | (uint32_t)(((size >> 32) & 0xF) << 24) |
                    (uint32_t)(((addr >> 32) & 0xF) << 28));
}

static uint32_t encode(const tc_req *q, uint32_t *w)
{
    uint32_t pos = 2;
    bool special = q->send_pid || q->ncopy;
    memset(w, 0, RL_IPC_MSG);
    if (special) {
        put(w, &pos, (q->send_pid ? 1u : 0u) | (q->ncopy << 1));
        if (q->send_pid) {
            put(w, &pos, 0);
            put(w, &pos, 0);
        }
        for (uint32_t i = 0; i < q->ncopy; i++)
            put(w, &pos, q->copy[i]);
    }
    for (uint32_t i = 0; i < q->nx; i++) {
        uint64_t a = q->x_addr[i];
        put(w, &pos, (i & 0x3F) | (uint32_t)(((a >> 36) & 0x3F) << 6) | (uint32_t)(((a >> 32) & 0xF) << 12) |
                         (uint32_t)(q->x_size[i] << 16));
        put(w, &pos, (uint32_t)a);
    }
    for (uint32_t i = 0; i < q->na; i++)
        buf_desc(w, &pos, q->a_addr[i], q->a_size[i]);
    for (uint32_t i = 0; i < q->nb; i++)
        buf_desc(w, &pos, q->b_addr[i], q->b_size[i]);
    uint32_t data_start = pos;
    uint8_t *b = (uint8_t *)w;
    uint32_t off;
    if (q->tipc) {
        off = pos * 4;
        memcpy(b + off, q->data, q->data_size);
        off += q->data_size;
    } else {
        off = (pos * 4 + 15) & ~15u;
        if (q->object_id) {
            uint16_t dsize = (uint16_t)(16 + q->data_size);
            b[off] = q->domain_cmd ? q->domain_cmd : 1;
            b[off + 1] = (uint8_t)q->n_in_objects;
            memcpy(b + off + 2, &dsize, 2);
            memcpy(b + off + 4, &q->object_id, 4);
            off += 16;
        }
        uint32_t hdr[4] = { q->raw_magic_bad ? 0x12345678u : CMIF_IN_MAGIC, 0, q->cmd, 0 };
        memcpy(b + off, hdr, 16);
        off += 16;
        memcpy(b + off, q->data, q->data_size);
        off += q->data_size;
        if (q->object_id) {
            memcpy(b + off, q->in_objects, 4 * q->n_in_objects);
            off += 4 * q->n_in_objects;
        }
        off += 16;
    }
    uint32_t ndata = (off - data_start * 4 + 3) / 4;
    pos = data_start + ndata;
    for (uint32_t i = 0; i < q->nc; i++) {
        put(w, &pos, (uint32_t)q->c_addr[i]);
        put(w, &pos, (uint32_t)((q->c_addr[i] >> 32) & 0xFFFF) | (uint32_t)(q->c_size[i] << 16));
    }
    w[0] = (q->tipc ? 16 + q->cmd : (q->type ? q->type : HIPC_REQUEST)) | (q->nx << 16) | (q->na << 20) |
           (q->nb << 24);
    w[1] = ndata | ((q->nc ? 2 + q->nc : 0) << 10) | (special ? 1u << 31 : 0);
    return pos;
}

static void decode(const uint32_t *w, bool domain, bool tipc, tc_res *r)
{
    uint32_t pos = 2;
    if (w[1] >> 31) {
        uint32_t sp = w[pos++];
        r->ncopy = (sp >> 1) & 0xF;
        r->nmove = (sp >> 5) & 0xF;
        if (sp & 1)
            pos += 2;
        for (uint32_t i = 0; i < r->ncopy; i++)
            r->copy[i] = w[pos++];
        for (uint32_t i = 0; i < r->nmove; i++)
            r->move[i] = w[pos++];
    }
    const uint8_t *b = (const uint8_t *)w;
    if (tipc) {
        memcpy(&r->result, b + pos * 4, 4);
        memcpy(r->data, b + pos * 4 + 4, sizeof r->data - 4);
        return;
    }
    uint32_t off = (pos * 4 + 15) & ~15u;
    uint32_t nobj = 0;
    if (domain) {
        memcpy(&nobj, b + off, 4);
        off += 16;
    }
    uint32_t hdr[4];
    memcpy(hdr, b + off, 16);
    r->result = hdr[0] == CMIF_OUT_MAGIC ? hdr[2] : 0xBADBAD;
    off += 16;
    memcpy(r->data, b + off, 32);
    if (domain) {
        r->nobjects = nobj;
        uint32_t ndata_end = (pos + (w[1] & 0x3FF)) * 4;
        for (uint32_t i = 0; i < nobj && i < 8; i++)
            memcpy(&r->objects[i], b + ndata_end - 4 * (nobj - i), 4);
    }
}


tc_res tc_call(rl_ctx *c, uint32_t h, const tc_req *q, bool domain)
{
    tc_res r;
    memset(&r, 0, sizeof r);
    uint32_t *tls = (uint32_t *)(void *)(t_mem() + c->tpidrro_el0);
    uint32_t w[RL_IPC_MSG / 4];
    encode(q, w);
    memcpy(tls, w, RL_IPC_MSG);
    r.svc_rc = t_svc(c, SVC_SEND_SYNC_REQUEST, h, 0, 0, 0, 0, 0);
    if (r.svc_rc == 0)
        decode(tls, domain, q->tipc, &r);
    return r;
}

tc_req tc_new(uint32_t cmd)
{
    tc_req q;
    memset(&q, 0, sizeof q);
    q.cmd = cmd;
    return q;
}

void tc_put(tc_req *q, const void *p, uint32_t n)
{
    memcpy(q->data + q->data_size, p, n);
    q->data_size += n;
}

uint32_t tc_u32(const tc_res *r, uint32_t off)
{
    uint32_t v;
    memcpy(&v, r->data + off, 4);
    return v;
}


void tc_put32(tc_req *q, uint32_t v) { tc_put(q, &v, 4); }

void tc_put64(tc_req *q, uint64_t v) { tc_put(q, &v, 8); }

uint64_t tc_u64(const tc_res *r, uint32_t off)
{
    uint64_t v;
    memcpy(&v, r->data + off, 8);
    return v;
}

tc_res tc_send(rl_ctx *c, tc_obj o, tc_req q)
{
    if (o.domain)
        q.object_id = o.id;
    return tc_call(c, o.handle, &q, o.domain);
}

uint32_t tc_cmd(rl_ctx *c, tc_obj o, uint32_t cmd)
{
    tc_res r = tc_send(c, o, tc_new(cmd));
    return r.svc_rc ? r.svc_rc : r.result;
}

uint32_t tc_connect_sm(rl_ctx *c, uint64_t scratch)
{
    memcpy(t_mem() + scratch, "sm:", 4);
    if (t_svc(c, SVC_CONNECT_TO_NAMED_PORT, 0, scratch, 0, 0, 0, 0) != 0)
        return 0;
    return (uint32_t)c->x[1];
}

tc_res tc_sm_get(rl_ctx *c, uint32_t sm, const char *name, bool tipc)
{
    tc_req q = tc_new(1);
    q.tipc = tipc;
    uint64_t n = rl_ipc_name(name);
    tc_put(&q, &n, 8);
    return tc_call(c, sm, &q, false);
}

uint32_t tc_sm_register(rl_ctx *c, uint32_t sm, bool tipc)
{
    tc_req q = tc_new(0);
    q.tipc = tipc;
    q.send_pid = true;
    uint64_t z = 0;
    tc_put(&q, &z, 8);
    tc_res r = tc_call(c, sm, &q, false);
    return r.svc_rc ? r.svc_rc : r.result;
}

uint32_t tc_open_service_result(rl_ctx *c, uint64_t scratch, const char *name)
{
    uint32_t sm = tc_connect_sm(c, scratch);
    if (!sm || tc_sm_register(c, sm, false) != 0)
        return 0xFFFFFFFFu;
    tc_res r = tc_sm_get(c, sm, name, false);
    uint32_t rc = r.svc_rc ? r.svc_rc : r.result;
    if (rc == 0 && r.nmove == 1)
        tc_close(c, r.move[0]);
    tc_close(c, sm);
    return rc;
}

tc_obj tc_open_service(rl_ctx *c, uint64_t scratch, const char *name)
{
    tc_obj o = { 0, 0, false };
    uint32_t sm = tc_connect_sm(c, scratch);
    if (!sm || tc_sm_register(c, sm, false) != 0)
        return o;
    tc_res r = tc_sm_get(c, sm, name, false);
    if (!r.svc_rc && !r.result && r.nmove == 1)
        o.handle = r.move[0];
    tc_close(c, sm);
    return o;
}

uint32_t tc_to_domain(rl_ctx *c, tc_obj *o)
{
    tc_req q = tc_new(0);
    q.type = HIPC_CONTROL;
    tc_res r = tc_call(c, o->handle, &q, false);
    uint32_t rc = r.svc_rc ? r.svc_rc : r.result;
    if (rc == 0) {
        o->domain = true;
        o->id = tc_u32(&r, 0);
    }
    return rc;
}

uint32_t tc_sub(rl_ctx *c, tc_obj o, tc_req q, tc_obj *out)
{
    tc_res r = tc_send(c, o, q);
    uint32_t rc = r.svc_rc ? r.svc_rc : r.result;
    out->handle = 0;
    out->id = 0;
    out->domain = false;
    if (rc != 0)
        return rc;
    if (o.domain) {
        if (r.nobjects != 1)
            return 0xFFFFFFFEu;
        out->handle = o.handle;
        out->id = r.objects[0];
        out->domain = true;
    } else {
        if (r.nmove != 1)
            return 0xFFFFFFFEu;
        out->handle = r.move[0];
    }
    return 0;
}

void tc_close(rl_ctx *c, uint32_t handle) { t_svc(c, SVC_CLOSE_HANDLE, handle, 0, 0, 0, 0, 0); }

void tc_hang_report(void)
{
    fprintf(stderr, "  guest threads at the timeout:\n");
    for (hos_thread *t = hos.threads; t; t = t->next_all)
        fprintf(stderr, "    thread %llu: state %d prio %d core %d active %d cur %d wait %d woken %d mutex 0x%llx\n",
                (unsigned long long)t->id, t->state, t->prio, t->core, t->active_core, t->cur_core, t->wait_kind,
                t->woken, (unsigned long long)t->mutex_addr);
    for (int i = 0; i < HOS_MAX_CORES; i++) {
        const hos_core *k = &hos.sched.cores[i];
        if (k->selected || k->holder)
            fprintf(stderr, "    core %d: selected %llu holder %llu\n", i,
                    (unsigned long long)(k->selected ? k->selected->id : 0),
                    (unsigned long long)(k->holder ? k->holder->id : 0));
    }
}
