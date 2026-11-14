/**
 * @file test_ipc.c
 * @brief IPC through svcSendSyncRequest: sm: over CMIF and TIPC, a test
 * service with raw data, A/B/X/C buffers, handles, pid and sub-objects, the
 * control commands, domains and error paths.
 *
 * The client encoder follows the message layout of libnx (ISC, libnx Authors)
 * sf/hipc.h and sf/cmif.h, written with guest addresses.
 */
#include "t.h"

#include <stdlib.h>
#include <unistd.h>

/** @brief A request being built. */
typedef struct creq {
    uint32_t type;
    uint32_t object_id;
    uint32_t cmd;
    bool send_pid;
    bool tipc;
    uint8_t data[64];
    uint32_t data_size;
    uint64_t x_addr[4], x_size[4];
    uint32_t nx;
    uint64_t a_addr[4], a_size[4];
    uint32_t na;
    uint64_t b_addr[4], b_size[4];
    uint32_t nb;
    uint64_t c_addr[4], c_size[4];
    uint32_t nc;
    uint32_t copy[4];
    uint32_t ncopy;
    uint32_t in_objects[4];
    uint32_t n_in_objects;
    uint8_t domain_cmd;
    bool raw_magic_bad;
} creq;

/** @brief A parsed response. */
typedef struct cres {
    uint32_t svc_rc;
    uint32_t result;
    uint8_t data[64];
    uint32_t copy[8], move[8], objects[8];
    uint32_t ncopy, nmove, nobjects;
} cres;

static void put(uint32_t *w, uint32_t *pos, uint32_t v) { w[(*pos)++] = v; }

static void buf_desc(uint32_t *w, uint32_t *pos, uint64_t addr, uint64_t size)
{
    put(w, pos, (uint32_t)size);
    put(w, pos, (uint32_t)addr);
    put(w, pos, (uint32_t)(((addr >> 36) & 0x3FFFFF) << 2) | (uint32_t)(((size >> 32) & 0xF) << 24) |
                    (uint32_t)(((addr >> 32) & 0xF) << 28));
}

static uint32_t encode(const creq *q, uint32_t *w)
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

static void decode(const uint32_t *w, bool domain, bool tipc, cres *r)
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

static cres call(rl_ctx *c, uint32_t h, const creq *q, bool domain)
{
    cres r;
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

static creq req(uint32_t cmd)
{
    creq q;
    memset(&q, 0, sizeof q);
    q.cmd = cmd;
    return q;
}

static void put_data(creq *q, const void *p, uint32_t n)
{
    memcpy(q->data + q->data_size, p, n);
    q->data_size += n;
}

static uint32_t u32_at(const cres *r, uint32_t off)
{
    uint32_t v;
    memcpy(&v, r->data + off, 4);
    return v;
}

static uint32_t connect_sm(rl_ctx *c, uint64_t scratch)
{
    memcpy(t_mem() + scratch, "sm:", 4);
    if (t_svc(c, SVC_CONNECT_TO_NAMED_PORT, 0, scratch, 0, 0, 0, 0) != 0)
        return 0;
    return (uint32_t)c->x[1];
}

static cres sm_get(rl_ctx *c, uint32_t sm, const char *name, bool tipc)
{
    creq q = req(1);
    q.tipc = tipc;
    uint64_t n = rl_ipc_name(name);
    put_data(&q, &n, 8);
    return call(c, sm, &q, false);
}

static uint32_t sm_register(rl_ctx *c, uint32_t sm, bool tipc)
{
    creq q = req(0);
    q.tipc = tipc;
    q.send_pid = true;
    uint64_t z = 0;
    put_data(&q, &z, 8);
    cres r = call(c, sm, &q, false);
    return r.svc_rc ? r.svc_rc : r.result;
}

/** @brief State of a test service object. */
typedef struct tsvc {
    uint32_t value;
} tsvc;

static rl_ipc_object *tsvc_new(uint32_t value);

static rl_result t_add(rl_ipc_req *r)
{
    uint32_t in[2];
    if (!rl_ipc_in(r, in, 8))
        return SF_RESULT_INVALID_HEADER;
    *(uint32_t *)rl_ipc_out(r, 4) = in[0] + in[1] + ((tsvc *)r->self->state)->value;
    return KR_SUCCESS;
}

static rl_result t_buffers(rl_ipc_req *r)
{
    uint8_t tmp[256];
    if (r->na < 1 || r->nb < 1 || r->a[0].size > sizeof tmp || r->b[0].size < r->a[0].size)
        return SF_RESULT_INVALID_HEADER;
    if (!rl_ipc_buf_read(&r->a[0], tmp, r->a[0].size))
        return KR_INVALID_MEM_STATE;
    uint64_t n = r->a[0].size;
    for (uint64_t i = 0; i < n / 2; i++) {
        uint8_t t = tmp[i];
        tmp[i] = tmp[n - 1 - i];
        tmp[n - 1 - i] = t;
    }
    if (!rl_ipc_buf_write(&r->b[0], tmp, n))
        return KR_INVALID_MEM_STATE;
    if (r->nx < 1 || r->nc < 1)
        return SF_RESULT_INVALID_HEADER;
    rl_ipc_buf x = r->x[0];
    rl_ipc_buf cbuf = r->c[0];
    if (x.size > sizeof tmp || !rl_ipc_buf_read(&x, tmp, x.size) || !rl_ipc_buf_write(&cbuf, tmp, x.size))
        return KR_INVALID_MEM_STATE;
    *(uint32_t *)rl_ipc_out(r, 4) = (uint32_t)(n + x.size);
    return KR_SUCCESS;
}

static rl_result t_get_event(rl_ipc_req *r)
{
    hos_revent *e = rl_hos_event_create();
    rl_hos_event_signal(e);
    rl_result rc = rl_ipc_push_copy(r, &e->obj);
    hos_kobj_unref(&e->obj);
    return rc;
}

static rl_result t_open_sub(rl_ipc_req *r)
{
    uint32_t v = 0;
    rl_ipc_in(r, &v, 4);
    return rl_ipc_push_object(r, tsvc_new(v));
}

static rl_result t_take_handle(rl_ipc_req *r)
{
    uint32_t ok = 0;
    if (r->n_in_copy == 1) {
        hos_lock();
        hos_kobj *o = hos_handle_get(r->in_copy[0], KOBJ_READABLE_EVENT);
        hos_unlock();
        if (o) {
            ok = 1;
            hos_kobj_unref(o);
        }
    }
    *(uint32_t *)rl_ipc_out(r, 4) = ok;
    return KR_SUCCESS;
}

static rl_result t_get_pid(rl_ipc_req *r)
{
    if (!r->has_pid)
        return SF_RESULT_INVALID_HEADER;
    *(uint64_t *)rl_ipc_out(r, 8) = r->pid;
    return KR_SUCCESS;
}

static rl_result t_in_object(rl_ipc_req *r)
{
    if (r->n_in_objects != 1 || !r->in_objects[0])
        return SF_RESULT_INVALID_IN_OBJECT;
    *(uint32_t *)rl_ipc_out(r, 4) = ((tsvc *)r->in_objects[0]->state)->value;
    return KR_SUCCESS;
}

static rl_result t_auto_echo(rl_ipc_req *r)
{
    uint8_t tmp[64];
    rl_ipc_buf in = rl_ipc_auto_in(r, 0), out = rl_ipc_auto_out(r, 0);
    if (in.size > sizeof tmp || !rl_ipc_buf_read(&in, tmp, in.size) || !rl_ipc_buf_write(&out, tmp, in.size))
        return KR_INVALID_MEM_STATE;
    *(uint32_t *)rl_ipc_out(r, 4) = (uint32_t)in.size;
    return KR_SUCCESS;
}

static const rl_ipc_cmd tsvc_cmds[] = {
    { 7, "AutoEcho", t_auto_echo },
    { 0, "Add", t_add },
    { 1, "Buffers", t_buffers },
    { 2, "GetEvent", t_get_event },
    { 3, "OpenSub", t_open_sub },
    { 4, "TakeHandle", t_take_handle },
    { 5, "GetPid", t_get_pid },
    { 6, "InObject", t_in_object },
};

static const rl_ipc_class tsvc_class = { "rltest", tsvc_cmds, sizeof tsvc_cmds / sizeof tsvc_cmds[0], 0x500, free };

static rl_ipc_object *tsvc_new(uint32_t value)
{
    tsvc *s = calloc(1, sizeof *s);
    s->value = value;
    return rl_ipc_object_new(&tsvc_class, s);
}

static rl_ipc_object *tsvc_factory(void) { return tsvc_new(0); }

static uint32_t open_test_service(rl_ctx *c, uint64_t scratch, bool tipc)
{
    rl_ipc_register_service("rltest", tsvc_factory);
    uint32_t sm = connect_sm(c, scratch);
    if (!sm || sm_register(c, sm, tipc) != 0)
        return 0;
    cres r = sm_get(c, sm, "rltest", tipc);
    if (r.svc_rc || r.result || r.nmove != 1)
        return 0;
    return r.move[0];
}

static uint32_t add(rl_ctx *c, uint32_t h, uint32_t object_id, uint32_t a, uint32_t b, uint32_t *out)
{
    creq q = req(0);
    q.object_id = object_id;
    uint32_t in[2] = { a, b };
    put_data(&q, in, 8);
    cres r = call(c, h, &q, object_id != 0);
    *out = u32_at(&r, 0);
    return r.svc_rc ? r.svc_rc : r.result;
}

TEST(sm_unknown_service)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t sm = connect_sm(c, heap);
    CHECK(sm);
    cres r = sm_get(c, sm, "nope", false);
    CHECK_EQ(r.svc_rc, 0);
    CHECK_EQ(r.result, SM_RESULT_INVALID_CLIENT);
    CHECK_EQ(sm_register(c, sm, false), 0);
    r = sm_get(c, sm, "nope", false);
    CHECK_EQ(r.result, SM_RESULT_NOT_REGISTERED);
    CHECK_EQ(r.nmove, 0);
    r = sm_get(c, sm, "", false);
    CHECK_EQ(r.result, SM_RESULT_INVALID_SERVICE_NAME);
    memcpy(t_mem() + heap, "xyz", 4);
    CHECK_EQ(t_svc(c, SVC_CONNECT_TO_NAMED_PORT, 0, heap, 0, 0, 0, 0), KR_NOT_FOUND);
    CHECK_EQ(t_svc(c, SVC_CONNECT_TO_NAMED_PORT, 0, hos.lay.heap_end, 0, 0, 0, 0), KR_USER_COPY_FAILED);
}

TEST(sm_tipc)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = open_test_service(c, heap, true);
    CHECK(h);
    uint32_t sm = connect_sm(c, heap);
    CHECK_EQ(sm_register(c, sm, true), 0);
    cres r = sm_get(c, sm, "nope", true);
    CHECK_EQ(r.result, SM_RESULT_NOT_REGISTERED);
    creq q = req(0);
    q.tipc = true;
    uint32_t in[2] = { 40, 2 };
    put_data(&q, in, 8);
    r = call(c, h, &q, false);
    CHECK_EQ(r.svc_rc, 0);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(u32_at(&r, 0), 42);
    q = req(99);
    q.tipc = true;
    r = call(c, h, &q, false);
    CHECK_EQ(r.result, SF_RESULT_UNKNOWN_COMMAND);
    q = req(0);
    q.type = TIPC_CLOSE;
    q.tipc = false;
    uint32_t *tls = (uint32_t *)(void *)(t_mem() + c->tpidrro_el0);
    memset(tls, 0, RL_IPC_MSG);
    tls[0] = TIPC_CLOSE;
    CHECK_EQ(t_svc(c, SVC_SEND_SYNC_REQUEST, h, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_SEND_SYNC_REQUEST, h, 0, 0, 0, 0, 0), KR_PORT_REMOTE_CLOSED);
}

TEST(cmif_roundtrip)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = open_test_service(c, heap, false);
    CHECK(h);
    uint32_t v = 0;
    CHECK_EQ(add(c, h, 0, 40, 2, &v), 0);
    CHECK_EQ(v, 42);

    uint64_t a = heap + 0x1000, b = heap + 0x2000, x = heap + 0x3000, cb = heap + 0x4000;
    for (int i = 0; i < 10; i++)
        t_mem()[a + (unsigned)i] = (uint8_t)i;
    memcpy(t_mem() + x, "pointer!", 8);
    creq q = req(1);
    q.na = q.nb = q.nx = q.nc = 1;
    q.a_addr[0] = a;
    q.a_size[0] = 10;
    q.b_addr[0] = b;
    q.b_size[0] = 16;
    q.x_addr[0] = x;
    q.x_size[0] = 8;
    q.c_addr[0] = cb;
    q.c_size[0] = 8;
    cres r = call(c, h, &q, false);
    CHECK_EQ(r.svc_rc, 0);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(u32_at(&r, 0), 18);
    CHECK_EQ(t_mem()[b], 9);
    CHECK_EQ(t_mem()[b + 9], 0);
    CHECK(memcmp(t_mem() + cb, "pointer!", 8) == 0);

    q.b_addr[0] = hos.lay.heap_end - 0x1000;
    r = call(c, h, &q, false);
    CHECK_EQ(r.result, KR_INVALID_MEM_STATE);

    r = call(c, h, &(creq){ .cmd = 2 }, false);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.ncopy, 1);
    *(uint32_t *)(void *)(t_mem() + heap + 0x800) = r.copy[0];
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 1, 0, 0, 0), 0);
    uint32_t ev = r.copy[0];

    q = req(4);
    q.ncopy = 1;
    q.copy[0] = ev;
    r = call(c, h, &q, false);
    CHECK_EQ(u32_at(&r, 0), 1);

    q = req(5);
    q.send_pid = true;
    r = call(c, h, &q, false);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(u32_at(&r, 0), HOS_PID);

    q = req(3);
    uint32_t seven = 7;
    put_data(&q, &seven, 4);
    r = call(c, h, &q, false);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    uint32_t sub = r.move[0];
    CHECK_EQ(add(c, sub, 0, 1, 1, &v), 0);
    CHECK_EQ(v, 9);

    r = call(c, h, &(creq){ .cmd = 99 }, false);
    CHECK_EQ(r.svc_rc, 0);
    CHECK_EQ(r.result, SF_RESULT_UNKNOWN_COMMAND);
    CHECK_EQ(add(c, h, 0, 1, 2, &v), 0);
    CHECK_EQ(v, 3);

    q = req(3);
    q.type = HIPC_CONTROL;
    r = call(c, h, &q, false);
    CHECK_EQ(r.result, 0);
    uint16_t pbs;
    memcpy(&pbs, r.data, 2);
    CHECK_EQ(pbs, 0x500);

    q = req(2);
    q.type = HIPC_CONTROL;
    r = call(c, h, &q, false);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    CHECK_EQ(add(c, r.move[0], 0, 5, 5, &v), 0);
    CHECK_EQ(v, 10);
}

TEST(cmif_auto_select_buffers)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = open_test_service(c, heap, false);
    CHECK(h);
    memcpy(t_mem() + heap + 0x1000, "mapped", 6);
    memcpy(t_mem() + heap + 0x3000, "static", 6);
    creq q = req(7);
    q.na = q.nb = q.nx = q.nc = 1;
    q.a_addr[0] = heap + 0x1000;
    q.a_size[0] = 6;
    q.b_addr[0] = heap + 0x2000;
    q.b_size[0] = 6;
    q.x_addr[0] = heap + 0x3000;
    q.x_size[0] = 0;
    q.c_addr[0] = heap + 0x4000;
    q.c_size[0] = 0;
    cres r = call(c, h, &q, false);
    CHECK_EQ(r.result, 0);
    CHECK(memcmp(t_mem() + heap + 0x2000, "mapped", 6) == 0);
    q.a_size[0] = 0;
    q.b_size[0] = 0;
    q.x_size[0] = 6;
    q.c_size[0] = 6;
    r = call(c, h, &q, false);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(u32_at(&r, 0), 6);
    CHECK(memcmp(t_mem() + heap + 0x4000, "static", 6) == 0);
}

TEST(cmif_domain)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = open_test_service(c, heap, false);
    CHECK(h);
    creq q = req(0);
    q.type = HIPC_CONTROL;
    cres r = call(c, h, &q, false);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(u32_at(&r, 0), 1);
    r = call(c, h, &q, false);
    CHECK(r.result != 0);
    uint32_t v = 0;
    CHECK_EQ(add(c, h, 1, 20, 22, &v), 0);
    CHECK_EQ(v, 42);

    q = req(3);
    q.object_id = 1;
    uint32_t five = 5;
    put_data(&q, &five, 4);
    r = call(c, h, &q, true);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 0);
    CHECK_EQ(r.nobjects, 1);
    uint32_t sub = r.objects[0];
    CHECK_EQ(sub, 2);
    CHECK_EQ(add(c, h, sub, 1, 1, &v), 0);
    CHECK_EQ(v, 7);

    q = req(6);
    q.object_id = 1;
    q.n_in_objects = 1;
    q.in_objects[0] = sub;
    r = call(c, h, &q, true);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(u32_at(&r, 0), 5);

    q = req(1);
    q.type = HIPC_CONTROL;
    uint32_t one = 1;
    put_data(&q, &one, 4);
    r = call(c, h, &q, false);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    CHECK_EQ(add(c, r.move[0], 0, 2, 2, &v), 0);
    CHECK_EQ(v, 4);

    q = req(0);
    q.object_id = sub;
    q.domain_cmd = 2;
    r = call(c, h, &q, true);
    CHECK_EQ(r.svc_rc, 0);
    CHECK_EQ(add(c, h, sub, 1, 1, &v), SF_RESULT_TARGET_NOT_FOUND);
    CHECK_EQ(add(c, h, 1, 1, 1, &v), 0);
    CHECK_EQ(v, 2);
}

TEST(cmif_domain_clone_shares_the_domain)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = open_test_service(c, heap, false);
    CHECK(h);
    creq q = req(0);
    q.type = HIPC_CONTROL;
    CHECK_EQ(call(c, h, &q, false).result, 0);

    q = req(2);
    q.type = HIPC_CONTROL;
    cres r = call(c, h, &q, false);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    uint32_t clone = r.move[0];
    uint32_t v = 0;
    CHECK_EQ(add(c, clone, 1, 20, 22, &v), 0);
    CHECK_EQ(v, 42);

    q = req(3);
    q.object_id = 1;
    uint32_t five = 5;
    put_data(&q, &five, 4);
    r = call(c, clone, &q, true);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nobjects, 1);
    uint32_t sub = r.objects[0];
    CHECK_EQ(add(c, h, sub, 1, 1, &v), 0);
    CHECK_EQ(v, 7);

    t_svc(c, SVC_CLOSE_HANDLE, clone, 0, 0, 0, 0, 0);
    CHECK_EQ(add(c, h, sub, 2, 2, &v), 0);
    CHECK_EQ(v, 9);
}

TEST(ipc_errors)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = open_test_service(c, heap, false);
    CHECK(h);
    CHECK_EQ(t_svc(c, SVC_SEND_SYNC_REQUEST, 0x1234, 0, 0, 0, 0, 0), KR_INVALID_HANDLE);
    creq q = req(0);
    q.raw_magic_bad = true;
    cres r = call(c, h, &q, false);
    CHECK_EQ(r.svc_rc, SF_RESULT_INVALID_HEADER);
    uint32_t v;
    CHECK_EQ(add(c, h, 0, 1, 1, &v), 0);
    uint32_t *tls = (uint32_t *)(void *)(t_mem() + c->tpidrro_el0);
    memset(tls, 0, RL_IPC_MSG);
    tls[0] = HIPC_CLOSE;
    CHECK_EQ(t_svc(c, SVC_SEND_SYNC_REQUEST, h, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(add(c, h, 0, 1, 1, &v), KR_PORT_REMOTE_CLOSED);
    CHECK_EQ(t_svc(c, SVC_CLOSE_HANDLE, h, 0, 0, 0, 0, 0), 0);
}

TEST(ipc_unknown_command_strict)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = open_test_service(c, heap, false);
    CHECK(h);
    rl_cfg()->strict = true;
    creq q = req(77);
    call(c, h, &q, false);
    CHECK_EQ(t_svc_rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
}

TEST(ipc_trace)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    rl_cfg()->trace_os = true;
    uint32_t h = open_test_service(c, heap, false);
    CHECK(h);
    uint32_t v;
    CHECK_EQ(add(c, h, 0, 1, 1, &v), 0);
}

/** @brief Starts capturing stderr into a pipe; returns the saved fd. */
static int capture_begin(int fds[2])
{
    fflush(stderr);
    if (pipe(fds) != 0)
        return -1;
    int saved = dup(2);
    dup2(fds[1], 2);
    return saved;
}

/** @brief Stops capturing, restores stderr and reads what was written into buf. */
static void capture_end(int fds[2], int saved, char *buf, size_t n)
{
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(fds[1]);
    ssize_t got = read(fds[0], buf, n - 1);
    buf[got > 0 ? got : 0] = 0;
    close(fds[0]);
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle))
        n++;
    return n;
}

TEST(unknown_command_logged_once)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = open_test_service(c, heap, false);
    CHECK(h);
    int fds[2];
    char out[4096];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    cres r[4];
    for (int i = 0; i < 3; i++)
        r[i] = call(c, h, &(creq){ .cmd = 55 }, false);
    r[3] = call(c, h, &(creq){ .cmd = 56 }, false);
    for (int i = 0; i < 2; i++)
        t_svc(c, 0x7d, 0, 0, 0, 0, 0, 0);
    capture_end(fds, saved, out, sizeof out);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(r[i].svc_rc, 0);
        CHECK_EQ(r[i].result, SF_RESULT_UNKNOWN_COMMAND);
    }
    CHECK_EQ(count_of(out, "unknown command 55"), 1);
    CHECK_EQ(count_of(out, "unknown command 56"), 1);
    CHECK_EQ(count_of(out, "unimplemented svc 0x7d"), 1);
    CHECK_EQ(c->x[0], KR_NOT_IMPLEMENTED);
}
