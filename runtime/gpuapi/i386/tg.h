/**
 * @file tg.h
 * @brief The i386 core of the GPU-API forwarding (relift-vkgen and the
 * hand-written OpenGL, GLX, EGL and Vulkan thunks): the per-call state,
 * guest memory access, pointer, string and handle translation, callback
 * pools and host library binding.
 *
 * A thunk is an rl_fn: it reads its i386 cdecl arguments from the guest
 * stack, converts them to the host's LP64 types, calls the host library,
 * converts the results back and returns through the CPU layer. The guest
 * register file belongs to the CPU layer, so thunks reach it only through
 * the tg_i386_* functions declared here, which the CPU layer (or a test
 * harness) defines.
 */
#ifndef RELIFT_TG_H
#define RELIFT_TG_H

#include <relift/abi_core.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** @brief The guest stack pointer (esp). */
uint32_t tg_i386_esp(rl_ctx *c);
/** @brief Sets eax. */
void tg_i386_set_eax(rl_ctx *c, uint32_t v);
/** @brief Sets edx. */
void tg_i386_set_edx(rl_ctx *c, uint32_t v);
/**
 * @brief Returns from the thunk the way `ret` (pop = 0) or `ret $pop`
 * does: pops the return address and pop more bytes, and gives what the
 * lifted function would return.
 */
uint64_t tg_i386_return(rl_ctx *c, uint8_t *mem, uint32_t pop);
/**
 * @brief Calls the guest function at guest address fn with n 4-byte cdecl
 * argument slots, on the guest stack below the current esp, and runs it
 * until it returns; gives eax, and edx in *edx when edx is not NULL. esp is
 * the same afterwards.
 */
uint32_t tg_i386_call_guest(rl_ctx *c, uint8_t *mem, uint32_t fn, const uint32_t *slots, unsigned n, uint32_t *edx);
/** @brief Allocates n zeroed bytes of guest memory, 16-aligned; 0 when out of memory. */
uint32_t tg_guest_alloc(size_t n);
/** @brief Frees what tg_guest_alloc gave (n is the size asked for). */
void tg_guest_free(uint32_t g, size_t n);

/** @brief Host bytes a call converts into before the arena spills to the heap. */
#define TG_ARENA 4096

/** @brief The state of one thunk call. */
typedef struct tg_call {
    /** @brief The guest context. */
    rl_ctx *c;
    /** @brief Guest memory base. */
    uint8_t *mem;
    /** @brief esp at entry (the return address). */
    uint32_t esp;
    /** @brief Bytes of the arena in use. */
    size_t used;
    /** @brief Heap blocks this call allocated beyond the arena. */
    void *spill[16];
    /** @brief Number of spill blocks. */
    unsigned nspill;
    /** @brief The call this one runs inside of on the same thread (a thunk called from a callback). */
    struct tg_call *prev;
    /** @brief Converted values for this call. */
    _Alignas(16) uint8_t arena[TG_ARENA];
} tg_call;

/** @brief A wrapped function: its name and its thunk. */
typedef struct tg_fn {
    /** @brief The symbol name. */
    const char *name;
    /** @brief The thunk. */
    rl_fn thunk;
} tg_fn;

/** @brief A forwarded GPU-API library: the soname the guest links and its functions. */
typedef struct tg_lib {
    /** @brief The soname. */
    const char *soname;
    /** @brief Number of functions. */
    uint32_t count;
    /** @brief The functions, sorted by name. */
    const tg_fn *fns;
    /** @brief Loads the host library and binds its functions; the guest layer calls it once before the guest runs. */
    void (*bind)(void);
} tg_lib;

/** @brief Starts a call. */
void tg_begin(tg_call *k, rl_ctx *c, uint8_t *mem);
/** @brief Ends a call: frees its spill and returns through the CPU layer, popping pop bytes after the return address. */
uint64_t tg_end(tg_call *k, uint32_t pop);
/** @brief The innermost thunk call running on this thread; stops the program when there is none (fn names the callback). */
tg_call *tg_current(const char *fn);
/**
 * @brief The host function for guest function g from a pool of n
 * trampolines of one signature (fns holds each one's guest function): the
 * same g always gets the same trampoline. NULL for 0; a handle token (a
 * host function the guest got back) is that host function.
 */
void *tg_cb_bind(uint32_t *fns, void *const *tramps, unsigned n, uint32_t g, const char *sig);
/**
 * @brief The host function behind guest address g when g is the guest
 * address of one of a reverse pool's thunks (a host function the guest got
 * and passes back), else NULL.
 */
void *tg_cb_bind_host(uint32_t g, void *const *fns, const uint32_t *at, unsigned n);
/**
 * @brief A guest address the guest can call for host function h of one
 * signature: a thunk of the signature's reverse pool, bound to h (the same h
 * keeps its address). 0 for NULL.
 */
uint32_t tg_cb_guest_host(const void *h, void **fns, uint32_t *at, const rl_fn *thunks, unsigned n, const char *sig);
/** @brief A guest address whose call runs thunk fn (the guest layer registers it with its dispatch). */
uint32_t tg_guest_slot(rl_fn fn);
/** @brief The guest address of symbol name of the guest's library soname: the thunk slot for a wrapped library, the lifted code otherwise; 0 when there is none (the guest layer gives it). */
uint32_t tg_guest_symbol(const char *soname, const char *name);
/** @brief The guest function behind trampoline h of a pool, or 0 when h is not one of them. */
uint32_t tg_cb_guest(const uint32_t *fns, void *const *tramps, unsigned n, const void *h);
/** @brief n zeroed bytes, 16-aligned, that live until the call ends. */
void *tg_alloc(tg_call *k, size_t n);
/** @brief Stops the program: a conversion the thunk cannot make exactly. */
_Noreturn void tg_fail(const char *fn, const char *what);

/** @brief Reads the 4-byte argument slot at byte offset off past the return address. */
static inline uint32_t tg_arg32(tg_call *k, uint32_t off)
{
    uint32_t v;
    memcpy(&v, k->mem + (uint32_t)(k->esp + 4 + off), 4);
    return v;
}

/** @brief Reads an 8-byte argument from two slots. */
static inline uint64_t tg_arg64(tg_call *k, uint32_t off)
{
    return (uint64_t)tg_arg32(k, off) | (uint64_t)tg_arg32(k, off + 4) << 32;
}

/** @brief An element count from a guest argument: a negative signed count is 0 (the host call reports the error). */
static inline uint32_t tg_count(uint32_t v)
{
    return (int32_t)v < 0 ? 0 : v;
}

/** @brief Reads n bytes of guest memory. */
static inline void tg_get(tg_call *k, void *dst, uint32_t g, size_t n)
{
    memcpy(dst, k->mem + g, n);
}

/** @brief Writes n bytes of guest memory. */
static inline void tg_put(tg_call *k, uint32_t g, const void *src, size_t n)
{
    memcpy(k->mem + g, src, n);
}

/** @brief Reads a little-endian unsigned integer of n (1, 2, 4 or 8) bytes at host address p. */
static inline uint64_t tg_ld(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    memcpy(&v, p, n);
    return v;
}

/** @brief Writes the low n bytes of v at host address p. */
static inline void tg_st(uint8_t *p, unsigned n, uint64_t v)
{
    memcpy(p, &v, n);
}

/** @brief Sign-extends the low n bytes of v. */
static inline uint64_t tg_sext(uint64_t v, unsigned n)
{
    unsigned s = 64 - 8 * n;
    return n >= 8 ? v : (uint64_t)((int64_t)(v << s) >> s);
}

/** @brief A guest pointer as a host pointer into the guest window; 0 is NULL. */
static inline void *tg_ptr(tg_call *k, uint32_t g)
{
    return g ? k->mem + g : NULL;
}

/** @brief A host pointer into the guest window as a guest pointer; NULL is 0. A pointer outside the window stops the program. */
uint32_t tg_unptr(tg_call *k, const void *h, const char *fn);
/**
 * @brief A host string as a guest pointer: in the window, its guest
 * address; else a guest copy kept per host address and copied again on
 * every return, so a host buffer the library rewrites (an error message)
 * reads the same through the guest copy.
 */
uint32_t tg_str_out(tg_call *k, const char *h);
/** @brief n bytes of host data as a guest pointer, the way tg_str_out copies strings. */
uint32_t tg_copy_out(tg_call *k, const void *h, size_t n);
/** @brief A token for an opaque host pointer (the same pointer, the same token; an address in guest memory reserved for tokens); NULL is 0. */
uint32_t tg_handle(const void *h);
/** @brief True when t is a token tg_handle gave. */
int tg_is_handle(uint32_t t);
/** @brief The host pointer of a token; 0 is NULL, an unknown token stops the program. */
void *tg_unhandle(uint32_t t, const char *fn);
/** @brief The host address of guest memory (the guest layer gives it). */
uint8_t *tg_mem_base(void);
/** @brief Calls function name of the guest's lifted library soname with n argument slots, inside the current thunk call; gives edx:eax. */
uint64_t tg_guest_libc_in(const char *soname, const char *name, const uint32_t *slots, unsigned n);
/**
 * @brief The host function name in library soname, or, when the library
 * does not export it, what the library's lookup function (such as
 * glXGetProcAddressARB; NULL for none) gives; NULL when missing.
 */
void *tg_dlsym(const char *soname, const char *name, const char *lookup);

#endif
