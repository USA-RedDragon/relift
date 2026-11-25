# Debug a game with gdb

This page is for a developer who is porting a game with relift and needs to see what the guest is doing: which thread waits where, what a register holds, who writes a word of guest memory. It covers the runtime's GDB server, which shows gdb the guest's threads, registers and memory, and plain gdb on the host process, which shows the lifted C and the runtime. The two work on the same process at the same time. Read [Recompile a game](workflow.md) first; this page assumes a project that builds and runs.

The output below is real. It comes from a dev build of a hand-assembled Switch test program (the one the end-to-end tests build) on x86-64 Linux with gdb 18, and from the runtime's own test guest. No game was used. Paths are shortened.

## What you get, and where

| Tool | Sees | Stops | Works on |
|---|---|---|---|
| The GDB server (`debug.gdb`) | guest threads, guest registers (A64, or ARMv4T on the Game Boy Advance), guest memory by guest address, lifted functions by guest address | the guest, at a safe point | Switch, Switch 2 and Game Boy Advance programs; every host but Windows and iOS |
| gdb on the host process | the lifted C (`-gline-tables-only`), the runtime, host threads and their stacks | everything, including the server | every guest and host gdb runs on |

The server is in the runtime of dev builds (`relift run`, `relift build --dev`), of desktop builds with `package.appimage = "never"`, and of APKs signed with the debug key. It is never in a player package: an AppImage, a signed macOS app, an iOS app, or an APK signed with the project's own key (`package.android-keystore`) is built with `RL_NO_DEBUG_STUB`. It listens only when `debug.gdb` names a port. Game Boy, Nintendo 64 and Nintendo DS programs do not start the server today: `rl_gdb_start` is called by the Switch layer (`guests/switch/kthread.c`) and the Game Boy Advance run (`guests/gba/run/cpu.c`) only. For those guests use gdb on the host process (below).

## Start a game with the server

Two settings turn it on, `debug.gdb` (`RELIFT_DEBUG_GDB=<port>`) and `debug.gdb_wait` (`RELIFT_DEBUG_GDB_WAIT=1`); their types, ranges and descriptions are in [Debugging settings](../reference/debug-settings.md). The environment variable is the quickest way. Start with the wait on, so the game does not run past the code you want to see:

```text
$ RELIFT_DEBUG_GDB=2345 RELIFT_DEBUG_GDB_WAIT=1 relift run
[relift info gdb] GDB server on 127.0.0.1:2345: gdb -ex 'target remote :2345'
[relift info gdb] debug.gdb_wait: waiting for a debugger to attach and continue
```

A built program takes the same settings as `--relift-set debug.gdb=2345`. The server binds the loopback address only. On a phone, reach it with `adb forward tcp:2345 tcp:2345`.

## Attach gdb

The server speaks A64 registers, so gdb must know the aarch64 target. A distribution's plain `gdb` often does not (`gdb-multiarch` does); check with `gdb -batch -nx -ex 'set architecture aarch64'`. Then, from the relift checkout so the helper script resolves:

```text
$ gdb -ex 'set architecture aarch64' -ex 'source runtime/tools/gdb/relift_guest.py' -ex 'target remote :2345'
```

`relift_guest.py` gives `bt` the guest call chain and names each frame (see [Backtraces](#backtraces-and-the-lifted-code)). Without it the server still answers threads, registers and memory. `relift_frames.py` is the helper for host-side gdb (below).

### What gdb sees

- **Threads.** Each live guest thread is a gdb thread; its number is the guest thread id. `info threads` shows the state and the lifted function at the thread's pc, as `<module>+0x<offset> <name>`.
- **Registers.** The A64 set, numbered as gdb numbers it; [the GDB server reference](../reference/gdb-stub.md#registers) lists it. `pc` is the guest address the context names, and `cpsr` can be stale, since NZCV is stored only where [the ABI](../abi.md) says.
- **Memory.** Guest memory by guest address, read and written as the kernel does. A page the guest cannot touch answers an error. `x/8gx $sp`, `set {int}0x... = 1` and `p/x $x0` work as on a real target.

Attaching, or Ctrl-C after a `continue`, stops every guest thread at its next safe point: a system call, a runtime entry or a preemption check. Registers are exact where a thread stopped at a system call or a runtime entry; the thread list says `stopped`. A thread stopped at a preemption check says so and shows the registers of its last call boundary, since lifted code keeps the rest in C locals. A thread that reaches no safe point within 2 seconds (a long call-free loop) is listed as `running (did not reach a safe point)`. With `debug.gdb_wait` set, before the guest has started, the one thread lists that way with pc 0:

```text
(gdb) target remote :2345
0x0000000000000000 in ?? ()
(gdb) info threads
  Id   Target Id                                                   Frame 
* 1    Thread 1 "1" (running (did not reach a safe point); pc 0x0) 0x0000000000000000 in ?? ()
```

`detach` leaves the game running.

### Name a guest address

`monitor sym` names the lifted function that holds an address. It takes a number, or a register of the current thread with or without `$`, and an optional `+` or `-` offset in hex:

```text
(gdb) monitor sym $pc
nest+0xa00 inner
(gdb) monitor sym x30 - 4
nest+0x900 mid
(gdb) monitor sym 0x1234
no lifted function holds 0x1234
```

`nest` is the module, `+0xa00` the function's offset from the module's base, and the last word the function's host name. Unnamed functions are `fn_<hex offset>`; a function with a symbol in the guest binary carries it as an Itanium name in the `guest` namespace (`_ZN5guest4main3fooEv` is `guest::main::foo()`, [the ABI](../abi.md) section 11). Load `relift_frames.py` in a gdb that talks to the host process to see those names demangled; the server's answers show the raw name.

gdb sends the text after `monitor` as typed and does not evaluate it, so the server reads registers by the names in its target description itself. A gdb expression such as `$x30 & ~0xf` or a convenience variable is not understood; print it with `p/x` and pass the number.

## A Game Boy Advance program

The server speaks gdb's `arm` target for a Game Boy Advance program, which a plain `gdb` knows. There is one thread, the CPU, and it stops at its next sync point, where every register is exact and `pc` is the next instruction. This is the runtime's GBA test, counting in a loop:

```text
$ gdb -ex 'set architecture arm' -ex 'target remote :2345'
0x08000204 in ?? ()
(gdb) info threads
  Id   Target Id                            Frame 
* 1    Thread 1 "1" (stopped; pc 0x8000204) 0x08000204 in ?? ()
(gdb) p/x $r5
$1 = 0x55
(gdb) x/2hx 0x04000200
0x4000200:	Cannot access memory at address 0x4000200
```

Registers are the current mode's `r0` to `r12`, `sp`, `lr`, `pc` and `cpsr`. Memory is the bus by address, read without wait states or side effects; a write goes through the bus as the CPU's store would. The I/O registers at `0x04000000` cannot be read from gdb, because a device answers a read only after catching up to the CPU's cycle. A stop does not move the guest's clock: the server waits for the CPU's own next sync point instead of ending its cycle budget, and the test `gdb_server_stops_a_gba_run_at_a_sync_point_and_keeps_its_timing` checks that a run stopped halfway ends on the same cycle as one never stopped. Breakpoints, watchpoints, `monitor bt` and `relift_guest.py` are for Switch programs only.

## Breakpoints

On x86-64 Linux hosts the server takes breakpoints and write watchpoints. A breakpoint must be at the entry of a lifted function:

```text
(gdb) break *0x8000000
Breakpoint 1 at 0x8000000
(gdb) continue
Breakpoint 1, 0x0000000008000000 in ?? ()
(gdb) info threads
  Id   Target Id                                                                  Frame 
* 1    Thread 1 "1" (stopped; pc 0x8000000 main+0x0 _ZN5guest4main11fn_00000000E) 0x0000000008000000 in ?? ()
(gdb) p/x $pc
$1 = 0x8000000
```

The server writes an `int3` over the first byte of the function's host code, and a thread that reaches it stops with exact registers and pc at the function. Three limits follow from that:

- A call clang inlined into its caller in the same shard has no entry to stop at. In the test program above, the function at `0x8000030` is called from the one at `0x8000000`; a breakpoint at `0x8000030` never hit and the program ran to its end, while one at `0x8000000` stopped. Break at the caller, or at a function the runtime calls (an entry, a callee of an indirect call).
- A guest address that is not a function entry is refused.
- `stepi` and `next` run the thread to its next safe point or breakpoint, because lifted code has no single guest instruction to stop after. Step at the host level for line by line (below).

When the program ends, gdb says `Remote connection closed`.

## Watchpoints: who writes this word

`watch` on a guest address is the question the server answers best. The server keeps the host pages that hold the watched bytes write-protected; a guest store into them stops its thread right after the store, with the old and new value, and other stores to those pages run on after one stepped host instruction.

```text
(gdb) watch *(long *)0x198000040
Hardware watchpoint 2: *(long *)0x198000040
(gdb) continue

Hardware watchpoint 2: *(long *)0x198000040

Old value = 7
New value = 8
```

That session is the runtime's own test guest, whose heap sits at `0x198000000`. In a game, find the address with `p/x $x0` and `x/8gx $sp` at a stop, or from the guest address a log line prints. After a hit, `info threads` names the lifted function at the thread's pc; the registers are those of the thread's last call boundary. The watched store's own function is therefore the lifted function at pc, not the instruction.

Stores the runtime makes for the guest (system calls, the GPU mirror's readback) are not seen. Prefer this watchpoint to a host `watch` on the process: host gdb has to pass every write-protect `SIGSEGV` that relift takes, which starves the game. A run that goes quiet after a hit is usually the game waiting on something else; compare the log tail with an identical run without gdb before blaming the server (the tests `gdb_server_continues_after_each_write_watch_hit` and `gdb_server_steps_over_a_write_watch_hit_and_continues` cover continuing after hits).

## Backtraces and the lifted code

With `relift_guest.py` loaded, `bt` on a guest thread lists its guest calls, innermost first, named as `monitor sym` names them. This is the runtime's test guest, stopped in the innermost of three nested calls:

```text
(gdb) bt
#0  0x0000000198000a08 in nest+0xa00 inner ()
#1  0x0000000198000914 in nest+0x900 mid ()
#2  0x0000000198000814 in nest+0x800 outer ()
```

`monitor bt` prints the same walk with any client, lldb included:

```text
(gdb) monitor bt
#0 0x198000a08 nest+0xa00 inner
#1 0x198000914 nest+0x900 mid
#2 0x198000814 nest+0x800 outer
```

The server walks the guest's frame records in guest memory, the chain A64 code keeps through x29 and x30, and the helper hands that walk to gdb (the packet is `qRelift.frames`, [reference](../reference/gdb-stub.md#guest-backtrace)). gdb cannot do it alone: without symbols its aarch64 unwinder reads the frame record 16 bytes past x29 and stops after the first frame. The walk has three limits:

- Code built without frame pointers leaves gaps: a function that keeps no record is missing from the chain past the innermost frame.
- At a preemption check the registers are those of the thread's last call boundary, so the walk starts there.
- `frame N` and `info frame` show pc, sp and x29 for each frame; other registers of outer frames are those of frame 0.

gdb on the host process shows the real chain too, lifted callers and runtime together (next section).

## gdb on the host process

The server does not replace gdb on the process. Run the program itself under gdb, or attach with `gdb -p <pid>`, for the lifted C, the runtime and the host threads. Start from the build's dev folder, because the debug info records `.` as the compilation directory (so the cache is relocatable) and the lifted sources are found from there:

```text
$ cd ~/.cache/relift/projects/<project>/build/host/dev
$ gdb -ex 'source ~/relift/runtime/tools/gdb/relift_frames.py' \
      -ex 'handle SIGSEGV nostop noprint pass' \
      -ex 'set breakpoint pending on' \
      -ex 'break _ZN5guest4main11fn_00000000E' -ex run --args ./<project>
```

Two lines matter. `handle SIGSEGV nostop noprint pass`: relift uses write-protect faults for guest memory, and gdb would stop on each one. `source relift_frames.py`: gdb does not demangle the name of a function from a C compilation unit, so without the filter `bt` shows `_ZN5guest4main11fn_00000000E`. With it:

```text
Thread 1 "tiny" hit Breakpoint 1, _ZN5guest4main11fn_00000000E () at ./../c/0_main/shard0000.c:35
#0  guest::main::fn_00000000 () at ./../c/0_main/shard0000.c:35
#1  0x00007ffff6fb0810 in rl_call_guest (c=c@entry=0x55555559f900, mem=0x7ff6dd7fe000 "", entry=entry@entry=134217728) at ./../../runtime/cpus/arm64/arm64.c:126
#2  0x00007ffff6fb2a4f in run_entry (c=0x55555559f900, entry=134217728) at ./../../runtime/guests/switch/kthread.c:164
#3  rl_hos_run_main (entry=134217728, stack_top=15435038720) at ./../../runtime/guests/switch/kthread.c:400
#4  0x00007ffff6f2f955 in run_program (arg=arg@entry=0x7fffffffd138) at ./../../runtime/guests/switch/loader.c:574
#5  0x00007ffff6f2f0b9 in rl_switch_main (p=<optimized out>, argc=<optimized out>, argv=<optimized out>) at ./../../runtime/guests/switch/loader.c:630
```

The backtrace goes from the lifted function through the runtime's entry (`rl_call_guest`) to the program's main. Calls between lifted functions are host calls, so they show as frames too.

### How lifted C maps to guest code

Each lifted function is a C function named for its guest address (`fn_00000030` is the function at module offset 0x30), and each guest instruction opens a label `L_<guest address>` in the C, in the shard file that holds the function (`build/host/c/<index>_<module>/shardNNNN.c`; a shard is a 64 KiB window of the guest). This is the file from the test program:

```c
L_8000000: {
    r19 = UINT64_C(0x3);
}
L_8000004: {
    r0 = UINT64_C(0x8001000);
}
L_8000008: {
    r30 = UINT64_C(0x800000c);
    r1 = UINT64_C(0x11);
}
L_800000c: {
    rl_svc_site(c, UINT64_C(0x800000c));
```

To stop at a guest instruction, find its label with `grep -n 'L_8000010' build/host/c/*/shard*.c` and set `break shard0000.c:<line>`. `next` and `step` then move one C statement at a time, and `list` shows the lines around. Lifted code compiles with line tables only (`-gline-tables-only`), so locals such as `r19` (the guest register held in a C variable) are not visible to `print`; a guest register is in the context `c->x[n]` only at a call, system call or return. A Switch dev build compiles lifted C at `-O2`, so a variable can be optimized out and clang inlines small callees; the Game Boy and Game Boy Advance dev builds use `-O0`, which steps most predictably. `emit.line-tables = false` drops the line tables (the build is about 40% faster, and a crash no longer names the guest function), so leave it at its default of true when you debug. Run `relift report` on a stop report for the lifted function and the C at the transfer site.

### Guest CPUs on fibers

On Switch the device scheduler runs each guest CPU core on a fiber, and `bt` shows only the running one. `relift_frames.py` adds `relift-fibers` and `relift-fiber-bt N` for the parked ones:

```text
(gdb) relift-fibers
#0  guest CPU 0    running      stack 0x7ffff5e00000-0x7ffff6e00000
#1  guest CPU 1    parked       stack 0x7ffff4c00000-0x7ffff5c00000
(gdb) relift-fiber-bt 1
```

### Both at once

The server is a socket in the process, so a client can be attached to it while host gdb has the process under ptrace. A host breakpoint stops every host thread, the server's included, so the guest client waits until the host side continues. The usual order is to find the guest-side cause with the server (threads, memory, a watch), then break on the host to see how the runtime got there. Two cautions from the project's notes: set no host `watch` on guest memory before the page is mapped (read `rl_mem_base()` plus the guest address first), because gdb falls back to a software watchpoint that single-steps the whole game; and break on `rl_crash_signal` for a host crash, which dumps every thread's backtrace.

## Reference

The packets the server answers, its limits, the monitor commands and the register map are in [GDB server](../reference/gdb-stub.md), read from the source. The settings are in [Debugging settings](../reference/debug-settings.md).

## Check the server

`make -C runtime gdb-test` runs gdb against the runtime's test guest: it attaches, lists threads, reads and writes registers and memory, stops at a breakpoint and a write watchpoint, and detaches (`runtime/tools/gdb/stub-test.sh`). It skips when gdb is missing, and fails under CI when no gdb knows the aarch64 target.

## Not available

- No server for Game Boy, Nintendo 64 and Nintendo DS programs, and no GBA I/O register reads, as above.
- Breakpoints and write watchpoints are x86-64 Linux only. On other hosts the server answers registers, memory and threads, and gdb reports the breakpoint as unsupported.
- No breakpoint at a guest address that is not a function entry, and none at a function clang inlined.
