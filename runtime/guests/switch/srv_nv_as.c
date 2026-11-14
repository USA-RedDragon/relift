/**
 * @file srv_nv_as.c
 * @brief /dev/nvhost-as-gpu: GPU address spaces. Only the bookkeeping of
 * virtual ranges is kept (reservations, mapped buffers and the free ranges),
 * so offsets handed out round trip through map, unmap and free.
 *
 * Ioctl numbers, argument layouts, the address space layout and the results
 * follow Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors)
 * NvHostAsGpuDeviceFile, AddressSpaceContext and NvMemoryAllocator.
 */
#include "nv.h"

#include <stdlib.h>
#include <string.h>

#define AS_SMALL_PAGE 0x1000u
#define AS_BIG_PAGE 0x10000u
#define AS_SMALL_REGION_LIMIT 0x400000000ull
#define AS_DEFAULT_USER_SIZE (1ull << 37)
#define AS_SPACE_END (1ull << 40)
#define AS_DEFAULT_START (1ull << 32)
#define AS_FLAG_FIXED_OFFSET 1u
#define AS_FLAG_REMAP_SUB_RANGE 0x100u

typedef struct as_range {
    uint64_t start;
    uint64_t size;
    uint64_t phys;
    bool va_allocated;
} as_range;

typedef struct as_list {
    as_range *v;
    uint32_t n, cap;
} as_list;

typedef struct as_priv {
    nv_va va;
    as_list maps;
    as_list resv;
} as_priv;

typedef struct bind_channel {
    int32_t fd;
} bind_channel;

typedef struct alloc_space {
    uint32_t pages;
    uint32_t page_size;
    uint32_t flags;
    uint32_t padding;
    uint64_t offset;
} alloc_space;

typedef struct free_space {
    uint64_t offset;
    uint32_t pages;
    uint32_t page_size;
} free_space;

typedef struct unmap_buffer {
    uint64_t offset;
} unmap_buffer;

typedef struct map_buffer_ex {
    uint32_t flags;
    int32_t kind;
    int32_t nvmap_handle;
    int32_t page_size;
    uint64_t buffer_offset;
    uint64_t mapping_size;
    uint64_t offset;
} map_buffer_ex;

typedef struct va_region {
    uint64_t offset;
    uint32_t page_size;
    uint32_t padding;
    uint64_t pages;
} va_region;

typedef struct get_va_regions {
    uint64_t unused;
    uint32_t buffer_size;
    uint32_t padding;
    va_region regions[2];
} get_va_regions;

typedef struct initialize_ex {
    uint32_t flags;
    int32_t as_fd;
    uint32_t big_page_size;
    uint32_t reserved;
    uint64_t unknown[3];
} initialize_ex;

typedef struct remap_entry {
    uint16_t flags;
    uint16_t kind;
    int32_t nvmap_handle;
    uint32_t map_offset;
    uint32_t gpu_offset;
    uint32_t pages;
} remap_entry;

_Static_assert(sizeof(alloc_space) == 24, "alloc space");
_Static_assert(sizeof(free_space) == 16, "free space");
_Static_assert(sizeof(map_buffer_ex) == 40, "map buffer ex");
_Static_assert(sizeof(va_region) == 0x18, "va region");
_Static_assert(sizeof(get_va_regions) == 0x40, "get va regions");
_Static_assert(sizeof(initialize_ex) == 40, "initialize ex");
_Static_assert(sizeof(remap_entry) == 20, "remap");

static const uint32_t page_sizes[2] = { AS_SMALL_PAGE, AS_BIG_PAGE };
static const uint64_t region_start[2] = { (uint64_t)AS_BIG_PAGE << 16, AS_SMALL_REGION_LIMIT };
static const uint64_t region_limit[2] = { AS_SMALL_REGION_LIMIT, AS_DEFAULT_USER_SIZE };

static as_priv *priv_of(nv_file *f) { return f->priv; }

static bool list_add(as_list *l, as_range r)
{
    if (l->n == l->cap) {
        uint32_t cap = l->cap ? l->cap * 2 : 16;
        as_range *p = realloc(l->v, cap * sizeof *p);
        if (!p)
            return false;
        l->v = p;
        l->cap = cap;
    }
    l->v[l->n++] = r;
    return true;
}

static as_range *list_exact(as_list *l, uint64_t start)
{
    for (uint32_t i = 0; i < l->n; i++)
        if (l->v[i].start == start)
            return &l->v[i];
    return NULL;
}

static as_range *list_containing(as_list *l, uint64_t addr)
{
    for (uint32_t i = 0; i < l->n; i++)
        if (addr >= l->v[i].start && addr - l->v[i].start < l->v[i].size)
            return &l->v[i];
    return NULL;
}

static void list_remove(as_list *l, as_range *r)
{
    *r = l->v[--l->n];
}

static int32_t as_bind_channel(nv_file *f, nv_args *a)
{
    bind_channel *p = (bind_channel *)(void *)a->data;
    nv_file *ch = nv_file_get(f->sess, p->fd);
    if (!ch || !nv_ch_bind(ch, f->fd))
        return NVE_INVALID_INPUT;
    return NVE_OK;
}

static int32_t as_alloc_space(nv_file *f, nv_args *a)
{
    as_priv *as = priv_of(f);
    alloc_space *p = (alloc_space *)(void *)a->data;
    uint64_t size = (uint64_t)p->pages * p->page_size;
    uint64_t addr = 0;
    bool ok;
    if (p->flags & AS_FLAG_FIXED_OFFSET) {
        addr = p->offset;
        ok = nv_va_is_free(&as->va, addr, size);
    } else {
        ok = nv_va_find(&as->va, size, p->offset, AS_DEFAULT_START, &addr);
    }
    if (!ok || !nv_va_take(&as->va, addr, size)) {
        p->offset = 0;
        return NVE_OUT_OF_MEMORY;
    }
    as_range r = { addr, size, 0, false };
    if (list_exact(&as->resv, addr) || !list_add(&as->resv, r)) {
        nv_va_put(&as->va, addr, size);
        p->offset = 0;
        return NVE_INVALID_INPUT;
    }
    p->offset = addr;
    return NVE_OK;
}

static int32_t as_free_space(nv_file *f, nv_args *a)
{
    as_priv *as = priv_of(f);
    free_space *p = (free_space *)(void *)a->data;
    as_range *r = list_exact(&as->resv, p->offset);
    if (!r)
        return NVE_INVALID_INPUT;
    nv_va_put(&as->va, r->start, r->size);
    list_remove(&as->resv, r);
    return NVE_OK;
}

static int32_t as_unmap_buffer(nv_file *f, nv_args *a)
{
    as_priv *as = priv_of(f);
    unmap_buffer *p = (unmap_buffer *)(void *)a->data;
    as_range *r = list_exact(&as->maps, p->offset);
    if (r) {
        if (r->va_allocated && r->size)
            nv_va_put(&as->va, r->start, r->size);
        list_remove(&as->maps, r);
    }
    return NVE_OK;
}

static bool fixed_buffer_ok(as_priv *as, uint64_t address, uint64_t size, uint64_t alignment)
{
    uint64_t end = address + size;
    if (end <= address)
        return false;
    if (address & (alignment - 1))
        return false;
    if (!list_containing(&as->resv, address))
        return false;
    const as_range *lt = NULL;
    for (uint32_t i = 0; i < as->maps.n; i++) {
        const as_range *m = &as->maps.v[i];
        if (m->start < end && (!lt || m->start > lt->start))
            lt = m;
    }
    return !(lt && lt->start + lt->size > address);
}

static int32_t as_map_buffer_ex(nv_file *f, nv_args *a)
{
    as_priv *as = priv_of(f);
    map_buffer_ex *p = (map_buffer_ex *)(void *)a->data;
    if (p->flags & AS_FLAG_REMAP_SUB_RANGE) {
        as_range *m = list_containing(&as->maps, p->offset);
        return m ? NVE_OK : NVE_INVALID_INPUT;
    }
    nv_map *map = nv_map_get(p->nvmap_handle);
    if (!map)
        return NVE_INVALID_INPUT;
    uint64_t page_size = (uint64_t)(uint32_t)p->page_size;
    if (page_size == 0)
        page_size = (uint64_t)(uint32_t)map->align;
    uint64_t phys = map->addr + p->buffer_offset;
    uint64_t size = p->mapping_size ? p->mapping_size : map->size;
    bool va_allocated = (p->flags & AS_FLAG_FIXED_OFFSET) == 0;
    uint64_t va = p->offset;
    if (!va_allocated) {
        if (!fixed_buffer_ok(as, va, size, page_size))
            return NVE_INVALID_INPUT;
    } else if (!nv_va_find(&as->va, size, page_size, AS_DEFAULT_START, &va) || !nv_va_take(&as->va, va, size)) {
        p->offset = 0;
        return NVE_INVALID_INPUT;
    }
    as_range r = { va, size, phys, va_allocated };
    if (list_exact(&as->maps, va) || !list_add(&as->maps, r)) {
        if (va_allocated)
            nv_va_put(&as->va, va, size);
        p->offset = 0;
        return NVE_INVALID_INPUT;
    }
    p->offset = va;
    return NVE_OK;
}

static int32_t as_get_va_regions(nv_file *f, nv_args *a)
{
    (void)f;
    get_va_regions *p = (get_va_regions *)(void *)a->data;
    uint32_t entries = p->buffer_size / (uint32_t)sizeof(va_region);
    if (entries > 2)
        entries = 2;
    for (uint32_t i = 0; i < entries; i++) {
        p->regions[i].page_size = page_sizes[i];
        p->regions[i].offset = region_start[i];
        p->regions[i].pages = (region_limit[i] - region_start[i]) / page_sizes[i];
        p->regions[i].padding = 0;
    }
    p->buffer_size = 2 * (uint32_t)sizeof(va_region);
    return NVE_OK;
}

static int32_t as_initialize_ex(nv_file *f, nv_args *a)
{
    (void)f;
    (void)a;
    return NVE_OK;
}

static int32_t as_remap(nv_file *f, nv_args *a)
{
    as_priv *as = priv_of(f);
    uint32_t count = a->size / (uint32_t)sizeof(remap_entry);
    for (uint32_t i = 0; i < count; i++) {
        remap_entry e;
        memcpy(&e, a->data + (size_t)i * sizeof e, sizeof e);
        uint64_t gpu_va = (uint64_t)e.gpu_offset << 16;
        uint64_t size = (uint64_t)e.pages << 16;
        if (e.nvmap_handle == 0) {
            for (uint32_t k = 0; k < as->maps.n;) {
                as_range *m = &as->maps.v[k];
                if (m->start >= gpu_va && m->start - gpu_va < size)
                    list_remove(&as->maps, m);
                else
                    k++;
            }
            continue;
        }
        nv_map *map = nv_map_get(e.nvmap_handle);
        if (!map)
            return NVE_INVALID_INPUT;
        as_range *old = list_exact(&as->maps, gpu_va);
        if (old)
            list_remove(&as->maps, old);
        as_range r = { gpu_va, size, map->addr + ((uint64_t)e.map_offset << 16), false };
        if (!list_add(&as->maps, r))
            return NVE_OUT_OF_MEMORY;
    }
    return NVE_OK;
}

static int32_t as_open(nv_file *f)
{
    as_priv *as = calloc(1, sizeof *as);
    if (!as)
        return NVE_OUT_OF_MEMORY;
    nv_va_init(&as->va, NV_PAGE, AS_SPACE_END);
    f->priv = as;
    return NVE_OK;
}

static void as_close(nv_file *f)
{
    as_priv *as = priv_of(f);
    nv_va_destroy(&as->va);
    free(as->maps.v);
    free(as->resv.v);
    free(as);
    f->priv = NULL;
}

#define NA(nr, var, stub, min, name, fn) { NV_MAGIC_GPU_AS, (nr), (var), (stub), (min), (name), (fn) }
#define V1 NV_V_IOCTL
#define V13 (NV_V_IOCTL | NV_V_IOCTL3)
static const nv_ioctl_entry as_ioctls[] = {
    NA(0x01, V1, false, 4, "BindChannel", as_bind_channel),
    NA(0x02, V1, false, 24, "AllocSpace", as_alloc_space),
    NA(0x03, V1, false, 16, "FreeSpace", as_free_space),
    NA(0x05, V1, false, 8, "UnmapBuffer", as_unmap_buffer),
    NA(0x06, V1, false, 40, "MapBufferEx", as_map_buffer_ex),
    NA(0x08, V13, false, 0x40, "GetVaRegions", as_get_va_regions),
    NA(0x09, V1, true, 40, "InitializeEx", as_initialize_ex),
    NA(0x14, V1, false, 0, "Remap", as_remap),
};
#undef NA
#undef V1
#undef V13

const nv_dev_class nv_dev_as_gpu = {
    .path = "/dev/nvhost-as-gpu",
    .ioctls = as_ioctls,
    .nioctls = sizeof as_ioctls / sizeof as_ioctls[0],
    .open = as_open,
    .close = as_close,
};
