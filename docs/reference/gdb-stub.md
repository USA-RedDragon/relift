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
| Most frames of a guest backtrace | <!-- docgen:const file=runtime/core/gdbstub.c name=MAX_FRAMES -->`64`<!-- /docgen:const --> |

Breakpoints and watchpoints exist on x86-64 and arm64 Linux hosts, Android
included (the source defines `RL_GDB_TRAPS` there); on other hosts the
server answers a `Z` packet with an empty reply, which gdb reports as
unsupported. What is host-specific is the trap instruction (`int3`, or
`BRK #0x7e5`), where the trapped function's `rl_ctx` is (rdi, or x0) and how
one host instruction is stepped: the x86-64 trap flag, or on arm64, which
has no flag a signal handler can set:

<!-- docgen:code file=runtime/core/gdbtrap.c region=arm64-step -->
```c
* arm64 has no trap flag a signal handler can set (single-step is ptrace's), so one host instruction is stepped by
* planting a second trap at every instruction that can run after it: the next one, or for a branch its targets from
* the saved registers (B and BL, both ways of B.cond, CBZ, CBNZ, TBZ and TBNZ, and the register of BR, BLR and RET).
* The instruction itself runs in place, so PC-relative ones (ADR, ADRP, LDR literal) are right. When one of the
* planted traps is hit, all of them go and the breakpoint is set again, or the watched page closed again and a hit
* reported.
```
<!-- /docgen:code -->

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
                    cpu_for(t->ctx)->reg_write(t->ctx, r, buf);
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
                cpu_for(t->ctx)->reg_write(t->ctx, (uint32_t)r, buf);
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
`QStartNoAckMode` and `vContSupported`. `qRelift.frames:<thread>` is
relift's own: the [guest backtrace](#guest-backtrace) of one thread. A step (`s`, `S`, `vCont;s`) runs the
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
    b->orig = code_at(b->host);
    b->on = true;
    poke_code(b->host, TRAP_INSN);
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
```
<!-- /docgen:code -->

`sym` names the lifted function that holds a guest address, as
`<module>+0x<offset> <name>` through `name_at` below. `bt` prints the
[guest backtrace](#guest-backtrace) of the current thread, one named frame a
line. gdb sends the text after `monitor` as typed, so the server reads the
address itself:

<!-- docgen:code file=runtime/core/gdbstub.c region=monitor-args -->
```c
* @brief The address argument of a monitor command. gdb sends the text after `monitor` as typed, without evaluating
* it, so the server reads it here: a hex number (0x optional), or a register of the current thread by its name in the
* target description, with or without `$` (`$pc`, `x30`), either one optionally followed by `+` or `-` and a hex
* number. False when the text is none of these.
```
<!-- /docgen:code -->

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

## Guest backtrace

The CPU layer walks a stopped thread's guest frames (`rl_gdb_cpu.frames`).
The A64 layer follows the frame records in guest memory:

<!-- docgen:code file=runtime/cpus/arm64/gdb.c region=frames -->
```c
* @brief The guest backtrace: the AAPCS64 frame-record chain in guest memory. Each function that keeps a record
* stores its caller's x29 and its return address as a pair at x29, so [x29] is the caller's record and [x29 + 8]
* where the caller resumes. The first step is the one guess: a thread stopped at a function's entry, or in a leaf
* that keeps no record, still has its return address in x30, and x30 then lies outside the function that holds pc
* (a function that already called something holds a return address into itself there). So the caller is x30 when
* x30 is in another lifted function and the record at x29 does not already name it, else [x29 + 8]. The walk ends
* at a zero or unreadable record, a record that is not above the last one, or a zero return address.
```
<!-- /docgen:code -->

`monitor bt` prints the walk. `qRelift.frames:<thread id in hex>` answers it
as `pc,sp,fp,name` per frame, innermost first, separated by `;`, with the
numbers in hex and the lifted function's name hex-encoded (empty where no
lifted function holds pc); a thread with no walk answers `E01`.
`runtime/tools/gdb/relift_guest.py` reads that packet at each stop and hands
gdb the frames through a Python unwinder, so `bt` shows them; it sends the
packet outside the unwinder, because gdb 18 crashes when an unwinder sends a
packet.

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
    /**
     * @brief For a waiting thread whose context does not hold its pc: the guest pc it resumes at (pc_exact), or the
     * next pc of its last sync point (registers exact, pc from before its current access); has_pc false otherwise.
     */
    bool has_pc, pc_exact;
    uint64_t pc;
    /**
     * @brief The register layer of this thread's CPU, or NULL for the server's. It numbers registers as the server's
     * target description does; ctx is then whatever that layer reads (a coprocessor's own state).
     */
    const struct rl_gdb_cpu *cpu;
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
    /** @brief Writes register n of c to out, reg_size(n) bytes in the target's byte order (big_endian). */
    void (*reg_read)(const rl_ctx *c, uint32_t n, uint8_t *out);
    /** @brief Sets register n of c from in, reg_size(n) bytes in the target's byte order. */
    void (*reg_write)(rl_ctx *c, uint32_t n, const uint8_t *in);
    /** @brief The guest pc of a stopped context, for stop replies and thread names. */
    uint64_t (*pc)(const rl_ctx *c);
    /** @brief The number of the pc register (8 bytes), which a breakpoint stop reports as the function's address. */
    uint32_t pc_reg;
    /** @brief Registers go over the wire big-endian (a big-endian guest), else little-endian. */
    bool big_endian;
    /**
     * @brief Fills out with the guest call frames of a stopped context whose pc is pc, innermost first, at most max,
     * reading guest memory with read; returns how many. NULL where the CPU keeps no frame chain.
     */
    uint32_t (*frames)(const rl_ctx *c, uint64_t pc, rl_gdb_frame *out, uint32_t max, rl_gdb_read_fn read);
} rl_gdb_cpu;
```
<!-- /docgen:code -->

<!-- docgen:code file=runtime/core/gdbstub.h symbol=rl_gdb_guest -->
```c
typedef struct rl_gdb_guest {
    /** @brief Fills up to max threads into out and returns how many there are. */
    uint32_t (*threads)(rl_gdb_thread *out, uint32_t max);
    /** @brief The guest address space, for memory reads and writes, or NULL when mem_read and mem_write are set. */
    struct rl_vm *vm;
    /**
     * @brief Reads n bytes at guest address a into dst without side effects (a register read never clears a flag or
     * pops a queue); returns how many it read from the start. NULL: read vm.
     */
    size_t (*mem_read)(uint64_t tid, uint64_t a, uint8_t *dst, size_t n);
    /**
     * @brief Writes n bytes at guest address a as the guest's own stores would; false where it cannot. NULL: write vm.
     * Both callbacks get the id of the thread the debugger selected, for guests whose CPUs see different maps.
     */
    bool (*mem_write)(uint64_t tid, uint64_t a, const uint8_t *src, size_t n);
    /**
     * @brief The guest runs on a cycle clock whose own events bring it to a sync point often (rl_sync): a stop waits
     * for that and leaves the budget alone, since ending the budget early would move the guest's clock.
     */
    bool clocked;
    /**
     * @brief The guest's threads take turns on one host thread (fibers of one scheduler): a stop is complete once one
     * of them parks, since no other can run until it resumes.
     */
    bool one_runs;
} rl_gdb_guest;
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

## Clocked guests

A guest that runs on a cycle clock (the Game Boy Advance) sets `clocked` in
its `rl_gdb_guest`, and a stop then leaves the cycle budget alone. Its safe
point is the sync point:

<!-- docgen:code file=runtime/core/sync.c region=sync-point -->
```c
/* A sync point is a safe point for the GDB server: the exit's registers are stored and next is the guest pc. A
 * stop request ends the budget (rl_preempt_request), so the thread parks here at its next sync point. */
rl_gdb_point_at(c, next);
```
<!-- /docgen:code -->

Its memory comes from the guest's own callbacks:

<!-- docgen:code file=runtime/guests/gba/run/cpu.c region=gdb-memory -->
```c
* @brief Guest memory for the GDB server by bus address: reads are peeks (rl_gba_peek, no wait states, no side
* effect), and the I/O registers (0x04000000 to 0x04FFFFFF) are read with rl_gba_io_peek, each device answering
* from its state without catching up; writes go through the bus as the CPU's stores do (rl_gba_write).
```
<!-- /docgen:code -->

An I/O register is read through its device's `peek` (`rl_sched_peek` in
`core/sched.c`), at the device's own tick and without catching it up. The
value is the device's state as of its last catch-up, which can lag the CPU:
a timer counter (`TM0CNT_L`) or `VCOUNT` read while stopped may be a few
cycles behind what the CPU would read. That is the price of a read that
changes nothing, not a bug.

<!-- docgen:code file=runtime/guests/gba/io.c region=io-peek -->
```c
* @brief A peek of the keypad registers: the keys as last sampled and KEYCNT. Its read samples the host's input first,
* which can raise the keypad interrupt, so a debugger's read must not. Every other GBA I/O reader is a function of the
* device's state and the tick alone, so it is its own peek.
```
<!-- /docgen:code -->

The Nintendo DS sets `one_runs` as well: its two CPUs are fibers of one host
thread, and a thread that is not parked reports the next pc of its last
sync point (`rl_sched_sync_pc`).

<!-- docgen:code file=runtime/guests/nds/run/cpu.c region=gdb -->
```c
* @brief The GDB server's threads: 1 the ARM9, 2 the ARM7. Both CPUs are fibers of one host thread, so a stop ends
* when the first of them parks at a sync point (one_runs); the other is then suspended inside the scheduler with its
* registers stored, at its last sync point (pc exact) or at a memory access it made since (pc of that sync point,
* marked so in the thread list).
```
<!-- /docgen:code -->

<!-- docgen:code file=runtime/guests/nds/run/cpu.c region=gdb-memory -->
```c
* @brief Memory of the selected CPU's map (thread 1 the ARM9's, 2 the ARM7's) without timing or side effects
* (rl_nds_peek: the I/O registers through their devices' peeks); writes as the CPU's untimed stores (rl_nds_write).
```
<!-- /docgen:code -->

<!-- docgen:code file=runtime/guests/nds/io.c region=io-peek -->
```c
* @brief The debugger's read of the NDS I/O registers (rl_device.peek): read32 with peek, so the units that run on
* a read (geometry engine, sound) answer as they last ran, the cartridge's data port gives its last word, and the IPC
* receive FIFO shows its front word without taking it. The other registers' reads change nothing already.
```
<!-- /docgen:code -->

## Registers

### A64

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

### ARMv4T

<!-- docgen:code file=runtime/cpus/arm32/gdb.c region=registers -->
```c
* @brief The ARMv4T registers for the GDB server (core/gdbstub.h), as gdb's arm
* target names them: r0 to r12 (0 to 12), sp (13), lr (14), pc (15) and cpsr
* (16), 32 bits each, the current mode's bank. pc is the guest address the
* thread resumes at where it parked at a sync point. Writing cpsr changes mode
* as an MSR would (rl_arm32_set_cpsr).
```
<!-- /docgen:code -->

### SM83

<!-- docgen:code file=runtime/cpus/sm83/gdb.c region=registers -->
```c
* @brief The SM83 registers for the GDB server (core/gdbstub.h), as gdb's
* gbz80 architecture numbers them: af (a high, f low), bc, de, hl, sp, pc,
* then ix, iy, af', bc', de', hl' and ir, which the SM83 does not have and
* read as zero, 16 bits each, little-endian. pc is the guest address the
* thread resumes at where it parked at a sync point; its bank is the one
* mapped at 0x4000 now.
```
<!-- /docgen:code -->

The Game Boy parks in `rl_gb_sync`, its own sync point:

<!-- docgen:code file=runtime/guests/gb/exact/exact.c region=gdb-point -->
```c
/* Between two instructions, the lifted code's registers stored: a safe point for the GDB server at pc. */
rl_gdb_point_at(c, pc);
```
<!-- /docgen:code -->

<!-- docgen:code file=runtime/guests/gb/exact/exact.c region=gdb-memory -->
```c
* @brief Memory for the GDB server: the CPU's 16-bit bus through the device library's peek (rlgbd_peek, no timing
* effect; the runtime reads opcodes with it mid-run), so 0x4000 to 0x7FFF shows the ROM bank mapped now. Writes go
* through rlgbd_poke, which charges no dots: video RAM, cartridge RAM, work RAM, OAM and high RAM take them; ROM and
* mapper registers, I/O registers and IE are refused, since the guest's own write there acts at a cycle. The
* registers are written in the stopped context (the SM83 layer).
```
<!-- /docgen:code -->

`scripts/ci/gdb-gb.sh` (run by the gb-corpus CI job) builds a test cart and
stops it with a real gdb.

### VR4300

<!-- docgen:code file=runtime/cpus/mips/gdb.c region=registers -->
```c
* @brief The VR4300 registers for the GDB server (core/gdbstub.h), in the
* order of gdb's 64-bit mips target: r0 to r31 (0 to 31), status (32), lo
* (33), hi (34), badvaddr (35), cause (36), pc (37), f0 to f31 (38 to 69),
* fcsr (70) and fir (71), 64 bits each, in the guest's big-endian order (gdb
* needs `set endian big`). pc is the guest address the thread resumes at
* where it parked at a sync point. fir reads as the VR4300's (0xB00) and
* ignores writes.
```
<!-- /docgen:code -->

### RSP

A thread can bring its own register layer (`rl_gdb_thread.cpu`) in the
server's numbering; the N64's RSP does:

<!-- docgen:code file=runtime/guests/n64/run/cpu.c region=rsp -->
```c
* @brief The RSP as the GDB server's thread 2, in the same mips register numbering: r0 to r31 sign-extended from 32
* bits, pc the IMEM address SP_PC names in the CPU's map (0x04001000 + SP_PC), the rest zero. It runs in slices inside
* the CPU's device catch-up, so while the CPU is stopped it is between slices. Its registers are read-only here.
* Thread 2 reads the SP memory window (0x04000000 to 0x04001FFF, DMEM then IMEM, also by its kseg0 and kseg1
* addresses), where the RSP's code and data are.
```
<!-- /docgen:code -->

<!-- docgen:code file=runtime/guests/n64/run/cpu.c region=gdb-memory -->
```c
* @brief Memory for the GDB server by the VR4300's virtual address, translated as its loads are (rl_mips_translate,
* kseg0 and kseg1 directly, the rest through the TLB; a 32-bit address is sign-extended, as gdb in a 32-bit ABI sends
* it). RDRAM reads as the CPU sees it, its data cache's dirty lines
* included, with no side effect; writes are the CPU's stores. Other physical addresses (registers, cartridge) are not
* the SP, DP, MI, VI, AI, PI, RI and SI registers read by rl_n64_peek, as the devices last ran (no RSP catch-up, so a
* value can lag the CPU); the cartridge ROM reads from its image; the PIF bus is refused. Register writes are refused.
```
<!-- /docgen:code -->

## Tests

`make -C runtime gdb-test` runs the frame filter test and the server test
(`runtime/tools/gdb/stub-test.sh`) with a real gdb; the server test also
checks `bt` with `relift_guest.py`, `monitor bt` and `monitor sym` with
registers, then runs gdb's `arm` target against a GBA run
(`gba_run_tests`, which also has the timing test). The server's own tests are
in `runtime/guests/switch/tests/test_gdb.c`.
