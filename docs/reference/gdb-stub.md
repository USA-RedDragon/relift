# GDB server

The reference for the runtime's GDB remote-protocol server. The guide is
[Debug a game with gdb](../guides/debug-gdb.md); the settings are
`debug.gdb` and `debug.gdb_wait` ([Debugging settings](debug-settings.md)).
Every excerpt on this page is read from the source.

## What it is

<!-- docgen:code file=runtime/core/gdbstub.h region=overview -->
```c
* @brief A GDB remote-protocol server for guest state: gdb or lldb attach over
* TCP and see guest threads, guest registers and guest memory by guest address
* (.work/design/debug-stub.md). The core owns the protocol and the stop; a CPU
* layer describes its registers with an rl_gdb_cpu, a guest layer lists its
* threads with an rl_gdb_guest and calls rl_gdb_point at its safe points.
*
* Off unless the debug.gdb setting names a port; the server listens on
* 127.0.0.1 only (on a phone, reach it with adb forward). Built where
* RL_DEBUG_STUB is defined: this header defines it on Linux, Android and macOS
* unless RL_NO_DEBUG_STUB is, which relift build sets for every player package
* (an AppImage, a signed .app, an iOS app, a release-signed APK). Elsewhere
* every call is an empty inline and the server is not in the program.
```
<!-- /docgen:code -->

## Limits

| Limit | Value |
|---|---|
| Most guest threads listed | <!-- docgen:const file=runtime/core/gdbstub.c name=MAX_THREADS -->`512`<!-- /docgen:const --> |
| Packet size (hex payload) | <!-- docgen:const file=runtime/core/gdbstub.c name=PACKET -->`16384`<!-- /docgen:const --> |
| Wait for running threads to reach a safe point | <!-- docgen:const file=runtime/core/gdbstub.c name=STOP_WAIT_NS print=value -->`2000000000`<!-- /docgen:const --> nanoseconds |
| Most breakpoints | <!-- docgen:const file=runtime/core/gdbstub.c name=MAX_BREAKS -->`64`<!-- /docgen:const --> |
| Most write watchpoints | <!-- docgen:const file=runtime/core/gdbstub.c name=MAX_WATCHES -->`16`<!-- /docgen:const --> |

Breakpoints and watchpoints exist on x86-64 Linux hosts only (the source
defines `RL_GDB_TRAPS` there); on other hosts the server answers a `Z` packet
with an empty reply, which gdb reports as unsupported.

## How a stop works

<!-- docgen:code file=runtime/core/gdbstub.c region=stops -->
```c
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
```
<!-- /docgen:code -->

## Packets

The server answers these packets and replies with an empty packet to any
other, which tells gdb the packet is unsupported. This is the dispatch, as
the source has it:

<!-- docgen:code file=runtime/core/gdbstub.c symbol=serve -->
```c
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
```
<!-- /docgen:code -->

`qSupported` announces `qXfer:features:read`, `qXfer:threads:read`,
`QStartNoAckMode` and `vContSupported`. A step (`s`, `S`, `vCont;s`) runs the
thread to its next safe point or breakpoint, because lifted code has no single
guest instruction to stop after.

### Breakpoints and watchpoints

`Z0` and `Z1` set a breakpoint at the entry of a lifted function; any other
address is refused with error 22, and a full table with error 28:

<!-- docgen:code file=runtime/core/gdbstub.c symbol=break_set -->
```c
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
```
<!-- /docgen:code -->

`Z2` sets a write watchpoint over a guest address range by keeping its host
pages write-protected.

## Monitor commands

`monitor <command>` reaches `qRcmd`:

<!-- docgen:code file=runtime/core/gdbstub.c symbol=monitor -->
```c
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
```
<!-- /docgen:code -->

`sym` names the lifted function that holds a guest address, as
`<module>+0x<offset> <name>` through this function. The argument is text, not
a gdb expression, so give it a number, not `$pc`:

<!-- docgen:code file=runtime/core/gdbstub.c symbol=name_at -->
```c
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
```
<!-- /docgen:code -->

## Threads

The guest layer lists its threads through this structure and the CPU layer
describes its registers through the next one:

<!-- docgen:code file=runtime/core/gdbstub.h symbol=rl_gdb_thread -->
```c
typedef struct rl_gdb_thread {
    /** @brief The guest's thread id, above 0. */
    uint64_t id;
    /** @brief Its context. */
    rl_ctx *ctx;
    /** @brief The guest core it holds, or -1. */
    int32_t core;
    /** @brief True while it may be running lifted code (it holds a core); false when it waits in the runtime. */
    bool running;
} rl_gdb_thread;
```
<!-- /docgen:code -->

<!-- docgen:code file=runtime/core/gdbstub.h symbol=rl_gdb_cpu -->
```c
typedef struct rl_gdb_cpu {
    /** @brief The target description (target.xml), naming the architecture and every register in gdb's order. */
    const char *target_xml;
    /** @brief Number of registers. */
    uint32_t nregs;
    /** @brief Bytes of register n. */
    uint32_t (*reg_size)(uint32_t n);
    /** @brief Writes register n of c to out, little-endian, reg_size(n) bytes. */
    void (*reg_read)(const rl_ctx *c, uint32_t n, uint8_t *out);
    /** @brief Sets register n of c from in, little-endian, reg_size(n) bytes. */
    void (*reg_write)(rl_ctx *c, uint32_t n, const uint8_t *in);
    /** @brief The guest pc of a stopped context, for stop replies and thread names. */
    uint64_t (*pc)(const rl_ctx *c);
    /** @brief The number of the pc register (8 bytes), which a breakpoint stop reports as the function's address. */
    uint32_t pc_reg;
} rl_gdb_cpu;
```
<!-- /docgen:code -->

The Switch layer fills the thread list from its live threads, with the guest
thread id as the gdb thread id and the core it holds:

<!-- docgen:code file=runtime/guests/switch/kthread.c symbol=gdb_threads -->
```c
static uint32_t gdb_threads(rl_gdb_thread *out, uint32_t max)
{
    uint32_t n = 0;
    hos_lock();
    for (hos_thread *t = hos.threads; t; t = t->next_all) {
        if (!t->rt || !t->rt->ctx)
            continue;
        if (n < max)
            out[n] = (rl_gdb_thread){ .id = t->id,
                                      .ctx = t->rt->ctx,
                                      .core = t->cur_core,
                                      .running = t->cur_core >= 0 || hos.sched.free };
        n++;
    }
    hos_unlock();
    return n;
}
```
<!-- /docgen:code -->

## Registers

<!-- docgen:code file=runtime/cpus/arm64/gdb.c region=registers -->
```c
* @brief The A64 registers for the GDB server (core/gdbstub.h), in gdb's own
* aarch64 numbering: x0 to x30 (0 to 30), sp (31), pc (32), cpsr (33), v0 to v31
* (34 to 65), fpsr (66), fpcr (67), and the read-only thread pointers tpidr_el0
* and tpidrro_el0 (68, 69).
*
* pc is the guest address the context names: a system call's own address
* (rl_svc_site) at a system call, the callee at an indirect call. NZCV is stored
* only where docs/abi.md says, so cpsr can be stale.
```
<!-- /docgen:code -->

## Tests

`make -C runtime gdb-test` runs the frame filter test and the server test
(`runtime/tools/gdb/stub-test.sh`) with a real gdb. The server's own tests are
in `runtime/guests/switch/tests/test_gdb.c`.
