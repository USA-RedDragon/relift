/**
 * @file tg.c
 * @brief The i386 core of the GPU-API forwarding (tg.h).
 */
#include "tg.h"

#include "hostlib.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t tg_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local tg_call *tg_cur;

void tg_begin(tg_call *k, rl_ctx *c, uint8_t *mem)
{
    k->prev = tg_cur;
    tg_cur = k;
    k->c = c;
    k->mem = mem;
    k->esp = tg_i386_esp(c);
    k->used = 0;
    k->nspill = 0;
}

uint64_t tg_end(tg_call *k, uint32_t pop)
{
    for (unsigned i = 0; i < k->nspill; i++)
        free(k->spill[i]);
    k->nspill = 0;
    tg_cur = k->prev;
    return tg_i386_return(k->c, k->mem, pop);
}

void *tg_alloc(tg_call *k, size_t n)
{
    size_t r = (n + 15) & ~(size_t)15;
    if (r == 0)
        r = 16;
    if (k->used + r <= TG_ARENA) {
        void *p = k->arena + k->used;
        k->used += r;
        memset(p, 0, n);
        return p;
    }
    if (k->nspill == sizeof k->spill / sizeof k->spill[0])
        tg_fail("tg_alloc", "too many large conversions in one call");
    void *p = calloc(1, r);
    if (!p)
        tg_fail("tg_alloc", "out of memory");
    k->spill[k->nspill++] = p;
    return p;
}

tg_call *tg_current(const char *fn)
{
    if (!tg_cur)
        tg_fail(fn, "a callback on a thread that is not in a guest call");
    return tg_cur;
}

void *tg_cb_bind(uint32_t *fns, void *const *tramps, unsigned n, uint32_t g, const char *sig)
{
    if (!g)
        return NULL;
    if (tg_is_handle(g))
        return tg_unhandle(g, sig);
    pthread_mutex_lock(&tg_lock);
    unsigned free_slot = n;
    for (unsigned i = 0; i < n; i++) {
        if (fns[i] == g) {
            pthread_mutex_unlock(&tg_lock);
            return tramps[i];
        }
        if (!fns[i] && free_slot == n)
            free_slot = i;
    }
    if (free_slot == n)
        tg_fail(sig, "too many guest callbacks of one signature");
    fns[free_slot] = g;
    pthread_mutex_unlock(&tg_lock);
    return tramps[free_slot];
}

uint32_t tg_cb_guest(const uint32_t *fns, void *const *tramps, unsigned n, const void *h)
{
    for (unsigned i = 0; i < n; i++)
        if (tramps[i] == h)
            return fns[i];
    return 0;
}

void *tg_cb_bind_host(uint32_t g, void *const *fns, const uint32_t *at, unsigned n)
{
    for (unsigned i = 0; g && i < n; i++)
        if (at[i] == g)
            return fns[i];
    return NULL;
}

uint32_t tg_cb_guest_host(const void *h, void **fns, uint32_t *at, const rl_fn *thunks, unsigned n, const char *sig)
{
    if (!h)
        return 0;
    pthread_mutex_lock(&tg_lock);
    unsigned free_slot = n;
    for (unsigned i = 0; i < n; i++) {
        if (fns[i] == h) {
            pthread_mutex_unlock(&tg_lock);
            return at[i];
        }
        if (!fns[i] && free_slot == n)
            free_slot = i;
    }
    if (free_slot == n)
        tg_fail(sig, "too many host functions of one signature held by the guest");
    if (!at[free_slot])
        at[free_slot] = tg_guest_slot(thunks[free_slot]);
    fns[free_slot] = (void *)h;
    uint32_t g = at[free_slot];
    pthread_mutex_unlock(&tg_lock);
    return g;
}

_Noreturn void tg_fail(const char *fn, const char *what)
{
    fprintf(stderr, "relift: thunk %s: %s\n", fn, what);
    abort();
}

uint32_t tg_unptr(tg_call *k, const void *h, const char *fn)
{
    if (!h)
        return 0;
    uintptr_t d = (uintptr_t)h - (uintptr_t)k->mem;
    if ((uintptr_t)h < (uintptr_t)k->mem || d > UINT32_MAX)
        tg_fail(fn, "the host returned a pointer outside guest memory");
    return (uint32_t)d;
}

/** @brief One entry of a pointer-keyed table: a host pointer and its guest value. */
typedef struct tg_pair {
    const void *h;
    uint32_t g;
} tg_pair;

/** @brief A growable table of tg_pair, searched linearly (tables stay small). */
typedef struct tg_table {
    tg_pair *v;
    size_t n, cap;
} tg_table;

static tg_table tg_handles;

static tg_pair *tg_find_h(tg_table *t, const void *h)
{
    for (size_t i = 0; i < t->n; i++)
        if (t->v[i].h == h)
            return &t->v[i];
    return NULL;
}

static void tg_add(tg_table *t, const void *h, uint32_t g)
{
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 64;
        tg_pair *v = realloc(t->v, cap * sizeof *v);
        if (!v)
            tg_fail("tg_add", "out of memory");
        t->v = v;
        t->cap = cap;
    }
    t->v[t->n++] = (tg_pair){h, g};
}

uint32_t tg_str_out(tg_call *k, const char *h)
{
    return h ? tg_copy_out(k, h, strlen(h) + 1) : 0;
}

/** @brief A guest copy of host data: the host address, the copy and its capacity. */
typedef struct tg_copy {
    const void *h;
    uint32_t g;
    size_t cap;
} tg_copy;

static tg_copy *tg_copies;
static size_t tg_ncopy, tg_capcopy;


uint32_t tg_copy_out(tg_call *k, const void *h, size_t n)
{
    if (!h)
        return 0;
    uintptr_t d = (uintptr_t)h - (uintptr_t)k->mem;
    if ((uintptr_t)h >= (uintptr_t)k->mem && d <= UINT32_MAX)
        return (uint32_t)d;
    pthread_mutex_lock(&tg_lock);
    tg_copy *c = NULL;
    for (size_t i = 0; i < tg_ncopy; i++)
        if (tg_copies[i].h == h)
            c = &tg_copies[i];
    if (!c) {
        if (tg_ncopy == tg_capcopy) {
            size_t cap = tg_capcopy ? tg_capcopy * 2 : 64;
            tg_copy *v = realloc(tg_copies, cap * sizeof *v);
            if (!v)
                tg_fail("tg_copy_out", "out of memory");
            tg_copies = v;
            tg_capcopy = cap;
        }
        c = &tg_copies[tg_ncopy++];
        *c = (tg_copy){h, 0, 0};
    }
    if (n > c->cap) {
        size_t cap = n < 64 ? 64 : n;
        c->g = tg_guest_alloc(cap);
        if (!c->g)
            tg_fail("tg_copy_out", "out of guest memory");
        c->cap = cap;
    }
    memcpy(k->mem + c->g, h, n);
    uint32_t g = c->g;
    pthread_mutex_unlock(&tg_lock);
    return g;
}

/**
 * @brief Tokens are addresses inside guest memory reserved for them, so no
 * token is ever also a guest function or object address: TG_TOKENS_PER
 * tokens 16 bytes apart per block.
 */
#define TG_TOKENS_PER 4096u

static uint32_t *tg_token_blocks;
static size_t tg_ntoken_blocks;

/** @brief Token i; with tg_lock held. */
static uint32_t tg_token(size_t i)
{
    size_t b = i / TG_TOKENS_PER;
    if (b == tg_ntoken_blocks) {
        uint32_t *v = realloc(tg_token_blocks, (b + 1) * sizeof *v);
        uint32_t g = tg_guest_alloc(16u * TG_TOKENS_PER);
        if (!v || !g)
            tg_fail("tg_handle", "out of memory");
        v[b] = g;
        tg_token_blocks = v;
        tg_ntoken_blocks = b + 1;
    }
    return tg_token_blocks[b] + 16u * (uint32_t)(i % TG_TOKENS_PER);
}

/** @brief The index of token t, or SIZE_MAX when t is none; with tg_lock held. */
static size_t tg_token_index(uint32_t t)
{
    for (size_t b = 0; b < tg_ntoken_blocks; b++) {
        uint32_t d = t - tg_token_blocks[b];
        if (t >= tg_token_blocks[b] && d < 16u * TG_TOKENS_PER && d % 16 == 0) {
            size_t i = b * TG_TOKENS_PER + d / 16;
            return i < tg_handles.n ? i : SIZE_MAX;
        }
    }
    return SIZE_MAX;
}

uint32_t tg_handle(const void *h)
{
    if (!h)
        return 0;
    pthread_mutex_lock(&tg_lock);
    tg_pair *p = tg_find_h(&tg_handles, h);
    uint32_t g = p ? p->g : tg_token(tg_handles.n);
    if (!p)
        tg_add(&tg_handles, h, g);
    pthread_mutex_unlock(&tg_lock);
    return g;
}

int tg_is_handle(uint32_t t)
{
    pthread_mutex_lock(&tg_lock);
    int ok = tg_token_index(t) != SIZE_MAX;
    pthread_mutex_unlock(&tg_lock);
    return ok;
}

void *tg_unhandle(uint32_t t, const char *fn)
{
    if (!t)
        return NULL;
    pthread_mutex_lock(&tg_lock);
    size_t i = tg_token_index(t);
    void *h = i != SIZE_MAX ? (void *)tg_handles.v[i].h : NULL;
    pthread_mutex_unlock(&tg_lock);
    if (!h)
        tg_fail(fn, "an unknown handle");
    return h;
}

void *tg_dlsym(const char *soname, const char *name, const char *lookup)
{
    const char *const names[] = { soname, NULL };
    void *lib = rl_hostlib_open(names);
    if (!lib)
        return NULL;
    void *f = rl_hostlib_sym(lib, name);
    if (!f && lookup) {
        void *(*get)(const unsigned char *) = (void *(*)(const unsigned char *))rl_hostlib_sym(lib, lookup);
        if (get)
            f = get((const unsigned char *)name);
    }
    return f;
}

uint64_t tg_guest_libc_in(const char *soname, const char *name, const uint32_t *slots, unsigned n)
{
    tg_call *k = tg_current(name);
    uint32_t f = tg_guest_symbol(soname, name);
    if (!f)
        tg_fail(name, "missing in the guest's lifted libraries");
    uint32_t hi = 0;
    uint32_t lo = tg_i386_call_guest(k->c, k->mem, f, slots, n, &hi);
    return (uint64_t)hi << 32 | lo;
}
