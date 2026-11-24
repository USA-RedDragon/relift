/**
 * @file gl_hand.c
 * @brief Hand-written GL pieces: buffer offsets and buffer mapping.
 *
 * A pointer argument of a vertex array, index or pixel command is a buffer
 * offset while a buffer is bound to its target, and a client pointer
 * otherwise; only the GL state tells which, so tg_gl_offset asks it.
 *
 * Buffer mapping: the host maps a buffer in
 * its own memory, outside the guest window, so glMapBuffer gives the guest
 * a copy in guest memory (filled from the mapping unless the access is
 * write-only), glUnmapBuffer writes the copy back (unless read-only) and
 * frees it, and glGetBufferPointerv answers with the copy's address.
 */
#include "tg.h"

#include <pthread.h>

/** @brief A GL pointer argument: a buffer offset while a buffer is bound to binding, else a guest pointer. */
const void *tg_gl_offset(tg_call *k, uint32_t g, uint32_t binding);
/** @brief A pointer GL gives back: a guest address, or a buffer offset as it is. */
uint32_t tg_gl_unptr(tg_call *k, const void *h, const char *fn);

/** @brief The GL constants these thunks use. */
enum {
    TG_GL_READ_ONLY = 0x88B8,
    TG_GL_WRITE_ONLY = 0x88B9,
    TG_GL_BUFFER_SIZE = 0x8764,
    TG_GL_BUFFER_MAP_POINTER = 0x88BD,
};

/** @brief One mapped buffer. */
typedef struct tg_gl_map {
    /** @brief The buffer object name, 0 for a free entry. */
    uint32_t name;
    /** @brief The access mapBuffer was given. */
    uint32_t access;
    /** @brief The host mapping. */
    uint8_t *host;
    /** @brief The guest copy. */
    uint32_t guest;
    /** @brief Its size. */
    uint32_t size;
} tg_gl_map;

static tg_gl_map tg_gl_maps[64];
static pthread_mutex_t tg_gl_lock = PTHREAD_MUTEX_INITIALIZER;

/** @brief The host GL function name. */
static void *tg_gl_fn(const char *name)
{
    void *f = tg_dlsym("libGL.so.1", name, "glXGetProcAddressARB");
    if (!f)
        tg_fail(name, "missing in the host library libGL.so.1");
    return f;
}

/** @brief The binding query of a buffer target, or 0 for one GL 2.1 does not map. */
static uint32_t tg_gl_binding(uint32_t target)
{
    switch (target) {
    case 0x8892: return 0x8894;
    case 0x8893: return 0x8895;
    case 0x88EB: return 0x88ED;
    case 0x88EC: return 0x88EF;
    default: return 0;
    }
}

/** @brief The buffer bound to target; 0 for none. */
static uint32_t tg_gl_bound(uint32_t target)
{
    uint32_t b = tg_gl_binding(target);
    int v = 0;
    if (b)
        ((void (*)(unsigned, int *))tg_gl_fn("glGetIntegerv"))(b, &v);
    return (uint32_t)v;
}

const void *tg_gl_offset(tg_call *k, uint32_t g, uint32_t binding)
{
    int v = 0;
    ((void (*)(unsigned, int *))tg_gl_fn("glGetIntegerv"))(binding, &v);
    return v ? (const void *)(uintptr_t)g : tg_ptr(k, g);
}

uint32_t tg_gl_unptr(tg_call *k, const void *h, const char *fn)
{
    uintptr_t d = (uintptr_t)h - (uintptr_t)k->mem;
    if ((uintptr_t)h >= (uintptr_t)k->mem && d <= UINT32_MAX)
        return (uint32_t)d;
    if ((uintptr_t)h <= UINT32_MAX)
        return (uint32_t)(uintptr_t)h;
    tg_fail(fn, "GL gave a pointer outside guest memory");
}

/** @brief The entry of buffer name (a free one for 0); NULL when there is none. */
static tg_gl_map *tg_gl_find(uint32_t name)
{
    for (size_t i = 0; i < sizeof tg_gl_maps / sizeof tg_gl_maps[0]; i++)
        if (tg_gl_maps[i].name == name)
            return &tg_gl_maps[i];
    return NULL;
}

uint64_t tgh_glMapBuffer(rl_ctx *c, uint8_t *mem)
{
    tg_call kk, *k = &kk;
    tg_begin(k, c, mem);
    uint32_t target = tg_arg32(k, 0), access = tg_arg32(k, 4);
    void *p = ((void *(*)(unsigned, unsigned))tg_gl_fn("glMapBuffer"))(target, access);
    uint32_t g = 0;
    if (p) {
        int size = 0;
        ((void (*)(unsigned, unsigned, int *))tg_gl_fn("glGetBufferParameteriv"))(target, TG_GL_BUFFER_SIZE, &size);
        uint32_t name = tg_gl_bound(target);
        pthread_mutex_lock(&tg_gl_lock);
        tg_gl_map *m = tg_gl_find(0);
        if (!name)
            tg_fail("glMapBuffer", "a mapped target with no bound buffer");
        if (!m)
            tg_fail("glMapBuffer", "too many mapped buffers");
        g = tg_guest_alloc(size > 0 ? (size_t)size : 1);
        if (!g)
            tg_fail("glMapBuffer", "out of guest memory");
        if (access != TG_GL_WRITE_ONLY && size > 0)
            memcpy(mem + g, p, (size_t)size);
        *m = (tg_gl_map){name, access, p, g, (uint32_t)(size > 0 ? size : 0)};
        pthread_mutex_unlock(&tg_gl_lock);
    }
    tg_i386_set_eax(c, g);
    return tg_end(k, 0);
}

uint64_t tgh_glUnmapBuffer(rl_ctx *c, uint8_t *mem)
{
    tg_call kk, *k = &kk;
    tg_begin(k, c, mem);
    uint32_t target = tg_arg32(k, 0);
    pthread_mutex_lock(&tg_gl_lock);
    uint32_t bound = tg_gl_bound(target);
    tg_gl_map *m = bound ? tg_gl_find(bound) : NULL;
    tg_gl_map done = m ? *m : (tg_gl_map){0};
    if (m)
        m->name = 0;
    pthread_mutex_unlock(&tg_gl_lock);
    if (done.name && done.access != TG_GL_READ_ONLY)
        memcpy(done.host, mem + done.guest, done.size);
    unsigned char r = ((unsigned char (*)(unsigned))tg_gl_fn("glUnmapBuffer"))(target);
    if (done.name)
        tg_guest_free(done.guest, done.size ? done.size : 1);
    tg_i386_set_eax(c, r);
    return tg_end(k, 0);
}

uint64_t tgh_glGetBufferPointerv(rl_ctx *c, uint8_t *mem)
{
    tg_call kk, *k = &kk;
    tg_begin(k, c, mem);
    uint32_t target = tg_arg32(k, 0), pname = tg_arg32(k, 4), out = tg_arg32(k, 8);
    void *hp = NULL;
    ((void (*)(unsigned, unsigned, void **))tg_gl_fn("glGetBufferPointerv"))(target, pname, &hp);
    uint32_t g = 0;
    if (pname == TG_GL_BUFFER_MAP_POINTER && hp) {
        pthread_mutex_lock(&tg_gl_lock);
        uint32_t bound = tg_gl_bound(target);
        tg_gl_map *m = bound ? tg_gl_find(bound) : NULL;
        g = m ? m->guest : 0;
        pthread_mutex_unlock(&tg_gl_lock);
        if (!m)
            tg_fail("glGetBufferPointerv", "a buffer mapped outside the thunks");
    }
    if (out && (pname == TG_GL_BUFFER_MAP_POINTER))
        tg_put(k, out, &g, 4);
    return tg_end(k, 0);
}
