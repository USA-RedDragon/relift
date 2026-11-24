/**
 * @file start.c
 * @brief Process start: the guest address space and its layout, the program's
 * segments, the initial stack (argc, argv, envp and the auxiliary vector), the
 * system call dispatcher and the run of the entry point.
 *
 * Layout, from the bottom of the address space: the program as linked, the
 * heap the break grows into, the area mmap takes from (growing down from
 * the gap below the stack), the main stack at the top. The main stack is the
 * kernel's default of 8 MiB; the stack size the file asks for is the size
 * of a thread's stack, not of the main stack.
 */
#include "hostio.h"
#include "linux_priv.h"
#include "arch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lx_state lx;

#if defined(_WIN32)
#define host_environ _environ
#else
extern char **environ;
#define host_environ environ
#endif

enum {
    MAIN_STACK = 8u << 20,
    STACK_GAP = 256u << 20,
    HEAP_ALIGN = 1u << 20,
    TOP_RESERVE = 1u << 20
};

static uint32_t vm_perm_of(uint32_t p)
{
    switch (p) {
    case RL_LINUX_RW:
        return RL_VM_RW;
    case RL_LINUX_RX:
        return RL_VM_RX;
    case RL_LINUX_RWX:
        return RL_VM_RW | RL_VM_X;
    default:
        return RL_VM_R;
    }
}

static int log_fail(const char *what)
{
    rl_log(RL_LOG_ERROR, "linux", "%s", what);
    return 1;
}

static const rl_linux_module *module_of(const rl_linux_program *p, uint32_t role)
{
    for (uint32_t i = 0; i < p->module_count; i++)
        if (p->modules[i].role == role)
            return &p->modules[i];
    return NULL;
}

/**
 * @brief Maps a file the way the kernel's ELF loader does: each segment's
 * bytes from the file at its address, the rest zero, with the segment's
 * permissions. Returns the end of the mapping, 0 after logging why not.
 */
static uint64_t load_module(const rl_linux_module *m)
{
    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint32_t i = 0; i < m->segment_count; i++) {
        const rl_linux_segment *s = &m->segments[i];
        if (s->file_size > s->mem_size || s->image_offset + s->file_size > m->file_size)
            return log_fail("a segment's bytes lie outside the file"), 0;
        if (s->addr + s->mem_size < s->addr)
            return log_fail("a segment wraps the address space"), 0;
        uint64_t slo = lx_page_down(s->addr), shi = lx_page_up(s->addr + s->mem_size);
        if (slo < lo)
            lo = slo;
        if (shi > hi)
            hi = shi;
    }
    if (m->segment_count == 0 || hi > lx.vm.size / 2)
        return log_fail("the program does not fit the address space"), 0;
    if (rl_vm_set(&lx.vm, lo, hi - lo, RL_VM_RW, 1, RL_VM_FRESH) != 0)
        return log_fail("cannot map the program"), 0;
    for (uint32_t i = 0; i < m->segment_count; i++) {
        const rl_linux_segment *s = &m->segments[i];
        rl_vm_kernel_write(&lx.vm, s->addr, m->file + s->image_offset, s->file_size);
    }
    uint64_t prev_hi = 0;
    uint32_t prev_perm = 0;
    for (uint32_t i = 0; i < m->segment_count; i++) {
        const rl_linux_segment *s = &m->segments[i];
        uint64_t slo = lx_page_down(s->addr), shi = lx_page_up(s->addr + s->mem_size);
        uint32_t perm = vm_perm_of(s->perm);
        if (i > 0 && slo < prev_hi) {
            if (rl_vm_set(&lx.vm, slo, L_PAGE, perm | prev_perm, 1, RL_VM_KEEP) != 0)
                return log_fail("cannot protect the program"), 0;
            slo += L_PAGE;
        }
        if (slo < shi && rl_vm_set(&lx.vm, slo, shi - slo, perm, 1, RL_VM_KEEP) != 0)
            return log_fail("cannot protect the program"), 0;
        prev_hi = shi;
        prev_perm = perm;
    }
    return hi;
}

/**
 * @brief The program as the kernel leaves it: the executable and the program
 * interpreter mapped, the libraries' range reserved for the interpreter to
 * map into, and the heap above the lot.
 */
static int load_image(const rl_linux_program *p)
{
    uint64_t hi = p->code_hi;
    for (uint32_t i = 0; i < p->module_count; i++)
        if ((p->modules[i].role == RL_LINUX_MAIN || p->modules[i].role == RL_LINUX_INTERPRETER) && !load_module(&p->modules[i]))
            return 1;
    lx.lib_lo = 0;
    lx.lib_hi = 0;
    for (uint32_t i = 0; i < p->module_count; i++) {
        const rl_linux_module *m = &p->modules[i];
        if (m->role != RL_LINUX_LIBRARY)
            continue;
        if (!lx.lib_lo || m->base < lx.lib_lo)
            lx.lib_lo = m->base;
        if (m->base + m->span > lx.lib_hi)
            lx.lib_hi = m->base + m->span;
    }
    if (hi > lx.vm.size / 2)
        return log_fail("the program does not fit the address space");
    lx.brk_start = lx_page_up((hi + HEAP_ALIGN - 1) & ~(uint64_t)(HEAP_ALIGN - 1));
    lx.brk = lx.brk_start;
    lx.brk_mapped = lx.brk_start;
    uint64_t span = (uint64_t)1 << 32;
    if (span > lx.vm.size / 16)
        span = lx.vm.size / 16;
    lx.brk_limit = lx.brk_start + span;
    return 0;
}

static uint64_t push_bytes(uint64_t *sp, const void *b, uint64_t n)
{
    *sp -= n;
    rl_vm_kernel_write(&lx.vm, *sp, b, n);
    return *sp;
}

static uint64_t push_str(uint64_t *sp, const char *s) { return push_bytes(sp, s, strlen(s) + 1); }

/** @brief Writes n words of the architecture's word size at guest address at; returns the address after them. */
static uint64_t put_words(uint64_t at, const uint64_t *w, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        uint32_t w32 = (uint32_t)w[i];
        rl_vm_kernel_write(&lx.vm, at, lx_arch_word == 8 ? (const void *)&w[i] : (const void *)&w32, lx_arch_word);
        at += lx_arch_word;
    }
    return at;
}

static int build_stack(rl_ctx *c, const rl_linux_program *p, int argc, char **argv)
{
    uint64_t top = lx.stack_hi;
    if (lx_map(lx.stack_lo, top - lx.stack_lo, L_PROT_READ | L_PROT_WRITE) != 0)
        return log_fail("cannot map the stack");
    uint64_t sp = top;
    uint64_t execfn = push_str(&sp, p->exec_path);
    int envn = 0;
    while (host_environ && host_environ[envn])
        envn++;
    uint64_t *envp = calloc((size_t)envn + 1, 8);
    uint64_t *argp = calloc((size_t)argc + 1, 8);
    if (!envp || !argp)
        return log_fail("out of memory");
    for (int i = envn - 1; i >= 0; i--)
        envp[i] = push_str(&sp, host_environ[i]);
    for (int i = argc - 1; i >= 0; i--)
        argp[i] = push_str(&sp, argv[i]);
    uint64_t platform = push_str(&sp, lx_arch_platform);
    uint8_t rnd[16];
    if (!rl_host_random(rnd, sizeof rnd))
        memset(rnd, 0x5a, sizeof rnd);
    uint64_t random = push_bytes(&sp, rnd, sizeof rnd);
    sp &= ~15ull;
    uint32_t uid = hio_uid(), gid = hio_gid();
    const rl_linux_module *main_module = module_of(p, RL_LINUX_MAIN);
    const rl_linux_module *interp = module_of(p, RL_LINUX_INTERPRETER);
    uint64_t aux[24][2] = {
        { L_AT_PHDR, main_module->phdr },
        { L_AT_PHENT, main_module->phent },
        { L_AT_PHNUM, main_module->phnum },
        { L_AT_PAGESZ, L_PAGE },
        { L_AT_BASE, interp ? interp->base : 0 },
        { L_AT_FLAGS, 0 },
        { L_AT_ENTRY, main_module->entry },
        { L_AT_UID, uid },
        { L_AT_EUID, uid },
        { L_AT_GID, gid },
        { L_AT_EGID, gid },
        { L_AT_SECURE, 0 },
        { L_AT_RANDOM, random },
        { L_AT_PLATFORM, platform },
        { L_AT_EXECFN, execfn },
        { L_AT_CLKTCK, 100 },
    };
    size_t aux_n = 16;
    aux_n += lx_arch_aux_extra(p, aux + aux_n);
    aux[aux_n][0] = L_AT_NULL;
    aux[aux_n][1] = 0;
    aux_n++;
    uint64_t words = 1 + (uint64_t)argc + 1 + (uint64_t)envn + 1 + aux_n * 2;
    sp -= words * lx_arch_word;
    sp &= ~15ull;
    uint64_t at = sp;
    uint64_t n = (uint64_t)argc;
    at = put_words(at, &n, 1);
    at = put_words(at, argp, (uint64_t)argc + 1);
    at = put_words(at, envp, (uint64_t)envn + 1);
    put_words(at, &aux[0][0], aux_n * 2);
    free(envp);
    free(argp);
    lx_arch_start_regs(c, sp, 0);
    return 0;
}

static int64_t locked(lx_sys_fn *f, rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    lx_mutex_lock(&lx_big);
    int64_t r = f(c, mem, a);
    lx_mutex_unlock(&lx_big);
    return r;
}

int64_t lx_dispatch(rl_ctx *c, uint8_t *mem, uint32_t nr, const uint64_t a[6])
{
    int64_t r;
    switch (nr) {
    case NR_brk: return locked(lx_sys_brk, c, mem, a);
    case NR_mmap: return locked(lx_sys_mmap, c, mem, a);
    case NR_munmap: return locked(lx_sys_munmap, c, mem, a);
    case NR_mprotect: return locked(lx_sys_mprotect, c, mem, a);
    case NR_mremap: return locked(lx_sys_mremap, c, mem, a);
    case NR_madvise: return locked(lx_sys_madvise, c, mem, a);
    case NR_clone: return lx_sys_clone(c, mem, a);
    case NR_rt_sigreturn: return lx_sigreturn(c);
    case NR_rseq:
    case NR_clone3: return -L_ENOSYS;
    case NR_eventfd2:
    case NR_epoll_create1:
    case NR_timerfd_create: return locked(lx_event_call(nr), c, mem, a);
    case NR_openat:
    case NR_close:
    case NR_dup:
    case NR_dup3:
    case NR_pipe2:
    case NR_fcntl:
    case NR_getdents64:
        r = locked(lx_fs_call(nr), c, mem, a);
        return r;
    default: break;
    }
    lx_sys_fn *f = lx_fs_call(nr);
    if (!f)
        f = lx_event_call(nr);
    if (!f)
        f = lx_proc_call(nr);
    if (!f)
        f = lx_signal_call(nr);
    if (f)
        return f(c, mem, a);
    if (rl_report_unknown(0x4c58535900000000ull | nr, "linux", "system call %u is not implemented (returns ENOSYS)", nr))
        rl_log(RL_LOG_WARN, "linux", "arguments 0x%llx 0x%llx 0x%llx", (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2]);
    return -L_ENOSYS;
}

/**
 * @brief Whether the interrupted call with kernel result r (one of enum
 * rl_linux_restart) runs again: signal(7) and the kernel's arch signal
 * code. ERESTARTSYS restarts after a handler with SA_RESTART; ERESTARTNOHAND
 * and ERESTART_RESTARTBLOCK only when no handler runs.
 */
static bool restarts(int64_t r)
{
    uint64_t flags;
    if (!lx_signal_peek(&flags))
        return true;
    return r == -L_ERESTARTSYS && (flags & L_SA_RESTART) != 0;
}

int lx_syscall_entry(rl_ctx *c, uint8_t *mem)
{
    lx_arch_sys_begin(c);
    uint64_t a[6];
    uint32_t nr;
    lx_arch_sys_args(c, &nr, a);
    for (;;) {
        if (rl_cfg()->trace_os)
            rl_log(RL_LOG_INFO, "linux", "syscall %u (0x%llx, 0x%llx, 0x%llx, 0x%llx)", nr, (unsigned long long)a[0],
                   (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3]);
        int64_t r = lx_arch_syscall(c, mem, nr, a);
        if (rl_cfg()->trace_os) {
            char path[96] = "";
            if (lx_arch_traces_path(nr) && lx_get(path, a[1], sizeof path - 1))
                path[sizeof path - 1] = 0;
            rl_log(RL_LOG_INFO, "linux", "  syscall %u = %lld %s", nr, (long long)r, path);
        }
        if (r == LX_PENDING)
            return RL_PENDING;
        bool restart = r == -L_ERESTARTSYS || r == -L_ERESTARTNOHAND || r == -L_ERESTART_RESTARTBLOCK;
        if (!restart) {
            lx_arch_sys_result(c, r);
            return lx_signal_deliver(c, mem);
        }
        restart = restarts(r);
        if (restart)
            lx_arch_sys_restart(c, nr, a);
        else
            lx_arch_sys_result(c, -(int64_t)L_EINTR);
        int rc = lx_signal_deliver(c, mem);
        if (rc != RL_OK || !restart)
            return rc;
        lx_arch_sys_begin(c);
    }
}

static void describe_program(const rl_linux_program *p)
{
    rl_report_info("guest", "linux");
    rl_report_info("program", p->name);
    for (uint32_t i = 0; i < p->module_count; i++)
        if (p->modules[i].role != RL_LINUX_DATA)
            rl_report_module(p->modules[i].name, p->modules[i].base, p->modules[i].span, "");
}

static int finish(rl_ctx *c, int rc)
{
    if (rc != RL_PENDING)
        return 0;
    switch (c->pend.kind) {
    case RL_PEND_EXIT_PROCESS:
        return (int)c->pend.code;
    case RL_PEND_UNWIND:
        rl_log(RL_LOG_ERROR, "thread", "guest unwind to pc 0x%llx sp 0x%llx left the thread entry", (unsigned long long)c->pend.pc,
               (unsigned long long)c->pend.sp);
        return RL_EXIT_SOFTWARE;
    default:
        return 0;
    }
}

int lx_start(const rl_linux_program *p, int argc, char **argv, rl_ctx **out)
{
    memset(&lx, 0, sizeof lx);
    lx.prog = p;
    if (!rl_cfg()->loaded)
        rl_config_load();
    hio_init();
    if (lx_arch_core_init() != 0)
        return log_fail("cannot start the runtime core");
    if (lx_arch_vm_init(&lx.vm) != 0)
        return log_fail("cannot reserve the guest address space");
    rl_set_mem_base(lx.vm.base);
    rl_fault_install(&lx.vm);
    if (!module_of(p, RL_LINUX_MAIN))
        return log_fail("the program has no executable");
    if (load_image(p) != 0)
        return 1;
    lx.stack_hi = lx_arch_space_top(&lx.vm) - TOP_RESERVE;
    lx.stack_lo = lx.stack_hi - MAIN_STACK;
    lx.mmap_top = lx.stack_lo - STACK_GAP;
    lx.mmap_floor = lx.brk_limit + STACK_GAP;
    lx.dynamic = module_of(p, RL_LINUX_INTERPRETER) != NULL;
    if (lx.mmap_floor >= lx.mmap_top)
        return log_fail("the address space is too small");
    lx_fs_init();
    lx_signal_init();
    lx_arch_install_hooks();
    if (lx_arch_map_extra() != 0)
        return log_fail("cannot map the architecture's pages");
    lx_proc_init();
    rl_dispatch_set_image(p->image_id);
    if (rl_dispatch_reserve(p->code_lo, p->code_hi) != 0)
        return log_fail("cannot allocate the dispatch table");
    if (p->unit_count && rl_units_register(p->units, p->unit_count) != 0)
        return log_fail("a lifted unit was refused");
    if (p->code_map_count && rl_code_maps_register(p->code_maps, p->code_map_count, NULL) != 0)
        return log_fail("the code maps disagree with the lifted units");
    describe_program(p);
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return log_fail("cannot create the main thread");
    rl_thread_bind(t);
    lx_thread *main_thread = calloc(1, sizeof *main_thread);
    if (!main_thread)
        return log_fail("out of memory");
    main_thread->waker = hio_waker_new();
    main_thread->rt = t;
    main_thread->main = true;
    main_thread->tid = lx.pid;
    t->guest = main_thread;
    lx.threads = main_thread;
    lx.live = 1;
    rl_ctx *c = t->ctx;
    char *self[1] = { (char *)p->name };
    if (argc < 1) {
        argc = 1;
        argv = self;
    }
    if (build_stack(c, p, argc, argv) != 0)
        return 1;
    *out = c;
    return 0;
}

/**
 * @brief The run summary line of the cold copy (docs/abi.md section 5.2):
 * how many instructions ran there, printed when RELIFT_STATS=1.
 */
void lx_report_cold(void)
{
    const char *e = getenv("RELIFT_STATS");
    if (e && e[0] == '1')
        rl_log(RL_LOG_INFO, "cold", "%llu instructions ran in the cold copy",
               (unsigned long long)__atomic_load_n(&rl_counted_insns, __ATOMIC_RELAXED));
}

int rl_linux_main(const rl_linux_program *p, int argc, char **argv)
{
    rl_ctx *c;
    if (lx_start(p, argc, argv, &c) != 0)
        return 1;
    const rl_linux_module *start = module_of(p, RL_LINUX_INTERPRETER);
    if (!start)
        start = module_of(p, RL_LINUX_MAIN);
    int rc = lx_arch_call_guest(c, rl_mem_base(), start->entry);
    int status = finish(c, rc);
    if (rc == RL_PENDING && c->pend.kind == RL_PEND_EXIT_THREAD) {
        lx_thread_end(lx_cur());
        lx_wait_others();
        status = (int)lx.exit_status;
    }
    lx_fs_shutdown();
    lx_report_cold();
    rl_process_exit((uint32_t)status);
    return status;
}
