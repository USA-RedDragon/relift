/**
 * @file gdbstub.c
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
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int rl_gdb_stopping;

/** @brief Most threads the stub lists. */
#define MAX_THREADS 512
/** @brief Largest packet the stub takes or sends (hex payload), and the PacketSize it announces. */
#define PACKET 16384
/** @brief How long a stop waits for running threads to reach a safe point. */
#define STOP_WAIT_NS 2000000000ull

/** @brief A parked thread: its context and whether its registers are exact. */
typedef struct parked {
    rl_ctx *ctx;
    bool exact;
} parked;

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

void rl_gdb_park(rl_ctx *c, bool exact)
{
    pthread_mutex_lock(&G.lock);
    if (!__atomic_load_n(&rl_gdb_stopping, __ATOMIC_ACQUIRE) || G.nparked == MAX_THREADS) {
        pthread_mutex_unlock(&G.lock);
        return;
    }
    G.park[G.nparked++] = (parked){ c, exact };
    pthread_cond_broadcast(&G.arrived);
    while (__atomic_load_n(&rl_gdb_stopping, __ATOMIC_ACQUIRE))
        pthread_cond_wait(&G.wake, &G.lock);
    parked *p = find_parked(c);
    if (p)
        *p = G.park[--G.nparked];
    pthread_mutex_unlock(&G.lock);
}

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
    for (uint32_t i = 0; i < G.nth; i++)
        if (G.th[i].ctx)
            rl_preempt_request(G.th[i].ctx);
    uint64_t until = now_ns() + STOP_WAIT_NS;
    pthread_mutex_lock(&G.lock);
    for (;;) {
        list_threads();
        bool all = true;
        for (uint32_t i = 0; i < G.nth && all; i++)
            all = !G.th[i].running || find_parked(G.th[i].ctx);
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

static void resume_all(void)
{
    pthread_mutex_lock(&G.lock);
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

static void threads_xml(const char *args)
{
    static char xml[MAX_THREADS * 320 + 64];
    size_t o = (size_t)snprintf(xml, sizeof xml, "<?xml version=\"1.0\"?><threads>");
    pthread_mutex_lock(&G.lock);
    for (uint32_t i = 0; i < G.nth && o < sizeof xml - 400; i++) {
        rl_gdb_thread *t = &G.th[i];
        parked *p = find_parked(t->ctx);
        char nm[200] = "", esc[260];
        uint64_t pc = t->ctx && G.cpu->pc ? G.cpu->pc(t->ctx) : 0;
        name_at(nm, sizeof nm, pc);
        escape_xml(esc, sizeof esc, nm);
        const char *state = p ? (p->exact ? "stopped" : "stopped at a preemption check, registers of its last call")
                            : t->running ? "running (did not reach a safe point)"
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
        const char *p = cmd + 4;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        uint64_t a = parse_hex(&p);
        char nm[512] = "";
        if (!name_at(nm, sizeof nm, a))
            snprintf(nm, sizeof nm, "no lifted function holds 0x%llx", (unsigned long long)a);
        snprintf(text, sizeof text, "%s\n", nm);
    } else {
        snprintf(text, sizeof text, "relift monitor commands: sym <guest address>\n");
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

/** @brief Runs the guest until the debugger interrupts it; false when the connection closed. */
static bool run_until_interrupt(void)
{
    resume_all();
    for (;;) {
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
                G.cpu->reg_read(t->ctx, r, buf);
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
                G.cpu->reg_read(t->ctx, (uint32_t)r, buf);
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
            reply("vCont;c;C");
        } else if (p[0] == 'c' || p[0] == 'C' || after(p, "vCont;c") || after(p, "vCont;C")) {
            if (!run_until_interrupt())
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
    if (port <= 0 || !cpu || !guest || !guest->threads || !guest->vm || G.listen_fd >= 0)
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
