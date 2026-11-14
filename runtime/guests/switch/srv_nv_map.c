/**
 * @file srv_nv_map.c
 * @brief /dev/nvmap: the table of memory handles.
 *
 * Ioctl numbers, argument layouts and results follow Ryujinx (MIT, Copyright
 * (c) Ryujinx Team and Contributors) NvMapDeviceFile. The table is shared by
 * every session of the process, as it is in Ryujinx. Alloc records the guest
 * address it is given; nothing is allocated for a handle that has none.
 */
#include "nv.h"

#include <stdlib.h>
#include <string.h>

#define MAP_FLAG_NOT_FREED_YET 1
#define MAP_HEAP_RESULT 0x40000000
#define MAP_MAX_HANDLES (1u << 20)

enum { PARAM_SIZE = 1, PARAM_ALIGN = 2, PARAM_BASE = 3, PARAM_HEAP = 4, PARAM_KIND = 5, PARAM_COMPR = 6 };

static nv_map **tab;
static uint32_t tab_cap;
static int32_t last_id;

typedef struct map_create {
    uint32_t size;
    int32_t handle;
} map_create;

typedef struct map_from_id {
    int32_t id;
    int32_t handle;
} map_from_id;

typedef struct map_alloc {
    int32_t handle;
    int32_t heap_mask;
    int32_t flags;
    int32_t align;
    int64_t kind;
    uint64_t address;
} map_alloc;

typedef struct map_free {
    int32_t handle;
    int32_t padding;
    uint64_t address;
    uint32_t size;
    int32_t flags;
} map_free;

typedef struct map_param {
    int32_t handle;
    int32_t param;
    int32_t result;
} map_param;

typedef struct map_get_id {
    int32_t id;
    int32_t handle;
} map_get_id;

_Static_assert(sizeof(map_create) == 8, "nvmap create");
_Static_assert(sizeof(map_from_id) == 8, "nvmap from id");
_Static_assert(sizeof(map_alloc) == 0x20, "nvmap alloc");
_Static_assert(sizeof(map_free) == 0x18, "nvmap free");
_Static_assert(sizeof(map_param) == 12, "nvmap param");
_Static_assert(sizeof(map_get_id) == 8, "nvmap get id");

nv_map *nv_map_get(int32_t id)
{
    if (id <= 0 || (id & 3))
        return NULL;
    uint32_t i = (uint32_t)id / 4;
    return i < tab_cap ? tab[i] : NULL;
}

void nv_map_reset(void)
{
    for (uint32_t i = 0; i < tab_cap; i++)
        free(tab[i]);
    free(tab);
    tab = NULL;
    tab_cap = 0;
    last_id = 0;
}

static int32_t map_add(uint32_t size)
{
    uint32_t i = (uint32_t)(last_id / 4) + 1;
    if (i >= MAP_MAX_HANDLES)
        return 0;
    if (i >= tab_cap) {
        uint32_t cap = tab_cap ? tab_cap * 2 : 64;
        while (cap <= i)
            cap *= 2;
        nv_map **p = realloc(tab, cap * sizeof *p);
        if (!p)
            return 0;
        memset(p + tab_cap, 0, (cap - tab_cap) * sizeof *p);
        tab = p;
        tab_cap = cap;
    }
    nv_map *m = calloc(1, sizeof *m);
    if (!m)
        return 0;
    m->id = (int32_t)(i * 4);
    m->size = size;
    m->refs = 1;
    tab[i] = m;
    last_id = m->id;
    return m->id;
}

static int32_t map_create_fn(nv_file *f, nv_args *a)
{
    (void)f;
    map_create *p = (map_create *)(void *)a->data;
    if (p->size == 0)
        return NVE_INVALID_INPUT;
    uint64_t size = ((uint64_t)p->size + NV_PAGE - 1) & ~(NV_PAGE - 1);
    if (size > UINT32_MAX)
        return NVE_INVALID_INPUT;
    int32_t id = map_add((uint32_t)size);
    if (!id)
        return NVE_OUT_OF_MEMORY;
    p->handle = id;
    return NVE_OK;
}

static int32_t map_from_id_fn(nv_file *f, nv_args *a)
{
    (void)f;
    map_from_id *p = (map_from_id *)(void *)a->data;
    nv_map *m = nv_map_get(p->id);
    if (!m)
        return NVE_INVALID_INPUT;
    m->refs++;
    p->handle = p->id;
    return NVE_OK;
}

static int32_t map_alloc_fn(nv_file *f, nv_args *a)
{
    (void)f;
    map_alloc *p = (map_alloc *)(void *)a->data;
    nv_map *m = nv_map_get(p->handle);
    if (!m)
        return NVE_INVALID_INPUT;
    uint32_t align = (uint32_t)p->align;
    if (align & (align - 1))
        return NVE_INVALID_INPUT;
    if (align < NV_PAGE)
        p->align = (int32_t)NV_PAGE;
    if (m->allocated)
        return NVE_OK;
    if (p->address == 0)
        return NVE_OUT_OF_MEMORY;
    m->allocated = true;
    m->align = p->align;
    m->kind = (uint8_t)p->kind;
    m->flags = (uint32_t)p->flags;
    m->heap_mask = (uint32_t)p->heap_mask;
    m->addr = p->address;
    return NVE_OK;
}

static int32_t map_free_fn(nv_file *f, nv_args *a)
{
    (void)f;
    map_free *p = (map_free *)(void *)a->data;
    nv_map *m = nv_map_get(p->handle);
    if (!m)
        return NVE_INVALID_INPUT;
    uint32_t size = m->size;
    if (--m->refs <= 0) {
        p->address = m->addr;
        p->flags = 0;
        tab[(uint32_t)m->id / 4] = NULL;
        free(m);
    } else {
        p->address = 0;
        p->flags = MAP_FLAG_NOT_FREED_YET;
    }
    p->size = size;
    return NVE_OK;
}

static int32_t map_param_fn(nv_file *f, nv_args *a)
{
    (void)f;
    map_param *p = (map_param *)(void *)a->data;
    nv_map *m = nv_map_get(p->handle);
    if (!m)
        return NVE_INVALID_INPUT;
    switch (p->param) {
    case PARAM_SIZE: p->result = (int32_t)m->size; break;
    case PARAM_ALIGN: p->result = m->align; break;
    case PARAM_HEAP: p->result = MAP_HEAP_RESULT; break;
    case PARAM_KIND: p->result = m->kind; break;
    case PARAM_COMPR: p->result = 0; break;
    default: return NVE_INVALID_INPUT;
    }
    return NVE_OK;
}

static int32_t map_get_id_fn(nv_file *f, nv_args *a)
{
    (void)f;
    map_get_id *p = (map_get_id *)(void *)a->data;
    if (!nv_map_get(p->handle))
        return NVE_INVALID_INPUT;
    p->id = p->handle;
    return NVE_OK;
}

static int32_t map_not_supported(nv_file *f, nv_args *a)
{
    (void)f;
    (void)a;
    return NVE_NOT_SUPPORTED;
}

#define NM(nr, stub, min, name, fn) { NV_MAGIC_MAP_CUSTOM, (nr), NV_V_IOCTL, (stub), (min), (name), (fn) }
static const nv_ioctl_entry map_ioctls[] = {
    NM(0x01, false, 8, "Create", map_create_fn),
    NM(0x02, false, 0, "Unsupported02", map_not_supported),
    NM(0x03, false, 8, "FromId", map_from_id_fn),
    NM(0x04, false, 0x20, "Alloc", map_alloc_fn),
    NM(0x05, false, 0x18, "Free", map_free_fn),
    NM(0x06, false, 0, "Unsupported06", map_not_supported),
    NM(0x07, false, 0, "Unsupported07", map_not_supported),
    NM(0x08, false, 0, "Unsupported08", map_not_supported),
    NM(0x09, false, 12, "Param", map_param_fn),
    NM(0x0A, false, 0, "Unsupported0A", map_not_supported),
    NM(0x0C, false, 0, "Unsupported0C", map_not_supported),
    NM(0x0D, false, 0, "Unsupported0D", map_not_supported),
    NM(0x0E, false, 8, "GetId", map_get_id_fn),
    NM(0x0F, false, 0, "Unsupported0F", map_not_supported),
    NM(0x10, false, 0, "Unsupported10", map_not_supported),
    NM(0x11, false, 0, "Unsupported11", map_not_supported),
};
#undef NM

const nv_dev_class nv_dev_nvmap = {
    .path = "/dev/nvmap",
    .ioctls = map_ioctls,
    .nioctls = sizeof map_ioctls / sizeof map_ioctls[0],
};

bool rl_nv_map_lookup(uint32_t handle_id, rl_nv_map_info *out)
{
    nv_lock();
    nv_map *m = nv_map_get((int32_t)handle_id);
    bool ok = m != NULL;
    if (ok) {
        out->handle_id = (uint32_t)m->id;
        out->size = m->size;
        out->alignment = (uint32_t)m->align;
        out->flags = m->flags;
        out->kind = (uint32_t)m->kind;
        out->heap_id = m->heap_mask;
        out->addr = m->addr;
        out->allocated = m->allocated;
    }
    nv_unlock();
    return ok;
}
