/**
 * @file srv_am_storage.c
 * @brief IStorage, IStorageAccessor and ITransferStorageAccessor: the byte
 * containers an application and its library applets trade through the
 * applet channels. A storage holds its own bytes, or the guest memory a
 * transfer memory lends (ILibraryAppletCreator 11, read and written in
 * place), or a transfer memory handle passed on as it is (12).
 *
 * Command ids and behavior follow Ryujinx (MIT, Copyright (c) Ryujinx Team
 * and Contributors), libnx (ISC, Copyright (c) libnx Authors) and
 * switchbrew.org (Applet Manager services).
 */
#include "srv_am_priv.h"

#include <stdlib.h>

/** @brief What a storage holds. */
enum { ST_BYTES, ST_TMEM, ST_HANDLE };

typedef struct am_storage {
    uint32_t kind;
    uint8_t *data;
    uint64_t size;
    bool writable;
    hos_tmem *tmem;
} am_storage;

static void storage_destroy(void *state)
{
    am_storage *s = state;
    if (s->tmem)
        hos_kobj_unref(&s->tmem->obj);
    free(s->data);
    free(s);
}

static void accessor_destroy(void *state) { rl_ipc_object_unref(state); }

static const rl_ipc_class accessor_class;
static const rl_ipc_class transfer_accessor_class;

static rl_result open_accessor(rl_ipc_req *r, const rl_ipc_class *cls)
{
    rl_ipc_object_ref(r->self);
    rl_ipc_object *acc = rl_srv_object(cls, r->self);
    if (!acc) {
        rl_ipc_object_unref(r->self);
        return KR_OUT_OF_RESOURCE;
    }
    return rl_ipc_push_object(r, acc);
}

static rl_result storage_open(rl_ipc_req *r)
{
    const am_storage *s = r->self->state;
    if (s->kind == ST_HANDLE)
        return AM_INVALID_STORAGE_TYPE;
    return open_accessor(r, &accessor_class);
}

static rl_result storage_open_transfer(rl_ipc_req *r)
{
    const am_storage *s = r->self->state;
    if (s->kind != ST_HANDLE)
        return AM_INVALID_STORAGE_TYPE;
    return open_accessor(r, &transfer_accessor_class);
}

static const rl_ipc_cmd storage_cmds[] = {
    { 0, "Open", storage_open },
    { 1, "OpenTransferStorage", storage_open_transfer },
};

static const rl_ipc_class storage_class = { "nn::am::service::IStorage", storage_cmds, AM_N(storage_cmds), 0, storage_destroy };

static rl_ipc_object *storage_wrap(am_storage *s)
{
    rl_ipc_object *o = rl_srv_object(&storage_class, s);
    if (!o)
        storage_destroy(s);
    return o;
}

rl_ipc_object *am_storage_new(const void *data, uint64_t size)
{
    am_storage *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->data = calloc(1, size ? size : 1);
    if (!s->data) {
        free(s);
        return NULL;
    }
    if (data)
        memcpy(s->data, data, size);
    s->kind = ST_BYTES;
    s->size = size;
    s->writable = true;
    return storage_wrap(s);
}

/** @brief A storage over transfer memory t: size bytes of the lent guest memory, or the handle itself. Takes a reference. */
static rl_ipc_object *storage_new_tmem(hos_tmem *t, uint64_t size, bool writable, uint32_t kind)
{
    am_storage *s = calloc(1, sizeof *s);
    if (!s) {
        hos_kobj_unref(&t->obj);
        return NULL;
    }
    s->kind = kind;
    s->tmem = t;
    s->size = size < t->size ? size : t->size;
    s->writable = writable;
    return storage_wrap(s);
}

bool am_storage_is(const rl_ipc_object *o) { return o && o->cls == &storage_class; }

uint64_t am_storage_size(const rl_ipc_object *o) { return am_storage_is(o) ? ((const am_storage *)o->state)->size : 0; }

bool am_storage_read(const rl_ipc_object *o, uint64_t offset, void *dst, uint64_t size)
{
    if (!am_storage_is(o))
        return false;
    const am_storage *s = o->state;
    if (s->kind == ST_HANDLE || offset > s->size || size > s->size - offset)
        return false;
    if (s->kind == ST_TMEM)
        rl_vm_kernel_read(&hos.vm, dst, s->tmem->addr + offset, size);
    else
        memcpy(dst, s->data + offset, size);
    return true;
}

bool am_storage_write(rl_ipc_object *o, uint64_t offset, const void *src, uint64_t size)
{
    if (!am_storage_is(o))
        return false;
    am_storage *s = o->state;
    if (s->kind == ST_HANDLE || !s->writable || offset > s->size || size > s->size - offset)
        return false;
    if (s->kind == ST_TMEM)
        rl_vm_kernel_write(&hos.vm, s->tmem->addr + offset, src, size);
    else
        memcpy(s->data + offset, src, size);
    return true;
}

static am_storage *accessor_storage(const rl_ipc_req *r) { return ((rl_ipc_object *)r->self->state)->state; }

static rl_result accessor_get_size(rl_ipc_req *r)
{
    *(uint64_t *)rl_ipc_out(r, 8) = accessor_storage(r)->size;
    return KR_SUCCESS;
}

static rl_result accessor_write(rl_ipc_req *r)
{
    am_storage *s = accessor_storage(r);
    AM_IN(uint64_t, offset);
    if (!s->writable)
        return AM_OBJECT_INVALID;
    if (offset > s->size)
        return AM_OUT_OF_BOUNDS;
    rl_ipc_buf b = rl_ipc_auto_in(r, 0);
    uint64_t n = b.size < s->size - offset ? b.size : s->size - offset;
    if (!n)
        return KR_SUCCESS;
    uint8_t *tmp = s->kind == ST_TMEM ? malloc(n) : s->data + offset;
    if (!tmp)
        return KR_OUT_OF_RESOURCE;
    bool ok = rl_ipc_buf_read(&b, tmp, n);
    if (ok && s->kind == ST_TMEM)
        rl_vm_kernel_write(&hos.vm, s->tmem->addr + offset, tmp, n);
    if (s->kind == ST_TMEM)
        free(tmp);
    return ok ? KR_SUCCESS : KR_INVALID_MEM_STATE;
}

static rl_result accessor_read(rl_ipc_req *r)
{
    am_storage *s = accessor_storage(r);
    AM_IN(uint64_t, offset);
    if (offset > s->size)
        return AM_OUT_OF_BOUNDS;
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    uint64_t n = b.size < s->size - offset ? b.size : s->size - offset;
    if (!n)
        return KR_SUCCESS;
    uint8_t *tmp = s->kind == ST_TMEM ? malloc(n) : s->data + offset;
    if (!tmp)
        return KR_OUT_OF_RESOURCE;
    if (s->kind == ST_TMEM)
        rl_vm_kernel_read(&hos.vm, tmp, s->tmem->addr + offset, n);
    bool ok = rl_ipc_buf_write(&b, tmp, n);
    if (s->kind == ST_TMEM)
        free(tmp);
    return ok ? KR_SUCCESS : KR_INVALID_MEM_STATE;
}

static const rl_ipc_cmd accessor_cmds[] = {
    { 0, "GetSize", accessor_get_size },
    { 10, "Write", accessor_write },
    { 11, "Read", accessor_read },
};

static const rl_ipc_class accessor_class = {
    "nn::am::service::IStorageAccessor", accessor_cmds, AM_N(accessor_cmds), 0, accessor_destroy
};

static rl_result transfer_get_size(rl_ipc_req *r)
{
    *(uint64_t *)rl_ipc_out(r, 8) = accessor_storage(r)->size;
    return KR_SUCCESS;
}

/** @brief GetHandle: the size and a copy of the transfer memory handle. */
static rl_result transfer_get_handle(rl_ipc_req *r)
{
    am_storage *s = accessor_storage(r);
    *(uint64_t *)rl_ipc_out(r, 8) = s->size;
    return rl_ipc_push_copy(r, &s->tmem->obj);
}

static const rl_ipc_cmd transfer_accessor_cmds[] = {
    { 0, "GetSize", transfer_get_size },
    { 1, "GetHandle", transfer_get_handle },
};

static const rl_ipc_class transfer_accessor_class = {
    "nn::am::service::ITransferStorageAccessor", transfer_accessor_cmds, AM_N(transfer_accessor_cmds), 0,
    accessor_destroy
};

/** @brief The transfer memory behind copy handle h, with a reference, or NULL. */
static hos_tmem *tmem_of(uint32_t h)
{
    hos_lock();
    hos_tmem *t = (hos_tmem *)(void *)hos_handle_get(h, KOBJ_TRANSFER_MEMORY);
    hos_unlock();
    return t;
}

rl_result am_create_transfer_memory_storage(rl_ipc_req *r)
{
    struct {
        uint8_t writable;
        uint8_t pad[7];
        int64_t size;
    } q;
    if (!rl_srv_in(r, &q, sizeof q) || r->n_in_copy < 1)
        return SF_RESULT_INVALID_HEADER;
    if (q.size <= 0)
        return AM_OBJECT_INVALID;
    hos_tmem *t = tmem_of(r->in_copy[0]);
    if (!t)
        return KR_INVALID_HANDLE;
    return am_push_object(r, storage_new_tmem(t, (uint64_t)q.size, q.writable & 1, ST_TMEM));
}

rl_result am_create_handle_storage(rl_ipc_req *r)
{
    AM_IN(int64_t, size);
    if (r->n_in_copy < 1)
        return SF_RESULT_INVALID_HEADER;
    if (size <= 0)
        return AM_OBJECT_INVALID;
    hos_tmem *t = tmem_of(r->in_copy[0]);
    if (!t)
        return KR_INVALID_HANDLE;
    return am_push_object(r, storage_new_tmem(t, (uint64_t)size, false, ST_HANDLE));
}
