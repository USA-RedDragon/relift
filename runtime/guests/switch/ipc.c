/**
 * @file ipc.c
 * @brief HIPC parsing and building, CMIF requests, control requests and
 * domains, TIPC, and session objects.
 *
 * Message layout from switchbrew.org (IPC Marshalling) and libnx (ISC)
 * sf/hipc.h, cmif.h and tipc.h; control commands and domain handling follow
 * Ryujinx (MIT) IpcService and ServerBase.
 */
#include "ipc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

rl_ipc_object *rl_ipc_object_new(const rl_ipc_class *cls, void *state)
{
    rl_ipc_object *o = calloc(1, sizeof *o);
    if (!o)
        return NULL;
    o->refs = 1;
    o->cls = cls;
    o->state = state;
    return o;
}

void rl_ipc_object_ref(rl_ipc_object *o)
{
    __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
}

void rl_ipc_object_unref(rl_ipc_object *o)
{
    if (!o || __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    if (o->cls->destroy)
        o->cls->destroy(o->state);
    free(o);
}

static void session_destroy(hos_kobj *k)
{
    hos_session *s = (hos_session *)k;
    rl_ipc_object_unref(s->root);
    hos_domain *d = NULL;
    hos_lock();
    if (s->dt && --s->dt->refs == 0)
        d = s->dt;
    hos_unlock();
    if (d) {
        for (int i = 0; i < 64; i++)
            rl_ipc_object_unref(d->obj[i]);
        free(d);
    }
    free(s);
}

rl_result rl_ipc_session_open(rl_ipc_object *o, uint32_t *handle)
{
    hos_session *s = calloc(1, sizeof *s);
    if (!s)
        return KR_OUT_OF_RESOURCE;
    hos_kobj_init(&s->obj, KOBJ_SESSION, session_destroy);
    rl_ipc_object_ref(o);
    s->root = o;
    hos_lock();
    rl_result rc = hos_handle_add(&s->obj, handle);
    hos_unlock();
    hos_kobj_unref(&s->obj);
    return rc;
}

void *rl_ipc_out(rl_ipc_req *r, uint32_t size)
{
    if (size > sizeof r->out)
        size = sizeof r->out;
    memset(r->out, 0, size);
    r->out_size = size;
    return r->out;
}

bool rl_ipc_in(const rl_ipc_req *r, void *dst, uint32_t size)
{
    if (size > r->in_size)
        return false;
    memcpy(dst, r->in, size);
    return true;
}

rl_result rl_ipc_push_object(rl_ipc_req *r, rl_ipc_object *o)
{
    if (r->n_out_objects >= RL_IPC_MAX) {
        rl_ipc_object_unref(o);
        return SF_RESULT_OUT_OF_DOMAIN_ENTRIES;
    }
    r->out_objects[r->n_out_objects++] = o;
    return KR_SUCCESS;
}

static rl_result push_handle(uint32_t *list, uint32_t *n, hos_kobj *k)
{
    if (*n >= 8)
        return KR_OUT_OF_RESOURCE;
    hos_lock();
    rl_result rc = hos_handle_add(k, &list[*n]);
    hos_unlock();
    if (rc == KR_SUCCESS)
        (*n)++;
    return rc;
}

rl_result rl_ipc_push_copy(rl_ipc_req *r, hos_kobj *k) { return push_handle(r->out_copy, &r->n_out_copy, k); }
rl_result rl_ipc_push_move(rl_ipc_req *r, hos_kobj *k) { return push_handle(r->out_move, &r->n_out_move, k); }

rl_ipc_buf rl_ipc_auto_in(const rl_ipc_req *r, uint32_t i)
{
    rl_ipc_buf none = { 0, 0, 0 };
    if (i < r->na && r->a[i].size)
        return r->a[i];
    for (uint32_t k = 0; k < r->nx; k++)
        if (r->x[k].attr == i)
            return r->x[k];
    return none;
}

rl_ipc_buf rl_ipc_auto_out(const rl_ipc_req *r, uint32_t i)
{
    rl_ipc_buf none = { 0, 0, 0 };
    if (i < r->nb && r->b[i].size)
        return r->b[i];
    if (i < r->nc)
        return r->c[i];
    return none;
}

bool rl_ipc_buf_read(const rl_ipc_buf *b, void *dst, uint64_t size)
{
    if (size > b->size)
        return false;
    if (!size)
        return true;
    hos_lock();
    bool ok = hos_user_range(b->addr, size, HOS_PERM_R);
    if (ok)
        memcpy(dst, hos.vm.base + b->addr, size);
    hos_unlock();
    return ok;
}

bool rl_ipc_buf_write(const rl_ipc_buf *b, const void *src, uint64_t size)
{
    if (size > b->size)
        return false;
    if (!size)
        return true;
    hos_lock();
    bool ok = hos_user_range(b->addr, size, HOS_PERM_W);
    if (ok)
        memcpy(hos.vm.base + b->addr, src, size);
    hos_unlock();
    return ok;
}

/** @brief A message as 32-bit words with a read position. */
typedef struct msg {
    uint32_t w[RL_IPC_MSG / 4];
    uint32_t pos;
    bool bad;
} msg;

static uint32_t take(msg *m)
{
    if (m->pos >= RL_IPC_MSG / 4) {
        m->bad = true;
        return 0;
    }
    return m->w[m->pos++];
}

/** @brief Header fields of a parsed request. */
typedef struct parsed {
    uint32_t type;
    uint32_t data_off;
    uint32_t data_end;
} parsed;

static bool parse(msg *m, rl_ipc_req *r, parsed *p)
{
    uint32_t w0 = take(m), w1 = take(m);
    p->type = w0 & 0xFFFF;
    r->nx = (w0 >> 16) & 0xF;
    r->na = (w0 >> 20) & 0xF;
    r->nb = (w0 >> 24) & 0xF;
    r->nw = (w0 >> 28) & 0xF;
    uint32_t ndata = w1 & 0x3FF;
    uint32_t recv_mode = (w1 >> 10) & 0xF;
    if (w1 >> 31) {
        uint32_t sp = take(m);
        r->n_in_copy = (sp >> 1) & 0xF;
        r->n_in_move = (sp >> 5) & 0xF;
        if (sp & 1) {
            take(m);
            take(m);
            r->has_pid = true;
            r->pid = HOS_PID;
        }
        for (uint32_t i = 0; i < r->n_in_copy; i++)
            r->in_copy[i] = take(m);
        for (uint32_t i = 0; i < r->n_in_move; i++)
            r->in_move[i] = take(m);
    }
    for (uint32_t i = 0; i < r->nx; i++) {
        uint32_t d0 = take(m), d1 = take(m);
        r->x[i].attr = d0 & 0x3F;
        r->x[i].size = d0 >> 16;
        r->x[i].addr = d1 | ((uint64_t)((d0 >> 12) & 0xF) << 32) | ((uint64_t)((d0 >> 6) & 0x3F) << 36);
    }
    rl_ipc_buf *lists[3] = { r->a, r->b, r->w };
    uint32_t counts[3] = { r->na, r->nb, r->nw };
    for (int k = 0; k < 3; k++)
        for (uint32_t i = 0; i < counts[k]; i++) {
            uint32_t s = take(m), a = take(m), d2 = take(m);
            lists[k][i].attr = d2 & 3;
            lists[k][i].addr = a | ((uint64_t)((d2 >> 28) & 0xF) << 32) | ((uint64_t)((d2 >> 2) & 0x3FFFFF) << 36);
            lists[k][i].size = s | ((uint64_t)((d2 >> 24) & 0xF) << 32);
        }
    p->data_off = m->pos * 4;
    p->data_end = p->data_off + ndata * 4;
    m->pos += ndata;
    r->nc = recv_mode > 2 ? recv_mode - 2 : (recv_mode == 2 ? 1 : 0);
    for (uint32_t i = 0; i < r->nc; i++) {
        uint32_t d0 = take(m), d1 = take(m);
        r->c[i].addr = d0 | ((uint64_t)(d1 & 0xFFFF) << 32);
        r->c[i].size = d1 >> 16;
    }
    return !m->bad && p->data_end <= RL_IPC_MSG;
}

/** @brief Response flavors. */
enum { RESP_CMIF = 0, RESP_DOMAIN = 1, RESP_TIPC = 2 };

static uint32_t build(msg *m, const rl_ipc_req *r, int kind, rl_result result, const uint32_t *obj_handles,
                      uint32_t n_obj_handles, const uint32_t *obj_ids, uint32_t n_obj_ids)
{
    memset(m->w, 0, sizeof m->w);
    uint32_t nmove = n_obj_handles + r->n_out_move;
    bool special = r->n_out_copy || nmove;
    uint32_t pos = 2;
    if (special) {
        m->w[pos++] = (r->n_out_copy << 1) | (nmove << 5);
        for (uint32_t i = 0; i < r->n_out_copy; i++)
            m->w[pos++] = r->out_copy[i];
        for (uint32_t i = 0; i < n_obj_handles; i++)
            m->w[pos++] = obj_handles[i];
        for (uint32_t i = 0; i < r->n_out_move; i++)
            m->w[pos++] = r->out_move[i];
    }
    uint32_t data_off = pos * 4;
    uint8_t *b = (uint8_t *)m->w;
    uint32_t end;
    if (kind == RESP_TIPC) {
        memcpy(b + data_off, &result, 4);
        memcpy(b + data_off + 4, r->out, r->out_size);
        end = data_off + 4 + r->out_size;
    } else {
        uint32_t p = (data_off + 15) & ~15u;
        if (kind == RESP_DOMAIN) {
            memcpy(b + p, &n_obj_ids, 4);
            p += 16;
        }
        uint32_t hdr[4] = { CMIF_OUT_MAGIC, 0, result, 0 };
        memcpy(b + p, hdr, 16);
        p += 16;
        memcpy(b + p, r->out, r->out_size);
        p += r->out_size;
        if (kind == RESP_DOMAIN) {
            memcpy(b + p, obj_ids, 4 * n_obj_ids);
            p += 4 * n_obj_ids;
        }
        end = p;
    }
    uint32_t ndata = (end - data_off + 3) / 4;
    m->w[0] = 0;
    m->w[1] = ndata | (special ? 1u << 31 : 0);
    return end;
}

static rl_result run_cmd(rl_ipc_req *r, bool *stop)
{
    const rl_ipc_class *cls = r->self->cls;
    for (uint32_t i = 0; i < cls->ncmds; i++)
        if (cls->cmds[i].id == r->cmd) {
            uint64_t t0 = rl_clock_ns();
            rl_result rc = cls->cmds[i].fn(r);
            if (rl_cfg()->trace_os)
                rl_log(RL_LOG_INFO, "ipc", "%s%s %u %s -> 0x%x (%llu us)", r->tipc ? "tipc " : "", cls->name, r->cmd,
                       cls->cmds[i].name, rc, (unsigned long long)((rl_clock_ns() - t0) / 1000));
            return rc;
        }
    if (rl_report_unknown(((uint64_t)(uintptr_t)cls << 20) ^ r->cmd ^ (r->tipc ? 1ull << 63 : 0), "ipc",
                          "%s: unknown %scommand %u", cls->name, r->tipc ? "tipc " : "", r->cmd))
        *stop = true;
    return SF_RESULT_UNKNOWN_COMMAND;
}

static void drop_outputs(rl_ipc_req *r)
{
    hos_kobj *drop[2 * RL_IPC_MAX];
    uint32_t n = 0;
    hos_lock();
    for (uint32_t i = 0; i < r->n_out_copy; i++)
        drop[n++] = hos_handle_remove(r->out_copy[i]);
    for (uint32_t i = 0; i < r->n_out_move; i++)
        drop[n++] = hos_handle_remove(r->out_move[i]);
    hos_unlock();
    for (uint32_t i = 0; i < n; i++)
        hos_kobj_unref(drop[i]);
    for (uint32_t i = 0; i < r->n_out_objects; i++)
        rl_ipc_object_unref(r->out_objects[i]);
    r->n_out_copy = r->n_out_move = r->n_out_objects = 0;
    r->out_size = 0;
}

static rl_result finish(msg *m, rl_ipc_req *r, int kind, rl_result rc, uint32_t *len)
{
    uint32_t handles[RL_IPC_MAX], ids[RL_IPC_MAX];
    uint32_t nh = 0, ni = 0;
    if (rc != KR_SUCCESS)
        drop_outputs(r);
    for (uint32_t i = 0; i < r->n_out_objects; i++) {
        rl_ipc_object *o = r->out_objects[i];
        if (kind == RESP_DOMAIN) {
            uint32_t id = 0;
            hos_lock();
            for (uint32_t k = 1; k < 64 && !id; k++)
                if (!r->session->dt->obj[k]) {
                    r->session->dt->obj[k] = o;
                    id = k;
                }
            hos_unlock();
            if (!id) {
                rl_ipc_object_unref(o);
                rc = SF_RESULT_OUT_OF_DOMAIN_ENTRIES;
                continue;
            }
            ids[ni++] = id;
        } else {
            uint32_t h;
            if (rl_ipc_session_open(o, &h) == KR_SUCCESS)
                handles[nh++] = h;
            rl_ipc_object_unref(o);
        }
    }
    r->n_out_objects = 0;
    *len = build(m, r, kind, rc, handles, nh, ids, ni);
    return KR_SUCCESS;
}

static rl_result control(msg *m, rl_ipc_req *r, uint32_t *len)
{
    rl_result rc = KR_SUCCESS;
    hos_session *s = r->session;
    uint32_t v = 0;
    switch (r->cmd) {
    case 0:
        hos_lock();
        if (s->domain) {
            rc = SF_RESULT_NOT_SUPPORTED;
        } else if (!(s->dt = calloc(1, sizeof *s->dt))) {
            rc = KR_OUT_OF_RESOURCE;
        } else {
            s->domain = true;
            s->dt->refs = 1;
            s->dt->obj[1] = s->root;
            rl_ipc_object_ref(s->root);
        }
        hos_unlock();
        if (rc == KR_SUCCESS)
            *(uint32_t *)rl_ipc_out(r, 4) = 1;
        break;
    case 1:
        if (!rl_ipc_in(r, &v, 4) || v >= 64 || !s->domain || !s->dt->obj[v]) {
            rc = SF_RESULT_TARGET_NOT_FOUND;
        } else {
            uint32_t h;
            rc = rl_ipc_session_open(s->dt->obj[v], &h);
            if (rc == KR_SUCCESS)
                r->out_move[r->n_out_move++] = h;
        }
        break;
    case 2:
    case 4: {
        uint32_t h;
        rc = rl_ipc_session_open(s->root, &h);
        if (rc == KR_SUCCESS && s->domain) {
            hos_lock();
            hos_session *n = (hos_session *)hos_handle_get(h, KOBJ_SESSION);
            if (n) {
                n->domain = true;
                n->dt = s->dt;
                s->dt->refs++;
            }
            hos_unlock();
            if (n)
                hos_kobj_unref(&n->obj);
        }
        if (rc == KR_SUCCESS)
            r->out_move[r->n_out_move++] = h;
        break;
    }
    case 3:
        *(uint16_t *)rl_ipc_out(r, 2) =
            s->root->cls->pointer_buffer_size ? s->root->cls->pointer_buffer_size : RL_IPC_POINTER_BUFFER_DEFAULT;
        break;
    default:
        (void)rl_report_unknown(0x434F4E54524F4C00ull ^ r->cmd, "ipc", "unknown control command %u", r->cmd);
        rc = SF_RESULT_UNKNOWN_COMMAND;
        break;
    }
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "ipc", "control %u on %s -> 0x%x", r->cmd, s->root->cls->name, rc);
    return finish(m, r, RESP_CMIF, rc, len);
}

static rl_result cmif(msg *m, rl_ipc_req *r, const parsed *p, uint32_t *len, bool *stop)
{
    const uint8_t *b = (const uint8_t *)m->w;
    uint32_t start = (p->data_off + 15) & ~15u;
    hos_session *s = r->session;
    bool control_req = p->type == HIPC_CONTROL || p->type == HIPC_CONTROL_CTX;
    int kind = RESP_CMIF;
    uint32_t hdr_at = start, payload_end = p->data_end;
    if (s->domain && !control_req) {
        if (start + 16 > p->data_end)
            return SF_RESULT_INVALID_HEADER;
        uint8_t dtype = b[start], nobj = b[start + 1];
        uint16_t dsize;
        uint32_t oid;
        memcpy(&dsize, b + start + 2, 2);
        memcpy(&oid, b + start + 4, 4);
        hos_lock();
        rl_ipc_object *o = oid < 64 ? s->dt->obj[oid] : NULL;
        if (dtype == 2 && o) {
            s->dt->obj[oid] = NULL;
            hos_unlock();
            rl_ipc_object_unref(o);
            r->self = s->root;
            return finish(m, r, RESP_DOMAIN, KR_SUCCESS, len);
        }
        if (o)
            rl_ipc_object_ref(o);
        hos_unlock();
        if (dtype != 1 || !o) {
            rl_ipc_object_unref(o);
            r->self = s->root;
            return finish(m, r, RESP_DOMAIN, SF_RESULT_TARGET_NOT_FOUND, len);
        }
        r->self = o;
        hdr_at = start + 16;
        payload_end = hdr_at + dsize;
        if (payload_end + 4u * nobj > RL_IPC_MSG) {
            rl_ipc_object_unref(o);
            return SF_RESULT_INVALID_HEADER;
        }
        for (uint32_t i = 0; i < nobj && i < RL_IPC_MAX; i++) {
            uint32_t id;
            memcpy(&id, b + payload_end + 4 * i, 4);
            r->in_objects[r->n_in_objects++] = id < 64 ? s->dt->obj[id] : NULL;
        }
        kind = RESP_DOMAIN;
    } else {
        r->self = s->root;
        rl_ipc_object_ref(r->self);
    }
    uint32_t hdr[4];
    if (hdr_at + 16 > payload_end) {
        rl_ipc_object_unref(r->self);
        return SF_RESULT_INVALID_HEADER;
    }
    memcpy(hdr, b + hdr_at, 16);
    if (hdr[0] != CMIF_IN_MAGIC) {
        rl_ipc_object_unref(r->self);
        return SF_RESULT_INVALID_HEADER;
    }
    r->cmd = hdr[2];
    r->in = b + hdr_at + 16;
    r->in_size = payload_end - (hdr_at + 16);
    rl_result rc;
    if (control_req) {
        rc = control(m, r, len);
    } else {
        rc = finish(m, r, kind, run_cmd(r, stop), len);
    }
    rl_ipc_object_unref(r->self);
    return rc;
}

static _Thread_local struct {
    bool set;
    const char *tag;
    char what[256];
} pending_stop;

void rl_ipc_stop(rl_ipc_req *r, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->stop_what, sizeof r->stop_what, fmt, ap);
    va_end(ap);
    r->stop = true;
    r->stop_tag = tag;
}

bool rl_ipc_take_stop(const char **tag, char *what, size_t n)
{
    if (!pending_stop.set)
        return false;
    pending_stop.set = false;
    *tag = pending_stop.tag;
    snprintf(what, n, "%s", pending_stop.what);
    return true;
}

rl_result hos_ipc_request(uint32_t handle, uint64_t msg_addr, uint64_t msg_size, bool *stop)
{
    *stop = false;
    if (msg_size < RL_IPC_MSG)
        return KR_INVALID_SIZE;
    hos_lock();
    hos_session *s = (hos_session *)hos_handle_get(handle, KOBJ_SESSION);
    bool ok = hos_user_range(msg_addr, RL_IPC_MSG, HOS_PERM_RW);
    hos_unlock();
    if (!s)
        return KR_INVALID_HANDLE;
    if (!ok) {
        hos_kobj_unref(&s->obj);
        return KR_INVALID_MEM_STATE;
    }
    if (s->closed) {
        hos_kobj_unref(&s->obj);
        return KR_PORT_REMOTE_CLOSED;
    }
    msg *m = calloc(1, sizeof *m);
    rl_ipc_req *r = calloc(1, sizeof *r);
    if (!m || !r) {
        free(m);
        free(r);
        hos_kobj_unref(&s->obj);
        return KR_OUT_OF_RESOURCE;
    }
    memcpy(m->w, hos.vm.base + msg_addr, RL_IPC_MSG);
    r->session = s;
    parsed p;
    rl_result rc = KR_SUCCESS;
    uint32_t len = 0;
    if (!parse(m, r, &p)) {
        rc = SF_RESULT_INVALID_HEADER;
    } else if (p.type == HIPC_CLOSE || p.type == TIPC_CLOSE) {
        s->closed = true;
        rc = KR_PORT_REMOTE_CLOSED;
    } else if (p.type == HIPC_REQUEST || p.type == HIPC_REQUEST_CTX || p.type == HIPC_CONTROL ||
               p.type == HIPC_CONTROL_CTX) {
        rc = cmif(m, r, &p, &len, stop);
    } else if (p.type >= TIPC_BASE) {
        r->tipc = true;
        r->self = s->root;
        r->cmd = p.type - TIPC_BASE;
        r->in = (const uint8_t *)m->w + p.data_off;
        r->in_size = p.data_end - p.data_off;
        rl_result res = run_cmd(r, stop);
        rc = finish(m, r, RESP_TIPC, res, &len);
    } else {
        (void)rl_report_unknown(0x4950435459504500ull ^ p.type, "ipc", "unsupported message type %u on %s", p.type,
                                s->root->cls->name);
        rc = SF_RESULT_INVALID_HEADER;
    }
    if (len) {
        hos_lock();
        memcpy(hos.vm.base + msg_addr, m->w, len);
        hos_unlock();
    }
    if (r->stop) {
        pending_stop.set = true;
        pending_stop.tag = r->stop_tag;
        snprintf(pending_stop.what, sizeof pending_stop.what, "%s", r->stop_what);
        *stop = true;
    }
    free(m);
    free(r);
    hos_kobj_unref(&s->obj);
    return rc;
}
