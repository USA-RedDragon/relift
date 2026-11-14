/**
 * @file srv_nv_va.c
 * @brief A free-range allocator for GPU and host1x virtual addresses.
 *
 * The behavior follows Ryujinx (MIT, Copyright (c) Ryujinx Team and
 * Contributors) NvMemoryAllocator: a sorted set of free
 * ranges, allocation searches from a start address upward and then wraps to
 * the ranges below it, freeing merges adjacent ranges.
 */
#include "nv.h"

#include <stdlib.h>
#include <string.h>

static uint64_t align_up(uint64_t v, uint64_t a)
{
    uint64_t r = v % a;
    return r ? v + (a - r) : v;
}

static bool reserve(nv_va *a, uint32_t want)
{
    if (a->cap >= want)
        return true;
    uint32_t cap = a->cap ? a->cap * 2 : 8;
    while (cap < want)
        cap *= 2;
    struct nv_va_range *p = realloc(a->free, cap * sizeof *p);
    if (!p)
        return false;
    a->free = p;
    a->cap = cap;
    return true;
}

void nv_va_init(nv_va *a, uint64_t lo, uint64_t hi)
{
    memset(a, 0, sizeof *a);
    a->lo = lo;
    a->hi = hi;
    if (reserve(a, 1)) {
        a->free[0].start = lo;
        a->free[0].end = hi;
        a->n = 1;
    }
}

void nv_va_destroy(nv_va *a)
{
    free(a->free);
    memset(a, 0, sizeof *a);
}

static bool fit(const struct nv_va_range *r, uint64_t from, uint64_t size, uint64_t align, uint64_t *out)
{
    uint64_t c = align_up(from > r->start ? from : r->start, align);
    if (c < r->start || c > r->end || size > r->end - c)
        return false;
    *out = c;
    return true;
}

bool nv_va_find(const nv_va *a, uint64_t size, uint64_t align, uint64_t start, uint64_t *out)
{
    if (align == 0)
        align = 1;
    align = align_up(align, NV_PAGE);
    for (uint32_t i = 0; i < a->n; i++)
        if (a->free[i].end > start && fit(&a->free[i], start, size, align, out))
            return true;
    for (uint32_t i = 0; i < a->n; i++)
        if (a->free[i].start < start && fit(&a->free[i], 0, size, align, out))
            return true;
    return false;
}

bool nv_va_is_free(const nv_va *a, uint64_t va, uint64_t size)
{
    if (size > UINT64_MAX - va)
        return false;
    for (uint32_t i = 0; i < a->n; i++)
        if (a->free[i].start <= va && va + size <= a->free[i].end)
            return true;
    return false;
}

bool nv_va_take(nv_va *a, uint64_t va, uint64_t size)
{
    if (size == 0)
        return true;
    for (uint32_t i = 0; i < a->n; i++) {
        struct nv_va_range r = a->free[i];
        if (va < r.start || va >= r.end || size > r.end - va)
            continue;
        uint64_t end = va + size;
        bool left = va > r.start;
        bool right = end < r.end;
        if (left && right) {
            if (!reserve(a, a->n + 1))
                return false;
            memmove(&a->free[i + 2], &a->free[i + 1], (a->n - i - 1) * sizeof *a->free);
            a->free[i].end = va;
            a->free[i + 1].start = end;
            a->free[i + 1].end = r.end;
            a->n++;
        } else if (left) {
            a->free[i].end = va;
        } else if (right) {
            a->free[i].start = end;
        } else {
            memmove(&a->free[i], &a->free[i + 1], (a->n - i - 1) * sizeof *a->free);
            a->n--;
        }
        return true;
    }
    return false;
}

void nv_va_put(nv_va *a, uint64_t va, uint64_t size)
{
    if (size == 0 || size > UINT64_MAX - va)
        return;
    uint64_t s = va, e = va + size;
    if (s < a->lo)
        s = a->lo;
    if (e > a->hi)
        e = a->hi;
    if (s >= e)
        return;
    uint32_t i = 0;
    while (i < a->n && a->free[i].end < s)
        i++;
    uint32_t j = i;
    while (j < a->n && a->free[j].start <= e) {
        if (a->free[j].start < s)
            s = a->free[j].start;
        if (a->free[j].end > e)
            e = a->free[j].end;
        j++;
    }
    uint32_t removed = j - i;
    if (removed == 0) {
        if (!reserve(a, a->n + 1))
            return;
        memmove(&a->free[i + 1], &a->free[i], (a->n - i) * sizeof *a->free);
        a->n++;
    } else if (removed > 1) {
        memmove(&a->free[i + 1], &a->free[j], (a->n - j) * sizeof *a->free);
        a->n -= removed - 1;
    }
    a->free[i].start = s;
    a->free[i].end = e;
}
