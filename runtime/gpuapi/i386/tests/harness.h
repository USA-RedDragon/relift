/**
 * @file harness.h
 * @brief A stand-in for the x86 CPU layer and the guest memory of the i386
 * Linux guest, for testing generated thunks on the host: a guest window,
 * a bump allocator in it, the tg_i386_* register functions over a small
 * register file, and cdecl calls framed on a guest stack.
 */
#include "../tg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/** @brief The registers a thunk touches. */
struct rl_ctx {
    uint32_t esp, eax, edx;
    uint8_t st0[10];
    int x87_depth;
};

/** @brief Guest window size; guest address 0 stays unused (NULL). */
#define WINDOW (64u << 20)
/** @brief Top of the guest stack. */
#define STACK_TOP (WINDOW - 4096)
/** @brief The return address calls use; a thunk must return to it. */
#define RET_SENTINEL 0x0badc0deu

static uint8_t *mem;
static uint32_t brk_next = 0x10000;
static struct rl_ctx ctx;

uint32_t tg_i386_esp(rl_ctx *c) { return c->esp; }
void tg_i386_set_eax(rl_ctx *c, uint32_t v) { c->eax = v; }
void tg_i386_set_edx(rl_ctx *c, uint32_t v) { c->edx = v; }

void tg_i386_push_x87(rl_ctx *c, const uint8_t v[10])
{
    memcpy(c->st0, v, 10);
    c->x87_depth++;
}

uint64_t tg_i386_return(rl_ctx *c, uint8_t *m, uint32_t pop)
{
    uint32_t ret;
    memcpy(&ret, m + c->esp, 4);
    c->esp += 4 + pop;
    return ret;
}

/** @brief A stand-in for a lifted guest function: reads its cdecl arguments at esp + 4 and returns eax. */
typedef uint32_t harness_guest_fn(rl_ctx *c, uint8_t *m);

/** @brief Guest address of the first stand-in function; each takes 16 bytes. */
#define GUEST_CODE 0x1000u

static harness_guest_fn *guest_fns[64];
static unsigned guest_nfns;

/** @brief Gives fn a guest address that tg_i386_call_guest runs it at. */
static inline uint32_t harness_guest(harness_guest_fn *fn)
{
    guest_fns[guest_nfns] = fn;
    return GUEST_CODE + 16 * guest_nfns++;
}

/** @brief The 4-byte argument slot i of the running stand-in. */
static inline uint32_t harness_arg(rl_ctx *c, uint8_t *m, unsigned i)
{
    uint32_t v;
    memcpy(&v, m + c->esp + 4 + 4 * i, 4);
    return v;
}

/** @brief Guest address of the first thunk slot (tg_guest_slot); each takes 16 bytes. */
#define THUNK_SLOTS 0x8000u

static rl_fn slot_fns[512];
static unsigned slot_n;

uint32_t tg_guest_slot(rl_fn fn)
{
    for (unsigned i = 0; i < slot_n; i++)
        if (slot_fns[i] == fn)
            return THUNK_SLOTS + 16 * i;
    if (slot_n == sizeof slot_fns / sizeof slot_fns[0]) {
        fprintf(stderr, "harness: too many thunk slots\n");
        exit(2);
    }
    slot_fns[slot_n] = fn;
    return THUNK_SLOTS + 16 * slot_n++;
}

/** @brief The wrapped libraries tg_guest_symbol finds, and stand-ins for the guest's C library. */
static const tg_lib *harness_libs[8];
static unsigned harness_nlibs;
static struct {
    const char *soname;
    const char *name;
    uint32_t at;
} harness_libc[32];
static unsigned harness_nlibc;

uint32_t tg_guest_symbol(const char *soname, const char *name)
{
    for (unsigned l = 0; l < harness_nlibs; l++)
        if (strcmp(harness_libs[l]->soname, soname) == 0)
            for (uint32_t i = 0; i < harness_libs[l]->count; i++)
                if (strcmp(harness_libs[l]->fns[i].name, name) == 0)
                    return tg_guest_slot(harness_libs[l]->fns[i].thunk);
    for (unsigned i = 0; i < harness_nlibc; i++)
        if (strcmp(harness_libc[i].name, name) == 0 &&
            strcmp(harness_libc[i].soname ? harness_libc[i].soname : "libc.so.6", soname) == 0)
            return harness_libc[i].at;
    return 0;
}

uint32_t tg_i386_call_guest(rl_ctx *c, uint8_t *m, uint32_t fn, const uint32_t *slots, unsigned n, uint32_t *edx)
{
    if (fn >= THUNK_SLOTS && fn < THUNK_SLOTS + 16 * slot_n && (fn - THUNK_SLOTS) % 16 == 0) {
        uint32_t saved = c->esp;
        c->esp = ((c->esp - 4 * n) & ~15u) - 4;
        uint32_t ret = RET_SENTINEL;
        memcpy(m + c->esp, &ret, 4);
        if (n)
            memcpy(m + c->esp + 4, slots, 4 * n);
        uint64_t back = slot_fns[(fn - THUNK_SLOTS) / 16](c, m);
        if (back != RET_SENTINEL) {
            fprintf(stderr, "harness: a thunk slot returned to %llx\n", (unsigned long long)back);
            exit(2);
        }
        c->esp = saved;
        if (edx)
            *edx = c->edx;
        return c->eax;
    }
    unsigned i = (fn - GUEST_CODE) / 16;
    if (fn < GUEST_CODE || (fn - GUEST_CODE) % 16 || i >= guest_nfns) {
        fprintf(stderr, "harness: call to unknown guest function %x\n", fn);
        exit(2);
    }
    uint32_t saved = c->esp;
    c->esp = ((c->esp - 4 * n) & ~15u) - 4;
    uint32_t ret = RET_SENTINEL;
    memcpy(m + c->esp, &ret, 4);
    if (n)
        memcpy(m + c->esp + 4, slots, 4 * n);
    c->edx = 0;
    uint32_t r = guest_fns[i](c, m);
    c->esp = saved;
    if (edx)
        *edx = c->edx;
    return r;
}

uint32_t tg_guest_alloc(size_t n)
{
    uint32_t a = (brk_next + 15) & ~15u;
    if ((uint64_t)a + n > STACK_TOP - (1u << 20)) {
        fprintf(stderr, "harness: guest window full\n");
        exit(2);
    }
    brk_next = a + (uint32_t)n;
    memset(mem + a, 0, n);
    return a;
}

uint8_t *tg_mem_base(void)
{
    return mem;
}

void tg_guest_free(uint32_t g, size_t n)
{
    memset(mem + g, 0xa5, n);
}

typedef uint32_t gaddr;

static inline gaddr galloc(size_t n)
{
    return tg_guest_alloc(n);
}

static inline uint8_t *gp(gaddr a)
{
    return mem + a;
}

/** @brief Maps the guest window. */
static void harness_init(void)
{
    mem = mmap(NULL, WINDOW, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }
}

/** @brief Runs the thunk of name in lib with n i386 cdecl argument slots; returns eax. */
static uint32_t harness_call(const tg_lib *lib, const char *name, const uint32_t *args, unsigned n)
{
    rl_fn fn = NULL;
    for (uint32_t i = 0; i < lib->count; i++)
        if (strcmp(lib->fns[i].name, name) == 0)
            fn = lib->fns[i].thunk;
    if (!fn) {
        fprintf(stderr, "harness: no thunk %s\n", name);
        exit(2);
    }
    uint32_t esp = ((STACK_TOP - 4 * n) & ~15u) - 4;
    uint32_t ret = RET_SENTINEL;
    memcpy(mem + esp, &ret, 4);
    if (n)
        memcpy(mem + esp + 4, args, 4 * n);
    ctx.esp = esp;
    ctx.eax = 0xcccccccc;
    uint64_t back = fn(&ctx, mem);
    if (back != RET_SENTINEL || ctx.esp != esp + 4) {
        fprintf(stderr, "harness: %s returned to %llx with esp %x (want %x)\n", name,
                (unsigned long long)back, ctx.esp, esp + 4);
        exit(2);
    }
    return ctx.eax;
}
