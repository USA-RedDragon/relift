# Runtime ABI contract

This is the contract between relift-emitted C and the relift C runtime. It
lives in two headers: `runtime/core/include/relift/abi_core.h`, the
CPU-neutral half (export macros, return codes, the pending record, the
context block the core owns, unit registration, the dev protocol), and
`runtime/cpus/a64/include/relift/a64/abi.h`, the ARM64 half (`rl_ctx`,
memory access, exclusives, barriers, dispatch lookup, system instructions,
floating point), which includes the first. Lifted A64 code includes only the
A64 header and builds with `-Iruntime/core/include
-Iruntime/cpus/a64/include`. This file gives the reason for each choice.
Contract version: `RL_ABI_VERSION` 4.

## 0. Runtime layers

The runtime is three C layers, mirroring the Rust folders:

| Layer | Folder | Owns |
|---|---|---|
| core | `runtime/core` | No guest OS and no guest CPU. Guest memory (reservation, the exact map at the guest page size, the core's own permission flags, an opaque per-block guest tag), faults at guest addresses, host threads holding an opaque context, the clock at the guest tick rate, unit registration and the dispatch table, pending records, logging, configuration and the unknown-call reporter |
| CPU | `runtime/cpus/a64` | `rl_ctx` and everything of sections 3 to 10 that names an ARM64 register or instruction: FPCR, WFE, CNTPCT and CNTFRQ, exclusives, `rl_undefined`, `rl_resume`, the miss entry, register summaries for reports |
| guest | `runtime/guests/switch` | Horizon: the kernel, `rl_svc`, IPC, `sm:`, the NPDM-driven layout, the 4 KiB page, the 19.2 MHz counter, and the translation between Horizon's permission bits and the core's |

The layers above describe themselves to the core at start-up with two small
structs (`runtime/core/core.h`): `rl_guest_desc` (page size, widest address
space, tick rate) and `rl_cpu_desc` (context size and alignment, the offset
of the core's block in the context, the code granule, data byte order, and
init, bind and describe hooks). The core reads no register: it reaches its
own fields through the descriptor's offset, and asks the CPU layer for a
register summary when it reports a crash. Each layer compiles with only its
own include paths and those below it, each layer's tests link nothing above
it, and `runtime/charter/check.py` fails when core includes a CPU or guest
header or spells a guest or CPU word (see `runtime/README.md`). [verified:
`core_tests` run the core against a fake guest and CPU with 8 KiB pages, a
32-bit space, a 40.5 MHz clock, big-endian data and a 2-byte granule]

Tags:
- **[verified]**: backed by a prototype test or by a check run for this
  contract (listed under "Checks run for this contract").
  The prototypes are not yet in the repo.
- **[inferred]**: reasoning only, not run.
- **[open]**: not decided or not proven; listed again under "Open items".

Prototype names used in the tags:
- **memory prototype**: a benchmark of guest memory access strategies
  (base+offset, masking, global base) on x86-64 Linux and an Apple M2 Mac.
- **Sail prototype**: a generator from the Arm Sail model to a small IR and
  an interpreter for it, checked against Unicorn on a set of instruction
  forms.
- **libnx test programs**: small libnx programs (TLS, exclusives, spin flag,
  C++ exceptions, sleep) that the lifted output must reproduce.
- **exceptions prototype**: a hand-written lifted-style C++ exception test on
  x86-64 Linux and an Apple M2 Mac.
- **placement prototype**: a test of Vulkan placed memory mapping, host
  pointer import and shadow buffers inside a guest reservation, on an NVIDIA
  GPU and llvmpipe on x86-64 Linux and MoltenVK on an Apple M2 Mac.
- **16 KiB-page prototype**: copy, ownership-flip and trap-and-emulate
  handling of 4 KiB guest mappings on 16 KiB host pages, on an Apple M2 Mac.
- **slice prototype**: a lift, compile and compare run over a small set of
  functions, checked against a reference with many inputs.
- **hot-load prototype (not yet in the repo)**: a runtime as a shared library,
  a stand-in for relift's dev server, and a hand-written host, run end to end
  (miss, socket round trip, compile, load, register, resume) on x86-64 Linux
  an Apple M2 Mac and a Windows 11 24H2 x86-64 VM.
- **probe**: the ABI probe programs described under "Checks run for this
  contract".

## 1. Hosts and compilers

- 64-bit little-endian hosts only; clang or zig cc (gcc-compatible builtins).
  The header refuses anything else at compile time. Reason: every target in
  scope (x86-64 and arm64 on Linux, Windows gnullvm, macOS, Android, iOS) is
  64-bit little-endian, and the helpers use `__atomic_*`, `__int128` and
  `musttail`. [verified: header compiles for x86_64-linux-gnu,
  aarch64-linux-gnu, x86_64-pc-windows-gnu, Apple clang on arm64]
- Lifted C is built with `-std=c11 -O2 -ffp-contract=off`. Without
  `-ffp-contract=off` arm64 hosts fuse `a * b + c` and diverge from x86.
  [verified: memory prototype]
- x86-64 builds pass `-mcx16`, and the header refuses to compile without it.
  Without it, 16-byte compare-and-swap becomes a call to
  `__atomic_compare_exchange_16`, which may take a lock. [verified: clang 23
  and zig cc 0.13.0 and 0.17.1 emit `lock cmpxchg16b` with the flag and a
  library call without it]
- Player builds use zig cc 0.17.1 with `-O2 -fno-sanitize=undefined`, plus
  `-mcx16` on x86-64. zig compiles and links the exclusive helpers for
  x86-64 and arm64 Linux, Windows gnullvm and macOS; on arm64 they are inlined
  (0.17.1) or use outline helpers that zig's compiler-rt provides (0.13.0).
  [verified: a contended exclusives test gives correct totals on x86-64 Linux,
  arm64 Linux under qemu, and arm64 and x86-64 (Rosetta) macOS on an Apple M2
  Mac, and x86-64 Windows 11; arm64 Windows builds link but were not run]
- On arm64 without `-march=...+lse`, clang emits outline atomics
  (`__aarch64_cas16_acq_rel`); these come from compiler-rt, which zig cc
  ships. [verified: disassembly; zig link not tried, open]
- Runtime C that replaces guest libc routines is built with `-fno-builtin`,
  or clang turns byte loops into calls to the routine itself. [inferred from the slice
  prototype]

## 2. Guest memory

- **Base passing.** Every lifted function takes the guest memory base as its
  second parameter, `uint8_t *mem`. A guest address `a` is `mem + a`. The
  base is never a global and never loaded from the context. Reason: an
  externally visible base is reloaded after every store (1.5x to 1.6x
  slower); a base in a local is as fast as raw host pointers and lets clang
  vectorize. A parameter is a local that also survives calls in a register.
  [verified: memory prototype]
- **`mem` is not `restrict`.** Reason: with `restrict`, clang treats guest
  memory as unreachable by anything else and moves loads across compiler
  barriers, so `RL_LOOP_FENCE` stops working. [verified: a spin loop with
  `restrict` mem and `__atomic_signal_fence` compiled to `jmp .` on x86-64
  and arm64; without `restrict` it reloads each iteration]
- **Accesses.** Plain loads and stores go through `rl_ld*`/`rl_st*`, which
  are `memcpy` of the access size, so any alignment works. [verified: slice
  prototype, 40,000 inputs including misaligned ones]
- **Reservation.** The runtime reserves the guest space (2^39 on desktop;
  smaller on Android) `PROT_NONE` with
  `MAP_NORESERVE`, plus a 1 MiB guard below the base and a 4 GiB guard above
  it. The low guard covers null pointer plus immediate offset (imm9 and LDP
  imm7 reach -1024; LDR unsigned offsets are positive). The high guard
  covers a 32-bit offset added to a pointer near the top. A guest address
  beyond both guards is a wild pointer that would fault on Horizon too; here
  it may hit host memory. No masking: masking costs 1.2x to 2.5x.
  [verified cost: memory prototype; guard sizes inferred]
- **Faults.** The runtime catches SIGSEGV and SIGBUS (macOS arm64 raises
  SIGBUS for `PROT_NONE`). A fault address inside the reservation is a guest
  fault at `si_addr - mem`. [verified: placement prototype]
- **16 KiB host pages.** Lifted code does not care; only the runtime's
  memory manager does. `mmap(MAP_FIXED)` and `mprotect` at a 4 KiB offset fail
  with EINVAL on an Apple M2 Mac [verified: placement and 16 KiB-page
  prototypes]. The runtime keeps the exact 4 KiB guest state for
  `svcQueryMemory` and friends, and handles each case as follows:
  - **Module segments and permission changes with unaligned edges:** a host
    page gets the union of the permissions of the guest pages in it.
    Execute permission never reaches host memory (code is lifted), so only
    read-only next to read-write over-grants, and only a guest bug notices.
    Consequence: a guard page smaller than 16 KiB is not enforced. A strict
    mode keeps the one mixed host page `PROT_NONE` and emulates each access
    to it in the fault handler, at about 2 us per access.
  - **Permission changes on 16 KiB-aligned ranges:** plain `mprotect`.
  - **`svcMapMemory` (move semantics, e.g. stack mirrors):** copy the range to
    the destination, make the source fault, copy back on unmap. Exact; costs
    about 5 us for 4 KiB to 350 us for 1 MiB.
  - **Shared memory that is not 16 KiB aligned:** a real host alias. Each
    object has one host page more than it needs, and its bytes start at the
    4 KiB offset within a host page where the guest maps it, so the host
    pages that cover the guest range map the object's pages with no copy.
    The first guest view picks the offset and moves the bytes (services
    reach them through the kernel lock or a per-object lock, so the move is
    safe). The rest of each covered host page must be free guest address
    space; the kernel reserves it (reported as inaccessible) until the view
    is unmapped, and refuses the map when another mapping is already there.
    A second view at a different offset is refused while the first is
    mapped.
  - **Transfer memory:** lent in place, so it only changes permissions and
    follows the union rule above.
  - Vulkan host-pointer imports need 16 KiB alignment and size on macOS.
  [verified: 16 KiB-page prototype on an Apple M2 Mac, all correctness checks
  including 20,000-operation random alias tests against a reference; normal
  load 1.25 ns, emulated access about 2.06 us]

## 3. Guest context (`rl_ctx`)

One `rl_ctx` per guest thread, allocated by the core from the A64 CPU
descriptor, 64-byte aligned. Lifted code reaches it only through its first
parameter `c`. The fields from `pend` to `disp_count` (offset 840, 56 bytes)
are the core's block, `rl_ctx_common` in `abi_core.h`; static asserts tie
their offsets to it, and the A64 descriptor gives the core offset 840.

| offset | field | notes |
|---|---|---|
| 0 | `x[31]` | x0 to x30 |
| 248 | `sp` | |
| 256 | `v[32]` | 16 bytes each, union of lanes |
| 768 | `pc` | target of the current indirect call or jump |
| 776 | `nzcv` | packed, MRS NZCV layout |
| 780 | `fpcr` | |
| 784 | `fpsr` | |
| 788 | reserved | |
| 792 | `tpidr_el0` | |
| 800 | `tpidrro_el0` | |
| 808 | `excl` | exclusive monitor, 32 bytes |
| 840 | `pend` | pending record, 24 bytes |
| 864 | `thread` | runtime thread object |
| 872 | `disp_table`, `disp_lo`, `disp_count` | dispatch table view |
| 896 | `time` | preemption clock, an `rl_time` (section 14.1) |
| 960 | | size |

`_Static_assert`s in the header pin every offset, the size and the alignment.

Reasons:
- **x0-x30 then sp, 256 bytes; v at 256.** GPRs and SP fill the first 256
  bytes so `v` starts 16-byte aligned without padding. sp is separate from
  `x[]` because register 31 means sp or zr depending on the instruction, and
  the Sail cut line already reads them through separate accessors (the Sail
  prototype: X_read, SP_read). [verified: Sail prototype]
- **v0-v31 as 128-bit unions.** The Sail IR treats each V register as one
  128-bit slot with partial reads and writes (Vpart_read/Vpart_set); a union
  of `__int128`, `d`, `s`, `h`, `b` lanes matches that and needs no type
  punning beyond what C11 allows for unions. d8-d15 for exceptions are
  `v[8..15].d[0]`. [verified: Sail prototype]
- **NZCV packed, stored rarely.** Flags live in four C locals inside a
  function, and clang deletes them where they are dead (zero cost in the
  plain build). The context field is written only by MSR NZCV, by the
  full-state build at every boundary, and by the runtime when it captures a
  thread (svc, fault). Packed in the MRS layout because MRS and MSR then copy
  it as is and the full-state build stores one word. NZCV is not live across
  calls and returns in the plain build; AAPCS64 says it is not preserved,
  and the checker's full-state build catches code that relies on it.
  [verified: slice prototype, flag cost 0 in the plain build, full-state
  build catches a V-flag mutation the plain build misses]
- **pc** is not a program counter of the running code; it is the target of
  the last indirect transfer, written just before the lookup, so the miss
  handler can have the lifted signature (section 6). Host pc maps to guest
  pc for reports (section 11). [inferred]
- **tpidr_el0, tpidrro_el0.** Per-thread values. `tpidrro_el0` is the guest
  address of the thread's 0x200-byte TLS slot in guest memory; libnx keeps
  its thread pointer at `tpidrro_el0 + 0x1f8` and reads it with a plain
  load, so that slot is ordinary guest memory. nnSdk code reads
  `tpidrro_el0` too (the IPC buffer lives there) and uses `tpidr_el0`.
  [verified: disassembly of a libnx TLS test program]
- **excl** holds the local monitor in the context, not in locals, so an
  LDXR and STXR in different functions or across a call still pair up. With
  `c` declared `restrict`, clang keeps the fields in registers inside a loop.
  [verified: section 8 probe]
- **pend** is the exceptions prototype's resume record, extended with a kind (section 7).
- **thread** points to the runtime's thread object, opaque to lifted code.
- **Dispatch view** (`disp_table`, `disp_lo`, `disp_count`) is copied into
  each context so the inline lookup reads it through `c` (cached by clang
  thanks to `restrict`) instead of a global that a guest store could alias.
  [inferred from the memory prototype's global-reload result]
- **time** is the preemption clock: another host thread ends its budget to
  stop the thread at its next preemption point (section 14.1). It sits
  after the core's block, in a cache line of its own that only the
  scheduler writes, so the check of a running thread reads a line it
  keeps. [inferred]
- **64-byte alignment** keeps two threads' contexts off one cache line.
  [inferred]

## 4. Lifted function signature

```c
int F(rl_ctx *restrict c, uint8_t *mem);   /* RL_LIFTED(F) */
```

- Return `RL_OK` (0) for a normal return, `RL_PENDING` (1) when `c->pend`
  says the host stack must unwind (guest exception, thread exit, process
  exit). Reason: the exceptions prototype measured the check after a call at below noise on
  both hosts (one `test/jne` or `cbnz`), against 0.4 to 3.4 ns per entry for
  setjmp. Thread and process exit reuse the same path so the runtime never
  longjmps over lifted frames. [verified: exceptions prototype; exit reuse inferred]
- `c` is `restrict`: the context is never reachable through `mem` (it is
  host memory outside the reservation), and with `restrict` clang keeps
  context fields in registers across guest stores. [verified: with
  `restrict`, `c->x[1]` was not reloaded after a store through `mem`; without
  it, it was]
- Host overrides that replace a guest function use the same signature, so
  calls, dispatch entries and unwinding treat them the same.
- Considered: passing guest registers as C arguments (fewer context stores
  per call). Rejected for now: one signature keeps dispatch, hot-load and
  overrides simple, and the exceptions and slice prototypes measured the context
  traffic as the main cost only for tiny leaves. [open: measure on real code after phase 4]

### Locals and the context

- Inside a function every guest register is a C local. At entry the
  function loads the registers it reads from `c`.
- **Call boundary rule:** before any call (BL, BLR, imported or override
  call, `rl_svc`, any runtime entry that reads registers) the caller stores
  every register it has written since entry or since the last boundary, and
  `sp`. After the call it reloads the registers it reads afterwards. So at
  every boundary `c` holds the full architectural state (NZCV excepted, see
  above). This does not depend on the guest following AAPCS64.
  [verified shape: exceptions prototype; the slice prototype found this
  traffic to be the main per-call cost]
- **Exit rule:** at RET and before a tail call, store every register written
  on any path. A path that did not write a register stores the value it
  loaded, which forces a load (in the slice prototype a strlen loaded x10/x11 only for
  this). Allowed optimization: the emitter may skip storing x9-x17 and
  v16-v31 (and the upper halves of v8-v15) at exits when its interprocedural
  analysis shows no caller reads them before writing. The full-state checker
  build always stores everything. [inferred; open: the analysis]
- **Dead writes.** Inside a function the emitter drops a register write
  when every path from it writes the register again before any read and
  before any point that stores the context (a call, trap, return, tail
  call, indirect jump, guard or instruction that does not lift). Such a
  value is never read, so the state at every boundary is unchanged. A
  transfer between the pieces of a split function may store the stale
  value to `c`, but the next piece writes it again before it reads it. NZCV computations a later compare
  overwrites are the common case. [verified: the libnx corpus]

### Split functions

A guest function whose C would pass the emitter's piece budget
(`emit.piece-kib`, 1 MiB by default) is written as several C functions, so
no one function or file grows past what clang can compile in a few GB.

- The guest function keeps its name `F` as a stub with the lifted signature:
  it sets `c->pc` to the guest entry and tail-calls the piece that holds it.
  Dispatch, direct calls and the unit table name only `F`.
- Each piece `F_part<k>` holds a run of the function's instructions in
  address order, cut at instruction boundaries. It loads the registers it
  reads from `c` and starts with `switch (c->pc)` over the instructions
  other pieces enter it at, each a `goto` to that instruction's label.
- A transfer to an instruction in another piece follows the exit rule: store
  every register written, set `c->pc` to the target and
  `RL_MUSTTAIL return F_part<j>(c, mem);`. So `c` holds the full state at
  every transfer, as at a call boundary, and a piece is entered like a
  function. Calls, the unwind-flag returns and the pending checks inside a
  piece are as in an unsplit function.
- Landing pads: every piece of a function with resume points has its own
  `lp_dispatch`, which reaches a resume point in another piece by the same
  transfer.
- Pieces go in shards of their own (`emit.shard-kib`, 4 MiB by default, the
  byte budget for every shard).
- **Maps.** Every piece has its own `rl_fn_info` in the map of the shard
  that holds it: `fn` is the piece, `guest_addr` its lowest instruction,
  `guest_size` up to the next piece (the last piece to the end of the
  function), and `fn_addr` the entry of the guest function. The stub has an
  entry with `guest_size` 0 at the `guest_addr` of the first piece, which
  keeps the table sorted; guest lookup never selects it. Host lookup goes by
  the start of each host function, so a pc inside a piece resolves to the
  right guest function in whatever order clang places the pieces; guest
  lookup finds the piece that holds an address.

## 5. Calls

- **Direct (BL):** store per the boundary rule, set x30 to the guest return
  address, call `F_<target>(c, mem)` by name, then the pending check.
  PLT stubs are resolved at build time to direct calls when the target is
  known. Each shard declares only the callees it uses. [inferred]
- **Indirect (BLR):** `rc = rl_indirect(c, target)(c, mem);`
  `rl_indirect` stores `target` in `c->pc` and calls `rl_lookup`:
  `off = target - disp_lo`, `i = rotate_right(off, 2)`, then
  `i < disp_count ? disp_table[i] : NULL`, and NULL becomes
  `rl_dispatch_miss`. The rotate folds the alignment check into the bounds
  check (a misaligned target gets high bits and misses). Hit path: one
  subtract, one rotate, one compare, one load, one indirect call. Miss path:
  the miss handler, called in place with the same arguments, so the
  emitted code is identical for hit and miss. [verified: probe, 10 million
  indirect tail calls, misaligned and out-of-range targets miss]
- **Table shape:** one flat array with an entry per code granule of the CPU
  (`rl_cpu_desc.code_shift`; a 4-byte word for A64) over the
  guest code range of all loaded modules, zero for "no entry". Functions
  are spread through the whole text, so nearly every table page holds an
  entry and the resident cost is close to the full 2 bytes of table per byte
  of guest text. [verified: static count of function starts against table
  pages on real SDK-based guest code; 83% of 4 KiB pages and 96% of 16 KiB
  pages touched]
- **Per host target:** desktop hosts use the flat table (one load per
  indirect call). Android and iOS use an open-addressing hash table keyed by
  guest address, about 16 bytes per known entry at 50% load, an order of
  magnitude smaller, at the cost of a hash and a probe per indirect call.
  The emitter calls the same `rl_dispatch` helper either way. [decided;
  hash cost not yet measured]
- **Table reads:** shipped builds never change the table after start-up, so
  `rl_lookup` uses a plain load. Dev builds (`-DRL_HOTLOAD`) use an acquire
  load, paired with the release store at registration (section 6).
  [inferred]
- **Tail calls (B to another function, BR resolved to a function):**
  `RL_MUSTTAIL return F_<target>(c, mem);` or
  `RL_MUSTTAIL return rl_indirect(c, target)(c, mem);`. `musttail` makes
  the host stack not grow for guest tail-call chains. No pending check:
  the callee's code is returned as is. [verified: clang 23 emits `jmp`/`b`
  on x86-64 Linux, x86-64 Windows gnu and arm64; probe ran 10 million
  indirect tail calls on x86-64 Linux, arm64 under qemu and an Apple M2 Mac]
- **Unresolved BR inside a function** (unknown jump table): treated as an
  indirect tail call to the target. The target gets its own entry
  (`RL_FN_FRAGMENT`); since the context holds the full state at that point
  and the guest frame is in guest memory, the fragment continues correctly.
  Its landing pads come from the LSDA of the FDE that contains it.
  [inferred]
- **Pending through the dispatcher:** the lookup is not a frame; the caller
  gets the callee's return code directly, and the miss handler returns the
  return code of the function it finally calls. No extra code.
  [verified for the hit path by the probe; miss path inferred]

## 5.1 Linking between modules

A title with several modules (a dynamic loader, the program, SDK and
middleware libraries) links them at run time: the guest's own loader walks
the loaded modules, applies their relocations, resolves each import against
the other modules' exports and fills the GOT. relift lifts that loader like
any other module and lets it do this work, instead of resolving imports at
build time. [decided]

- **What the runtime provides:** every module mapped at the address it was
  lifted for, text reported by the memory query as static code with
  read and execute permission (the guest loader finds modules that way),
  and one dispatch table over the code of all modules, so a call through a
  GOT slot the guest loader filled is an ordinary indirect call.
- **Why not build-time linking:** the guest loader also runs the init
  arrays and picks the definition that wins when several modules export one
  name; doing that in relift would copy behavior the guest already
  carries, and it would have to match the guest's lookup order exactly.
  Lifting it costs only the loader's few functions. Cross-module calls stay
  indirect (one table load each, section 5); build-time rewriting of PLT
  stubs to direct calls remains possible later as an optimization when the
  winning definition is known.
- **Discovery:** exported functions are roots of their own module, so the
  targets the guest loader writes into GOT slots are lifted entries. A slot
  that resolves to a function no module exports goes to the miss path
  (section 6).

## 6. Missed code

`int rl_dispatch_miss(rl_ctx *c, uint8_t *mem)` has the lifted signature
and reads the target from `c->pc`.

### Dev builds (`relift run`, `RL_HOTLOAD` defined)

1. Take the process miss lock. If another thread already loaded the target,
   release the lock and continue at step 6.
2. Connect once per process to the socket named by `RELIFT_DEV_SOCKET`
   (Unix domain socket; AF_UNIX on Windows 10 1803+ and 11, verified on
   Windows 11 24H2) and send
   `RL_DEV_HELLO` (ABI version, host page size, image id, pid). relift
   refuses a different image id or ABI version.
3. Send `RL_DEV_LIFT` with the target, the call site, the guest thread id,
   the reason (`RL_MISS_CALL`, `RL_MISS_JUMP`, `RL_MISS_ENTRY`,
   `RL_MISS_RESUME`) and the thread's floating-point mode
   (`rl_dev_lift_req.fp_mode`; the FPCR for A64). `site` and `reason` are best
   effort and relift must not depend on them: at a miss the runtime only
   knows `c->pc` (`rl_indirect` stores nothing else), so it cannot tell an
   indirect call from an indirect jump (it sends `RL_MISS_CALL`), and the
   site is 0 when unknown and lost after a tail call. No ABI version bump.
   [verified: hot-load prototype sends `RL_MISS_CALL` and site 0]
4. relift lifts the target and all code newly reachable from it, compiles
   **one** shared library for the whole request, adds the target to the
   hint list in `game.toml`, and answers `RL_DEV_LIFT_REPLY`: status,
   absolute library path, message text. One library per request because
   macOS pays about 125 to 260 ms on the first load of any new library
   file; reopening a loaded file costs under 1.3 ms, and a byte-identical
   copy at a new path pays again, so the cause is inferred to be a per-file
   system check. [verified: hot-load prototype on an Apple M2 Mac; cause
   inferred] A loaded library is never unloaded.
5. The runtime opens the library, calls its exported `relift_unit()`, and
   refuses the unit unless `abi_version == RL_ABI_VERSION`,
   `ctx_size == sizeof(rl_ctx)` and `image_id` matches. It then stores
   each entry into an empty table slot with a release store (existing
   entries win, except `RL_FN_OVERRIDE`), and adds the unit's functions and
   line map to the crash map.
6. Release the lock and `RL_MUSTTAIL return rl_lookup(c, c->pc)(c, mem);`.

Only the missing thread waits; other guest threads keep running, because
table entries change only from NULL to a function, once. [inferred]

**Implementation** (`runtime/core/devload.c`, `runtime/core/dispatch.c`,
`relift/src/dev.rs`) [verified: `relift/tests/e2e_hotload.rs` on x86-64
Linux, an Apple M2 and x86-64 Windows 11 24H2, where relift is a gnu-target
build and the program and the hot-loaded libraries are built with `zig cc`
0.17.1; the arm64 Windows runtime compiles but no arm64 Windows machine ran
it]:

- On Windows relift listens on an AF_UNIX socket through the `uds_windows`
  crate (the standard library has none there), the runtime connects with
  Winsock, and the dev runtime is `relift_rt.dll` beside `game.exe`, which the
  program and every hot-loaded library link against directly (lld links a
  DLL as an input, so no separate import library is built). The program
  image defines the text bounds markers (`RL_TEXT_BOUNDS_DEFINE`) in its
  main file and each hot-loaded library in its export file.
- Without Ninja relift compiles in parallel itself, and when it finds no
  clang it drives `zig cc`.

- The guest layer sizes the dispatch table over the code of every module
  before it registers the static units (`rl_dispatch_reserve`), so a
  hot-loaded unit's entries fit and the table never moves while threads
  hold it. `rl_unit_add` refuses entries outside it.
- The image id is a SHA-256 over the modules in load order (name, load
  address, SHA-256 of the image) and of `game.lock` when there is one. The
  Switch program names it in `rl_switch_program.image_id` (defined in
  `relift_info.c`), so the lifted unit tables do not change with the mod
  set; the runtime sets it before it registers the static units
  (`rl_dispatch_set_image`), or else takes it from the first static unit
  that carries one, and sends it in the hello. Hot-loaded units carry it.
- relift keeps the entries of the build (`entries.txt` in the build
  directory, written by the lifting step) and, per request, discovers from
  the target past those entries only (`relift_analyze::discover_more`): a
  direct call from new code to a known function goes through `rl_indirect`
  with a constant target, since the unit cannot name the program's
  symbols. The new entries join the known set for later requests.
- A unit is compiled with the flags of lifted code plus `-fPIC
  -fvisibility=hidden -DRL_HOTLOAD`, with FPSR flags
  (`-DRL_FP_FLAGS -ffp-exception-behavior=strict`) whenever any function of
  the build produces them: the code was reached from where the analysis
  could not follow, so its region cannot be bounded.
- The runtime logs each load at info level with the time of the round trip,
  the load and the registration; `relift run` prints the lift and compile
  times of each request.
- The next `relift build` adds each hint as a root (`RootKind::Hint`); the
  FPSR analysis counts hinted roots as entered from outside.

**Frames:** every message is `rl_dev_hdr` (magic `RLDV`, kind, seq,
payload length) and a fixed payload struct; replies add `path_len` and
`msg_len` bytes of UTF-8 after the struct. Little-endian, fixed sizes pinned
by `_Static_assert`. Binary because the runtime is C and the socket is local
to one machine. [inferred]

**Hot-loaded code** calls everything outside itself through `rl_indirect`,
never by symbol, so it does not need the main program's lifted symbols. It
calls runtime entry points by name: in dev builds the runtime is a shared
library (`librelift_rt`) that both the program and hot-loaded units link
against. Shipped builds link it statically (`RL_STATIC_RUNTIME` on
Windows). [verified: hot-load prototype on x86-64 Linux, an Apple M2 Mac and
Windows 11 24H2 x86-64 built with zig cc 0.17.1]

**Linking a unit:** a unit links against the runtime library: `-lrelift_rt`,
or the import library (`librelift_rt.dll.a`) on Windows. macOS rejects
unresolved symbols otherwise, and Windows needs the import library.
Linux units build with `-Wl,--no-undefined`. Units are built with hidden
visibility; the only symbol a unit exports is `relift_unit`
(`RL_UNIT_EXPORT`). The runtime library exports every entry declared in the
header (`RL_RUNTIME_API`, defined as default visibility on ELF and Mach-O,
`dllexport` with `RL_BUILDING_RUNTIME` and `dllimport` otherwise on
Windows). [verified: hot-load prototype, one runtime instance checked from
inside a unit; Linux run with `-fvisibility=hidden -Wl,--no-undefined`]

**Failures:** `rl_dev_reply.status` is `RL_DEV_OK` (0), `RL_DEV_ERR_INTERNAL`
(1), `RL_DEV_ERR_HELLO` (2, relift refused the hello) or `RL_DEV_ERR_COMPILE`
(3, the lift or compile failed). A nonzero status, a missing or unreachable
socket, a unit that fails the checks in step 5, and a miss in a shipped
build all report on stderr and end the process with `RL_EXIT_SOFTWARE`
(70, EX_SOFTWARE). [verified: hot-load prototype, each case]

**Socket path:** it must fit `sun_path` (108 bytes on Linux, 104 on macOS,
NUL included). relift picks a short path; the runtime reports a longer one
and fails as above. [verified: length check in the hot-load prototype]

**Latency, miss to resume** (one function, one library, the stand-in
compiling with clang -O2): x86-64 Linux 42 to 63 ms (zig cc as the
compiler: 61 to 63 ms), macOS on an Apple M2 196 to 278 ms, Windows 11 x86-64
in a 4-vCPU VM 340 to 405 ms with zig cc 0.17.1. The load itself is 0.05 to
0.2 ms on Linux; on macOS it is 129 to 216 ms and on Windows about 140 to
165 ms, and dominates. Check and register take about 1 us (about 7 us on
Windows). [verified: hot-load prototype]

**Registration ABI** (`rl_unit`): ABI version, `sizeof(rl_ctx)`, 32-byte
image id, sorted `rl_fn_entry` array (guest address, guest size, flags,
host function), optional line map, code base, name, and the function maps of its shards
(section 11). The same record
describes statically linked code: the emitted program file holds the unit
table and passes it to the loader in `rl_switch_program` (`units` and
`unit_count`), and the runtime fills its table from it at start-up. Bundled code mods
(`relift/mods.h`) add a `mods` table there: each function a bundled mod
patches is registered as a switch that tail-calls through a slot, and at
start the loader points every slot at the variant lifted for the mods whose
bool settings (`mod.<name>`) are on, then writes the mods' data patches into
guest memory. The
runtime library names no symbol that the program provides, so it links as a
shared library on every host.

### Shipped builds

`rl_dispatch_miss` prints the target, the call site, the guest and host
backtraces and the module offset, writes the same to the log file, and
ends the process (`RL_PEND_EXIT_PROCESS`). Shipped builds never load code
at run time. [decided]

## 7. Exceptions

From the exceptions prototype [verified on x86-64 Linux and an Apple M2
Mac unless marked]:

- Every call whose callee may unwind is followed by a check:
  - call site inside an LSDA range with a landing pad:
    `if (RL_UNLIKELY(rc)) { if (c->pend.sp == sp) goto lp_dispatch; return RL_PENDING; }`
  - any other such call: `if (RL_UNLIKELY(rc)) return RL_PENDING;`
  This includes functions without an LSDA; dropping one plain check broke
  the rethrow test (negative control neg2).
- "May unwind" is every call until a call-graph analysis proves otherwise
  (no path to `__cxa_throw`, `__cxa_rethrow`, `_Unwind_Resume`, an indirect
  call or an `RL_FN_MAY_UNWIND` override). [open: the analysis]
- `lp_dispatch` reloads the registers the pads read (x0/x1 carry the
  exception pointer and selector) and `sp` from `c`, then switches on
  `c->pend.pc`, setting `pend.kind = RL_PEND_NONE` in each case. No case
  calls `rl_bad_resume(c, mem, fn)` (dev builds may first ask for the code
  with `RL_MISS_RESUME`).
- **Matching on sp:** the check compares `c->pend.sp` with the frame's sp
  local (the exceptions prototype compared `c->sp`; moving it into the record keeps the
  context's sp free for the installed registers). Exit kinds set
  `pend.sp = RL_SP_NONE`, which no 16-byte aligned sp equals, so they pass
  every frame up to the thread entry. [inferred change from the verified
  shape]
- **`rl_resume(c, mem, r)`** is the unwind primitive. It copies x0-x30, sp
  and d8-d15 (`v[8..15].d[0]`; upper halves untouched, AAPCS64 preserves
  only the low halves) into `c`, sets `pend = {pc, sp, RL_PEND_UNWIND}`, and
  returns `RL_PENDING`. The lifted unwinder's frames then return up to the
  throw site and on to the target frame.
- **Two shapes of register install**, and both are supported; a guest
  unwinder is one or the other (the `relift` end-to-end tests
  `e2e_eh` run a guest unwinder of each shape through `relift build`):
  - **A computed jump (libgcc).** `_Unwind_RaiseException`,
    `_Unwind_Resume` and friends end in the `__builtin_eh_return`
    epilogue: restore x19-x28 and d8-d15, pop the frame, then
    `add sp, sp, x5; br x6` (x5 the stack adjustment, x6 the landing pad,
    x0-x3 the exception data). Nothing is recognized: the `br` is an
    ordinary computed jump, and what makes it an unwind is its target
    (next item). [verified: disassembly of a libnx C++ exceptions test
    program built with devkitA64, which also runs]
  - **A return (LLVM libunwind, what nnSdk ships).** `unw_resume` reaches
    `UnwindCursor::jumpto`, which passes the register block to an
    unnamed routine (`__libunwind_Registers_arm64_jumpto`) that loads x2-x29,
    sp (from offset 0xf8), the pc (offset 0x100, into x30), d0-d31 (low 64
    bits, from 0x110) and finally x0-x1, then `ret`s to the pc. It does
    not touch FPCR or FPSR. A `ret` is a return to the host caller in
    lifted code, not a jump, so this shape needs an override:
    `program::known` recognizes the routine by its code (all 36
    instructions, so a different build of the library is not mistaken for
    it) and replaces it with `rl_a64_unw_jumpto` (`RL_FN_OVERRIDE |
    RL_FN_MAY_UNWIND`), which reads the block at x0, sets d0-d31, and calls
    `rl_resume` with the block's x0-x29, sp and pc (x30 is the pc, as on
    hardware). The jump to the pad is then the same table entry as in the
    libgcc shape. [verified: the SDK module's routine has exactly this
    code, and in a game's build the table holds the override at it; the
    synthetic unwinder in `relift/tests/tiny/eh.s` ends in it]. The
    routine is reached by a tail branch only, so the guest layer also scans
    the text for such routines and roots them (`known_routines`);
    otherwise it would be folded into its caller and never replaced.
  Another unwinder needs the same only if its install step is not a
  computed jump: write its recognizer in `program::known` and a host
  function that converts its register layout to `rl_resume_regs`.
- **A landing pad is a dispatch-table entry that unwinds.** The unit table
  has, at every landing pad of the unwind tables that lies in a lifted
  function and is not itself a function entry, an entry with
  `RL_FN_MAY_UNWIND | RL_FN_FRAGMENT` and the CPU layer's `rl_pad_resume`
  (`Frontend::pad_resume`). A computed jump or call to the pad (an
  unwinder's `br x6`, whatever its shape) therefore reaches
  `rl_pad_resume`, which records `pend = {pc = the pad, sp = c->sp,
  RL_PEND_UNWIND}` and returns `RL_PENDING`; the host stack unwinds to the
  frame whose `lp_dispatch` has a case for that pad and whose sp local
  equals `c->sp`. Normal indirect calls pay nothing, and a direct call to
  a pad does not occur. The registers the unwinder installed are in `c`
  (the jump stores the context first), and `lp_dispatch` reloads them.
- **Pads come from the unwind tables** (`.eh_frame` call-site tables in
  `.gcc_except_table`). A pad whose call-site range starts at an
  instruction of a function joins that function's extent as a root, is a
  resume point of it (`RL_FN_LANDING_PADS`, a case in `lp_dispatch`) and
  makes the check after every call of that function compare sp. A pad
  that is already a function entry (a hot-load hint) stays one. [verified:
  libnx `cxx_exceptions`]
- **d8-d15** are in `c->v` and in `rl_resume_regs`, so a landing pad sees
  the values the unwinder restored. [inferred; the exceptions prototype did not track FP
  registers]
- **Guest setjmp/longjmp:** section 7.1.
- **Host code that must raise a guest exception** (an override or a
  service): allocate with the guest `__cxa_allocate_exception`, construct
  the object in guest memory, put the arguments in `c->x[0..2]`, leave x30
  and sp as they were at the override's entry, and
  `RL_MUSTTAIL return rl_indirect(c, cxa_throw_addr)(c, mem);`. The guest
  unwinder then sees the override's caller as the throwing frame, with no
  host frame between, and the caller's call site has the full check
  because the override is flagged `RL_FN_MAY_UNWIND`. [inferred]
- **Exceptions do not cross host frames.** When the runtime or an override
  calls guest code and continues afterwards, it sets x30 to a runtime-owned
  guest address whose synthetic FDE marks the end of the stack, so the guest
  unwinder reports end of stack (and guest `std::terminate`) instead of
  walking into host code. Any other `RL_PENDING` that reaches such a host
  caller is propagated if the host caller has the lifted signature, else
  reported. [inferred; open: build the synthetic FDE]

## 7.1 setjmp and longjmp

Guest `setjmp` and `longjmp` use the same primitive as exceptions: the
host stack unwinds by `RL_PENDING` returns, never by a host `longjmp`.
[verified: `a64_tests jmp` on x86-64 Linux and an Apple M2 Mac]

- **Overrides.** `rl_a64_setjmp` and `rl_a64_longjmp` (`a64/abi.h`) have
  the lifted signature and replace the guest's `setjmp` and `longjmp`
  (`_setjmp`, `__builtin_setjmp` and `sigsetjmp` with a zero mask take the
  same overrides). The build registers them as `RL_FN_OVERRIDE` entries at
  the guest symbols' addresses; `longjmp` also carries `RL_FN_MAY_UNWIND`.
  - `rl_a64_setjmp` stores x19 to x30, sp and d8 to d15 (`v[8..15].d[0]`)
    into the jmp_buf at guest address x0 and returns `RL_OK` with x0 = 0.
    The buffer is guest memory, so guest code that reads or copies it sees
    what the guest's own `setjmp` would have written.
  - `rl_a64_longjmp` reads that buffer, sets x0 to w1 (1 when w1 is 0,
    zero-extended, as the guest's `cinc w0, w1, eq` does), and calls
    `rl_resume` with pc = the saved x30 and the saved sp. It returns
    `RL_PENDING` with `pend = {saved x30, saved sp, RL_PEND_UNWIND}`.
- **Layout.** `rl_a64_jmp_layout` gives the offsets of x19, sp and d8 in
  the guest libc's jmp_buf. The default `rl_a64_jmp_newlib` is x19 at 0,
  sp at 96, d8 at 112, 176 bytes [verified: disassembly of devkitA64
  newlib's `setjmp` and `longjmp`]. A guest layer whose libc differs calls
  `rl_a64_set_jmp_layout` at start-up; relift reads the offsets from the
  guest's `setjmp` (a run of `stp`/`str` from x0) and refuses to build when
  they do not fit the struct, or when the guest mangles the saved values
  (none of the libcs seen do). [inferred: the read from the guest's setjmp]
- **Emitter: resume points.** The call site of `setjmp` is a resume point
  at the return address. A function that calls `setjmp` gets
  `RL_FN_LANDING_PADS`, and every call in it after which control may come
  back through `longjmp` (in practice every call in the function) uses the
  landing-pad check of section 7:
  `if (RL_UNLIKELY(rc)) { if (c->pend.sp == sp) goto lp_dispatch; return RL_PENDING; }`.
  `lp_dispatch` gets one case per `setjmp` call site, next to the LSDA
  landing pads: reload the registers the code after the call reads (x0 is
  the longjmp value, x19 to x29 and d8 to d15 are the saved values) and sp
  from `c`, set `pend.kind = RL_PEND_NONE`, and jump to the label after the
  call. The `sp` match works because `bl` does not change sp, so the sp
  `setjmp` saved is the caller's sp local at the call.
- **Frames between.** Every frame between the `longjmp` and the `setjmp`
  frame has the plain check and returns `RL_PENDING`; a frame with its own
  resume points does not match, because its sp differs. Dropping a plain
  check runs code that `longjmp` skips (negative control
  `jmp_control_a_dropped_check_runs_code_longjmp_skipped`).
- **A `longjmp` to a frame that already returned** is undefined in C; here
  no frame matches and `RL_PENDING` with `RL_PEND_UNWIND` reaches the thread
  entry, which reports it as an escaped unwind.

## 7.2 Memory routine overrides

Lifted `memcpy`, `memmove` and `memset` of the guest's libc run the guest's
byte, word and pair loads and stores one after another; the host's versions
move 16 to 64 bytes per instruction. In a perf profile of a title's render
thread the SDK's `__memcpy_device` alone was 7.6% of the samples and
`memset` 0.7%. [verified: perf profile]

- **Overrides.** `rl_a64_memmove` (x0 = dst, x1 = src, x2 = n) and
  `rl_a64_memset` (x0 = dst, x1 = value, x2 = n) have the lifted signature,
  return `RL_OK`, leave every register but the ones a call may change as
  they were, and do `memmove` and `memset` on `mem + x`. The build registers
  them as `RL_FN_OVERRIDE` entries (not `RL_FN_MAY_UNWIND`) at the guest
  routine's address.
- **Recognition.** The code is matched by its first word, its length and an
  FNV-1a hash of all its bytes (`program::known`), so only routines whose
  whole code was read and checked to be a plain copy or fill are replaced:
  the SDK's `__memcpy_device` (772 bytes), `__memcpy_fast` (176),
  `__memmove_device` (408) and `memset` (208). Any other routine stays
  lifted. The shapes come from Nintendo SDK 9.5.0 (the only version checked;
  other versions compile different code and stay lifted until their routines
  are read and added). [verified: disassembly of the four routines]
- **Why `memmove` for `memcpy`.** Overlap is undefined for `memcpy`, so any
  behavior is allowed; `memmove` is the one that is always defined. The
  guest routines only touch the destination and source ranges and never
  read memory the copy does not.
- **Faults.** The guard pages and the fault handler (section 2) apply to the
  host routine as to lifted code: a copy that runs into a protected page
  faults at `si_addr - mem`, and a fault hook that maps the page lets the
  host instruction run again. The partial copy before the fault is not
  specified, as on hardware. The faulting host pc is in libc, so the guest
  backtrace starts at the caller of the routine.
- **Exclusive monitor.** The monitor is a value compare (section 8): a
  plain store does not clear it, it only changes the value the next `STXR`
  compares. The host copy writes the same bytes, so a reservation survives
  or fails exactly as after the lifted loop's stores. [verified:
  `memmove_is_seen_by_the_exclusive_monitor_as_a_plain_store`]
- **Vulkan shadow memory.** Shadow allocations are synchronized by
  comparing bytes at call boundaries, not by tracking stores, so a host
  write into shadowed guest memory is found the same as a guest store.
  [inferred: `vk_mem.c`]
- **Hot-load.** The hot-load path builds the same override entries from the
  same recognizer, and an `RL_FN_OVERRIDE` entry replaces an existing one
  (section 6). [inferred]
- **Not covered.** The routine's own loads and stores on device memory with
  access-size rules (a guest that relies on 4-byte accesses to MMIO) would
  need the original routine; none of the four is used that way in a
  Switch title, whose "device" memory is ordinary mapped memory.
  [inferred]

## 8. Exclusives

**Decision:** monitor emulation in `rl_ctx.excl`, with the store done as a
host compare-and-swap against the value the exclusive load returned.
Not pattern matching. [verified: probe; see below]

- LDXR/LDAXR (`rl_ldx`): atomic load; record address, size and value.
- STXR/STLXR (`rl_stx`): fail (status 1) unless the monitor holds the same
  address and size; otherwise strong CAS from the recorded value to the new
  one; status is 0 on success. The monitor opens either way.
- LDXP/STXP 64-bit pairs (`rl_ldxp64`/`rl_stxp64`): two 64-bit loads, 16-byte
  CAS (`cmpxchg16b` on x86-64, `ldaxp/stlxp` or `casp` on arm64). A torn
  pair read only makes the CAS fail. 32-bit pairs use the 8-byte form.
- CLREX (`rl_clrex`) and every `rl_svc` open the monitor, as an exception
  return does on hardware.
- LSE atomics and compare-and-swap (`rl_atomic`, `rl_cas`), like plain
  stores, leave the monitor as it is; whether a non-exclusive store by the
  same PE clears the local monitor is IMPLEMENTATION DEFINED.

Why not pattern matching: pairs that span blocks, calls or functions, STXR
without a preceding LDXR, and CLREX all work with the emulation without any
matcher, and when the pair sits in one loop clang reduces the emulation to
the same CAS loop a matcher would produce (the context fields stay in
registers because `c` is `restrict`). [inferred from codegen; perf not
measured]

**Correctness under contention:** a successful STXR is a CAS that saw the
value the LDXR read, so the read-modify-write is atomic. A failed STXR
leaves memory unchanged, as on hardware; spurious failure is allowed by the
architecture and guest code always retries. Progress is lock-free: a CAS
fails only because another thread's store succeeded. [inferred]

**ABA:** the CAS succeeds if other threads wrote and then restored the same
value between the LDXR and STXR, where hardware would fail. That is
harmless when the stored value depends only on the loaded value and
registers (counters, flags, locks, and every `compare_exchange` the guest
compiler expanded into LDAXR/STLXR, whose source semantics already are CAS).
It is visible only for hand-written sequences that load other memory
between the LDXR and the STXR and rely on the monitor to validate it
(classic lock-free stack pop). A path-sensitive scan of SDK-based guest
code found no pair with a load between LDXR and STXR on any path; every
pair is a pure read-modify-write or a compare-and-bail. [verified: static
scan] The emitter flags every LDXR/STXR pair with
another guest load in between in `relift coverage`. [inferred; open: count
such pairs in real code]

**Ordering:** LDAXR and STLXR use sequentially consistent operations, LDXR
and STXR relaxed ones (section 9 explains SC). Failed CAS is relaxed.

**Test:** the libnx test program `llsc_stress` (4 threads x 100,000:
LDAXR/STLXR 32-bit, LDXR/STXR 64-bit, LDXP/STXP pair with a no-tearing check,
LDAXR spinlock with STLR release protecting a plain counter, CLREX makes
STXR fail) must pass recompiled on x86-64 Linux and arm64 macOS. A probe that
reimplements its checks on top of these helpers (4 threads x 200,000) passed
on x86-64 Linux, arm64 under qemu, and an Apple M2 Mac. [verified for the
probe; the recompiled test program is phase 4]

Switch code targets Armv8.0 (Cortex-A57), so it has no LSE atomics and
uses LDAXR/STLXR loops.

**Reservation granule:** the monitor holds the exact address and size, so
an STXR to another address than the LDXR fails, also inside the same
reservation granule. The architecture leaves that case IMPLEMENTATION
DEFINED (a store to a different address in the granule may succeed); the
Apple M2 lets it succeed. relift's choice fails it, which is one of the
permitted behaviors and only costs a retry in correct guest code (no
compiler emits such a pair). The checker's exclusive prelude keeps its
other address outside the granule, so it does not compare this case.
[verified: `relift-check --oracle m2` on the exclusive forms]

**Alignment:** the lifted ordered, exclusive and atomic accesses (LDAR,
STLR, LDAPR, LDAPUR, STLUR, LDXR, STXR, LSE atomics, LRCPC3 pairs) check
alignment before any effect and leave the block through slow path
`SLOW_ALIGN` when the access faults: natural alignment without
`FEAT_LSE2` and for every exclusive, and with `FEAT_LSE2` a fault only
when an ordered or atomic access crosses a 16-byte boundary (Arm ARM,
`AArch64_UnalignedAccessFaults` at EL0 with SCTLR_EL1.A clear). The
runtime has no slow path for it, so it reports the instruction like an
undefined one, where hardware raises an alignment fault (SIGBUS).
[verified: unaligned states against QEMU and the M2]

## 8.1 LSE atomics

Switch code does not use them, but other A64 guests may, so the helpers are
in `a64/abi.h` (static inline, no runtime call):

- `rl_atomic(mem, a, size, op, v, ord)`: LDADD, LDCLR, LDEOR, LDSET,
  LDSMAX, LDSMIN, LDUMAX, LDUMIN and SWP (`op` is `RL_AT_*`) at size 1, 2, 4
  or 8, as one host atomic read-modify-write. Returns the old value,
  zero-extended. The ST<op> aliases are the same call with the result
  dropped.
- `rl_cas(mem, a, size, expected, v, ord)`: CAS, CASA, CASL, CASAL, and
  CASP with 32-bit registers as size 8. Returns the old value; a failed
  compare stores nothing.
- `rl_cas128` (CASP with 64-bit registers) and `rl_atomic128` (FEAT_LSE128
  LDCLRP, LDSETP, SWPP) as 16-byte compare-and-swap, a CAS loop for the
  read-modify-writes. Both need `-mcx16` on x86-64 (`cmpxchg16b`).

`ord` is `RL_ORD_PLAIN` for the plain forms and `RL_ORD_SC` for every
acquire or release form, the same lowering as LDAR and STLR (section 9).
The address must be aligned to the access size. Because these are host
atomics on guest memory, they stay atomic against the exclusive helpers
above and against other guest threads. [verified: `tests/test_atomic.c`,
each operation and size against a plain C model, signed min/max cases,
CAS success and failure, the 16-byte forms, and multi-threaded counters]

## 9. Memory ordering

- **Plain loads and stores** are plain C accesses (`memcpy`). Naturally
  aligned accesses up to 8 bytes compile to single host instructions, which
  are single-copy atomic on both host ISAs, matching Arm's guarantee for
  aligned accesses. [verified for the instruction shape in slice prototype
  disassembly; atomicity inferred from the host ISAs]
- **The hazard:** a loop that waits on a plain guest load (spin on a flag
  another thread sets) lets clang hoist the load out of the loop, so the
  loop never ends. [verified: the probe's lifted spin loop without a fence
  hung on x86-64 Linux and on an Apple M2 Mac and was killed by its alarm]
- **Policy:** the emitter puts `RL_LOOP_FENCE()` (a compiler-only signal
  fence, no instruction) before every jump that stays in the function and
  may go to the jump's own or a lower address. Every loop has such an edge
  (a cycle of addresses cannot only climb), so every loop reloads guest
  memory on each turn. The fence also costs loops whose exit depends only
  on registers (counted copies) their vectorization. [verified: with the fence, the spin loop
  reloads every iteration and passes on x86-64, qemu arm64 and an Apple M2 Mac]
  Covers the libnx `spin_flag` test program, which passes recompiled; without
  the fence its `pong` thread compiled to a jump to itself after the first
  round. [verified: x86-64 Linux, corpus03]
- **Barriers:** DMB and DSB with a full domain and both access types
  (ISH, SY, OSH, NSH) map to `rl_dmb_full` (`seq_cst` fence: `mfence` on
  x86-64, `dmb ish` on arm64); the LD type to `rl_dmb_ld` (acquire fence);
  the ST type to `rl_dmb_st` (release fence, stronger than ST, which only
  orders stores). DSB is treated like DMB: the guest has no device memory.
  ISB is a compiler barrier. clang treats these fences as barriers for all
  memory accesses, including non-atomic ones. [inferred]
- **LDAR/STLR** map to `seq_cst` atomic load and store (`rl_ldar`,
  `rl_stlr`). Reason: Arm's acquire/release are RCsc (a STLR followed by a
  LDAR is not reordered), and compilers map C/C++ `seq_cst` atomics to
  exactly LDAR/STLR, so guest seq_cst code relies on it. Plain acquire and
  release in C would allow store-to-load reordering on x86-64. Cost: STLR is
  `xchg` on x86-64. [inferred from the standard C/C++ to Armv8 mapping]
- Data races between plain accesses are undefined in C11. relift relies on
  clang compiling them as plain host accesses, which is what it does at
  `-O2` with the fence policy. [inferred; tested only by the probe and,
  later, the libnx test programs]

## 10. System instructions

| guest | entry | kind | notes |
|---|---|---|---|
| `SVC #imm` | `int rl_svc(rl_ctx *c, uint8_t *mem, uint32_t imm)` | runtime (guest layer) | call boundary; arguments and results in `c`; opens the monitor; `RL_PENDING` on exit |
| `MRS TPIDR_EL0` | `rl_mrs_tpidr_el0(c)` | inline | |
| `MSR TPIDR_EL0` | `rl_msr_tpidr_el0(c, v)` | inline | |
| `MRS TPIDRRO_EL0` | `rl_mrs_tpidrro_el0(c)` | inline | MSR is UNDEFINED at EL0 and goes to `rl_undefined` |
| `MRS CNTPCT_EL0`, `CNTVCT_EL0` | `uint64_t rl_mrs_cntpct(rl_ctx *c)` | runtime | host monotonic clock scaled to the guest's tick rate (`rl_guest_desc.tick_hz`; 19.2 MHz for the Switch guest) |
| `MRS CNTFRQ_EL0` | `rl_mrs_cntfrq_el0()` | inline | `RL_CNTFRQ`, 19,200,000 unless relift defines another rate for the guest; it must equal the guest's tick rate [inferred: a libnx sleep test program expects 19.2 MHz; not run on hardware] |
| `MRS FPCR` | `rl_mrs_fpcr(c)` | inline | |
| `MSR FPCR` | `void rl_msr_fpcr(rl_ctx *c, uint64_t v)` | runtime | section 10.1 |
| `MRS FPSR`, `MSR FPSR` | `rl_mrs_fpsr_flags(slot)`, slot write and `rl_msr_fpsr_flags()` | inline | section 10.1 |
| `MRS NZCV`, `MSR NZCV` | none | emitter | packs or unpacks the flag locals |
| `MRS CTR_EL0` | `rl_mrs_ctr_el0()` | inline | Cortex-A57 value [inferred from the A57 manual; open: confirm on hardware] |
| `MRS DCZID_EL0` | `rl_mrs_dczid_el0()` | inline | 64-byte DC ZVA [inferred, same] |
| `DC ZVA` | `rl_dc_zva(mem, a)` | inline | zeroes the 64-byte block |
| `DC CVAC/CVAU/CIVAC/CVAP`, `IC IVAU` | `rl_cache_maint(a)` | inline | compiler barrier only; hosts are cache coherent and relift does not support guest code generation |
| `WFE` | `void rl_wfe(rl_ctx *c)` | runtime | bounded wait or host yield; spurious wake is allowed |
| `YIELD` | `rl_yield()` | inline | `pause` / `yield` plus compiler barrier |
| `SEV`, `SEVL` | `rl_sev()` | inline | no effect: `rl_wfe` never waits unbounded |
| `DMB`, `DSB`, `ISB` | section 9 | inline | |
| `BRK #imm` | `int rl_brk(rl_ctx *c, uint8_t *mem, uint32_t imm)` | runtime | a trap (`Flow::Trap`), not a lift failure; crash report `breakpoint BRK #imm`, `RL_PEND_EXIT_PROCESS`. The guest pc is not passed: the crash handler's function map names the lifted function |
| `UDF`, `HLT`, unliftable | `int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn)` | runtime | crash report naming the kind and immediate (`BRK #1000`), `RL_PEND_EXIT_PROCESS` |

Reasons: entries that only read or write a context
field are inline so they cost one load or store; entries that touch host
state (clock, FPCR, scheduling, syscalls) are runtime calls, and those calls
are also call boundaries. [inferred]

## 10.1 Floating point

- **FPCR is a generation-time constant per function**, as in the Sail prototype:
  the Sail generator folds FPCR into the semantics, and only the FP cut
  layer below takes it as an argument (always a constant in emitted code).
  The default is the value Horizon gives new threads. [verified approach:
  Sail prototype; open: the exact initial FPCR on Horizon]
- `rl_msr_fpcr` stores the value; on arm64 hosts it also sets the host FPCR
  for the thread. If the new value's RMode, FZ, DN or AHP differ from the
  default, dev builds report it, because code generated for the default
  mode keeps running. In nnSdk, FPCR is written only by libc's `fesetenv`
  and `fesetround` (RMode only) and by the OS user exception handler's
  save and restore; library code such as a physics engine's FPU guard
  sets FPCR to 0 (round to nearest, no FZ, no DN) around scoped blocks.
  No per-mode function variants are needed while the default is 0.
  [verified: static scan of SDK-based guest code; open: Horizon's initial
  FPCR]
- **FPSR cumulative flags** (IOC, DZC, OFC, UFC, IXC, IDC, QC) are produced
  only for a program that can observe them. The build's FPSR census
  (`cpus/a64/src/flags.rs`) looks at every reachable `MRS Xt, FPSR`: it
  counts when some path uses `Xt` before overwriting it, a call uses
  `x0` to `x7`, a computed jump uses everything, and a value returned in
  `x0` counts when a caller uses `x0` after the call. libgcc's soft-float
  `__sfp_handle_exceptions`, linked into every libnx program, reads FPSR
  only to raise exceptions and does not count. Without a usable read the
  program is built flag-free and pays nothing.
- With one, flags are produced per function, not per program. FPSR's
  flags are sticky, so a read sees every flag raised on its thread since
  the last FPSR write. The flags-live analysis
  (`cpus/a64/src/flags/region.rs`, driven by `relift/src/fpflags.rs`)
  walks back from each live read to the writes that bound it: an `MSR
  FPSR`, a callee that writes FPSR on every path, or the start of a thread
  (the program entry, where `bind` has cleared the host flags). A call on
  the way puts the callee's tail (after its own last write) in the region;
  reaching a function's entry goes on before each of its call sites, so a
  region follows one thread. Only functions with an FP operation in some
  region, and the functions holding the reads, are compiled with
  `-DRL_FP_FLAGS -ffp-exception-behavior=strict`; every other function,
  including a function in a region that does no FP, keeps the flag-free
  fast path, because what it raises is never read or is nothing.
  [verified: `relift/tests/e2e_fpsr.rs`, the region unit tests]
- The build gets this without lifting twice: each module is emitted in two
  parts (`relift_emit::Options::parts`), and once every module is lifted,
  its `relift_parts.h` moves the chosen functions into part 1 of their
  shards, which is compiled a second time with the flag options and
  `-DRL_PART=1`. Each part has its own function map.
- A region is unbounded when it meets code the walk cannot follow: an
  indirect call or computed jump with unknown targets, the entry of a
  function whose address escapes, or a point entered from elsewhere (the
  return point of a `setjmp` override). Then every function produces
  flags, as a whole-program build. An address escapes through a relative
  relocation, `DT_INIT` or `DT_FINI`, a code address the code computes, a
  module entry other than the first module's, or a GOT or data word naming
  a symbol whose value or address reaches other code. A computed call or
  jump through a GOT or PLT word (a PLT stub, an optional call after a null
  check) goes to the definitions of its symbol in the program's modules.
  [inferred: exports no module names are not called, because a module the
  game loads at run time is outside the program the build sees; the
  runtime never enters the program entry again, since it does not deliver
  exceptions through it; landing pads, once lifted, must be marked as
  entered from elsewhere]
- The census names the functions the walk climbed through to the one where
  it stopped. A read whose value goes to memory counts as observed even
  when the program only restores it: a physics engine's FPU guard saves
  the environment with `fegetenv` into its own object and restores it with
  `fesetenv`, but the binary does not give object extents, so it cannot
  show that no other load reads the saved word. The guard runs inside
  virtual methods whose addresses escape, so the read's region is every
  instruction its thread may run since its start, and no run-time scheme
  is exact without producing flags there: the flags at the read depend on
  every FP result of that prefix. Such a program stays program-wide.
  [verified: a game built with that one read ignored is bounded; inferred:
  resolving the virtual call sites would leave most FP functions in the
  region]
- A read that cannot run has no region. A function is entered when its
  address escapes, when it has no call site, or from a block that runs in
  an entered function. In the program start (the first module's entry,
  when its address does not escape) only the blocks run that a forward
  propagation of the registers `rl_hos_run_main` sets (`x0` = 0,
  `program::START_REGS`) reaches; code nothing in the start reaches, such
  as a landing pad, starts with no register known. rtld's start takes a
  nonzero `x0` as an exception to deliver and jumps to the SDK's user
  exception handler, so reads that only that handler reaches never run.
  [inferred: the runtime enters the start once, for the main thread, and
  does not implement `svcReturnFromException`]
- `program.c` of a program with a live read calls
  `rl_a64_set_fp_flags(1)`, and `MSR FPSR` clears the host flags in every
  function, so a write in flag-free code still starts a region cleanly. In
  flag-producing code the flags live in two places: the FPSR
  slot (`c->fpsr`: QC, MSR values, the FPCR slow paths) and the host's
  sticky flags (MXCSR on x86-64, FPSR on arm64), which host FP operations
  raise and the exact layer (`fp.c`, called with a NULL `fpsr`) and
  IR-built compares and min/max (`rl_fp_raise`) raise too. `MRS FPSR` is
  `rl_mrs_fpsr_flags(slot)`, the slot ORed with the host flags mapped to
  Arm bits (IE, ZE, OE, UE, PE to IOC, DZC, OFC, UFC, IXC); `MSR FPSR` sets
  the slot and clears the host flags. Threads are host threads, so the
  host flags follow the guest thread; `bind` clears them, and `rl_svc`
  saves and restores them around the runtime's own FP work
  (`rl_a64_flags_enter`, `rl_a64_flags_leave`).
- Where x86-64 and Arm differ at FPCR zero, and what covers it:
  - Tininess: Arm detects underflow before rounding, x86 after. They differ
    only for a product or quotient that rounds up to the smallest normal
    magnitude (a sum or difference below it is exact, as every finite value
    is a multiple of the smallest subnormal). `rl_fp_flags_fixup` tests the
    result's bits once for a NaN or, for those two, that magnitude, and
    sends only those results to the exact layer (`test_fp_flags.c`, values
    from an Apple M2).
  - x86 DE (denormal operand) has no Arm meaning at FZ 0 and is not mapped;
    IDC arises only with FZ set, which runs on the slow path.
  - NaN results: the x86 operation raised the same IE as Arm (signaling
    operands, invalid operations); the NaN bits come from the exact layer.
  - FMA, square root, conversions to and from integers and between
    formats, rounding to integral: always the exact layer, exact flags.
    The exact layer raises its flags on the host with `rl_fp_host_raise`,
    which runs a host FP operation that raises the same flag (1 + 2^-100
    for IXC alone, inline; `rl_fp_host_raise_any` otherwise) instead of
    writing MXCSR or FPSR, which costs several nanoseconds per write.
  - Compares (IOC for signaling NaNs, and for any NaN in FCMPE, FCMGE and
    FCMGT), FMAX/FMIN and their NUM, pairwise and across-lanes forms (IOC
    for signaling NaNs), FRECPE and FRSQRTE (IOC for a signaling NaN or a
    negative FRSQRTE operand, DZC for a zero, OFC and IXC for an FRECPE
    result that overflows) and FRINTX's IXC: flags built in IR and raised
    under the instruction's condition (FCCMP raises nothing when the
    condition fails).
  - FRECPS and FRSQRTS: only the computation whose result is chosen gets
    the operands (the others get zeros, which raise nothing), so the host
    raises the flags of one fused operation, as Arm's single rounding does.
  - Fixed-point conversions with fraction bits: the scaling by a power of
    two is exact except where it overflows, and a value that large
    saturates; it is scaled as the smallest saturating value of its sign,
    so only the conversion's own flags remain.
  - A floating-point result written to XZR or WZR still raises its flags:
    the template keeps it alive with a flag raise that depends on it and is
    always zero.
  - FCVTXN (round to odd): the IR converts to nearest even, converts back
    and, when inexact, steps to the truncated value and sets its lowest
    bit. The host conversion's overflow and underflow flags do not match
    rounding to odd, so these blocks carry a second guard (`SLOW_FLAGS`, on
    the frontend's `FP_FLAGS` slot, homed at the constant `RL_FP_FLAGS_ON`)
    and run on the slow path in flag-producing programs only; the slow path
    converts with `RL_RM_ODD`.
  - Binary16 arithmetic, square root, rounding to integral, FRECPS and
    FRSQRTS and every conversion from or to binary16: the section 10.3
    helpers compute in software and raise no host flags, so these blocks
    carry the `SLOW_FLAGS` guard too. The slow path runs them in the exact layer at
    16 bits, which also applies FZ16, DN and AHP (AHP only in conversions,
    as Arm's `FPUnpack` ignores it elsewhere).
  - Non-default FPCR (RMode, FZ, DN, AHP, FZ16) runs on the slow path in
    every program, which produces exact flags.
- `relift/tests/emit_flags.rs` checks all of it on the host: random words
  of every FP form, lifted for a flag-producing program and compiled at the
  flagged flags, run on states full of FP special values, must give the
  result and cumulative flags of `rl_fp_slow` (the exact layer, which
  `relift-check` checks against an Apple M2 and QEMU). The same harness
  runs on an M2 with `RELIFT_EMIT_FLAGS_KEEP`.
- The reference mode `RELIFT_FP_EXACT=1` sets `RL_FPCR_EXACT` (FPCR bit 31,
  RES0, never visible to the guest or the host FPCR) so every guarded block
  runs on the slow path; it exists to check the host-flag path against.
- **FP cut layer** (the Sail prototype's option (b), 11 operations): FPAdd, FPSub,
  FPMul, FPDiv, FPMulAdd, FPSqrt, FPRoundInt, FPRSqrtEstimate (each for
  single and double), FPConvert, FPToFixed, FixedToFP. Every one has an
  exact out-of-line slow path (`rl_fp32_*`, `rl_fp64_*`, `rl_fp_convert`,
  `rl_fp_to_fixed`, `rl_fixed_to_fp`) that implements Arm's rules for every
  FPCR mode, NaN propagation and FPSR flags. FSQRT and FRSQRTE are the two
  forms the Sail route cannot generate, so they must
  be in the layer. The rest of FP (compares, min/max, FRECPE, FCSEL, moves)
  stays generated from Sail. [verified need: Sail prototype; implementations
  open]
- **Inline fast paths** (`rl_fadd32` ... `rl_fsqrt64`, `rl_fmuladd32/64`):
  - x86-64: valid when RMode is round-to-nearest and FZ, DN, AHP are clear
    and FPSR is not tracked. IEEE basic operations and fused multiply-add
    are correctly rounded on both ISAs, so non-NaN results are bit-exact.
    NaN results differ (x86-64 produces a negative default NaN and picks
    NaN operands differently), so a NaN result is recomputed by the slow
    path. [inferred from IEEE 754 and the x86 and Arm manuals; checker to
    prove]
  - arm64 hosts: the runtime keeps the host FPCR equal to the guest FPCR,
    so the native instruction is the guest instruction, for every mode and
    for NaNs; no fixup. [inferred]
  - Non-default modes on x86-64 go to the slow path. If Horizon's default
    FPCR turns out to set FZ or DN, x86-64 needs a fast path for that mode
    before performance work. [open]
- `__builtin_fmaf`/`__builtin_fma` lower to an FMA instruction where the
  target has one and to libm's correctly rounded `fma` otherwise. [inferred]

## 10.2 Crypto instructions

`a64/crypto.h` has one function per guest instruction of FEAT_AES,
FEAT_PMULL, FEAT_SHA1, FEAT_SHA256, FEAT_SHA512, FEAT_SM3 and FEAT_SM4:
`rl_aese`, `rl_aesd`, `rl_aesmc`, `rl_aesimc`, `rl_pmull64` (PMULL and
PMULL2 1Q), `rl_sha1{c,p,m,h,su0,su1}`,
`rl_sha256{h,h2,su0,su1}`, `rl_sha512{h,h2,su0,su1}`,
`rl_sm3{partw1,partw2,ss1,tt1a,tt1b,tt2a,tt2b}`, `rl_sm4e` and `rl_sm4ekey`.
A vector register is an `rl_u128` holding the register's bits (element `e`
of size `s` is bits `[e*s, e*s+s)`); `d` is the old destination value, `n`
and `m` the Vn and Vm operands. The header does not cover the 8-bit PMULL
or the FEAT_SHA3 instructions (EOR3, RAX1, XAR, BCAX), which are plain
integer and bit operations.

Each function calls a host path when the host has the instruction
(`rl_crypto_host_features`, detected on the first call and cached) and
otherwise a portable C path transcribed from the Arm pseudocode;
`rl_<name>_portable` is always exported, for tests.

| guest | arm64 host | x86-64 host | other |
|---|---|---|---|
| AESE, AESD, AESMC, AESIMC | `vaeseq_u8` ... (the same instructions) | AES-NI, mapping below | portable |
| PMULL 1Q | `vmull_p64` | PCLMULQDQ imm 0x00 | portable |
| SHA1C, SHA1P, SHA1M, SHA1SU0, SHA1SU1 | ARMv8 intrinsics | SHA-NI, mapping below | portable |
| SHA1H | ARMv8 intrinsic | portable (a rotate) | portable |
| SHA256H, SHA256H2, SHA256SU0, SHA256SU1 | ARMv8 intrinsics | SHA-NI, mapping below | portable |
| SHA512* | ARMv8 intrinsics (FEAT_SHA512) | portable | portable |
| SM3*, SM4E, SM4EKEY | ARMv8 intrinsics (FEAT_SM3, FEAT_SM4) | portable | portable |

Detection: `getauxval(AT_HWCAP)` on Linux and Android arm64,
`sysctlbyname("hw.optional.arm.FEAT_*")` on macOS and iOS, `cpuid` on
x86-64 (AES, PCLMUL and SHA each also require SSSE3 and SSE4.1). Host
functions carry target attributes, so `crypto.c` builds without `-march`
flags.

**AES on AES-NI.** Both ISAs keep byte `i` of the FIPS-197 state (column
`i / 4`, row `i % 4`) in byte `i` of the register; only the order of the
steps differs:

- Arm (Arm ARM pseudocode of AESE, AESD, AESMC, AESIMC):
  AESE = SubBytes(ShiftRows(state XOR key)),
  AESD = InvSubBytes(InvShiftRows(state XOR key)), AESMC = MixColumns,
  AESIMC = InvMixColumns.
- Intel (S. Gueron, "Intel Advanced Encryption Standard (AES) New
  Instructions Set", Intel white paper, rev. 3.01, 2012):
  AESENC = MixColumns(SubBytes(ShiftRows(s))) XOR rk, AESENCLAST the same
  without MixColumns, AESDEC and AESDECLAST the inverse steps in the same
  shape, AESIMC = InvMixColumns.

The round key is applied before the S-box on Arm and after MixColumns on
x86-64, so:

- AESE(d, k) = AESENCLAST(d XOR k, 0)
- AESD(d, k) = AESDECLAST(d XOR k, 0)
- AESIMC(x) = AESIMC(x)
- AESMC(x) = AESENC(AESDECLAST(x, 0), 0): the InvShiftRows and
  InvSubBytes of AESDECLAST are undone by the ShiftRows and SubBytes of
  AESENC (the two commute: one permutes bytes, the other maps each byte),
  leaving MixColumns.

**SHA on SHA-NI** (Intel SDM, SHA1RNDS4, SHA1MSG1, SHA1MSG2, SHA256RNDS2,
SHA256MSG1, SHA256MSG2):

- SHA-1: Intel keeps A and W0 in the top word where Arm keeps them in word
  0, so every operand is reversed by word (`pshufd 0x1b`). SHA1RNDS4 adds
  its own round constant and takes E already added to W0, while Arm's Vm
  holds W + K, so the constant is subtracted from each word (exact modulo
  2^32) and E is added to word 0. Arm's choose, parity and majority are
  selectors 0, 1 and 2. SHA1SU0(d, n, m) = rev(SHA1MSG1(rev d, rev n)) XOR
  m; SHA1SU1(d, n) = rev(SHA1MSG2(rev d, rev n)).
- SHA-256: Intel packs the state as ABEF and CDGH, Arm as ABCD and EFGH.
  Four Arm rounds are two SHA256RNDS2 with the state repacked before and
  after; SHA256H returns the new ABCD, SHA256H2 the new EFGH. The schedule
  keeps W0 in word 0 on both: SHA256SU0(d, n) = SHA256MSG1(d, n) and
  SHA256SU1(d, n, m) = SHA256MSG2(d + (n1, n2, n3, m0), m).

[verified: `tests/test_crypto.c` checks the portable paths against
FIPS-197 round states, the FIPS 180-4 and GB/T 32905 "abc" digests, the
GB/T 32907 SM4 example and known PMULL products, then every host path
against its portable path on random inputs; on an AMD Ryzen 9 7950X
(x86-64 Linux) the AES, PMULL, SHA-1 and SHA-256 host paths match, on an
Apple M2 (macOS) the AES, PMULL, SHA-1, SHA-256 and SHA-512 ones; SM3 and
SM4 host paths are unexercised, as neither host has them]

## 10.3 Half precision

`a64/fp16.h` is IEEE binary16 on `uint16_t` bit patterns, for the IR's
half-precision operations: `rl_fp16_add`, `_sub`, `_mul`, `_div`, `_sqrt`,
`_muladd` (a * b + c, one rounding), `_round_int` (an `RL_RM_*` mode),
`_to_f32`, `_to_f64` (exact), `_from_f32`, `_from_f64` (one rounding from
the wider bits), `_from_int` and `_to_int` (32 or 64 bits, signed or
unsigned; `_to_int` rounds in an `RL_RM_*` mode, saturates, and gives 0
for a NaN). Arithmetic rounds to nearest even, sets no flags and does not
flush subnormals. A NaN result is the first NaN operand quieted (sign and
payload kept, bit 9 set), else the default NaN 0x7e00; the IR leaves Arm's
NaN selection, FZ16 and AHP to the frontend (as for binary32 and binary64,
section 10.1): a block with a binary16 operation runs on the slow path
under non-default FPCR (`SLOW_FPCR`) and in flag-producing programs
(`SLOW_FLAGS`), where `rl_fp_slow` runs it in the exact layer. Every result is computed exactly in integers and rounded
once, so it does not depend on the host FPU or on `_Float16` support.
The reference is `core/ir/src/half.rs`. [verified: `tests/test_fp16.c`,
exhaustive round trips through binary32 and binary64, exhaustive square
roots and round-to-integral, add, sub, mul and div against binary64 rounded
once, fused multiply-add against an exact reference including sticky-bit
cases, integer conversion edges and saturation; on x86-64 Linux and an
Apple M2]

## 11. Crash mapping

### Names

Lifted functions are named with C identifiers that are themselves Itanium
mangled names inside `guest::<module>`, so lldb, perf, `addr2line -C`,
`nm -C` and llvm-cxxfilt demangle them as guest names. A mangled guest
symbol gets the prefix `5guest<len><module>` on its first name, and every
substitution reference is renumbered so it names the same entity.
Unscoped names are wrapped into nested ones, and `St`, `Sa`, `Sb`, `Ss`,
`Si`, `So` and `Sd` at the head are written out. A clone suffix such as
`.cold` becomes an ABI tag `sfx...` on the function's own name. A plain C
name becomes `_ZN5guest<len><module><len><name>E`, and an unnamed function
becomes `guest::<module>::fn_<hex address>`. `relift-switch-names`
(`guests/switch/names`) gives a name only when it maps back to exactly the
original module and symbol, so the map has no collisions; anything else
uses the address name. gdb does not demangle C function names, so
`runtime/tools/gdb/relift_frames.py` adds a frame filter for `bt`.
[verified: synthetic round-trip corpus in `cargo test`, checked against
`cpp_demangle`; the gdb test `make -C runtime gdb-test`]

### The function map

Every shard emits one `rl_fn_map` (`abi_core.h`), and the unit that links
the shards lists them in `rl_unit.maps` and `rl_unit.map_count`.
`rl_units_register` registers them for crash reports; a program without
units may call `rl_fn_maps_register` itself.

- `rl_fn_map`: `version` (`RL_FN_MAP_VERSION`, 2), `count`, `fns` sorted by
  guest address with ranges that do not overlap, the guest `module` name,
  the `shard` name, the module's guest load address `module_base` (reports
  print `module+offset`), and `text_lo`/`text_hi`, the bounds of the host
  section that holds only lifted functions, or NULL when unknown.
- `rl_fn_info` (48 bytes): `guest_addr`, `fn_addr` (the entry of the guest
  function, equal to `guest_addr` unless the entry is a piece of a split
  function, section 4), `guest_size`, `flags` (as in the
  dispatch entry), `fn` (the host function), `host_sym` (the identifier the
  emitter gave it, as above) and `guest_name` (the readable guest name,
  the demangled guest symbol, or NULL to print `host_sym`). The runtime has
  no demangler, so the emitter writes the readable name.
- **Lifted-code section.** The emitter defines every lifted function with
  `RL_TEXT` (`RL_TEXT RL_LIFTED(F) { ... }`). It puts the functions in the
  section `relift_text` on ELF, `__TEXT,__relift_text` on Mach-O and
  `relift$m` on COFF. Each map sets `text_lo = RL_TEXT_LO` and
  `text_hi = RL_TEXT_HI` after `RL_TEXT_BOUNDS_DECLARE();`; on Windows one
  translation unit per image (`rl_units.c` or the hot-load unit's own file)
  also expands `RL_TEXT_BOUNDS_DEFINE()`, two empty functions in `relift$a`
  and `relift$z` that the linker sorts around `relift$m`. ELF and Mach-O
  linkers define the bounds themselves (`__start_`/`__stop_`,
  `section$start$`/`section$end$`). Bounds are per image, so a hot-loaded
  library's maps carry its own section. Only lifted functions may go in the
  section: a host pc inside it belongs to the nearest function start below
  it, so the match is exact. Without bounds the runtime takes the nearest
  start below the pc if the pc is below the next start and less than 64 KiB
  past it, and marks the frame `(nearest)`. [verified: `crash_test` on
  x86-64 Linux, an Apple M2 Mac and Windows 11 x86-64]
- **Inlining.** A function clang inlines into another in the same shard has
  no host frame, so the guest backtrace shows its caller only. The line map
  below will recover such frames. [inferred]
- **Build flags.** Lifted code builds with `-fno-omit-frame-pointer
  -mno-omit-leaf-frame-pointer` on Linux and macOS, so the frame-pointer
  walk sees leaf frames; Windows x64 uses the unwind tables, which every
  function has. [verified: `crash_test`]

`relift build` packs a module's functions into shards by guest address
window, a sixty-fourth of `emit.shard-kib` of guest code each (64 KiB by
default): `shard<n>` holds the functions whose entries lie in window `n`
from the module base, and `shard<n>_<k>` the rest of a window whose C
exceeds the shard budget. The
build keeps each module's outlines (`analysis/` in the build directory) and
the C of each function (`emit/`, zstd frames per window) for later builds:
an unchanged module is not discovered again, a module changed by code mods
is rediscovered only where its bytes differ, and a function whose code and
direct-call targets are unchanged takes its C from the store. Shards are
written only when their text changes, so a build that changes one function
recompiles its shard and the unit tables.

### The crash handler

`runtime/core/crash.c` with `crash_posix.c` (Linux, macOS) and
`crash_win.c` (Windows), declared in `runtime/core/crash.h`. It depends
only on the C library and the host OS.

- **POSIX:** SIGSEGV, SIGBUS (macOS arm64 raises it for `PROT_NONE`) and
  SIGILL. The core's fault handler owns SIGSEGV and SIGBUS and calls
  `rl_crash_signal` for a fault it cannot resolve (no catcher, no guest
  hook). The host backtrace is a frame-pointer walk from the signal
  context's pc and frame pointer, bounded to 64 MiB above the faulting sp;
  host symbols come from `dladdr`. After the report the default action is
  restored and the signal ends the process (core dumps keep working).
- **Windows:** a vectored exception handler for access violations,
  in-page errors and illegal or privileged instructions (C++ and other
  exceptions pass through). The host backtrace comes from
  `RtlLookupFunctionEntry` and `RtlVirtualUnwind` on the exception's
  context; frames print as `module+offset`. After the report the process
  ends with the exception code. A stack overflow
  (`EXCEPTION_STACK_OVERFLOW`, 0xC00000FD) is reported too: the handler
  first switches to a fiber of its own (section 14), because the
  overflowed stack has only the thread's stack guarantee left, and walks the
  overflowed stack from there.
- **Report** (stderr; formatted in stack buffers, so it runs in the
  handler): the kind, the host fault address and, when it lies in the guest
  reservation or its guards (`rl_crash_set_space`, set by
  `rl_fault_install`), the guest address; the guest function that holds the
  faulting pc; the guest backtrace (the host frames that land in lifted
  code, innermost first, with guest address, module+offset and name); the
  host backtrace with the host symbol and guest name of lifted frames; and
  the CPU layer's register summary of the current guest thread at its last
  call boundary. Tail-called frames are missing, as in native code.
  When the faulting sp (or the fault address, for a fault in a guard page)
  lies in a fiber's stack, a line names the fiber, which for the device
  scheduler is the guest CPU (`on guest CPU 1 (fiber stack ...)`, or
  `stack overflow on guest CPU 1 ...` for its guard page). The host walk
  ends at the fiber's first frame.

```
[relift crash] SIGSEGV at host 0x7f01eda01230 = guest address 0x40001230, host pc 0x55efb0919d74
[relift crash] in guest function guest 0x7100001200 test+0x1200 guest::test::wild(), host _ZN5guest4test4wildEv+0x4
[relift crash] guest backtrace (lifted frames, innermost first; tail calls leave no frame):
[relift crash]   #0 guest 0x7100001200 test+0x1200 guest::test::wild()
[relift crash]   #1 guest 0x7100001100 test+0x1100 guest::test::inner()
[relift crash]   #2 guest 0x7100001000 test+0x1000 guest::test::outer()
[relift crash] host backtrace:
[relift crash]   #0 0x55efb0919d74 _ZN5guest4test4wildEv+0x4 (guest::test::wild())
...
```

### Still to come

- The host-offset to guest-address line map (`rl_line_entry`), produced by
  relift after linking from the binary's line table and the per-module
  listings (`#line` directives, one guest instruction per line; CodeView
  caps lines at 2^24, so listing lines, not raw guest addresses), embedded
  in the unit or written as a side file. With it a frame names the guest
  instruction, not only the function, and inlined frames come back.
- The guest x29 chain in guest memory is used only from a context the
  runtime captured (svc, override entry), because at a fault the live guest
  registers are host locals. [inferred]

## 12. Versioning

- `RL_ABI_VERSION` changes with any change to a type, layout, signature,
  constant or emitted call shape in this contract. Splitting the header into
  `abi_core.h` and `a64/abi.h` moved declarations and changed no layout,
  signature, constant or call shape (the one renamed field,
  `rl_dev_lift_req.fpcr` to `fp_mode`, keeps its offset and size), so the
  version stayed 1.
- Version 2 added the function maps (`rl_fn_info`, `rl_fn_map`,
  `rl_unit.map_count` and `rl_unit.maps` at the end of `rl_unit`) and the
  `RL_TEXT` placement of lifted functions (section 11). A unit written with
  designated initializers and no maps still compiles unchanged. The map
  has its own `RL_FN_MAP_VERSION`; the entry for each piece of a split
  function added `fn_addr` and made it 2.
- Hot-loaded units carry `abi_version`, `ctx_size` and `image_id`; the
  runtime refuses a unit if any differs (section 6). The dev hello carries
  the runtime's version and image id, so relift refuses to serve a
  mismatched runtime before any code is built.
- Static units carry the same fields and are checked at start-up, which
  catches a stale shard linked against a newer runtime.

## 13. Hardware-register guests

Guests such as the Game Boy and the Game Boy Advance (later NDS and N64)
drive devices through memory-mapped registers and depend on CPU timing
relative to them. Their lifted code carries a guest cycle clock, routes device accesses to the
runtime and gives the runtime points where it may run devices and deliver
interrupts. A64 code uses none of this: the Switch CPU layer has no clock
(`rl_cpu_desc.time_offset` is 0) and the A64 frontend emits none of the
operations below. The IR side is defined by the reference interpreter
(`core/ir/src/interp.rs`, `run_with` and the `Devices` trait) and checked by
`core/ir/tests/hw.rs`; the runtime side lives in `runtime/core/sched.c` and
`relift/sched.h`. [verified: IR semantics by `core/ir/tests/hw.rs`; the C
emitter by `relift/tests/emit_hw.rs`, which compiles random blocks of every
operation below next to a mock scheduler and compares registers, memory, the
clock, device events and the exit with the interpreter, with a mutation
control; the WebAssembly emitter follows the same rules, checked by
`core/emit-wasm/tests/diff/` (section 15)]

### IR operations

| IR | Meaning | Emitted C |
|---|---|---|
| `tick N` (`Op::Tick`) | the clock moves N guest cycles at this point | `budget -= N;` |
| `tick.if c, N` (`Op::TickIf`) | N more cycles when `c` is 1 (taken branch) | `budget -= c ? N : 0;` |
| `io.load.E.any a` (`Op::IoLoad`, `IoKind::Any`) | memory or device, by the guest's `IoMap` | inline range check, then a plain access or `rl_io_read` |
| `io.load.E.dev a` (`IoKind::Device`) | always a device | `rl_io_read` |
| `io.store.E.K a, v` (`Op::IoStore`) | the write twin, with the map's write classes | a plain store or `rl_io_write` |
| `sync` (`Op::Sync`, last instruction) | the exit is a sync point | budget check, then `rl_sync` |
| `bank HEX` in the block header (`Block::bank`) | which mapping of the bytes at `pc` was lifted | part of the function's identity |

- **Cycles.** The unit is the guest CPU's own cycle (Game Boy: T-cycles,
  4 per M-cycle); the scheduler turns cycles into timeline ticks with the
  clock's ratio (`rl_sched_add_cpu`). A frontend gives each instruction its
  cost and may split it around the instruction's accesses, so that each I/O
  access happens at its own cycle (`tick 4; io.store ...; tick 4`).
- **Ordering.** Ticks, I/O accesses and memory operations happen in program
  order. A guard may not follow a tick or an I/O access (`Block::verify`),
  so a block that leaves for a slow path has not charged any cycles.
- **Dead values.** `dce` keeps every tick, I/O access and sync: a device
  read can change device state even when its value is unused.

### The budget

- The clock is an `rl_time { int64_t budget; uint64_t deadline; }`
  (`abi_core.h`) that the CPU layer puts in its `rl_ctx` and names in
  `rl_cpu_desc.time_offset`. The CPU is at cycle `deadline - budget`.
- Inside a lifted function the emitter keeps `budget` in a local, loaded
  from `c` at entry. Each tick subtracts its cycles. The emitter may sum the
  ticks of a straight-line run and subtract once, as long as the local is
  exact at every I/O access, sync point and runtime call.
- Before any runtime call (`rl_io_read`, `rl_io_write`, `rl_sync`, an
  intrinsic, a guest call or return) the emitter stores the local to
  `c->time.budget`, and reloads it after the call: a device access may
  stall the CPU (`rl_sched_stall`), and `rl_sync`, events and interrupt
  entries change the budget and deadline.

### I/O accesses

- `rl_io_read(c, mem, addr, size)` and `rl_io_write(c, mem, addr, size,
  value)` take `size` 1, 2, 4 or 8 bytes. `value` is the bytes in memory
  order read as a little-endian number (byte 0 at `addr` is the low byte),
  so the emitter byte-swaps around the call for a big-endian access and
  truncates or extends to the IR width. An IR access of another whole-byte
  size (3, 5 to 7, 9 to 16 bytes) is split by the emitter into supported
  sizes in address order.
- The hook first brings the devices that observe the address up to the
  CPU's cycle, then does the access. It may move the CPU's clock
  (`rl_sched_stall`, wait states) or end its budget (`rl_sched_break`). It
  never changes a guest register and never redirects control: a change it
  makes to the interrupt state takes effect at the next sync point.
- `IoKind::Any` accesses get an inline range check compiled from the
  guest's `IoMap` (`core/ir/src/io.rs`): bytes that are memory for that
  direction take a plain access through `mem`, anything else the hook. An
  access that straddles a boundary counts as a device access as a whole,
  so it calls the hook once.
- **Straddling accesses in the scheduler.** `rl_sched_read` and
  `rl_sched_write` send an access to one owner when all its bytes are in
  one mapped range, or all unmapped. An access whose bytes touch more than
  one range, or a range and unmapped bytes, is done as `size` one-byte
  accesses in address order, each through the same path (events due,
  catch-up, handler), each at the clock's tick when it runs. A read
  assembles the bytes little-endian; an unmapped byte reads 0xff and an
  unmapped byte write is dropped. A device therefore never sees an access
  wider than the part of the range it owns, whatever the emitter passed.
  `IoKind::Device` accesses call the hook directly, and plain `Load` and
  `Store` never reach it.
- `classify_io` (`core/ir/src/opt.rs`) runs before emission: an `any`
  access whose address is constant (`opt::constants`: built from constants
  by pure operations, such as `LDH (n),A` giving `0xff00 + n`) becomes a plain
  access or a `dev` access, so only accesses through a register keep the
  range check.
- The guest layer builds its `IoMap` and its `rl_sched_map` ranges from one
  table. Every byte a device answers for a direction must be a device byte
  of the `IoMap` for that direction, since a memory byte bypasses the
  scheduler. Banked memory that reads like memory (a cartridge ROM bank) is
  memory to read: the guest layer remaps the bank in the reservation when
  its controller is written, and the controller write is a device write.

### Sync points and interrupts

- At a block with `sync`, after the exit's register stores, the emitter
  writes:

  ```c
  if (RL_UNLIKELY(budget <= 0)) {
      c->time.budget = budget;
      if (rl_sync(c, mem, next) != RL_OK) return RL_PENDING;
      budget = c->time.budget;
  }
  ```

  where `next` is the exit's target (computed before the check for a
  select or dynamic exit), then continues to `next` as the exit says.
- `rl_sync` runs the due events and devices up to the CPU's cycle and sets
  a new budget that ends at the next event. It then asks the clock's
  interrupt hook (`rl_irq_hook`, set per clock with `rl_sched_set_irq`)
  whether an interrupt is taken. The hook does the guest-visible entry
  (push `next` as the return address, clear the master enable, acknowledge
  the request, charge the entry cycles to the clock) and returns the
  handler's entry pc, or `RL_NO_IRQ`. A taken interrupt is entered as a
  nested call through dispatch, and when the handler returns `rl_sync`
  asks again, so several pending interrupts are entered one after another.
  `rl_sync` returns `RL_OK`, or `RL_PENDING` to propagate a pending record.
- **Where a handler returns to.** The CPU layer may set a return hook
  (`rl_irq_return_hook`, `rl_sched_set_irq_return`). After each handler
  returns, `rl_sync` calls it with `next`; the hook returns the guest pc
  the handler's return instruction actually went to (the layer reads it
  from the stack or register the return used). If that is `next`, `rl_sync`
  asks for another interrupt and finally returns `RL_OK`. If not, `rl_sync`
  sets `pend = {RL_PEND_REDIRECT, pc = target, sp = RL_SP_NONE}` and
  returns `RL_PENDING`; every frame passes it up, so the host stack above
  the thread entry is gone. A guest whose handlers can leave this way
  starts its threads with `rl_run_from(c, mem, pc)`, which runs the
  function at pc and, on a redirect record, clears it and runs the function
  at the target in a fresh frame. A handler that never returns to its caller
  and never redirects (it jumps away without the layer noticing) leaves
  entries open: `rl_sync` stops the process with a fatal report once
  `rl_sched_config.irq_depth` (default 32) entries of one CPU are open at
  once, so the host stack stays bounded.
- **Interrupt enable in a register.** A CPU layer whose enable lives in a
  register tells the scheduler with `rl_set_irq_mask(c, mask)` (the lines
  the new state allows; `rl_sched_set_mask` on the CPU's clock). The IR
  frontend spells it as an intrinsic `irq_mask(mask)` that an emitter lowers
  to this call; a plain `Set` of the register alone does not reach the
  scheduler. The call takes effect at the CPU's current cycle: if a line in
  the new mask is already raised, the budget ends and the next `rl_sync`
  enters it. A delay between the enable instruction and its effect (the
  Game Boy's EI takes hold after the next instruction) is the frontend's
  job: it places the `irq_mask` after that instruction, or sets the mask
  and keeps the entry hook returning `RL_NO_IRQ` for that one instruction.
  The runtime applies no delay. [open: emitters lower `irq_mask`]
- In IR terms (the interpreter's `Devices::sync`): the outcome of a block
  whose sync took an interrupt has `next` equal to the handler entry and
  `resume` equal to the exit's target. The nested call is the runtime's
  way of running that handler and coming back to `resume`.
- **Why the budget check is exact.** A sync at which no event is due and no
  interrupt can be taken has no guest-visible effect, so skipping it while
  `budget > 0` changes nothing, provided the runtime ends the budget
  whenever an interrupt becomes deliverable or an event becomes due
  earlier. The scheduler does this when a line in a CPU's mask is raised
  (`rl_sched_raise`), when the mask changes to include a raised line
  (`rl_sched_set_mask`) and when an event is posted earlier than the
  deadline (`rl_sched_post`). Interrupt enable state that a guest CPU keeps
  in a register (the Game Boy's IME, set by EI and RETI) must therefore
  reach the runtime through an intrinsic that updates the mask, never
  through a plain `Set` alone.
- **Granularity.** Where `sync` goes is a frontend and per-game choice, not
  an IR rule: at the end of each basic block of guest code (the default;
  every loop back edge, call and return is then a sync point), after each
  instruction with an I/O store as well (interrupts follow a register write
  at the next instruction), or after every instruction (exact interrupt
  timing, slowest). A guest's frontend may also place a sync at HALT-like
  instructions, which then wait through `rl_sched_halt`.

### Banked code

- `Block::bank` is `None` for code at a fixed address and the guest's bank
  number (or another guest-chosen id, such as a hash of code copied to RAM)
  otherwise. `(pc, bank)` is the identity of lifted code
  (`Block::key`): two blocks with one `pc` and different banks are
  different functions, and emitted names carry the bank. Exit targets stay
  plain guest addresses; the dispatch lookup of a banked region uses the
  bank mapped there at run time, and an analysis that tracks bank-switch
  writes may resolve a direct call to one bank at build time.
- The Game Boy guest (`guests/gb/guest`, `runtime/guests/gb/exact`) gives
  every switchable bank a unit of its own and keeps a table per bank of
  the window's entries; the dispatch table holds a forwarder at every window
  address that picks the function of the bank mapped now. A direct transfer
  that may reach lifted window code of its own bank carries `RL_GB_SYNC_CHECK`
  in its sync info, and its sync point unwinds to the run loop when another
  bank is mapped.

### The exact Game Boy mode

The device library (`guests/gb/devices`, rustyboi-core behind
`gbdev_abi.h`) owns the bus and the instruction boundary; lifted code runs
only the instruction bodies, which keeps every access, interrupt and DMA
stall where rustyboi places it.

- The frontend sends every access to the device hook (`IoMap` default
  `Device`) and names its own calls through `Frontend::io_calls`
  (`rl_gb_io_read`, `rl_gb_io_write`) and `Frontend::sync_call`
  (`rl_gb_sync(c, mem, next, info)`). Every instruction ends in a sync point;
  the budget is 0 when an instruction opens, so the sync always runs. The
  cycle of an access is minus the budget at the call: the runtime first makes
  the operand fetches and internal M-cycles before it, then the access.
- `rl_gb_sync` makes the operand fetches still due, ends the instruction
  (`rlgbd_end` with its T-cycles) and opens the next (`rlgbd_at` with pc and
  sp), entering interrupts there: an interrupt entry calls the handler's
  function on the host stack. `info` holds the block's kind (jump, call with
  its return address, return), the block's bank plus one and
  `RL_GB_SYNC_CHECK`.
- A shadow stack holds the return address of every open host frame. A
  return to another address, a return into a window bank that is no longer
  mapped, or a checked direct transfer under another bank leaves through
  `RL_PEND_REDIRECT` to the run loop, which opens the instruction at the
  guest pc and dispatches.
- `EI`, `DI`, `HALT` and `STOP` lift to a trap that runs them on the
  library's CPU (`rl_gb_trap`); so does an instruction whose fetched opcode
  is not the ROM byte the code was lifted from (OAM DMA), whose fetch did not
  advance pc (the HALT bug), or that may trigger the DMG OAM bug (a 16-bit
  register in `$FE00-$FEFF`). An instruction with operand bytes in ROM runs
  there too while an OAM DMA may hold the bus (the 656 dots after a write to
  `$FF46`), because its operand fetches may read the DMA's bytes rather than
  the ROM's.
- Every lifted instruction is a dispatch entry (`Options::enter_anywhere`):
  the unit table lists each one with the piece that switches to it on
  `c->pc` (`RL_FN_FRAGMENT`), so a redirect or the interpreter goes back to
  lifted code at the next instruction it reaches. Window instructions that a
  function of another bank's unit held are lifted once more in their own
  bank's unit.
- A run of more than 8 `nop` is a sled (`rl_gb_sled`): dispatch there runs
  each `nop` at its own boundary in one loop, with no C per byte.
- Code in RAM is lifted per (address, opcode): `rl_gb_ram` entries, with the
  immediate operands read as the instruction runs (`rl_gb_operand`, so code
  the program rewrites keeps working) and jump and call targets computed.
  Dispatch at a RAM address picks the function for the opcode the library
  fetched (for `CB`, with the byte after it), and the interpreter runs any
  other.
- The learned file (`learned.txt` in the import directory, `--learn FILE`,
  `--no-learn`): at exit the program adds the ROM locations where the
  interpreter was entered or jumped to (`BBB:AAAA`) and the RAM instructions
  it ran (`ram AAAA KKK`). The next `relift build` takes the ROM locations
  as analysis roots (`Origin::Seen`: code, because it ran) and lifts the RAM
  instructions, so a second build runs them lifted.
- Code with no lifted function runs on the library's CPU through the SM83
  dispatch miss (`rl_sm83_set_miss`) until the frame returns or it reaches
  lifted code. `--stats` counts its entries and instructions apart for ROM
  and RAM; `--interp-log FILE` writes every instruction it ran and every
  entry into it with the cause (a transfer from lifted code, an interrupt,
  the start, a return of the outermost frame, an unwind).
- [open: block-level syncing through the scheduler, which needs the library
  to report the next cycle anything can change]
## 14. Fibers

The device scheduler (`relift/sched.h`) runs every guest CPU of a machine on
one host thread, each on its own fiber: a host stack of its own, switched
by a hand-written register switch in `runtime/core` (`fiber.h`, `fiber.c`,
and the host-arch files `fiber_x86_64_sysv.S`, `fiber_x86_64_win.S`,
`fiber_aarch64.S`). A hand-over between guest CPUs is a switch of host
stacks, not a wake-up of another host thread.

- **Why:** the earlier hand-over between one host thread per CPU through a
  mutex and condition variable cost 3.3 to 3.7 us on Linux and 4 to 8 us on
  an Apple M2, so a 64-tick quantum cost about 100 ns per lifted block. A
  fiber switch costs 4 to 16 ns per round trip; ucontext (1 us, a system
  call for the signal mask) and `SwitchToFiber` (125 ns) are slower.
  [verified: sched-bench, below]
- **What a switch saves:** the host ABI's callee-saved registers and
  nothing else. x86-64 System V: rbx, rbp, r12-r15. x86-64 Windows: rbx,
  rbp, rdi, rsi, r12-r15, xmm6-xmm15, and the TEB's StackBase, StackLimit
  and DeallocationStack (without them `longjmp`, which checks its target
  against the stack limits, fails with STATUS_BAD_STACK). AArch64 (Linux,
  macOS, Windows): x19-x30 and d8-d15, plus the same TEB fields on Windows
  (x18 holds the TEB and is not touched). The switch returns by popping the
  return address and jumping to it (`br x30` on AArch64) rather than `ret`,
  which the return predictor mispredicts on every switch (2x slower on
  x86-64, 1.5x on the M2). [verified: spike measurements]
- **No FP control is saved.** Every fiber of a thread runs in the host's
  default FP mode (MXCSR 0x1F80 with any flags, FPCR 0): lifted code
  assumes it, and FP slow paths that need another mode set and restore it
  around their own work. Builds with `RL_CHECKS` (the runtime Makefile and
  `relift build` define it) check the mode at every hand-over and abort with
  a message when it differs (`rl_fiber_fp_mode_diff`). The check does not
  depend on `NDEBUG`, which zig cc defines at `-O2`. [verified: fiber_tests]
- **Stacks:** each spawned CPU gets a reservation of `rl_sched_config.stack`
  bytes (16 MiB by default) with a 64 KiB guard below. POSIX: the guard is
  `PROT_NONE` and the stack is mapped read-write. Windows: up to 64 KiB are
  committed at the top, always with a `PAGE_GUARD` page below, and the TEB
  fields set, so Windows grows the stack page by page like a thread's, moves
  StackLimit down, and raises STATUS_STACK_OVERFLOW at the end; the guard
  below the stack is growth room there. A stack without a `PAGE_GUARD` page
  overflows into reserved memory and the process ends without a report.
  [verified: fiber_tests, crash_test on Windows 11]
- **Signal stack:** a fault in a guard page can only be reported on another
  stack. `rl_sched_spawn` and `rl_sched_main` install an alternate signal
  stack on the host thread when it has none (`rl_fiber_thread_init`); on
  Windows it sets a stack guarantee and the crash handler reports from a
  fiber of its own. [verified: crash_test, overflow case, on x86-64 Linux,
  an Apple M2 and Windows 11 x86-64]
- **First frame:** a fiber starts in `rl_fiber_start`, which pushes a zero
  frame record and marks the return address undefined in its unwind info
  (CFI on ELF and Mach-O, SEH on Windows), so frame-pointer walks, the
  Windows unwinder, gdb and lldb stop there cleanly.
- **Unwinding:** lifted code returns `RL_PENDING` frame by frame on its own
  stack, so the switch needs nothing for it; no host unwinder ever crosses
  fibers (section 7). Host `setjmp`/`longjmp` within one fiber works.
  [verified: fiber_tests]
- **Debugging:** `bt` shows the running fiber. `relift-fibers` in
  `runtime/tools/gdb/relift_frames.py` lists the live fibers (label,
  running, parked or not started, stack range) from `rl_fiber_live`, and
  `relift-fiber-bt N` loads the registers saved on a parked fiber's stack
  into the innermost frame, prints its backtrace and puts them back.
  [verified: `make gdb-test`, gdb 18.1, x86-64 Linux]
- **Scheduler API:** `rl_sched_spawn(cpu, body, arg)` gives a CPU its
  fiber; `rl_sched_main(s)` runs the CPUs on the calling thread until all
  have left; `rl_sched_enter` is gone (a fiber starts only when its CPU
  holds the run). A CPU that was not spawned runs on the calling thread's
  own stack, which keeps the one-CPU case free of fibers. These are runtime
  calls, not part of the lifted-code contract, so they leave
  `RL_ABI_VERSION` alone.
- **Not supported:** Intel CET shadow stacks and Windows CET compatibility
  would reject any user-mode stack switch; relift binaries are not marked
  for CET, so it does not trigger.

Measured with `make sched-bench` (2 CPUs, 2 million blocks each, budget step
4 cycles, ns per hand-over and per block):

| host | quantum | threads + condvar | fibers |
|---|---|---|---|
| Linux x86-64 (Ryzen 9 7950X, clang 23) | 64 | 3559 ns, 111 ns per block | 54 ns, 1.70 ns per block |
| Linux x86-64 | 4096 | 5217 ns, 2.55 ns per block | 1000 ns, 0.49 ns per block |
| macOS arm64 (Apple M2, Apple clang 21) | 64 | 4111 ns, 128 ns per block | 61 ns, 1.9 ns per block |
| macOS arm64 | 4096 | 8009 ns, 3.9 ns per block | 800 to 1630 ns, 0.39 to 0.80 ns per block |
| Windows 11 24H2 x86-64 VM (zig cc 0.13) | 64 | not built (needs pthreads) | 60 to 65 ns, 1.9 to 2.0 ns per block |

With 4096 ticks a hand-over is rare (about 2000 per run), so its time per
hand-over includes cache refill and varies from run to run. Windows arm64
builds (zig cc, llvm-mingw clang) link but were not run.

## 14.1 Preemption points

A guest OS whose kernel preempts a running thread when a better one becomes
ready (Horizon does, at any instruction) needs lifted code to stop where it
is, not only at its next system call. Preemption reuses the `rl_time` clock
of the device scheduler's sync points (section 13), with a budget that
another host thread ends instead of one the code counts down.

- **Which exits.** A frontend whose `preempt_clock()` names an `rl_time`
  (A64: `c->time`) makes these block exits preemption points: a jump back
  inside the function (to the block's own or a lower address) that closes
  a loop holding a call, a trap, a computed jump or another loop, or a loop
  that waits on memory; every computed jump; and a jump or call to the
  block's own or a lower address outside the function (recursion, a tail
  call back). Returns, traps and indirect calls are not. Other frontends
  emit nothing.
- **A loop that waits on memory** is one whose jump back or a branch out
  of it decides on a value that comes, through the loop's instructions
  before it in address order, from a load, an exclusive access or an
  intrinsic (a clock read): a spin-wait. It must have a check: a thread
  spinning for a higher-priority thread of its core that wakes from a
  sleep would otherwise never give the core up. [verified: `e2e_spin`
  times out without the check]
- **Why not every loop.** A check in a loop without calls that does not
  wait on memory (a copy, a hash, a counted search) keeps clang from
  unrolling and vectorizing it; checking every jump back cost
  malloc_stress 48% more instructions and 17% more cycles. Such a loop
  delays a preemption by one run of it, until the loop or function around
  it reaches a check. A long loop of that kind at the top of a thread's
  work is not preempted until it ends. [verified: cost table below]
- **The check**, before the exit and before the loop fence:

  ```c
  if (RL_UNLIKELY(rl_time_spent(&c->time)))
      rl_preempt(c);
  ```

  `rl_time_spent` is a relaxed atomic load of the budget compared with
  zero: one load and a branch that is never taken while no preemption is
  pending. The registers stay in their locals: `rl_preempt` only waits for
  the core, so it needs none of them, and storing them would keep values
  alive that the code otherwise drops (the flags above all). A first
  version that stored and reloaded the context around `rl_sync` cost
  malloc_stress 2.4 times the instructions; this form costs 1.5 times
  (table below). [verified: perf stat, x86-64 Linux]
- **The budget.** The A64 `ctx_init` sets it to `INT64_MAX`.
  `rl_preempt_request(c)` stores 0 (any host thread);
  `rl_preempt_clear(c)` stores `INT64_MAX` again.
- **The slow path.** `rl_preempt` calls the hook set with
  `rl_set_preempt_hook`, which must not touch the context's registers (they
  are stale); without a hook it only clears the budget. The Switch layer's
  hook is its svc exit check, `hos_sched_point`: the thread gives its core
  up when the scheduler chose another thread there and waits until it is
  chosen again.
- **The Switch scheduler** (`guests/switch/ksched.c`) requests a
  preemption of a core's holder when its selection for the core changes
  and clears it when the holder keeps or gets the core, so the holder
  leaves at its next svc or preemption point. `RELIFT_SCHED=free` never
  requests one.

Cost on libnx corpus programs (x86-64 Linux, Ryzen 9 7950X, clang 23,
`perf stat -r 10` pinned to 4 cores, mean of 3 such runs, against the
same build without checks; the host had a load average of about 50 from
other work, so cycles are noisy while instruction counts are exact, and
math_fp and dispatch run 2 to 3 ms, mostly start-up):

| program | instructions, no checks | with the checks | cycles, no checks | with the checks |
|---|---|---|---|---|
| math_int | 128.5 M | 131.7 M (+2.5%) | 53.3 M | 47.7 M (-10%) |
| math_fp | 2.74 M | 2.83 M (+3.1%) | 2.68 M | 2.62 M (-2.5%) |
| malloc_stress | 2025 M | 2062 M (+1.8%) | 755 M | 757 M (+0.3%) |
| dispatch | 5.95 M | 6.60 M (+11%) | 3.79 M | 4.26 M (+12%) |

The cycle differences are within the noise: dispatch's three runs of the
same binary spread from 3.80 M to 4.53 M cycles, and another session of the
same pair measured -1.1%. Where dispatch's extra instructions sit was not
broken down.

The loop fence alone (section 9), measured the same way, changes the
instruction counts by under 1% (math_int -0.8%, malloc_stress +0.15%).

## 15. WebAssembly modules

`relift-emit-wasm` (`core/emit-wasm`) emits lifted functions as
WebAssembly modules instead of C, for hosts that run WebAssembly (a
browser first). It follows the contract above wherever WebAssembly allows
it; this section lists the module interface and every difference. The
runtime that implements the imports (JavaScript, or the C runtime compiled
to WebAssembly) does not exist yet. [verified: every rule below is held by
`core/emit-wasm/tests/diff/` against the IR interpreter in wasmtime, with a
stand-in runtime written in the test; the browser side is [inferred]]

### Modules and the program

- A program is one linear memory, one `funcref` table and any number of
  modules. Each module holds a batch of guest functions and imports the
  memory as `env.memory` and the table as `env.table`. It defines nothing
  else that is shared, so a module compiled later (code found at run time,
  section 6) joins the program by being instantiated against the same
  memory and table.
- One memory only: Safari has no multi-memory, so the guest memory, every
  context and the runtime's data share the one linear memory. The guest
  memory starts at the offset `mem`; a guest address `a` is the linear
  address `mem + a` modulo 2^32. A guest whose address space does not fit
  in the 32-bit memory (the Switch) needs memory64 or a smaller layout
  first; the Game Boy, GBA and NDS fit.
- Function `i` of a module goes to table slot `Options::table_base + i`
  through an active element segment, so the runtime gives each module a
  free range of slots, grows the table before instantiating it, and adds
  `Output::slots` (entry, bank, slot) to its lookup. Slot 0 is the
  runtime's by convention; the tests put the miss entry there.
- Functions are exported by name only with `Options::export`; the runtime
  calls them through the table.

### Lifted functions

- Each guest function is one WebAssembly function of type
  `(c: i32, mem: i32) -> i32`: the context address, the guest memory base,
  and the return code. Return codes: 0 (`RL_OK`), 1 (`RL_PENDING`: unwind,
  the runtime's code decides why), and, only in modules without tail calls,
  2 (`RC_CHAIN`, below). Any other nonzero code from a runtime entry is
  passed up unchanged.
- As in C (section 4), registers live in locals: each register the
  function uses is loaded from the context at entry and after every call,
  and each register it writes is stored before every call, trap, slow
  path, intrinsic import and exit. A value up to 64 bits is one `i64`
  local, zero-extended; a wider one (up to 128) a low and a high `i64`.
- A function with more than one entry point (its entry, branch targets
  inside it, call return points) is a `loop` over nested blocks with a
  `br_table` on a label local, one case per entry point; code between
  entry points falls through.

### Context layout

The context is a CPU-neutral block in the linear memory, aligned to 16,
not the CPU layer's `rl_ctx`:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `pc`: the target of the last indirect transfer, read by the lookup's miss path; in step mode every exit's target |
| 8 | 8 | exclusive monitor address |
| 16 | 4 | exclusive monitor size in bytes |
| 20 | 4 | exclusive monitor armed (0 or 1) |
| 24 | 8 | `rl_time.budget` (i64) |
| 32 | 8 | `rl_time.deadline` (u64) |
| 40 | 24 | the runtime's (pending record, thread) |
| 64 | 16 per slot | register slot `k` at `64 + 16 k`, little-endian, zero-extended |

Why not `rl_ctx`: the C emitter names each register's home with the C
lvalue the frontend gives (`Frontend::home`), which is a C string. The
WebAssembly emitter stays free of any CPU by giving every register of the
block's `RegFile` one slot. Flags are one slot each, not packed into a
word. The runtime that ports a CPU layer to WebAssembly reads registers by
slot.

### Calls and transfers

- A direct call to a function of the same module (and not banked) is a
  `call`; any other call stores the target in `pc`, gets a slot from
  `rt.lookup(c, target)` and does a `call_indirect` through the table. The
  return code is checked after each call (section 7): a nonzero code is
  returned at once, so `RL_PENDING` unwinds the host stack as in C.
- A transfer to another function (a jump or branch out, a computed jump
  that no jump-table case of this function matches) is a tail call:
  `return_call` to a function of the same module, `return_call_indirect`
  through `rt.lookup` otherwise. This is the counterpart of C `musttail`.
  A return is `return 0`. Jump tables (`Function::tables`) whose targets
  are inside the function become compares and branches.
- **Fallback without tail calls** (`Options::tail_calls = false`): an
  engine without the tail-call proposal rejects `return_call`, so such a
  transfer stores the target in `pc` and returns `RC_CHAIN`. Each call
  site passes the callee's code through the module's dispatch loop, which
  while it sees `RC_CHAIN` looks up `pc` and calls that function; an
  entry from the runtime that returns `RC_CHAIN` asks the runtime to do
  the same. The host stack stays bounded either way. Tail calls are in
  every current engine (Chrome, Firefox, Safari 18.2 and later); the
  fallback is for older ones.
- `rt.lookup(c, target) -> i32` returns the slot for a guest address, or
  the miss slot, whose function reads `pc` (section 6: a dev build loads
  the code, a shipped build reports the crash). Banked functions
  (`Function::bank`, section 13) are reached only through the lookup,
  which knows the bank mapped at run time.

### Runtime imports

Every module imports these from `rt`, in this order (`RUNTIME_IMPORTS`):

| Import | Type | C counterpart |
|---|---|---|
| `slow(c, mem, code, pc, insn) -> rc` | `(i32, i32, i32, i64, i64) -> i32` | `Frontend::guard_slow` (`rl_fp_slow` for `SLOW_FPCR`): runs the instruction on the slow path with the context stored, 0 to continue after it |
| `lookup(c, target) -> slot` | `(i32, i64) -> i32` | `rl_indirect`'s table lookup |
| `trap(c, mem, code, pc) -> rc` | `(i32, i32, i32, i64) -> i32` | `Frontend::trap` (`rl_svc`) |
| `undefined(c, mem, pc, insn) -> rc` | `(i32, i32, i64, i64) -> i32` | `rl_undefined` |
| `fma32(a, b, c)`, `fma64(a, b, c)` | `(f32, f32, f32) -> f32`, `(f64, f64, f64) -> f64` | `fma`: `a * b + c` with one rounding, which WebAssembly has no instruction for |
| `div128(alo, ahi, blo, bhi, op, bits) -> (lo, hi)` | `(i64, i64, i64, i64, i32, i32) -> (i64, i64)` | the 128-bit `unsigned __int128` division (op 0 udiv, 1 sdiv, 2 urem, 3 srem), with the IR's rules for 0 and overflow |
| `io_read(c, mem, addr, size) -> value` | `(i32, i32, i64, i32) -> i64` | `rl_io_read` |
| `io_write(c, mem, addr, size, value)` | `(i32, i32, i64, i32, i64) -> ()` | `rl_io_write` |
| `sync(c, mem, next) -> rc` | `(i32, i32, i64) -> i32` | `rl_sync` |

- **Guards.** An `Op::Guard` (the FPCR guard of A64 templates and any
  other slow-path guard) is checked where it stands; when it holds, the
  registers are stored and `rt.slow` is called with the instruction. It is
  not hoisted to function entry yet (a speed change, as in C's
  `guard_state`). Floating point is otherwise WebAssembly's own IEEE
  arithmetic, which is what `FpOp` means (round to nearest even,
  saturating conversions, any NaN).
- **Binary16.** WebAssembly has no half precision, so an `FpOp` with a
  16-bit operand or result is inline code in the module, no import: the
  operands are widened to binary64 exactly by integer code, the operation
  runs in binary64, and one helper rounds the result to nearest even
  binary16 (NaNs keep their sign and the top of the payload, quieted).
  This is how `relift_ir` defines binary16 (`core/ir/src/half.rs`), and
  it is exact: binary64 has more than twice binary16's precision plus two
  bits, so a sum, difference, product, quotient or square root rounded
  twice is correctly rounded; a product of two binary16 values is exact
  in binary64, and adding the third operand of a fused multiply-add is
  exact unless the exponents are 33 or more apart, when the error stays
  below the distance to the nearest binary16 rounding boundary.
  Conversions to integers round in binary64 and saturate at the integer
  width (16, 32 or 64; NaN gives 0); integers convert to binary64 and
  round once. The C side calls the A64 runtime's `rl_fp16_*` with FPCR
  zero, which is the same IEEE fast path; imports named after the IR
  operations would only move this code into every runtime.
- **Cipher steps** (`Op::Crypto`) are inline as well, so a runtime needs
  no cipher code: AES `MixColumns` and `InvMixColumns` work on each
  64-bit half (two columns) with byte-parallel doubling in GF(2^8), the
  inverse as `MixColumns` after `x ^= 4 (x ^ (x rotated by two rows))`.
  The AES S-box, its inverse and the SM4 S-box are 256-byte tables held
  in code: each is a helper that picks one of 32 `i64` constants with a
  `br_table` on the byte's top five bits and shifts by the low three. The
  tables are computed at emit time from `relift_ir`'s definition and
  live in code, not in a data segment, so the module still writes
  nothing into the shared memory. Every module carries the cipher and
  binary16 helpers, used or not: an empty module grows from 953 to 3,791
  bytes.
- **Atomics** (`Op::AtomicRmw`, `Op::CompareSwap`) are plain
  load-update-store sequences of 1 to 16 bytes in either byte order, with
  the alignment and splitting of ordinary accesses. This is single-copy
  atomic because a module runs one guest thread per memory: the memory
  is imported unshared and nothing else runs between the load and the
  store. A compare-and-swap stores only when the old value equals the
  expected one and gives the old value either way. Both leave the
  exclusive monitor as it is, like a plain store, the interpreter and the
  C runtime's `rl_atomic` and `rl_cas` (whether such a store clears the
  local monitor is IMPLEMENTATION DEFINED). Orderings are dropped like fences. A
  build for shared memory (threads, not done) would use
  `i32/i64.atomic.rmw*.{add,and,or,xor,xchg}` and `atomic.rmw*.cmpxchg`
  for naturally aligned 1, 2, 4 and 8 bytes with sequentially consistent
  ordering, a `cmpxchg` loop for `Clear` and the minimum and maximum
  updates, and a runtime import for 16 bytes, other sizes and big-endian
  data.
- **Section 13.** Ticks subtract from a budget local loaded at entry; the
  local is stored before every runtime call and reloaded after it. I/O
  accesses of kind `any` get the same inline range check from the
  `IoMap` (`Options::io_map`), `dev` accesses call the hook, and accesses
  of other sizes are split in address order into 1, 2, 4 and 8 bytes. At
  a sync point, when the budget is 0 or less, the registers are stored and
  `rt.sync(c, mem, next)` is called; a nonzero code returns, otherwise the
  exit continues to `next`. A taken interrupt is the runtime's to enter as
  a nested call, as in C.
- **Intrinsics.** The frontend says how each intrinsic is carried
  (`Frontend::wasm_intrinsic`, `relift_guest::WasmIntrinsic`): `Import`
  becomes `intrinsic.<name>` with `(c, mem)` and the operands, one `i64`
  per operand up to 64 bits and two above, returning nothing, one `i64` or
  two; `Nop`, `Const` and `ZeroBlock` are inline. A64 carries `CNTPCT` and
  `WFE` as imports, `CNTFRQ`, `CTR_EL0` and `DCZID_EL0` as the constants
  of `a64/abi.h`, `DC ZVA` as a 64-byte zeroing, and cache maintenance and
  `SEV` as no-ops (one thread per memory, no instruction cache to keep
  coherent); `cpus/a64` checks the constants against the header.
- **Exclusives** use the monitor fields of the context: a load-exclusive
  arms it with address and size, a store-exclusive succeeds when it is
  armed for the same address and size, and a store-exclusive, a clear,
  an atomic update and a compare-and-swap disarm it. Fences are no-ops:
  one memory per thread, no shared memory yet.

### Modes

`Options::exits` selects how exits leave a function. `Chain` is the real
mode above. `Step` is the checker's: every exit stores its target in `pc`
and returns 0, and a sync point that calls `rt.sync` lets the runtime
redirect `pc` to a taken interrupt, so a caller steps one exit at a time
the way the IR interpreter does.

### What it reports

An instruction that did not lift, or that uses something with no
WebAssembly form here (an address or computed target wider than 64 bits,
a memory access of other than 1 to 16 whole bytes, floating point of
other than 16, 32 or 64 bits, an intrinsic the frontend does not carry or
with a carrier that does not fit), calls `rt.undefined` at run time with
its address and raw unit; the latter kind is also listed in
`Output::errors`, never dropped. An IR operation added later fails to
compile the emitter until it is handled.

### Not done yet

The browser shell and the JavaScript runtime (the imports above, the
lookup and table management), the C runtime compiled to WebAssembly,
registers kept in locals across calls and lazy flags (speed), hoisted
guards, an exact `fma64` for engines without one (the stand-in uses the
host's), shared memory with the threads proposal (atomics above), and
linking a guest's device model.

## 16. Differential checks

`relift diff FUNCTION` compares one lifted function with a reference engine
inside a live run. FUNCTION is a host name (`_ZN5guest...`), `module:symbol`
(the guest's mangled symbol) or `module+0xoffset` for an unnamed function.

1. relift makes a dev build and starts it under gdb with
   `runtime/tools/gdb/relift_diff.py` (the script also works by hand:
   `source` it, then `relift-diff SYMBOL [COUNT [SKIP]]`).
2. At each call, the script reads the `rl_ctx` in the first argument and
   finds the function's guest address from the dispatch table slot that holds
   the host function. It starts the engine and sends `STATE pc=<hex>
   stop=<hex> name=<hex>...` (x0 to x30, sp, nzcv, fpcr, tpidr_el0,
   tpidrro_el0, v0 to v31; the return address in x30 is `stop`).
3. The engine runs the function from that state. It asks for each guest page
   it touches with `R <page hex>`; the script answers `OK\n` and 4096 bytes
   or `NO\n` from the live process. The engine keeps its own copy, so the
   process is not changed.
4. The engine answers `RAN ok steps=N`, `RAN left` or `RAN failed`. It stops
   with `left` at a system call (`svc`) and at any address that is not guest
   code, which is where a runtime-provided function (a Vulkan thunk, for
   example) lives; the report names the call and nothing is compared. A step
   limit, an unliftable instruction or a fault is `failed`.
5. After `RAN ok` the script freezes every other thread
   (`scheduler-locking on`), runs the lifted function to its return
   (`finish`), unfreezes, and sends `AFTER name=<hex>...` with the registers
   in the context. nzcv and the thread registers are not sent: the ABI stores
   NZCV only where a callee reads it.
6. The engine compares the registers, then fetches every page it touched
   again from the process and compares the bytes. Bytes in the stack frames
   the function used (from the lowest stack pointer it reached up to the
   entry's) are dead after the return and are skipped. `RESULT match`,
   `RESULT mismatch` or `RESULT stopped` ends the case; the report (first
   differing registers, then pages with the byte count and the first
   differing address) goes to the engine's standard error.

The command exits 0 when every compared call matched, 1 after any mismatch
and 2 when no call was compared. `--count` and `--skip` choose calls.

Engines: the default is `relift diff --serve`, the `relift-ir` interpreter
over the A64 lifter (`relift-cpu-a64`), so it needs no GPL code. `--engine
COMMAND` runs another program that speaks the same protocol; `check/` builds
one on Unicorn (`relift-diff-unicorn`), which is GPL and so is not linked by
anything else. `--flip-read N` flips a bit of the Nth memory read of the
interpreter run: a control, a correct build must then report a mismatch.
`RELIFT_EMIT_TEST_FAULT=<guest address hex>` makes a build emit the first
add of that function as a subtract, the miscompile the end-to-end test looks
for.

Limits: only the function runs, so state other threads change during the
call is seen by the lifted code and not the engine, which shows as a
mismatch; a function that calls a runtime-provided function is not
compared (check its callees one by one); the function must not be inlined
into its callers by the C compiler (dev builds do not inline across guest
functions that are reached through the dispatch table). [verified: the tiny
program in `relift/tests/e2e_diff.rs` (match, test fault, system call) and
`check/tests/diff_unicorn.rs`; the prototype it comes from matched 40 of 40
keyframe calls and functions of up to 9.6k guest instructions in a game and
caught a flipped read in 5 of 5 calls]

## Checks run for this contract

All with clang 23.1.1 on Linux unless noted. Probe sources are scratch, not
committed.

| check | result |
|---|---|
| `clang -std=c11 -Wall -Wextra -Werror -fsyntax-only -x c-header` on `a64/abi.h` (with `abi_core.h`), default target, `--target=aarch64-linux-gnu`, `--target=x86_64-pc-windows-gnu` | clean |
| a TU that includes the header, same three targets, plus `-Wpedantic` | clean |
| same header check with Apple clang 21 on an Apple M2 Mac; gcc 16 `-fsyntax-only` | clean |
| a lifted-style function using every call shape (direct call with the LSDA check, indirect call, musttail, landing-pad switch, FP fast paths) at `-O2 -Wpedantic -Werror`: x86-64 Linux, x86-64 Windows gnu, arm64 Linux, Apple clang arm64 | compiles |
| exclusives probe (llsc_stress checks on `rl_ldx`/`rl_stx`/`rl_ldxp64`/`rl_stxp64`/`rl_clrex`, 4 threads x 200,000), x86-64 Linux, arm64 Linux under qemu, Apple M2 Mac | 15/15 PASS each |
| spin-flag probe: lifted spin loop with `RL_LOOP_FENCE` | PASS on all three; without the fence it hangs (alarm) on x86-64 Linux and an Apple M2 Mac |
| dispatch probe: 10 million `musttail` indirect calls through `rl_indirect`, plus misaligned, below-range and above-range targets reaching the miss handler | PASS on all three |
| `restrict` on `mem` vs on `c`, signal fence vs asm barrier in a spin loop, `musttail` and `cmpxchg16b` codegen | see the sections above |

## Open items

1. Horizon's initial FPCR value; if it is not 0, scoped `fesetenv` blocks
   run in a different mode from the rest (10.1).
2. FP cut-layer implementations (11 operations, slow paths) and their
   checker run against the Sail interpreter (10.1).
3. The synthetic end-of-stack FDE for host-to-guest calls (7).
4. Interprocedural analyses: "may unwind" to drop plain checks; exit-store
   pruning of caller-dead registers (4, 7).
5. Cost of the hash-table dispatch on Android and iOS (5).
6. `spin_flag` and `llsc_stress` recompiled from the libnx test programs, on
   x86-64 Linux and arm64 macOS (8, 9).
7. CTR_EL0 and DCZID_EL0 values on hardware; Horizon svc register
   preservation (which registers `rl_svc` may change besides outputs) (10).
8. Whether any guest program maps one shared memory object twice at
   different 4 KiB offsets within a 16 KiB page, or next to another mapping
   in the same 16 KiB page (2).
9. Guard sizes against real wild-pointer patterns (2).
10. Whether passing guest arguments in host registers is worth a second
    signature after phase 4 measurements (4).
13. The dispatch table shape for banked code regions (13).
14. The JavaScript runtime for WebAssembly modules, and an exact `fma64`
    for it (15).
