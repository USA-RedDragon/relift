/**
 * @file gdbstub.c
 * docgen:region stops
 * @brief The GDB remote-protocol server (gdbstub.h, .work/design/debug-stub.md).
 *
 * All-stop. On attach and on an interrupt from the debugger, rl_gdb_stopping is
 * set and every guest thread's budget is ended (rl_preempt_request), so each
 * thread that runs lifted code reaches a safe point soon and parks there in
 * rl_gdb_park. The guest is reported stopped once every thread that holds a
 * core has parked (or after a short timeout, which the thread list then names).
 * Threads waiting in the runtime cannot run guest code again without passing a
 * safe point, so their stored contexts are what gdb reads. Continuing clears the
 * flag and wakes the parked threads.
 *
 * Registers come from the context the thread stored: exact at a system call or
 * any runtime entry, the values of its last call boundary at a preemption check
 * (the thread list says which). Memory is guest memory by guest address, read and
 * written as the kernel does (rl_vm_kernel_read and rl_vm_kernel_write), so a
 * page the guest cannot touch answers an error instead of faulting.
 * docgen:end
 */
#include "gdbstub.h"

#if defined(RL_DEBUG_STUB)

#include <relift/settings.h>

#include "core.h"
#include "crash.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__) && defined(__x86_64__)
#include <ucontext.h>
/** @brief Breakpoints and watchpoints: they trap on the host and step one host instruction with the trap flag. */
#define RL_GDB_TRAPS 1
#endif

int rl_gdb_stopping;

/** @brief Most threads the stub lists. */
#define MAX_THREADS 512
/** @brief Largest packet the stub takes or sends (hex payload), and the PacketSize it announces. */
#define PACKET 16384
/** @brief How long a stop waits for running threads to reach a safe point. */
#define STOP_WAIT_NS 2000000000ull

/** @brief Most breakpoints and watchpoints. */
#define MAX_BREAKS 64
#define MAX_WATCHES 16
/** @brief Most guest frames a backtrace walks. */
#define MAX_FRAMES 64

/** @brief A parked thread: its context, whether its registers are exact, and the guest pc a breakpoint stopped it at. */
typedef struct parked {
    rl_ctx *ctx;
    bool exact;
    bool has_pc;
    uint64_t pc;
} parked;

/** @brief A breakpoint: the entry of a lifted function, trapped by an int3 written over its first host byte. */
typedef struct breakpoint {
    uint64_t guest;
    uint8_t *host;
    uint8_t orig;
    bool on;
} breakpoint;

/** @brief A write watchpoint over guest bytes [addr, addr + len): their host pages are kept read-only. */
typedef struct watchpoint {
    uint64_t addr, len;
    bool on;
} watchpoint;

/** @brief Why the guest stopped on its own: a breakpoint, a watchpoint or the end of a step. */
enum { HIT_NONE, HIT_BREAK, HIT_WATCH, HIT_STEP };

static struct {
    const rl_gdb_cpu *cpu;
    const rl_gdb_guest *guest;
    int listen_fd, fd;
    bool no_ack;
    pthread_mutex_t lock;
    pthread_cond_t wake, arrived;
    parked park[MAX_THREADS];
    uint32_t nparked;
    /** @brief The thread g, G, p, P, m and M act on (Hg), 0 for the first. */
    uint64_t cur;
    /** @brief The thread whose stop the last stop reply named. */
    uint64_t stop_thread;
    /** @brief Set once a debugger continued the first time (debug.gdb_wait). */
    bool released;
    rl_gdb_thread th[MAX_THREADS];
    uint32_t nth;
    breakpoint bp[MAX_BREAKS];
    watchpoint wp[MAX_WATCHES];
    /** @brief A stop a guest thread made (HIT_*), its context and the watched address it wrote; read by the server. */
    int hit;
    rl_ctx *hit_ctx;
    uint64_t hit_addr;
    /** @brief While stepping (rl_gdb_stopping is 2): the thread that stops at its next safe point. */
    rl_ctx *step_ctx;
    char in[PACKET + 64], out[PACKET + 64];
} G = { .lock = PTHREAD_MUTEX_INITIALIZER,
        .wake = PTHREAD_COND_INITIALIZER,
        .arrived = PTHREAD_COND_INITIALIZER,
        .listen_fd = -1,
        .fd = -1 };

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/** @brief The parked entry of c, or NULL. Lock held. */
static parked *find_parked(const rl_ctx *c)
{
    for (uint32_t i = 0; i < G.nparked; i++)
        if (G.park[i].ctx == c)
            return &G.park[i];
    return NULL;
}

/**
 * @brief Parks c until the debugger resumes the guest. While stepping, only the stepping thread parks, and it stops
 * the guest. A thread that hit a breakpoint or watchpoint (hit) stops the guest and names the stop.
 */
static void park(rl_ctx *c, bool exact, int hit, bool has_pc, uint64_t pc, uint64_t addr)
{
    pthread_mutex_lock(&G.lock);
    int st = __atomic_load_n(&rl_gdb_stopping, __ATOMIC_ACQUIRE);
    if (st == 2 && c == G.step_ctx && !hit)
        hit = HIT_STEP;
    if ((st == 2 && !hit) || (!st && !hit) || G.nparked == MAX_THREADS || !G.cpu) {
        pthread_mutex_unlock(&G.lock);
        return;
    }
    if (hit && G.hit == HIT_NONE) {
        G.hit = hit;
        G.hit_ctx = c;
        G.hit_addr = addr;
    }
    __atomic_store_n(&rl_gdb_stopping, 1, __ATOMIC_RELEASE);
    G.park[G.nparked++] = (parked){ c, exact, has_pc, pc };
    pthread_cond_broadcast(&G.arrived);
    /* 1 holds every thread; 0 (run) and 2 (step) let it go. */
    while (__atomic_load_n(&rl_gdb_stopping, __ATOMIC_ACQUIRE) == 1)
        pthread_cond_wait(&G.wake, &G.lock);
    parked *p = find_parked(c);
    if (p)
        *p = G.park[--G.nparked];
    pthread_mutex_unlock(&G.lock);
}

void rl_gdb_park(rl_ctx *c, bool exact) { park(c, exact, HIT_NONE, false, 0, 0); }

void rl_gdb_park_at(rl_ctx *c, uint64_t pc) { park(c, true, HIT_NONE, true, pc, 0); }

/** @brief Takes a fresh list of the guest's threads. */
static void list_threads(void)
{
    G.nth = G.guest->threads(G.th, MAX_THREADS);
    if (G.nth > MAX_THREADS)
        G.nth = MAX_THREADS;
}

static rl_gdb_thread *thread_by_id(uint64_t id)
{
    for (uint32_t i = 0; i < G.nth; i++)
        if (G.th[i].id == id)
            return &G.th[i];
    return G.nth && (id == 0 || id == (uint64_t)-1) ? &G.th[0] : NULL;
}

/**
 * @brief Stops the guest: sets the flag, ends every budget, and waits until each thread that holds a core has
 * parked or the wait runs out. Picks the thread the stop reply names: the first parked one with exact registers.
 */
static void stop_all(void)
{
    __atomic_store_n(&rl_gdb_stopping, 1, __ATOMIC_RELEASE);
    list_threads();
    for (uint32_t i = 0; i < G.nth && !G.guest->clocked; i++)
        if (G.th[i].ctx)
            rl_preempt_request(G.th[i].ctx);
    uint64_t until = now_ns() + STOP_WAIT_NS;
    pthread_mutex_lock(&G.lock);
    for (;;) {
        list_threads();
        bool all = true, any = false, live = false;
        for (uint32_t i = 0; i < G.nth; i++) {
            bool parked_now = find_parked(G.th[i].ctx) != NULL;
            all = all && (!G.th[i].running || parked_now);
            any = any || parked_now;
            live = live || G.th[i].running;
        }
        if (G.guest->one_runs)
            all = any || !live;
        if (all || now_ns() >= until)
            break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&G.arrived, &G.lock, &ts);
    }
    G.stop_thread = G.nth ? G.th[0].id : 1;
    for (uint32_t i = 0; i < G.nth; i++) {
        parked *p = find_parked(G.th[i].ctx);
        if (p && p->exact) {
            G.stop_thread = G.th[i].id;
            break;
        }
    }
    pthread_mutex_unlock(&G.lock);
    G.cur = G.stop_thread;
}

/** @brief Resumes the guest; with step set, the thread step stops again at its next safe point or breakpoint. */
static void resume(rl_ctx *step)
{
    pthread_mutex_lock(&G.lock);
    G.hit = HIT_NONE;
    G.step_ctx = step;
    __atomic_store_n(&rl_gdb_stopping, step ? 2 : 0, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&G.wake);
    G.released = true;
    pthread_mutex_unlock(&G.lock);
}

static void resume_all(void)
{
    pthread_mutex_lock(&G.lock);
    G.hit = HIT_NONE;
    G.step_ctx = NULL;
    __atomic_store_n(&rl_gdb_stopping, 0, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&G.wake);
    G.released = true;
    pthread_mutex_unlock(&G.lock);
}

static const char hexd[] = "0123456789abcdef";

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static uint64_t parse_hex(const char **p)
{
    uint64_t v = 0;
    int d;
    while ((d = hexv(**p)) >= 0) {
        v = v << 4 | (uint64_t)d;
        (*p)++;
    }
    return v;
}

static void to_hex(char *dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        dst[2 * i] = hexd[src[i] >> 4];
        dst[2 * i + 1] = hexd[src[i] & 15];
    }
    dst[2 * n] = 0;
}

static bool from_hex(uint8_t *dst, const char *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int a = hexv(src[2 * i]), b = hexv(src[2 * i + 1]);
        if (a < 0 || b < 0)
            return false;
        dst[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}

static bool send_all(const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(G.fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/** @brief Sends one packet, escaping the protocol's special bytes, and waits for its ack unless acks are off. */
static bool reply(const char *body)
{
    static char frame[2 * PACKET + 16];
    size_t o = 0;
    uint8_t sum = 0;
    frame[o++] = '$';
    for (const char *p = body; *p && o < sizeof frame - 8; p++) {
        char c = *p;
        if (c == '$' || c == '#' || c == '}' || c == '*') {
            frame[o++] = '}';
            sum += '}';
            c ^= 0x20;
        }
        frame[o++] = c;
        sum += (uint8_t)c;
    }
    frame[o++] = '#';
    frame[o++] = hexd[sum >> 4];
    frame[o++] = hexd[sum & 15];
    if (!send_all(frame, o))
        return false;
    if (G.no_ack)
        return true;
    char a;
    return recv(G.fd, &a, 1, 0) == 1;
}

/**
 * @brief Reads one packet into G.in. Returns 1 for a packet, 2 for an interrupt byte (0x03) seen outside a packet,
 * 0 when the connection closed.
 */
static int read_packet(void)
{
    char c;
    for (;;) {
        ssize_t r = recv(G.fd, &c, 1, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            return 0;
        }
        if (c == 3)
            return 2;
        if (c != '$')
            continue;
        size_t n = 0;
        uint8_t sum = 0;
        for (;;) {
            if (recv(G.fd, &c, 1, 0) != 1)
                return 0;
            if (c == '#')
                break;
            sum += (uint8_t)c;
            if (c == '}') {
                if (recv(G.fd, &c, 1, 0) != 1)
                    return 0;
                sum += (uint8_t)c;
                c ^= 0x20;
            }
            if (n < PACKET)
                G.in[n++] = c;
        }
        G.in[n] = 0;
        char cs[2];
        if (recv(G.fd, cs, 2, MSG_WAITALL) != 2)
            return 0;
        int want = hexv(cs[0]) << 4 | hexv(cs[1]);
        if (!G.no_ack)
            send_all(want == sum ? "+" : "-", 1);
        if (want == sum)
            return 1;
    }
}

static void stop_reply(int sig)
{
    snprintf(G.out, sizeof G.out, "T%02xthread:%llx;", sig, (unsigned long long)G.stop_thread);
    reply(G.out);
}

/** @brief After a thread stopped the guest itself: stops the rest and names the thread and the reason. */
static void hit_reply(void)
{
    pthread_mutex_lock(&G.lock);
    int hit = G.hit;
    rl_ctx *c = G.hit_ctx;
    uint64_t addr = G.hit_addr;
    pthread_mutex_unlock(&G.lock);
    stop_all();
    for (uint32_t i = 0; i < G.nth; i++)
        if (G.th[i].ctx == c)
            G.stop_thread = G.th[i].id;
    G.cur = G.stop_thread;
    if (hit == HIT_WATCH)
        snprintf(G.out, sizeof G.out, "T05thread:%llx;watch:%llx;", (unsigned long long)G.stop_thread,
                 (unsigned long long)addr);
    else
        snprintf(G.out, sizeof G.out, "T05thread:%llx;", (unsigned long long)G.stop_thread);
    reply(G.out);
}

/** @brief Answers a qXfer read of text at "offset,length" with m (more) or l (last). */
static void xfer(const char *text, const char *args)
{
    const char *p = args;
    uint64_t off = parse_hex(&p);
    if (*p == ',')
        p++;
    uint64_t len = parse_hex(&p);
    size_t n = strlen(text);
    if (len > PACKET / 2)
        len = PACKET / 2;
    if (off >= n) {
        reply("l");
        return;
    }
    size_t take = n - off < len ? n - off : len;
    G.out[0] = off + take < n ? 'm' : 'l';
    memcpy(G.out + 1, text + off, take);
    G.out[1 + take] = 0;
    reply(G.out);
}

/** @brief Appends the name of the lifted function at guest address a ("module+0xoff name"), or nothing. */
static size_t name_at(char *dst, size_t n, uint64_t a)
{
    rl_fn_hit h;
    if (!rl_fn_find_guest(a, &h) || !h.fn)
        return 0;
    const char *nm = h.fn->guest_name ? h.fn->guest_name : h.fn->host_sym ? h.fn->host_sym : "?";
    int w = snprintf(dst, n, "%s+0x%llx %s", h.map->module ? h.map->module : "?",
                     (unsigned long long)(h.fn->fn_addr - h.map->module_base), nm);
    return w > 0 ? ((size_t)w < n ? (size_t)w : n - 1) : 0;
}

static void escape_xml(char *dst, size_t n, const char *s)
{
    size_t o = 0;
    for (; *s && o + 7 < n; s++) {
        const char *e = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : NULL;
        if (e) {
            memcpy(dst + o, e, strlen(e));
            o += strlen(e);
        } else {
            dst[o++] = *s;
        }
    }
    dst[o] = 0;
}

/**
 * @brief Answers qXfer:threads:read. The text is made at offset 0 and kept for the later pages: threads that wake
 * and park while the guest is stopped change their state, and pages of two different texts do not parse.
 */
static void threads_xml(const char *args)
{
    static char xml[MAX_THREADS * 320 + 64];
    if (args[0] != '0' || args[1] != ',') {
        xfer(xml, args);
        return;
    }
    size_t o = (size_t)snprintf(xml, sizeof xml, "<?xml version=\"1.0\"?><threads>");
    pthread_mutex_lock(&G.lock);
    for (uint32_t i = 0; i < G.nth && o < sizeof xml - 400; i++) {
        rl_gdb_thread *t = &G.th[i];
        parked *p = find_parked(t->ctx);
        char nm[200] = "", esc[260];
        uint64_t pc = t->ctx && G.cpu->pc ? G.cpu->pc(t->ctx) : 0;
        if (p && p->has_pc)
            pc = p->pc;
        else if (!p && t->has_pc)
            pc = t->pc;
        name_at(nm, sizeof nm, pc);
        escape_xml(esc, sizeof esc, nm);
        const char *state = p ? (p->exact ? "stopped" : "stopped at a preemption check, registers of its last call")
                            : t->running && !(G.guest->one_runs && G.nparked) ? "running (did not reach a safe point)"
                            : t->has_pc && !t->pc_exact
                                ? "waiting at a memory access; registers exact, pc of its last sync point"
                                : "waiting in the runtime";
        o += (size_t)snprintf(xml + o, sizeof xml - o, "<thread id=\"%llx\" core=\"%d\" name=\"%llu\">%s; pc 0x%llx %s</thread>",
                              (unsigned long long)t->id, t->core, (unsigned long long)t->id, state,
                              (unsigned long long)pc, esc);
    }
    pthread_mutex_unlock(&G.lock);
    snprintf(xml + o, sizeof xml - o, "</threads>");
    xfer(xml, args);
}

/** @brief Reads n bytes of guest memory at a, page by page, stopping at the first page the guest cannot read. */
static size_t mem_read(uint64_t a, uint8_t *dst, size_t n)
{
    if (G.guest->mem_read)
        return G.guest->mem_read(G.cur, a, dst, n);
    rl_vm *vm = G.guest->vm;
    size_t done = 0;
    while (done < n) {
        uint64_t at = a + done;
        if (!rl_vm_contains(vm, at, 1))
            break;
        rl_vm_block b = rl_vm_query(vm, at);
        if (!(b.perm & (RL_VM_R | RL_VM_X)))
            break;
        uint64_t room = b.addr + b.size - at;
        size_t take = n - done < room ? n - done : (size_t)room;
        rl_vm_kernel_read(vm, dst + done, at, take);
        done += take;
    }
    return done;
}

static bool mem_write(uint64_t a, const uint8_t *src, size_t n)
{
    if (G.guest->mem_write)
        return G.guest->mem_write(G.cur, a, src, n);
    rl_vm *vm = G.guest->vm;
    if (!rl_vm_contains(vm, a, n))
        return false;
    for (uint64_t at = a; at < a + n;) {
        rl_vm_block b = rl_vm_query(vm, at);
        if (!(b.perm & (RL_VM_R | RL_VM_W | RL_VM_X)))
            return false;
        at = b.addr + b.size;
    }
    rl_vm_kernel_write(vm, a, src, n);
    return true;
}

#if defined(RL_GDB_TRAPS)
/** @brief The guest page size the watchpoints work in: the host page of the guest's address space. */
static uint64_t host_page(void) { return G.guest->vm->host_page; }

/** @brief Writes byte v over host code at p (the page made writable for the write, then executable again). */
static void poke_code(uint8_t *p, uint8_t v)
{
    uintptr_t pg = (uintptr_t)sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)p & ~(pg - 1));
    size_t len = (uintptr_t)p + 1 - (uintptr_t)page <= pg ? pg : 2 * pg;
    mprotect(page, len, PROT_READ | PROT_WRITE | PROT_EXEC);
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
    mprotect(page, len, PROT_READ | PROT_EXEC);
}

/** @brief Per host thread: a breakpoint (index + 1) or a watched page to set again after one stepped instruction. */
static _Thread_local int t_reinsert;
static _Thread_local uint64_t t_rewatch;
static _Thread_local bool t_rewatch_on;
/** @brief The guest address of a watched store the stepped instruction made, 0 for none: stops after the store. */
static _Thread_local uint64_t t_watch_hit;
static struct sigaction g_old_trap;

static void set_tf(ucontext_t *u, bool on)
{
    if (on)
        u->uc_mcontext.gregs[REG_EFL] |= 0x100;
    else
        u->uc_mcontext.gregs[REG_EFL] &= ~(greg_t)0x100;
}

static void on_trap(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;
    uint8_t *rip = (uint8_t *)u->uc_mcontext.gregs[REG_RIP];
    if (t_reinsert || t_rewatch_on) {
        /* The one stepped instruction is done: put the breakpoint or the watch back. */
        if (t_reinsert) {
            breakpoint *b = &G.bp[t_reinsert - 1];
            if (b->on)
                poke_code(b->host, 0xcc);
            t_reinsert = 0;
        }
        if (t_rewatch_on) {
            rl_vm_host_watch(G.guest->vm, t_rewatch, host_page(), true);
            t_rewatch_on = false;
        }
        set_tf(u, false);
        uint64_t hit = t_watch_hit;
        struct rl_thread *t = rl_thread_current();
        t_watch_hit = 0;
        if (hit && t && t->ctx)
            park(t->ctx, false, HIT_WATCH, false, 0, hit);
        return;
    }
    for (int i = 0; i < MAX_BREAKS; i++) {
        breakpoint *b = &G.bp[i];
        if (!b->on || b->host != rip - 1)
            continue;
        u->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)b->host;
        /* A lifted function is entered as fn(rl_ctx *c, uint8_t *mem): c is in rdi, and its registers are exact. */
        rl_ctx *c = (rl_ctx *)(uintptr_t)u->uc_mcontext.gregs[REG_RDI];
        park(c, true, HIT_BREAK, true, b->guest, 0);
        if (b->on) {
            poke_code(b->host, b->orig);
            t_reinsert = i + 1;
            set_tf(u, true);
        }
        return;
    }
    if (g_old_trap.sa_flags & SA_SIGINFO && g_old_trap.sa_sigaction)
        g_old_trap.sa_sigaction(sig, si, uc);
    else if (g_old_trap.sa_handler != SIG_DFL && g_old_trap.sa_handler != SIG_IGN)
        g_old_trap.sa_handler(sig);
    else {
        signal(SIGTRAP, SIG_DFL);
        raise(SIGTRAP);
    }
}

/** @brief The debugger's fault hook: a write to a watched page stops the guest when it hits a watched range. */
static bool on_watch_fault(const rl_fault *f, void *uc)
{
    uint64_t hp = host_page(), page = f->guest_addr & ~(hp - 1);
    bool ours = false, hit = false;
    for (int i = 0; i < MAX_WATCHES; i++) {
        watchpoint *w = &G.wp[i];
        if (!w->on)
            continue;
        uint64_t lo = w->addr & ~(hp - 1), hi = (w->addr + w->len + hp - 1) & ~(hp - 1);
        ours |= page >= lo && page < hi;
        hit |= f->guest_addr >= w->addr && f->guest_addr < w->addr + w->len;
    }
    if (!ours)
        return false;
    /* Let the store happen: open the page for one host instruction, watch it again, and stop after a hit, as a
     * data breakpoint does, so the debugger sees the new value. */
    t_watch_hit = hit ? f->guest_addr : 0;
    rl_vm_host_access(G.guest->vm, page, hp, true);
    t_rewatch = page;
    t_rewatch_on = true;
    set_tf(uc, true);
    return true;
}

static void traps_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_trap;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, &g_old_trap);
    rl_fault_set_debug_hook(on_watch_fault);
}

/** @brief Z0/Z1 at guest address a: the entry of a lifted function. 0 or an error number. */
static int break_set(uint64_t a, bool on)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_BREAKS; i++) {
        if (G.bp[i].on && G.bp[i].guest == a) {
            if (!on) {
                G.bp[i].on = false;
                poke_code(G.bp[i].host, G.bp[i].orig);
            }
            return 0;
        }
        if (!G.bp[i].on && free_slot < 0)
            free_slot = i;
    }
    if (!on)
        return 0;
    rl_fn_hit h;
    if (!rl_fn_find_guest(a, &h) || !h.fn || h.fn->fn_addr != a || h.fn->guest_addr != a || !h.fn->fn)
        return 22;
    if (free_slot < 0)
        return 28;
    breakpoint *b = &G.bp[free_slot];
    b->guest = a;
    b->host = (uint8_t *)(uintptr_t)h.fn->fn;
    b->orig = *b->host;
    b->on = true;
    poke_code(b->host, 0xcc);
    return 0;
}

/** @brief Z2 over [a, a + len): protects the pages. 0 or an error number. */
static int watch_set(uint64_t a, uint64_t len, bool on)
{
    if (!G.guest->vm)
        return 22;
    uint64_t hp = host_page();
    int free_slot = -1;
    for (int i = 0; i < MAX_WATCHES; i++) {
        if (G.wp[i].on && G.wp[i].addr == a && G.wp[i].len == len) {
            if (!on)
                G.wp[i].on = false;
            free_slot = -2;
            break;
        }
        if (!G.wp[i].on && free_slot == -1)
            free_slot = i;
    }
    uint64_t lo = a & ~(hp - 1), hi = (a + len + hp - 1) & ~(hp - 1);
    if (!on) {
        /* Open the pages unless another watch still covers them. */
        for (uint64_t p = lo; p < hi; p += hp) {
            bool kept = false;
            for (int i = 0; i < MAX_WATCHES; i++)
                kept |= G.wp[i].on && p >= (G.wp[i].addr & ~(hp - 1)) && p < G.wp[i].addr + G.wp[i].len;
            if (!kept)
                rl_vm_host_access(G.guest->vm, p, hp, true);
        }
        return 0;
    }
    if (free_slot == -2)
        return 0;
    if (free_slot < 0)
        return 28;
    if (!len || !rl_vm_contains(G.guest->vm, a, len) || rl_vm_host_watch(G.guest->vm, lo, hi - lo, true) != 0)
        return 22;
    G.wp[free_slot] = (watchpoint){ a, len, true };
    return 0;
}

/** @brief Z and z packets: 0 software and 1 hardware breakpoints (both at function entries), 2 write watchpoints. */
static void z_packet(const char *p)
{
    bool on = p[0] == 'Z';
    int type = p[1] - '0';
    const char *q = p + 2;
    q += *q == ',';
    uint64_t a = parse_hex(&q);
    q += *q == ',';
    uint64_t len = parse_hex(&q);
    int err;
    if (type == 0 || type == 1)
        err = break_set(a, on);
    else if (type == 2)
        err = watch_set(a, len, on);
    else {
        reply("");
        return;
    }
    if (err)
        snprintf(G.out, sizeof G.out, "E%02x", err);
    reply(err ? G.out : "OK");
}
#else
static void z_packet(const char *p)
{
    (void)p;
    reply("");
}
#endif

static void reg_read(rl_ctx *c, uint32_t r, uint8_t *out);

/** @brief Reads guest memory for a frame walk: every byte or nothing. */
static bool frame_read(uint64_t a, void *dst, size_t n) { return mem_read(a, dst, n) == n; }

/** @brief The guest call frames of thread t, innermost first, with the pc gdb reads for it; 0 without a walk. */
static uint32_t walk(rl_gdb_thread *t, rl_gdb_frame *out, uint32_t max)
{
    if (!t || !t->ctx || !G.cpu->frames)
        return 0;
    uint8_t pcb[8];
    uint64_t pc;
    reg_read(t->ctx, G.cpu->pc_reg, pcb);
    memcpy(&pc, pcb, 8);
    return G.cpu->frames(t->ctx, pc, out, max, frame_read);
}

/** @brief The number of the register named name (len bytes) in the target description, or -1. */
static int reg_by_name(const char *name, size_t len)
{
    int n = 0;
    for (const char *p = G.cpu->target_xml; (p = strstr(p, "<reg ")); p++, n++) {
        const char *e = strchr(p, '>'), *rn = strstr(p, "regnum=\""), *nm = strstr(p, "name=\"");
        if (rn && rn < e)
            n = atoi(rn + 8);
        if (nm && nm < e && strncmp(nm + 6, name, len) == 0 && nm[6 + len] == '"')
            return n;
    }
    return -1;
}

/**
 * docgen:region monitor-args
 * @brief The address argument of a monitor command. gdb sends the text after `monitor` as typed, without evaluating
 * it, so the server reads it here: a hex number (0x optional), or a register of the current thread by its name in the
 * target description, with or without `$` (`$pc`, `x30`), either one optionally followed by `+` or `-` and a hex
 * number. False when the text is none of these.
 * docgen:end
 */
static bool monitor_addr(const char *p, uint64_t *out)
{
    while (*p == ' ')
        p++;
    bool reg = *p == '$';
    p += reg;
    const char *s = p;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')
        p++;
    const char *num = s + (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') ? 2 : 0), *q = num;
    uint64_t v = parse_hex(&q);
    if (reg || q != p || q == num) {
        int r = p > s ? reg_by_name(s, (size_t)(p - s)) : -1;
        rl_gdb_thread *t = thread_by_id(G.cur);
        if (r < 0 || !t || !t->ctx || G.cpu->reg_size((uint32_t)r) > 8)
            return false;
        uint8_t b[8] = { 0 };
        reg_read(t->ctx, (uint32_t)r, b);
        memcpy(&v, b, 8);
    }
    while (*p == ' ')
        p++;
    if (*p == '+' || *p == '-') {
        char op = *p++;
        while (*p == ' ')
            p++;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        uint64_t d = parse_hex(&p);
        v = op == '+' ? v + d : v - d;
    }
    while (*p == ' ')
        p++;
    *out = v;
    return *p == 0;
}

static void monitor(const char *hex)
{
    char cmd[256];
    size_t n = strlen(hex) / 2;
    if (n >= sizeof cmd || !from_hex((uint8_t *)cmd, hex, n)) {
        reply("E01");
        return;
    }
    cmd[n] = 0;
    char text[1024];
    if (strncmp(cmd, "sym ", 4) == 0) {
        uint64_t a = 0;
        char nm[512] = "";
        if (!monitor_addr(cmd + 4, &a))
            snprintf(nm, sizeof nm, "not an address or a register: %s", cmd + 4);
        else if (!name_at(nm, sizeof nm, a))
            snprintf(nm, sizeof nm, "no lifted function holds 0x%llx", (unsigned long long)a);
        snprintf(text, sizeof text, "%s\n", nm);
    } else if (strcmp(cmd, "bt") == 0) {
        rl_gdb_frame f[MAX_FRAMES];
        uint32_t nf = walk(thread_by_id(G.cur), f, MAX_FRAMES);
        size_t o = 0;
        text[0] = 0;
        if (!nf)
            snprintf(text, sizeof text, "no guest frames for this thread\n");
        for (uint32_t i = 0; i < nf && o + 80 < sizeof text; i++) {
            char nm[200] = "";
            name_at(nm, sizeof nm, f[i].pc);
            o += (size_t)snprintf(text + o, sizeof text - o, "#%u 0x%llx %s\n", i, (unsigned long long)f[i].pc,
                                  nm[0] ? nm : "(no lifted function)");
        }
    } else {
        snprintf(text, sizeof text, "relift monitor commands: sym <address, $register or $register+offset>, bt\n");
    }
    G.out[0] = 'O';
    to_hex(G.out + 1, (const uint8_t *)text, strlen(text));
    reply(G.out);
    reply("OK");
}

static const char *after(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0 ? s + n : NULL;
}

/** @brief Runs the guest (stepping the thread step when set) until it stops or the debugger interrupts it; false when the connection closed. */
static bool run_until_interrupt(rl_ctx *step)
{
    resume(step);
    for (;;) {
        struct pollfd pf = { .fd = G.fd, .events = POLLIN };
        int ready = poll(&pf, 1, 20);
        if (__atomic_load_n(&G.hit, __ATOMIC_ACQUIRE) != HIT_NONE) {
            hit_reply();
            return true;
        }
        if (ready <= 0)
            continue;
        int k = read_packet();
        if (k == 0)
            return false;
        if (k == 2) {
            stop_all();
            stop_reply(2);
            return true;
        }
        if (G.in[0] == 'D' || G.in[0] == 'k') {
            reply("OK");
            return false;
        }
        reply("");
    }
}

/** @brief Writes v as a register of w bytes in the target's byte order. */
static void put_reg(uint8_t *out, uint64_t v, uint32_t w)
{
    for (uint32_t i = 0; i < w; i++)
        out[G.cpu->big_endian ? w - 1 - i : i] = (uint8_t)(v >> (8 * i));
}

/** @brief Reads register r of c: the guest pc of a breakpoint stop where the thread stopped at one. */
static void reg_read(rl_ctx *c, uint32_t r, uint8_t *out)
{
    G.cpu->reg_read(c, r, out);
    pthread_mutex_lock(&G.lock);
    parked *p = find_parked(c);
    uint32_t w = G.cpu->reg_size(r) < 8 ? G.cpu->reg_size(r) : 8;
    if (p && p->has_pc && r == G.cpu->pc_reg)
        put_reg(out, p->pc, w);
    for (uint32_t i = 0; !p && r == G.cpu->pc_reg && i < G.nth; i++)
        if (G.th[i].ctx == c && G.th[i].has_pc)
            put_reg(out, G.th[i].pc, w);
    pthread_mutex_unlock(&G.lock);
}

/** @brief Serves one connection; returns when the debugger detaches or the connection closes. */
static void serve(void)
{
    G.no_ack = false;
    stop_all();
    rl_log(RL_LOG_INFO, "gdb", "debugger attached: %u guest threads stopped", G.nth);
    for (;;) {
        int k = read_packet();
        if (k == 0)
            break;
        if (k == 2) {
            stop_reply(2);
            continue;
        }
        const char *p = G.in, *a;
        list_threads();
        rl_gdb_thread *t = thread_by_id(G.cur);
        if ((a = after(p, "qSupported"))) {
            snprintf(G.out, sizeof G.out,
                     "PacketSize=%x;qXfer:features:read+;qXfer:threads:read+;QStartNoAckMode+;vContSupported+",
                     PACKET);
            reply(G.out);
        } else if (strcmp(p, "QStartNoAckMode") == 0) {
            reply("OK");
            G.no_ack = true;
        } else if ((a = after(p, "qXfer:features:read:target.xml:"))) {
            xfer(G.cpu->target_xml, a);
        } else if ((a = after(p, "qXfer:threads:read::"))) {
            threads_xml(a);
        } else if (strcmp(p, "qfThreadInfo") == 0) {
            size_t o = 0;
            G.out[o++] = 'm';
            for (uint32_t i = 0; i < G.nth && o < PACKET - 24; i++)
                o += (size_t)snprintf(G.out + o, sizeof G.out - o, "%s%llx", i ? "," : "", (unsigned long long)G.th[i].id);
            G.out[o] = 0;
            reply(G.nth ? G.out : "l");
        } else if (strcmp(p, "qsThreadInfo") == 0) {
            reply("l");
        } else if (strcmp(p, "qC") == 0) {
            snprintf(G.out, sizeof G.out, "QC%llx", (unsigned long long)(t ? t->id : 1));
            reply(G.out);
        } else if (strcmp(p, "qAttached") == 0) {
            reply("1");
        } else if ((a = after(p, "qRcmd,"))) {
            monitor(a);
        } else if ((a = after(p, "qRelift.frames:"))) {
            /* The guest backtrace of one thread for tools/gdb/relift_guest.py: pc,sp,fp,hex name per frame, innermost first. */
            rl_gdb_frame f[MAX_FRAMES];
            uint32_t n = walk(thread_by_id(parse_hex(&a)), f, MAX_FRAMES);
            size_t o = 0;
            G.out[0] = 0;
            for (uint32_t i = 0; i < n && o + 400 < sizeof G.out; i++) {
                char nm[180] = "";
                size_t w = name_at(nm, sizeof nm, f[i].pc);
                o += (size_t)snprintf(G.out + o, sizeof G.out - o, "%s%llx,%llx,%llx,", i ? ";" : "",
                                      (unsigned long long)f[i].pc, (unsigned long long)f[i].sp,
                                      (unsigned long long)f[i].fp);
                to_hex(G.out + o, (const uint8_t *)nm, w);
                o += 2 * w;
            }
            reply(n ? G.out : "E01");
        } else if (strcmp(p, "?") == 0) {
            stop_reply(5);
        } else if (p[0] == 'H' && (p[1] == 'g' || p[1] == 'c')) {
            const char *q = p + 2;
            uint64_t id = q[0] == '-' ? 0 : parse_hex(&q);
            if (p[1] == 'g')
                G.cur = id;
            reply(id == 0 || thread_by_id(id) ? "OK" : "E01");
        } else if (p[0] == 'T') {
            const char *q = p + 1;
            reply(thread_by_id(parse_hex(&q)) ? "OK" : "E01");
        } else if (p[0] == 'g' && t) {
            size_t o = 0;
            uint8_t buf[64];
            for (uint32_t r = 0; r < G.cpu->nregs; r++) {
                uint32_t sz = G.cpu->reg_size(r);
                reg_read(t->ctx, r, buf);
                to_hex(G.out + o, buf, sz);
                o += 2 * sz;
            }
            reply(G.out);
        } else if (p[0] == 'G' && t) {
            const char *q = p + 1;
            uint8_t buf[64];
            bool ok = true;
            for (uint32_t r = 0; r < G.cpu->nregs && ok; r++) {
                uint32_t sz = G.cpu->reg_size(r);
                ok = strlen(q) >= 2 * sz && from_hex(buf, q, sz);
                if (ok)
                    G.cpu->reg_write(t->ctx, r, buf);
                q += 2 * sz;
            }
            reply(ok ? "OK" : "E01");
        } else if (p[0] == 'p' && t) {
            const char *q = p + 1;
            uint64_t r = parse_hex(&q);
            uint8_t buf[64];
            if (r >= G.cpu->nregs) {
                reply("E01");
            } else {
                reg_read(t->ctx, (uint32_t)r, buf);
                to_hex(G.out, buf, G.cpu->reg_size((uint32_t)r));
                reply(G.out);
            }
        } else if (p[0] == 'P' && t) {
            const char *q = p + 1;
            uint64_t r = parse_hex(&q);
            uint8_t buf[64];
            if (r >= G.cpu->nregs || *q != '=' || strlen(q + 1) < 2 * G.cpu->reg_size((uint32_t)r) ||
                !from_hex(buf, q + 1, G.cpu->reg_size((uint32_t)r))) {
                reply("E01");
            } else {
                G.cpu->reg_write(t->ctx, (uint32_t)r, buf);
                reply("OK");
            }
        } else if (p[0] == 'm') {
            const char *q = p + 1;
            uint64_t addr = parse_hex(&q);
            q += *q == ',';
            uint64_t len = parse_hex(&q);
            static uint8_t buf[PACKET / 2];
            if (len > sizeof buf)
                len = sizeof buf;
            size_t got = mem_read(addr, buf, (size_t)len);
            if (!got && len) {
                reply("E14");
            } else {
                to_hex(G.out, buf, got);
                reply(G.out);
            }
        } else if (p[0] == 'M') {
            const char *q = p + 1;
            uint64_t addr = parse_hex(&q);
            q += *q == ',';
            uint64_t len = parse_hex(&q);
            static uint8_t buf[PACKET / 2];
            bool ok = *q == ':' && len <= sizeof buf && strlen(q + 1) >= 2 * len && from_hex(buf, q + 1, (size_t)len);
            reply(ok && mem_write(addr, buf, (size_t)len) ? "OK" : "E14");
        } else if (strcmp(p, "vCont?") == 0) {
            reply("vCont;c;C;s;S");
        } else if ((p[0] == 'Z' || p[0] == 'z') && p[1] >= '0' && p[1] <= '4') {
            z_packet(p);
        } else if (p[0] == 's' || p[0] == 'S' || after(p, "vCont;s") || after(p, "vCont;S")) {
            /* A step runs the thread to its next safe point or breakpoint: lifted code has no single guest instruction to stop after. */
            const char *q = strstr(p, ":");
            rl_gdb_thread *st = q ? (q++, thread_by_id(parse_hex(&q))) : t;
            if (!run_until_interrupt(st ? st->ctx : NULL))
                break;
        } else if (p[0] == 'c' || p[0] == 'C' || after(p, "vCont;c") || after(p, "vCont;C")) {
            if (!run_until_interrupt(NULL))
                break;
        } else if (p[0] == 'D' || p[0] == 'k') {
            reply("OK");
            break;
        } else {
            reply("");
        }
    }
    resume_all();
    rl_log(RL_LOG_INFO, "gdb", "debugger detached: the guest runs on");
}

static void *server(void *arg)
{
    (void)arg;
    for (;;) {
        int fd = accept(G.listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            return NULL;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        G.fd = fd;
        serve();
        close(fd);
        G.fd = -1;
    }
}

bool rl_gdb_start(const rl_gdb_cpu *cpu, const rl_gdb_guest *guest)
{
    int64_t port = rl_setting_int("debug.gdb");
    if (port <= 0 || !cpu || !guest || !guest->threads || !(guest->vm || (guest->mem_read && guest->mem_write)) ||
        G.listen_fd >= 0)
        return false;
    G.cpu = cpu;
    G.guest = guest;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 1) != 0) {
        rl_log(RL_LOG_ERROR, "gdb", "cannot listen on 127.0.0.1:%lld: %s", (long long)port, strerror(errno));
        close(fd);
        return false;
    }
    G.listen_fd = fd;
#if defined(RL_GDB_TRAPS)
    traps_install();
#endif
    pthread_t th;
    if (pthread_create(&th, NULL, server, NULL) != 0) {
        close(fd);
        G.listen_fd = -1;
        return false;
    }
    pthread_detach(th);
    rl_log(RL_LOG_INFO, "gdb", "GDB server on 127.0.0.1:%lld: gdb -ex 'target remote :%lld'", (long long)port,
           (long long)port);
    if (rl_setting_bool("debug.gdb_wait")) {
        rl_log(RL_LOG_INFO, "gdb", "debug.gdb_wait: waiting for a debugger to attach and continue");
        pthread_mutex_lock(&G.lock);
        while (!G.released) {
            pthread_mutex_unlock(&G.lock);
            rl_sleep_ns(20000000);
            pthread_mutex_lock(&G.lock);
        }
        pthread_mutex_unlock(&G.lock);
    }
    return true;
}

#endif
