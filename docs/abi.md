# Runtime ABI contract

This is the contract between relift-emitted C and the relift C runtime. It
lives in two headers: `runtime/core/include/relift/abi_core.h`, the
CPU-neutral half (export macros, return codes, the pending record, the
context block the core owns, unit registration, the dev protocol), and
`runtime/cpus/arm64/include/relift/arm64/abi.h`, the ARM64 half (`rl_ctx`,
memory access, exclusives, barriers, dispatch lookup, system instructions,
floating point), which includes the first. Lifted A64 code includes only the
A64 header and builds with `-Iruntime/core/include
-Iruntime/cpus/arm64/include`. This file gives the reason for each choice.
Contract version: `RL_ABI_VERSION` <!-- docgen:const file=runtime/core/include/relift/abi_core.h name=RL_ABI_VERSION print=value -->`11`<!-- /docgen:const -->.

## 0. Runtime layers

The runtime is three C layers, mirroring the Rust folders:

| Layer | Folder | Owns |
|---|---|---|
| core | `runtime/core` | No guest OS and no guest CPU. Guest memory (reservation, the exact map at the guest page size, the core's own permission flags, an opaque per-block guest tag), faults at guest addresses, host threads holding an opaque context, the clock at the guest tick rate, unit registration and the dispatch table, pending records, logging, configuration and the unknown-call reporter |
| CPU | `runtime/cpus/arm64` | `rl_ctx` and everything of sections 3 to 10 that names an ARM64 register or instruction: FPCR, WFE, CNTPCT and CNTFRQ, exclusives, `rl_undefined`, `rl_resume`, the miss entry, register summaries for reports |
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
- Lifted C is built with `-std=c11 -O2 -ffp-contract=off` (the dev builds of
  the cartridge guests use a lower level, [Optimization of lifted
  code](guides/usage.md#optimization-of-lifted-code)). Without
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
- **Reservation.** The runtime reserves the guest space up to the end of
  the Switch layout's last region (below) `PROT_NONE` with
  `MAP_NORESERVE`, plus a 1 MiB guard below the base and a 4 GiB guard above
  it. The low guard covers null pointer plus immediate offset (imm9 and LDP
  imm7 reach -1024; LDR unsigned offsets are positive). The high guard
  covers a 32-bit offset added to a pointer near the top. A guest address
  beyond both guards is a wild pointer that would fault on Horizon too; here
  it may hit host memory. No masking: masking costs 1.2x to 2.5x.
  [verified cost: memory prototype; guard sizes inferred]
- **Compact layout (`kernel.layout`, default `compact`).** relift is the
  kernel, so it places the regions of the NPDM's address space type at the
  lowest bases the console's kernel uses and ends the address space after
  the last one. GetInfo 12/13 (AddressSpace), 2-5 (Alias, Heap), 14/15
  (Stack) and QueryMemory report exactly that, and everything above the end
  is Inaccessible, so every address a game is told it may use is backed.
  Sizes, and why a game cannot tell them from the console's:
  - 36-bit: code 0x8000000-0x80000000, alias and heap 6 GiB each after it
    (the console's sizes and places), stack and TLS/IO inside the code
    region (as on the console), AddressSpace ends 4 GiB after the heap
    (0x480000000, 18 GiB) for the windows the runtime maps (Vulkan) and the
    game's own maps. The console's region is 0xff8000000 long; a game learns
    the size only from GetInfo 13 and QueryMemory, which agree.
  - 39-bit: code where the NPDM puts it, then alias 6 GiB (the console uses
    64 GiB; without a system resource size the alias region only takes
    MapMemory copies of heap, whose region is 6 GiB, so 6 GiB covers all a
    game can map there; a process whose NPDM grants a system resource size
    keeps the console's 64 GiB, since svcMapPhysicalMemory maps into the
    alias region and nn::os's virtual address memory reserves its address
    regions there, 12 GiB at once in one game), heap 6 GiB,
    stack 2 GiB and TLS/IO 2 GiB (the console uses 64 GiB; TLS/IO holds
    thread-local pages and IO mappings, megabytes in practice), then 16 GiB
    of AddressSpace for maps. Base placement is the console's without ASLR;
    the console randomizes it, so games cannot depend on exact bases.
  The reservation for the default 39-bit layout falls from 512 GiB to about
  36 GiB; a host that cannot reserve it fails at boot with the size it
  needs. Boot logs the layout; at exit the `stats` line counts the region
  GetInfo calls and the maps and queries within 1 GiB of the end, so a game
  that depends on the region sizes shows up. `console` keeps the console's
  sizes (2^39 or 2^36) for the check flow.
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
    space; the kernel reserves it until the view is unmapped, and refuses
    the map when another mapping is already there. svcQueryMemory reports
    the reserve as free, joined with the free space around it, because
    nn::os checks that the guard pages around a new view are free; a later
    map into the reserve is refused.
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
| 776 | `nzcv[4]` | N, Z, C, V: one byte each, 0 or 1 |
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
| 912 | `ra` | expected return address of a split function (section 5.4) |
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
- **NZCV one byte per flag, stored rarely.** Flags live in four C locals inside a
  function, and clang deletes them where they are dead (zero cost in the
  plain build). The context field is written only by MSR NZCV, by the
  full-state build at every boundary, and by the runtime when it captures a
  thread (svc, fault). One byte per flag (N, Z, C, V, each 0 or 1), so a
  boundary stores and loads each flag with one byte move and no shifts or
  masks; `rl_nzcv` and `rl_set_nzcv` give the MRS layout. NZCV is not live across
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
- **ra** carries a split function's expected return address from its stub
  to its pieces (section 5.4). It took padding at the end of the struct, so
  the size did not change. It shares the clock's cache line; only the
  running thread writes it, and only in split functions. [inferred]
- **64-byte alignment** keeps two threads' contexts off one cache line.
  [inferred]

## 4. Lifted function signature

```c
uint64_t F(rl_ctx *restrict c, uint8_t *mem);   /* RL_LIFTED(F) */
```

- Return `RL_PENDING` (1) when `c->pend` says the host stack must unwind
  (guest exception, thread exit, process exit). A normal return gives
  `RL_OK` (0) on a CPU without a link register, and on A64 the guest
  address the RET went to, which the call site compares with the call's
  return address (section 5.4). Reason: the exceptions prototype measured the check after a call at below noise on
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
- **Exit rule:** at RET and before a tail call, store every register that
  may be dirty: written since entry or since the last boundary on some path
  there. A register dirty on one path only is stored on every path, so a
  path that did not write it must hold its current value in the local: the
  emitter reloads (or loads at entry) every register a later read or store
  needs. A register neither dirty nor needed is not touched: its field
  already holds it (`core/emit/src/state.rs`). Allowed optimization: the emitter may skip storing x9-x17 and
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
- Returns (section 5.4): a piece returns its RET's target like any
  function, so the pieces carry nothing between them.
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
  Each shard declares only the callees it uses. [inferred]
- **Guessed targets (PLT stubs, GOT calls):** a computed call or jump whose
  target is a word the loader fills (a GOT or PLT slot, found by
  `relift_analyze::slot_uses`) is guessed as the function the slot's symbol
  is bound to: its first definition in load order. The emitter compares the
  target with the guess and, when they are equal and no hook replaced a
  function of the guess's module (`rl_switch_hooked[module]`, set by the
  loader before the program starts), calls or tail-calls the guess by name;
  otherwise it goes through `rl_indirect` as before. Both paths store the
  same registers, and dispatch would reach the same function, so the guess
  changes no behaviour, only the cost: a hit skips the table lookup and
  the indirect transfer. A guessed jump to a higher address skips the
  preemption check, as a direct jump forward has none (section 14.1).
  [verified: a commercial game, libnx corpus]
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
  `RL_MUSTTAIL return F_<target>(c, mem);` or, through dispatch,
  `RL_JUMP(target, site);` (section 6, shipped builds). `musttail` makes
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
  computed; the emitter only guesses where a slot points (section 5,
  guessed targets) and checks the guess at run time, so a loader that binds
  elsewhere, a lazy binding or a guest write to the slot still reaches the
  right function through dispatch.
- **Discovery:** exported functions are roots of their own module, so the
  targets the guest loader writes into GOT slots are lifted entries. A slot
  that resolves to a function no module exports goes to the miss path
  (section 6).

## 5.2 Total coverage

A Switch build is total: every aligned word of a module's executable pages
has a dispatch slot that is not empty, so no guest jump can miss.

- **Closed world.** The build first checks the NPDM: the supervisor calls
  both the ACI0 and the ACID allow must include none of `CreateCodeMemory`,
  `ControlCodeMemory`, `SetProcessMemoryPermission`, `MapProcessCodeMemory`
  and `UnmapProcessCodeMemory`, the ACI0 must not grant `jit:u`, and when it
  grants `ldr:ro` or `ro:1` the romfs and add-on content must hold no NRO or
  NRR image (by magic). Otherwise the build stops: the program could run
  code no build can lift. A missing NPDM stops it too.
- **Superset roots.** After discovery, every word of the executable pages
  that no function covers, that is not padding (zero, `nop`, `udf`, `brk`)
  and that lifts, and that no such word before it falls into, is a root
  of more discovery, stopping at the known entries, until none is left.
  These functions exist only in the cold copy.
- **Cold copy.** Each module gets a second unit, `rl_cold_<i>_<module>`:
  every function of the hot layout and every superset function, emitted
  with `enter_anywhere` in pieces of about 4 KiB of C (`emit.cold-piece-kib`;
  1 KiB pieces hold about three guest instructions, and their per-piece
  table entries and names cost more than the code), in shards of their
  own under `c/<i>_<module>/cold/`, built at `-Os` without debug info or
  unwind tables (crash reports walk frame pointers, and guest exceptions
  unwind through `RL_PENDING`, not host tables). The function maps name
  every piece by its function (`Options::piece_syms`). On x86-64 ELF the
  copy is built with `-mcmodel=large` and `-DRL_COLD_TEXT`, which puts it in
  the section `relift_cold`, and the link adds a script fragment that places
  that section after `.bss` in a segment of its own: hot code and the
  runtime reach `.data` and `.bss` with 32-bit PC-relative relocations, and
  the cold copy between them would push them past 2 GiB. On arm64 (ADRP
  reaches 4 GiB; the linkers add branch thunks) and Windows the copy stays
  in `relift_text`. `emit.cold-first`, for tests, registers the cold units
  first and stops cold code from calling hot functions, so every function
  runs from the cold copy. Its unit lists each run
  of consecutive instructions of one piece as one `RL_FN_RANGE` entry
  (`Options::entry_ranges`), which fills every empty slot of its range. A
  function every instruction of which is a hot entry gets no cold copy. Its direct calls and
  tail calls go to the hot functions by name (`Options::calls`); hot code
  never calls cold code, and reaches it only through dispatch at an
  address that is not a hot entry. A landing pad keeps its `rl_pad_resume`
  entry. Hot shards, their flags and their code do not change. The setting
  `emit.cold` turns the copy off.
- **Code map.** `rl_code_map` (`RL_CODE_MAP_VERSION` <!-- docgen:const file=runtime/core/include/relift/abi_core.h name=RL_CODE_MAP_VERSION print=value -->`1`<!-- /docgen:const -->) gives each slot of
  the executable pages its class: `RL_CODE_HOT` (an entry of the hot unit:
  function entries, landing pads, overrides), `RL_CODE_COLD` (an
  instruction only the cold unit enters) or `RL_CODE_TRAP` (padding or a
  word that does not lift). Pages of `RL_CODE_PAGE_SLOTS` slots point at
  shared two-bit patterns. The build refuses a map that leaves a word that
  lifts as a trap. The program lists the maps (`rl_switch_program.code_maps`),
  and the loader, after registering every unit hot first, calls
  `rl_code_maps_register`, which checks each slot against the units and the
  table and refuses any disagreement. Trap slots stay empty, so a dispatch
  there is a miss, and the miss path names the class (`rl_code_class`): the
  stop report says the target holds no instruction, and a dev build does not
  ask relift to lift it. In a dev build each cold slot
  first holds `rl_dispatch_cold`, which logs the first dispatch there
  (`first entry to cold code at <address> (<module>+<offset>)`) and puts
  the cold function back.

## 5.3 Registers kept across direct calls

After a direct call of a lifted function of the same module, the caller
does not reload a callee-saved register (`Frontend::callee_saved`; A64:
x19-x29 and sp) that the emitter proves the callee returns with its entry
value: its local already holds it. Everything else follows the boundary
rule, and the landing-pad dispatch still reloads every register a pad
reads. [verified: `core/emit/src/callee/tests.rs`, the libnx corpus]

- **The proof** (`core/emit/src/callee.rs`) looks at registers only, never
  at memory: per function, a forward dataflow gives each register at each
  instruction as an entry register plus a constant, or unknown. A register
  is kept when it holds its own entry value at every return, and at every
  tail call by name to a function that keeps it. A direct call makes every
  register its callee does not keep unknown. An indirect call or tail call,
  a call of an override, a trap, a sync point and a computed jump make
  everything unknown, and so does every landing pad and resume point,
  which the dispatch enters with what the unwinder installed. An
  instruction that does not lift, or that the emitter cannot express, ends
  its path: the frontend's undefined call never returns `RL_OK`. A guard's
  slow path counts as writing everything its block writes, and as going on
  after the instruction.
- **Recursion:** the module is solved as a greatest fixpoint, every
  function starting as keeping everything. That is sound for normal
  returns by induction on call depth: a call that does not return
  normally takes its caller off the paths that matter.
- **What this does not assume:** nothing about AAPCS64, memory or the
  callee's stack discipline. A register saved on the stack and restored
  from it is not kept, since another store could have changed the saved
  copy; a frame popped by arithmetic keeps sp.
- **In the C:** a kept register that is read after the call is live at the
  call, so the caller has it in its local (loaded at entry or after an
  earlier boundary) and stores it there if it is dirty; the callee then
  leaves the context field as it was.
- **Cold copy (5.2):** the proof covers functions of the emit it runs in.
  A call or tail call that `Options::calls` sends to a function of another
  emit (the cold copy's calls of hot functions) counts as unknown, so cold
  code reloads after it; the proof itself is over guest code, which every
  copy of a function shares, and dispatch into cold code at another
  address is an indirect transfer, which keeps nothing.
- **Reuse:** what a callee keeps is part of its `Target`, so a caller's C
  is taken from the emit store only when every callee keeps what it kept
  before.
- **Cost and effect:** a second pass over the module's lifted code before
  emission; on a 31,000-function program module it adds about 0.3 s to the
  emit stage, a third of the functions keep something, and 17% of the
  calls skip about 3.6 reloads each. That shrinks the lifted text of a
  3.2-million-instruction sample by 0.1% on x86-64 and 0.24% on arm64.
  Most functions keep nothing because they make an indirect call, jump
  through a PLT stub or table, or have landing pads, and their callers
  inherit that. [measured at the commit adding it]

## 5.4 Returns

An A64 lifted function returns the guest address its RET went to, and a
call site goes on only when that is the call's return address. Any other
target continues there through the runtime, as on the console. So a return
address that the guest rewrote (longjmp-style code, user-mode context
switches, a game patching its saved x30), one that was tampered with, or
one whose pointer authentication failed behaves as it does on hardware,
whichever RET form it is (RET, RET Xn, RETAA, RETAB, AUTIBSP then RET).
[verified: `e2e_ret`, `e2e_pac`, emitter and runtime unit tests]

- **The return value.** `rl_fn` returns `uint64_t` (section 4). A return
  is `return <the RET target>;` after the stores of the dirty registers. A
  host function with the lifted ABI (an override, a Vulkan thunk, `memcpy`)
  returns `rl_arm64_ret(c)`, which is x30. `RL_PENDING` (1) means a pending
  record is set; it is told apart from a RET to address 1 by `pend.kind`,
  which is `RL_PEND_NONE` whenever no record is pending, so a RET to 1 is
  a return to 1 (and faults there, as on the console). Other CPUs keep
  `RL_OK` (0) and `RL_PENDING` in the wider type.
- **The call site:**

  ```c
  rc = F(c, mem);
  if (RL_UNLIKELY(rc != UINT64_C(<return address>)))
      return rl_arm64_ret_elsewhere(c, rc, UINT64_C(<pc of the call>));
  ```

  The same line replaces the pending check that followed every call (the
  compare is against an immediate instead of zero), so a call site costs
  what it did before the check existed. With landing pads the slow path
  calls `rl_arm64_ret_elsewhere` and then runs the usual pad check.
  Traps, guard slow paths and the other runtime helpers still return
  `RL_OK` or `RL_PENDING` and keep the plain pending check.
- **Nothing in the callee.** The expected address is a constant of the call
  site, so the callee keeps no copy of x30 of entry: no entry load, no
  callee-saved host register across its calls, no compare at its returns
  and none at its tail transfers. A tail transfer (`B`, `BR` out of the
  function, crt0's `mov x30, exit; b main`) is a plain `RL_MUSTTAIL` call
  whose callee's return value goes up unchanged, so a foreign x30 at a tail
  transfer needs no check either: whatever the last RET goes to is what
  the call site sees. Split functions need nothing in the context.
- **The redirect.** `rl_arm64_ret_elsewhere` (inline, no calls) passes a
  pending callee up, and otherwise records
  `{RL_PEND_REDIRECT, pc = target, sp = RL_SP_NONE}` with `c->pc` = the
  call and returns `RL_PENDING`. No landing-pad check matches
  `RL_SP_NONE`, so every host frame returns, and `rl_call_guest` (the
  thread entry and every runtime call into guest code) continues at the
  target in a fresh host frame, with the state the RET stored. It returns
  to its own caller when a function it entered returns to, or a redirect
  reaches, the x30 it was called with. The Switch thread entry is
  `rl_call_guest` with x30 = 0.
- **The target needs a dispatch entry.** A redirect target is looked up
  like a computed jump: in a dev build a missing one is lifted and
  hot-loaded (section 6); in a shipped build it is missed code. The return
  sites that redirects reach in practice are therefore hints.
- **A non-canonical target** (a bit set at or above the guest's virtual
  address size, 39 bits by default; the Switch layer sets it from the
  address space it lays out with `rl_arm64_set_va_bits`; no top-byte ignore
  for instruction addresses) is an instruction abort: `rl_call_guest` and
  `rl_dispatch_miss` report it with `rl_guest_fatal`, naming the target,
  the call it returned from (or the function `rl_call_guest` entered) and
  that a failed pointer authentication gives such an address. That is how
  the console ends a failed authenticated return without FPAC: the
  authenticated value has an error code in its top bits and the fetch
  faults.
- **Other CPUs.** The check is the frontend's `return_link`; only A64 has
  one so far. The WebAssembly emitter (section 15) does not check returns
  yet.
- **Why not check in the callee.** The first design compared the target
  with x30 of entry in the callee (`if (rl_to == rl_ra) return RL_OK;`).
  That kept x30 of entry live across every call the function makes, so a
  function that calls needed one more callee-saved host register (push,
  pop, a move) besides the load, compare and branch, and every tail
  transfer of a function that writes x30 needed a compare too. On a commercial game
  that was +2.84% instructions per frame on the render thread (x86-64,
  12.835M against 12.835M + 0.364M). Here the only cost is the width of the
  compare: on x86-64 `cmp rax, imm32` replaces `test eax, eax` (the return
  addresses fit in 32 bits), and the callee's `xor eax, eax` becomes a move
  of the target, which is usually in a register already. On AArch64 the
  immediate does not fit a compare, so the call site builds it (`mov`,
  `movk`) and compares (`cmp`, `b.ne`) where it had one `cbnz`: two or
  three instructions per call, against the entry load, the callee-saved
  register and the compare of the first design.
- **Cost.** On x86-64 the fast path of a call site is the code without any
  check but for `cmp rax, imm32` in place of `test rax, rax` (a game's
  `OpCodes::~OpCodes`: the same instructions on the path through its call
  and tail call, where the callee check added 8: a callee-saved register
  pushed and popped with its stack alignment slot, a move, a second load
  of x30 and the compare and branch before the tail call). The slow path is out of line and cold
  (`rl_arm64_ret_elsewhere` is a call, not inline), about 2.8 instructions
  per call site at the end of the function (the call's address, the
  arguments, a copy of the epilogue). Static instructions of a game's hot
  shards (clang 23 -O2, x86-64), against the same emitter without any
  check:

  | module | callee check | call-site check |
  |---|---|---|
  | game (31,793 functions, 185,051 call sites) | +0.93% | +1.51% |
  | nnSdk (34,687 functions, 79,939 call sites) | +4.03% | +3.08% |

  The game module grows more than before because its callee checks were
  dropped where x30 provably kept its entry value, and every call site now
  has its slow path.

  Whole game: a commercial game's gameplay at 60 fps on x86-64 (i9-11900KF, clang
  23.1.3, headless on an RTX 3070), perf `instructions:u` and `cycles:u`
  of the render thread over 60 s from guest frame 15200, three runs each,
  alternating, host load average 1.1 to 1.2 (6.6 for the first main run):

  | build | render instructions per frame | render cycles per frame | `relift_text` |
  |---|---|---|---|
  | no check (main) | 12.844M, 12.832M, 12.830M (mean 12.835M) | 5.675M, 5.693M, 5.577M | 85,166,288 bytes |
  | callee check | 13.199M (+2.84%) | within noise of main | +1.93% |
  | call-site check | 12.869M, 12.861M, 12.859M (mean 12.863M, +0.22%) | 5.696M, 5.774M, 5.720M (+1.4% on the means; the spread of earlier runs was 5.17M to 5.83M) | 86,134,720 bytes (+1.14%) |

  A guest program that calls a 16-deep chain of functions that save x30
  and return, 100 million times (1.6 billion calls and returns), each
  function in a shard of its own so clang inlines none of them (`sep`), and
  all of them in one shard (`one`):

  | host | no check | callee check | call-site check |
  |---|---|---|---|
  | x86-64 (Ryzen 9 7950X, clang 23), `sep`, user instructions | 49.33e9 | 58.63e9 | 49.23e9 |
  | x86-64, `one`, user instructions | 12.50e9 | 31.90e9 | 32.30e9 |
  | AArch64 (Apple M2, Apple clang 21), `sep`, user s | 2.62 | 7.45 | 3.01 |
  | AArch64, `one`, user s | 0.93 | 4.10 | 2.15 |

  On the M2 the call site keeps its return address in a callee-saved
  register across the call (one more `stp`/`ldp` pair) and compares with
  `cmp`/`b.ne` where it had `cbnz`: about 0.25 ns per call in `sep`. The
  callee check's relaxed atomic entry load is gone with the check, so
  nothing is opaque to the inliner any more. The inlined chain (`one`)
  still keeps the compare of each call site where no check had none: a
  callee that reloads x30 from the guest stack after a call returns a value
  clang cannot prove is the address the caller stored, as the call could
  have changed the slot (on the console it would change the return), while
  without any check the inlined callee returned the constant `RL_OK` and
  the pending checks folded away. [open: a whole game on AArch64; the M2 numbers are from a host whose disk was full, so only the
  two builds above were remade]

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
   (`rl_dev_lift_req.fp_mode`; the FPCR for A64). `site` is the guest
   address of the transferring instruction, 0 when unknown, and `reason`
   says how the miss was entered: `RL_MISS_JUMP` for a transfer that leaves
   no host frame (`RL_JUMP` hands its site constant to the cold
   `rl_dispatch_jump_miss`), `RL_MISS_CALL` for a computed call (the
   dispatcher's host return address lies in lifted code, and the CPU layer
   reads the site from the return address the call set: x30 - 4 on A64, the
   top of the shadow stack on arm32, the return address on the guest stack
   on SM83), `RL_MISS_ENTRY` for an entry from the runtime (thread start,
   interrupt, redirect), `RL_MISS_RESUME` for a landing pad. relift uses
   them for its log only. [verified: `arm64_dispatch_through_lookup`,
   `core_stop_report_takes_a_call_site_from_the_cpu`, e2e_hotload]
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
   entries win, except `RL_FN_OVERRIDE`), and adds the unit's function maps
   to the crash map.
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
  with a constant target (a direct jump through `RL_JUMP`), since the unit
  cannot name the program's symbols. The new entries join the known set for later requests.
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

**Hot-loaded code** calls everything outside itself through `rl_indirect`
and jumps there through `RL_JUMP`, never by symbol, so it does not need the main program's lifted symbols. It
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
(<!-- docgen:const file=runtime/core/include/relift/abi_core.h name=RL_EXIT_SOFTWARE -->`70`<!-- /docgen:const -->, EX_SOFTWARE). [verified: hot-load prototype, each case]

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
host function), a line map field no build fills yet (section 11, still to
come), code base, name, and the function maps of its shards
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

A miss writes a stop report (section 11, stop reports) and ends the guest
with `RL_PEND_EXIT_PROCESS` and status `RL_EXIT_SOFTWARE` (<!-- docgen:const file=runtime/core/include/relift/abi_core.h name=RL_EXIT_SOFTWARE -->`70`<!-- /docgen:const -->). Shipped
builds never load code at run time. A release Game Boy build stops the
same way at the first instruction without a lifted body (it has no CPU
interpreter), and names the transfer site in the bank that was mapped;
a `--dev` build runs it on the device library's CPU. [decided]

Computed transfers that leave no host frame are emitted as
`RL_JUMP(target, site);` (`abi_core.h`): the CPU layer's `rl_jump_find`
stores the target in `c->pc` and loads the table slot; a full slot is a tail
call, an empty one stores the site constant in `c->pend.pc` (unused while
`pend.kind` is `RL_PEND_NONE`) and tail-calls `rl_dispatch_jump_miss`. The
constant lives on the cold branch only, so the hot path is a load, a test and
a jump, against a load, a select and a jump for the former
`rl_indirect(c, target)(c, mem)`. Computed calls keep `rl_indirect`, whose
miss is a host call from the lifted caller.

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
    it) and replaces it with `rl_arm64_unw_jumpto` (`RL_FN_OVERRIDE |
    RL_FN_MAY_UNWIND`), which reads the block at x0, sets d0-d31, and calls
    `rl_resume` with the block's x0-x29, sp and pc (x30 is the pc, as on
    hardware). The jump to the pad is then the same table entry as in the
    libgcc shape. [verified: the SDK module's routine has exactly this
    code, and in a commercial game's build the table holds the override at it; the
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
[verified: `arm64_tests jmp` on x86-64 Linux and an Apple M2 Mac]

- **Overrides.** `rl_arm64_setjmp` and `rl_arm64_longjmp` (`arm64/abi.h`) have
  the lifted signature and replace the guest's `setjmp` and `longjmp`
  (`_setjmp`, `__builtin_setjmp` and `sigsetjmp` with a zero mask take the
  same overrides). The build registers them as `RL_FN_OVERRIDE` entries at
  the guest symbols' addresses; `longjmp` also carries `RL_FN_MAY_UNWIND`.
  - `rl_arm64_setjmp` stores x19 to x30, sp and d8 to d15 (`v[8..15].d[0]`)
    into the jmp_buf at guest address x0 and returns x30 with x0 = 0.
    The buffer is guest memory, so guest code that reads or copies it sees
    what the guest's own `setjmp` would have written.
  - `rl_arm64_longjmp` reads that buffer, sets x0 to w1 (1 when w1 is 0,
    zero-extended, as the guest's `cinc w0, w1, eq` does), and calls
    `rl_resume` with pc = the saved x30 and the saved sp. It returns
    `RL_PENDING` with `pend = {saved x30, saved sp, RL_PEND_UNWIND}`.
- **Layout.** `rl_arm64_jmp_layout` gives the offsets of x19, sp and d8 in
  the guest libc's jmp_buf. The default `rl_arm64_jmp_newlib` is x19 at 0,
  sp at 96, d8 at 112, 176 bytes [verified: disassembly of devkitA64
  newlib's `setjmp` and `longjmp`]. A guest layer whose libc differs calls
  `rl_arm64_set_jmp_layout` at start-up; relift reads the offsets from the
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

### Non-local transfers without overrides

A guest whose libc relift does not know needs no `setjmp` or `longjmp`
override: any function that stores the return address it was entered with
to memory outside its own frame saves it for a later non-local jump.
`relift_analyze::escape::return_escapes` finds such functions with a
dataflow over each lifted function (a stored value derived from the link
register, at an address that is not the stack pointer plus a constant; a
function that tail-calls one is one too) [checked: musl's `setjmp` and
`sigsetjmp`, libunwind's `unw_getcontext`; unit tests in `relift-analyze`].
The return address of every direct call to such a function is then a landing
pad of the caller: the emitter gives the caller a dispatch case for it and
the dispatch table a `rl_pad_resume` entry, so the library's own `longjmp`
(restore the registers and the stack pointer, `br` or `ret` to the saved
address) unwinds the host stack to the frame whose stack pointer it carries
as section 7 describes for exceptions. A `ret` to a pad reaches it through
the call site's return check: `rl_arm64_ret_elsewhere` records an unwind
(`pend.sp` the context's stack pointer) rather than a redirect when the
target is a pad. An escaper that itself calls an escaper (musl's
`sigsetjmp`) has no frame when the jump comes; the address after its inner
call is made a function entry instead (a continuation) that runs on top of
the frames and returns to the saved address in the live frame, which is a
pad. Calls through a function pointer to an escaper are treated as calls to one when an escaper's address is taken anywhere in the program.

## 7.2 Memory routine overrides

Lifted `memcpy`, `memmove` and `memset` of the guest's libc run the guest's
byte, word and pair loads and stores one after another; the host's versions
move 16 to 64 bytes per instruction. In a perf profile of a title's render
thread the SDK's `__memcpy_device` alone was 7.6% of the samples and
`memset` 0.7%. [verified: perf profile]

- **Overrides.** `rl_arm64_memmove` (x0 = dst, x1 = src, x2 = n) and
  `rl_arm64_memset` (x0 = dst, x1 = value, x2 = n) have the lifted signature,
  return x30 (section 5.4), leave every register but the ones a call may change as
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
in `arm64/abi.h` (static inline, no runtime call):

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
  [verified: static scan of SDK-based guest code; Horizon's initial FPCR is
  0: the kernel's thread context starts with FPCR and FPSR 0, and
  `svcCreateThread` copies the creating thread's FPCR (Atmosphère's
  `KThreadContext::Initialize` and `CloneFpuStatus`)]
- **The FPCR proof drops the guards.** Each block whose semantics assume
  FPCR zero starts with the FPCR guard (`sail::SLOW_FPCR`), tested once per
  function entry, call return and FPCR write through the hoisted local
  `g<fpcr>`, written as `RL_FPCR_GUARD(r)`. Once every module is lifted, the
  build (`relift/src/fpcr.rs`) runs the proof of `cpus/arm64/src/fpcr.rs`:
  when every `MSR FPCR` of the program writes the default mode, lifted
  code (hot and cold copies, mod variants) is compiled with
  `-DRL_FPCR_CONST`, which makes `RL_FPCR_GUARD` 0, so clang drops every
  guard, its slow path and the stores the slow path needed. Any write it
  cannot classify keeps every guard, as before.
  - Soundness: the main thread starts with the default (the runtime's
    `ctx_init` sets `RL_FPCR_DEFAULT`) and a created thread with its
    creator's FPCR (`hos_create_thread`, as on the console); FPCR changes
    only through `MSR FPCR` (no other runtime entry or system call writes
    the guest bits of `c->fpcr`). So, by induction over the program's run,
    the mode stays the default if every `MSR FPCR` writes the default
    whenever every earlier `MRS FPCR` on its thread read it.
  - The proof checks exactly that, by abstract interpretation with `MRS
    FPCR` giving the default: values are known bits, pointers at a known
    offset from the stack a root function runs on (or a stack it switches
    to), and the function addresses PLT and GOT words hold. A root starts
    with nothing known; callees are inlined (the functions that reach a
    write within six calls at any depth, others a few levels below the
    nearest of those); calls it does not follow
    keep the callee-saved registers and sp (AAPCS64) and change only memory
    whose address escaped; landing pads start from what each such call
    leaves. A write whose value comes from the root's arguments, such as
    `fesetround(mode)` or `fesetenv(env)`, is decided at the callers: the
    search climbs from the function holding the write to every function
    that may call it (direct branches, an extent holding the entry or the
    instruction before it, PLT and GOT transfers, jump tables), up to six
    levels, and stops at an escaped address (`relift/src/fpflags.rs` names
    them) or the program start. A function with no callers whose address
    does not escape is never entered, unless the program can look a
    function up by name at run time (`nn::ro::LookupSymbol`,
    `nn::ro::LookupModuleSymbol`, `dlsym` reachable): then every exported
    function counts as escaped. The loader's binding, lazy PLT binding
    included, only fills slots that name a symbol, which the climb
    follows. A callee where the interpretation stops (a computed jump
    without known targets, a return it cannot place) and that it need not
    inline is treated as a call it does not follow and the root tried
    again. A root then holds when every
    write it reaches writes known zero guest bits. A scope guard that saves
    the environment with `fegetenv` into its stack frame, sets the default
    and later restores it with `fesetenv` proves because the saved bytes
    are the default and nothing can reach them.
  - The memory model: a program writes an object only through addresses
    derived from that object's own address (C and C++ code follows it; a
    stack buffer overflow does not). An object starts at an address the
    code takes and ends at the next one; a load overlapping an object whose
    address escaped (to memory, to a call the analysis does not follow, to
    another thread) gives an unknown value, and an address computed from a
    pointer with an unknown offset makes the object it points into
    escaped (an untaken local lies in the object before it). A pointer stored to tracked stack memory escapes when that
    memory does. A callee reads its callers' frame records (through x29)
    only to unwind, so the frame pointer does not escape at a call.
  - A computed jump without known targets, a call to a resume point
    (`setjmp`) or to a mod variant, or too much code stops a root without a
    result, and the guards stay. An `MSR FPCR` in no function keeps them
    too (code that discovery missed is not covered).
  - The build log names how many writes, roots and climbed functions it
    took, or why the guards stay. In a commercial game the 3 writes
    (`fesetround`, `fesetenv` and the user exception handler's save and
    restore) prove through 46 call sites of the game's FPU scope guard.
  - `emit.fpcr-guards = true` keeps the guards, for debugging and for the
    reference mode `RELIFT_FP_EXACT=1`, which runs through them (the
    runtime warns when that mode is asked of a build without them).
    `program.c` of a guard-free build calls `rl_arm64_set_fpcr_const(1)`,
    and `rl_msr_fpcr` then reports a non-default mode as an error: it
    would mean the proof was wrong.
- **FPSR cumulative flags** (IOC, DZC, OFC, UFC, IXC, IDC, QC) are produced
  only for a program that can observe them. The build's FPSR census
  (`cpus/arm64/src/flags.rs`) looks at every reachable `MRS Xt, FPSR`: it
  counts when some path uses `Xt` before overwriting it, a call uses
  `x0` to `x7`, a computed jump uses everything, and a value returned in
  `x0` counts when a caller uses `x0` after the call. libgcc's soft-float
  `__sfp_handle_exceptions`, linked into every libnx program, reads FPSR
  only to raise exceptions and does not count. Without a usable read the
  program is built flag-free and pays nothing.
- With one, flags are produced per function, not per program. FPSR's
  flags are sticky, so a read sees every flag raised on its thread since
  the last FPSR write. The flags-live analysis
  (`cpus/arm64/src/flags/region.rs`, driven by `relift/src/fpflags.rs`)
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
  `rl_arm64_set_fp_flags(1)`, and `MSR FPSR` clears the host flags in every
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
  (`rl_arm64_flags_enter`, `rl_arm64_flags_leave`).
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
  It needs the FPCR guards: build with `emit.fpcr-guards = true` when the
  FPCR proof dropped them. Program-wide or per-function FPSR flags do not
  go through the FPCR guard (they are the build's `-DRL_FP_FLAGS`
  choice and the `SLOW_FLAGS` guard), so they work the same without it.
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
    for NaNs. A NaN result still goes to the slow path: the compiler may
    fold an operation without keeping its NaN (x * 1.0 to x keeps a
    signaling NaN). [inferred]
  - **Dependency: the A64 frontend relies on these helpers for Arm's NaN.**
    Because the inline fast paths and their slow paths give Arm's NaN for
    32- and 64-bit add, subtract, multiply, divide, square root and fused
    multiply-add, the emitter's lift (`lift_for_emit`, through
    `sail::exact_host` in `cpus/arm64/src/lift/sail/rt.rs`) leaves out the
    IR's NaN handling after those operations. Lifted code then has no other
    NaN handling for them: any change that lets a host NaN through (dropping
    the NaN test of a fast path, `rl_fp_nan_fixup` returning 0, a slow path
    that does not follow `FPProcessNaNs` and `FPDefaultNaN`, or a new host
    path for these operations) breaks guest results, and must instead make
    `exact_host` keep the NaN handling for the affected operations. The
    reference lift that the interpreter and the checker run keeps it, and
    `relift/tests/emit_interp` and `emit_flags` compare the two. In a
    commercial game that handling was about a sixth of the lifted code.
    [verified: emit_interp, emit_flags]
  - After a block's FPCR guard the emitter passes the helpers FPCR as the
    constant 0 (the guard left the block otherwise), so a NaN or a flag
    fix-up calls `rl_fp32_add_cold` and its siblings: FPCR zero and no
    FPSR pointer, with clang's `preserve_all` convention on x86-64 and
    arm64, so the rare call does not make the lifted code spill the
    registers, vector registers included, it holds around the operation.
    [measured: a commercial game's lifted code]
  - Non-default modes on x86-64 go to the slow path. If Horizon's default
    FPCR turns out to set FZ or DN, x86-64 needs a fast path for that mode
    before performance work. [open]
- `__builtin_fmaf`/`__builtin_fma` lower to an FMA instruction where the
  target has one and to libm's correctly rounded `fma` otherwise. [inferred]

## 10.2 Crypto instructions

`arm64/crypto.h` has one function per guest instruction of FEAT_AES,
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

`arm64/fp16.h` is IEEE binary16 on `uint16_t` bit patterns, for the IR's
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

### Stop reports

`rl_report_stop` (`runtime/core/report.c`) writes the report of every fatal
guest stop: missed code, a target that holds no instruction (a trap slot),
an undefined or unsupported instruction (`rl_guest_stop_at`), a breakpoint,
a trap, a landing pad without a case (`rl_bad_resume`) and every other
`rl_guest_fatal`. It formats into static buffers (no heap), and one report
at a time is written. The report goes to a new file
`relift-stop-<UTC date>-<time>-<pid>.txt` (created exclusively; a second
report of the same second gets `-1`, `-2` and so on before `.txt`) in the
report folder: `rl_report_set_dir`, which the Android glue points at
`reports` in the app's files folder and the iOS glue at `reports` in its
Documents, where the Files app shows it; else `RELIFT_REPORT_DIR`; else, in
a shipped build only, `reports` in the app's user folder beside its
settings and saves (app `relift` when the program names none),
`$XDG_DATA_HOME/<app>` on Linux, `Application Support` on macOS,
`%APPDATA%` on Windows. A dev build (`RL_HOTLOAD`, or `RL_DEV_RUN` in a
`relift run` of a Game Boy or GBA project) writes a file only to a folder
set one of the first two ways, and `cargo` runs and the CI scripts set
`RELIFT_REPORT_DIR` to a folder of their own (`.cargo/config.toml`,
`scripts/ci/lib.sh`), so tests leave nothing in the user's folders. The log
gets one error line, `[relift error stop] <kind>: <summary>; report written
to <file>`, and at `RELIFT_LOG` info or below the whole report follows on
stderr. stderr is logcat on Android and the app's console on iOS; on Apple
hosts each line also goes to `os_log`. The format is plain text, one `key:
value` per line, and `relift report` reads it:

```
relift stop report 1
kind: missed code
summary: no function at guest 0x8000040
relift: 0.1.0
game: switch 0100000000001234 version 0
code mods: none
build: 0c490fe372813308
guest: switch
program: 0100000000001234
module: main base 0x8000000 size 0x1020 build 5c5a...
guest layer: switch
cpu: arm64
image: 1d2a4728...
pid: 4242
thread: 4243
time: 1794801317
transfer: call
target: 0x8000040 main+0x40
target code: not lifted (outside every lifted function)
target where: main text segment
target words 0x8000040: d2800221 d65f03c0 ...
site: 0x800001c main+0x1c
site function: 0x8000000 main+0x0 _ZN5guest4main11fn_00000000E +0x1c
site words 0x800000c: d2800089 9b097d08 10ffff6a 8b080148 d63f0100 ...
caller function: 0x8000000 main+0x0 _ZN5guest4main11fn_00000000E (host _ZN5guest4main11fn_00000000E+0x95)
registers:
  x0  0000000008001000  x1  0000000000008000  ...
  sp  0000000398000000  pc  0000000008000040  nzcv 00000000  fpcr 00000000  fpsr 00000000
guest backtrace: lifted frames, innermost first; tail calls leave no frame
  #0 0x8000000 main+0x0 _ZN5guest4main11fn_00000000E
host backtrace:
  #0 0x559b68d68c88 game+0xa2c88
  ...
end of report
```

- The header lines come from the program (`rl_report_info`: the generated
  info file gives `relift`, `game`, `code mods` and `build`, a digest of the
  embedded info document), the core (`mods`: bundled mods that are on) and
  the guest layer (`guest`, `module` lines with base, size and build id).
  `build` tells builds of one game with another relift, other code mods or
  another settings schema apart; it does not identify the emitted code
  itself (two builds with other emit settings of one relift share it).
  `image` is the hash of the guest images the code was lifted from, the
  value hot-loaded units must match; together with `relift` they say what
  the program was built from.
- A host fault in lifted code (SIGSEGV, SIGBUS, SIGILL, or the Windows
  exceptions) prints the crash report of the crash handler above and also
  writes its text to `relift-crash-<unix time>-<pid>.txt` in the same
  folder, with async-signal-safe calls only (the folder is worked out ahead,
  in `rl_fault_install` and `rl_report_set_dir`). It is the crash format,
  not a stop report: `relift report` does not read it.
- `transfer` is `call`, `jump`, `entry`, `resume` or `none` (a stop at an
  instruction, then `at:` replaces `target:`). `site` is exact for jumps
  (`RL_JUMP`) and computed calls; it is `unknown` for an entry from the
  runtime, for a Game Boy dispatch into a switchable bank, whose window
  entry carries no site, and for the resume of a landing pad, which no guest instruction
  makes (the emitter passes `RL_SITE_NONE`).
- `target code` says what the build knew: lifted, no instruction (a trap
  slot of the code map: padding or data), not lifted inside a module's code
  map or a lifted function's extent, or outside every lifted function.
  `target where` is the guest layer's place (Switch: module segment; GBA:
  memory region; Game Boy: region, ROM bank and file offset). The words are
  read through the guest layer's `peek`, which never faults.
- `registers` is the CPU layer's `regs` (A64: x0 to x30, sp, pc, NZCV, FPCR,
  FPSR, the thread pointers; arm32: r0 to r14, pc, CPSR and the open host
  frames' return addresses; SM83: the summary). The guest backtrace is the
  host frame walk of the stopping thread (`rl_crash_capture`) through the
  function maps.
- No UI shows the report; a crash dialog would be a UI change.
- `RELIFT_FORCE_MISS=<hex address>[,...]` empties those dispatch slots
  before the first guest thread runs, so a test or a developer can reach
  the miss path of any build at a chosen target (for a Switch game, an
  address-taken entry of `entries.txt` reached only through a computed
  call).

`relift report <file>` (in the project directory, or with `--build-dir`)
prints the report's transfer, the guest instructions around the site and at
the target (llvm-mc for A64 and arm32 when it is on `PATH`, the opcode
table for SM83), the guest backtrace, the site's lifted C from the build's
`c/` folder and what `entries.txt` and `unlifted.txt` say about the target,
and adds the target as a root for the next build: a `[hints]` entry in
`game.toml` (Switch, as `relift run` does for hot-loaded code, and GBA,
`rom+0x<offset>` for a cartridge ROM target, an odd offset for Thumb code,
which `relift-gba-analyze` takes as roots of origin `hint`) or a line of the
learned file (Game Boy: `BBB:AAAA` for a ROM target in bank `BBB`, `ram AAAA
KKK` for code in RAM with its opcode key). `--no-hint` only prints. The SM83
layer keeps what the last dispatch miss knew (`rl_sm83_last_miss`: the
target and the `RL_JUMP` site, or the CALL or RST before the return
address), because the Game Boy guest takes every miss through its own path
first and reports it only when no lifted code can run there.

It also says why analysis left the site unresolved, at no cost to the
build: for a GBA or Game Boy project the build's `unresolved.txt` already
lists every transfer analysis did not follow with its kind and detail, and
the lines at the site are printed; for a Switch project, the function that
holds the site is lifted again from the build's cached outline
(`analysis/<module index>.zst`) and its unresolved transfers are
classified as `relift coverage` does (the reason, where the target came
from, and the loader slot it goes through), or the report says that no
lifted function holds the site or that the site was a jump table the target
is not in.

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
  `abi_core.h` and `arm64/abi.h` moved declarations and changed no layout,
  signature, constant or call shape (the one renamed field,
  `rl_dev_lift_req.fpcr` to `fp_mode`, keeps its offset and size), so the
  version stayed 1.
- Version 2 added the function maps (`rl_fn_info`, `rl_fn_map`,
  `rl_unit.map_count` and `rl_unit.maps` at the end of `rl_unit`) and the
  `RL_TEXT` placement of lifted functions (section 11). A unit written with
  designated initializers and no maps still compiles unchanged. The map
  has its own `RL_FN_MAP_VERSION`; the entry for each piece of a split
  function added `fn_addr` and made it 2.
- Version 5 added `RL_FN_RANGE`, the code maps (`rl_code_map`, `enum rl_code_class`,
  `RL_CODE_PAGE_SLOTS`, `RL_CODE_MAP_VERSION`, section 5.2) and
  `rl_dispatch_trap` and `rl_dispatch_cold`.
- Version 6 added `RL_JUMP`, `rl_jump_find`, `rl_find` and
  `rl_dispatch_jump_miss` (section 6, shipped builds): lifted code's computed
  jumps now call the runtime there. `rl_dispatch_trap` is gone; trap slots
  stay empty.
- Version 8 added the return check (section 5.4): `rl_fn` returns
  `uint64_t`, an A64 function returns its RET's target and a host function
  with the lifted ABI x30 (`rl_arm64_ret`), each call site compares that
  with its return address and calls `rl_arm64_ret_elsewhere` otherwise, and
  lifted A64 code raises `RL_PEND_REDIRECT` records. `RL_PENDING` always
  comes with a pending record, which is how it is told from a RET to
  address 1.
- Version 10 added the RSP issue pipeline (`relift/rsp/abi.h`): lifted
  RSP blocks call `rl_rsp_pipe`, `rl_rsp_pipe_hold` and `rl_rsp_tick`, and
  an RSP image carries the pipeline each bound word is entered with.
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
  A hook whose handler does not return to the code it interrupted (the
  program's own vector switches threads with an exception return) may
  instead set `pend = {RL_PEND_REDIRECT, pc = entry, sp = RL_SP_NONE}` and
  answer `RL_IRQ_REDIRECT`; `rl_sync` then returns `RL_PENDING` and the run
  goes on at the entry with the host frames above the thread entry gone.
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

### The Game Boy Advance

ARMv4T code (`relift/arm32/abi.h`, `relift/gba/run.h`, the frontend in
`guests/gba/guest`) runs on the machine in `runtime/guests/gba` through the
CPU layer `runtime/cpus/arm32` and the glue in `runtime/guests/gba/run`.

- `rl_ctx` holds r0 to r14 of the current mode, the CPSR in parts (one byte
  per flag, the Thumb bit, the mode bits, the masks) and the SPSRs and banked
  registers of relift-cpu-arm32's register file, so a mode change in lifted
  code is a plain swap of fields. The dispatch granule is a halfword; one
  table covers ARM and Thumb code, so an address is the entry of one state.
- Every load and store goes to the machine's bus (`rl_gba_cpu_read`,
  `rl_gba_cpu_write`, from `Frontend::io_calls`), which charges the region's
  wait states to the clock. The bus gets the address as the CPU puts it out
  (before the lifter's alignment): the machine aligns it except for the
  8-bit backup. Each instruction's bus cycles reach the machine in the
  ARM7TDMI's order: the fetch two instructions ahead (`rl_gba_cpu_fetch`,
  through the machine's prefetch buffer, nonsequential after a data access
  or an internal cycle), the data accesses, the internal cycles
  (`rl_gba_cpu_idle`, which leave the bus to the prefetch buffer and DMA) and
  the refill after a taken branch (`rl_gba_cpu_refill`).
- Every instruction ends in a sync point (`rl_sync`). The IRQ line is a
  scheduler line masked while the I bit is set: lifted code reports writes of
  I (`rl_arm32_set_i`). An instruction ends with the IRQ taken only when the
  line rose by the first cycle of its last access (`rl_gba_irq_sample`); a
  later rise waits for the next instruction. An IRQ taken at a sync point enters IRQ mode, runs
  the HLE BIOS stub's entry, calls the handler as a nested host call with LR
  0x138, and at 0x138 runs the stub's return and restores the CPSR.
- SWI lifts to a trap after the exception entry; `rl_gba_swi_trap` runs the
  HLE BIOS call and returns from the exception. Inside IntrWait and Halt the
  machine calls back where the console takes the interrupt, and the handler
  runs there as on the console's BIOS.
- ARMv4T has no return instruction. A lifted call (BL, the second half of a
  Thumb BL pair, `MOV LR,PC` then `BX`) pushes its return address on the
  context's frame stack (`rl_arm32_call`), and a transfer to the address on
  top (`rl_indirect`, or `rl_jump_find` in `RL_JUMP`) returns from the host
  frame. A computed jump anywhere else goes through dispatch; a redirect
  (`rl_arm32_redirect`, SoftReset) restarts the run loop with no open frame.
- Missed code stops the program with a stop report (section 11);
  hot-loading for this guest is open.

### The VR4300

VR4300 code (`relift/mips/abi.h`, `relift/n64/run.h`, lifted by
relift-cpu-mips through the frontend in `guests/n64/guest`) runs on the CPU
layer `runtime/cpus/mips` and the Nintendo 64 machine in
`runtime/guests/n64`, through the glue in `runtime/guests/n64/run`.

- `rl_ctx` holds relift-cpu-mips's register slots in slot order
  (`RL_MIPS_SLOTS`, checked against the register file by
  `cpus/mips/tests/ctx_layout.rs`), then the core's block, the `rl_time`
  clock in PClock cycles, the 32-entry TLB and the frame stack. Data is
  big-endian; the dispatch granule is a word.
- Count advances every second cycle of the clock (`rl_mips_count`); a guest
  write of Count is seen at the next CP0 sync (`rl_mips_cp0_sync`, after a
  write of Status, Cause or Compare and at every interrupt check). Count
  reaching Compare sets IP7; a write of Compare clears it (the lifted
  template). `rl_mips_timer_cycles` gives the guest layer the cycle to post
  its timer event at. Random counts down from 31 to Wired, one step a cycle.
- An interrupt is taken when IE is set, EXL and ERL are clear and an IP bit
  is set under its IM bit. The guest layer's interrupt hook calls
  `rl_mips_irq_enter` with its lines (bit n is IP n, for 2 to 6), which
  enters the general vector with EPC at the sync point's next pc and opens a
  host frame; the handler's ERET to that pc closes it (`rl_mips_eret_to`). An
  ERET anywhere else while an interrupt is open (a thread switch) leaves
  every frame through a redirect.
- Guards (code 0x1000 plus ExcCode, 0x2000 for the FPU slow path, plus 0x100
  in a delay slot) leave to `rl_mips_guard`, which runs the instruction:
  a load or store outside kseg0 and kseg1 through the TLB and the bus
  (`rl_mips_set_bus`), a COP1 operation through the soft float, anything else
  as its exception with EPC, Cause.BD and CE, BadVAddr, Context, XContext and
  EntryHi as the VR4300 sets them, to the refill (0x000, 0x080 in 64-bit
  addressing), general (0x180) or BEV vectors. A guard in a delay slot goes
  to `rl_mips_guard_exit`, which also runs the branch.
- The FPU fast path runs normal operands to normal results in round to
  nearest with no enable set; FCR31's cause and flag bits come from
  `rl_mips_fcr31` and compares from `rl_mips_fcmp`. The slow path
  (`rl_mips_fpu`) implements every rule: subnormal and quiet NaN operands
  and out-of-range conversions are unimplemented operations, a signalling
  NaN is invalid, a subnormal result is unimplemented unless FS is set with
  the underflow and inexact enables clear (then it flushes by the rounding
  mode), and a NaN result is the default NaN. `relift-check --oracle ares
  lift --fp-corpus --c-slow-path FILE` compares it with ares.
- CACHE calls the hook of `rl_mips_set_cache_hook` (code invalidation is the
  guest layer's).
- Loads and stores are I/O accesses of kind `Any` on physical addresses.
  The N64 map makes RDRAM (below 8 MiB) plain memory, read and written in
  place through `mem` (the RDRAM base), and passes every other address to
  the machine (`rl_n64_cpu_read`, `rl_n64_cpu_write`, whose values are the
  bytes in host order). RDRAM holds the guest's big-endian data as
  host-order 32-bit words, the layout of N64 emulators and of parallel-rdp
  (the frontend's `ram_words`): a word is `rl_ld32` and `rl_st32` with no
  swap, a halfword is at its address XOR 2, a byte at its address XOR 3, a
  doubleword is its two words in address order (`rl_e_rot32`), and an
  access of 3, 5, 6 or 7 bytes (the cases of LWL, LDL and their kin) goes
  byte by byte. The runtime reads and writes RDRAM bytes through
  `rl_n64_rdram_get` and `rl_n64_rdram_put`; dumps and hashes of RDRAM are
  in the guest's byte order. An access the instruction makes only under a condition (a likely
  branch's delay slot, SC, the cases of LWL and its kin) first leaves through
  its TLB guard when its address is not in RDRAM, so the slow path makes it
  exactly and a device never sees an annulled access. The runtime's own
  accesses use `rl_mips_ld` and `rl_mips_st` (big-endian values).
- Each instruction ticks one issue cycle plus the cycles the overlay's
  `MIPS_Cycles` gives it, as ares counts them: MULT and MULTU 4 more, DMULT
  and DMULTU 7, DIV and DIVU 36, DDIV and DDIVU 68, and on the FPU ADD, SUB
  and the compares 2, MUL.S 4 and MUL.D 7, DIV and SQRT 28 (S) or 57 (D),
  CVT.S.D 1, the other conversions and roundings 4. A branch ticks before
  its delay slot runs, and a slot a likely branch annuls takes no cycle.
  Every block ends in a sync point.
- `rl_ctx` keeps the tags of the 16 KiB instruction cache (32-byte lines)
  and the 8 KiB write-back data cache (16-byte lines, a dirty bit). A fetch
  where a run may start a line (a line's first word, a function entry, a
  branch or jump target, a call's return, a delay slot that starts a line,
  the target of `rl_mips_eret_to`) calls `rl_mips_icache`: a kseg0 or
  mapped line that is not in the cache costs 48 cycles. Every load calls
  `rl_mips_dcache` after the access and every store before it writes
  memory: a cached hit takes 1 cycle (a store marks the line dirty), a miss
  40, plus 40 to write a dirty line back first. kseg1 costs nothing in
  either cache.
- The data cache holds data. RDRAM holds what the CPU sees, its lines
  included, since lifted code reads and writes it directly; each change of
  a data line goes to the line hook (`rl_mips_set_dline_hook`), and the
  machine keeps the bytes RDRAM itself holds under each line: a fill keeps
  RDRAM's line, a dirty line written back leaves RDRAM as it is, a line
  dropped (Hit Invalidate, or a clean line a device wrote under) gives its
  kept bytes back. DMA, the AI and the PIF read and write those kept bytes
  under a dirty line or one a device wrote, so they see no store until it
  is written back and the CPU's loads see a line's old bytes after a DMA
  under it. The renderer reads RDRAM itself: at a frame edge the kept bytes
  of such lines in the picture VI scans out are swapped in until the
  scan-out is done.
  CACHE (`rl_mips_cache_op`) invalidates, writes back, fills and loads and
  stores tags through TagLo with the same costs. A guard's slow path takes
  its instruction's fetch, its issue cycle and its data cache cycles, but
  not the multiplier's or the FPU's.
- A read outside RDRAM waits for the bus (`rl_n64_read_stall`): 20 cycles
  for the RCP's registers, SP memory and the PIF, plus 250 for the PI bus
  in the cartridge's space, or twice the rest of a PI bus write in flight.
  The device answers at the cycle the read starts; writes are posted. A
  fetch from SP memory (the boot code, uncached at its kseg1 address) waits
  the same 20 cycles: the frontend adds them to the tick of each
  instruction there, a branch's delay slot included.
- A read of Count calls `rl_mips_count`. A
  write of Status, Cause, Compare, Count or Wired ends the budget
  (`rl_n64_cp0_break`), and the block's sync point is `rl_n64_cp0_sync`,
  which applies the write with the block's register writes done (the mask,
  the timer event at `rl_mips_timer_cycles`) and then takes an interrupt
  that is due. A TLB instruction outside a delay slot ends in the trap
  `rl_n64_tlb`. `ERET` leaves before any effect to
  `rl_mips_eret_to(c, rl_mips_eret(c))`. `JAL` and `JALR` with a link
  register open a host frame (`rl_mips_call`).
- MI's interrupt output is scheduler line 2, the CPU's IP2. The timer is a
  scheduler event at the cycle Count reaches Compare.
- The boot runs the cartridge's boot code (IPL3, the ROM's bytes
  `0x40..0x1000`) as lifted code. Only the PIF's part (IPL1 and IPL2 in the
  PIF ROM, which relift does not have) is high level and takes no time:
  `rl_n64_pif_boot` copies the ROM's first 4 KiB to DMEM, sets the PI domain
  1 timing from the header word (latency bits 0 to 7, pulse width 8 to 15,
  page size 16 to 19, release 20 and 21) and the RI registers the PIF's boot
  leaves (`RI_SELECT` 0x14, so the boot code finds RDRAM set up), and the
  CPU starts at `0xA4000040` with t3 that address, s4 the TV type (1 NTSC,
  0 PAL), s6 the boot variant's seed, s3, s5 and s7 0, sp `0xA4001FF0`, ra
  `0xA4001554` and Status `0x34000000`. The boot code then loads the game
  with a PI DMA, checks it, writes the boot words at `0x80000300` and jumps
  to the entry, all at the cycles its own instructions take, so the VI and
  every later event keep the console's phase. The analysis lifts the boot
  code only for a boot variant known by its hash; a program without it boots
  at a high level (`rl_n64_boot`: the first MiB at the entry, the boot words
  and the RDRAM size word, the registers the boot code leaves at the jump to
  the game).
- Code is bound at run time, never interpreted. The build lifts every code
  run the analysis found, at its link address, one unit per segment linked
  in RDRAM or SP memory, and lists each run with its source, offset,
  length, link address and FNV-1a hash (`rl_n64_code`). No unit is in dispatch at the start: each
  run's slots hold the run layer's entry check, which binds the run whose
  bytes in RDRAM hash to its hash (the functions replace the check in every
  slot of the run). A DMA into RDRAM (PI, SP, SI) or a CACHE that
  invalidates instruction cache lines puts the check back over the runs it
  touches. An entry that no run matches is missed code: a report, an RDRAM
  dump with `--dump`, and the program stops. Dev builds have no hot-load
  for this guest yet. A transfer from one run to another is always a
  computed one, through dispatch, so the other run's check sees the bytes
  that are there when it runs. The boot code's runs, linked at their kseg1
  addresses in SP memory, are bound the same way against DMEM and IMEM in
  slots of their own (one per word of SP memory, outside the dispatch
  table): a dispatch miss first asks the guest layer's lookup
  (`rl_mips_set_lookup_hook`), which gives the slot there. Any write to SP
  memory (a CPU store or an SP DMA) puts the check back over the runs it
  touches.
- `RELIFT_N64_TRACE=PATH` (a development aid) records every lifted call
  site's first VI frame and cycle, its count and its last frame, and
  writes them to PATH at exit in the order of first call, for a diff
  against an emulator's executed-address trace. It is compiled out unless
  the build asks for it: `relift build` with `RELIFT_N64_TRACE=1` in its
  environment defines `RL_N64_TRACE` for the lifted code and the runtime,
  and only then does a lifted call test the hook or the run layer read the
  variable. A default build has no trace branch in lifted code.

### The RSP

RSP microcode (`relift/rsp/abi.h`, lifted by relift-cpu-rsp through
`guests/n64/guest/src/rsp.rs`) runs on the layer `runtime/cpus/rsp`, which
the machine's SP interface drives through `runtime/guests/n64/run`.

- `rl_ctx` is the RSP's own: relift-cpu-rsp's register slots in slot order
  (`RL_RSP_SLOTS`, checked by the frontend's tests), then `pc`, the `rl_time`
  clock in RSP cycles, the dispatch table and the layer's RSP. An RSP unit
  includes no other CPU layer's header; it shares only `rl_unit` and
  `rl_fn` with the VR4300 units, through pointers.
- `mem` is DMEM. The lifter masks every address to 12 bits and the lifted
  code swaps the big-endian data.
- An image is lifted at its own offsets, so one unit runs wherever the
  program loads its bytes. `c->pc` and the targets of transfers are offsets
  in the running image; `c->base` is the IMEM address of its offset 0, and
  `rl_rsp_base` gives it to the code where an IMEM address is a value: the
  link of `JAL`, `JALR`, `BGEZAL` and `BLTZAL` adds it, and the absolute
  targets of `J`, `JAL`, `JR` and `JALR` subtract it. The build cuts each
  run of RSP code the analysis finds into images of at most 4 KiB that
  overlap by 8 bytes, and lifts each IMEM layout the analysis finds a
  microcode making (its own code with the overlays its DMAs load over it)
  as an image at IMEM 0. An image of all 4 KiB runs across the end of IMEM:
  the delay slot of its last word is its first word.
- In a layout image, a branch or fall-through to a word that a DMA of the
  microcode may overwrite, or that the layout does not know, goes through
  `rl_indirect` (all of them when the analysis could not follow every path
  and DMA), and the word is no successor of the block, so the layer checks
  the bytes there on entry and binds the image of what IMEM holds.
- Each instruction charges one cycle (a branch and its delay slot two) and
  every block ends in a sync point whose slow path is `rl_rsp_sync`: it goes
  on while the slice has cycles left and the RSP runs, and otherwise stores
  the next pc and returns `RL_PENDING` to the layer's run loop.
- `MFC0` calls `rl_rsp_mfc0`, `MTC0` `rl_rsp_mtc0` and `BREAK`
  `rl_rsp_break`, each with the RSP cycle; a write and a `BREAK` end the
  budget, so the block's sync point sees a halt.
- Every transfer the function does not hold goes through `rl_indirect` to
  the dispatch slot of the target word, which also gives the base of the
  slot's image. A slot holds `rl_rsp_enter` until the layer has checked
  that IMEM holds the bytes of an image, at some base, at every word the
  lifted code there can reach without a computed jump; an IMEM write puts
  `rl_rsp_enter` back in every slot. A word no image matches is missed
  code: a report, IMEM and DMEM with `--dump`, and the program stops. So is
  a word that did not lift or that has no C form (`rl_rsp_undefined`); the
  build counts both among the instructions not lifted.
- VRCP, VRSQ and their variants read the ROMs from the formulas that
  define them; VRSQ's needs an f64 square root, which the lifted code calls
  as `rl_rsp_i2f64`, `rl_rsp_fsqrt64` and `rl_rsp_f2i_ceil64`.

### SI, the PIF and the cartridge's saves

`runtime/guests/n64` models the serial interface, the PIF's joybus and the
cartridge's save memory (ported from ares, ISC; see its `NOTICE`).

- SI: the registers, a 64-byte DMA each way with the durations ares uses
  (the block's time on the wire for PIF to RDRAM, 4065 RCP cycles for the
  other), `SI_STATUS` with its busy, state and interrupt bits, and the
  interrupt at the end. A CPU word write to the PIF's address space keeps SI
  busy for 2150 RCP cycles, ignores a second write, and a read in that
  window ends it and gives the latch. The PIF to RDRAM DMA runs the joybus
  commands when it ends, so the game sees the controller at that moment.
- The PIF's boot is high level: PIF ROM reads are 0 and the cartridge
  boot chip's challenge (the command bit 0x02 some boot variants use) is not
  answered. A block written to PIF RAM with the flag in byte 63 set is
  parsed (channels 0 to 4; 0xFF padding, 0x00 skip, 0xFD reset, 0xFE end);
  a block is run channel 4 first and channel 0 last, an empty or failing
  channel's receive byte gets 0x80 and a reply longer than the command
  allows 0x40.
- A controller (`rl_n64_set_pad`, or the `p1` port of the native table with
  `rl_n64_pad_use_ports`) answers status (pak absent), state (the button
  word, then the stick x right and y up) and the Controller Pak commands as
  an empty slot (zero data with the inverted data CRC). A port with nothing
  plugged in is an error reply. L, R and Start together read as the reset
  button with the stick centered; opposite directions cancel.
- Saves: no table names a game's save type. SRAM answers at 0x08000000
  (32 KiB, mirrored) until the game writes the Flash command register at
  0x08010000, from when the 128 KiB Flash chip answers (a Macronix MX29L1101
  with its command set, status and silicon ID, erase and program times).
  EEPROM answers on joybus channel 4 and reports 16 Kbit (2048 bytes); a
  512-byte `.eep` file found at load makes it 4 Kbit. Every store the game
  changes is kept as a raw image next to the others under one base path:
  `.eep`, `.sra`, `.fla`. They are written atomically a second after the
  last change and at exit, only for the stores that changed; a flash file at
  load makes the chip answer from the start.
- Cartridge bus accesses go halfword by halfword through the selected
  device (ROM, SRAM, Flash or none); an access to nothing gives the bus
  latch, which holds the address. A CPU word write to the bus keeps the PI
  busy for 400 ares clocks and a PI DMA to domain 2 writes the save memory.

### VI and AI: the video and audio output

`runtime/guests/n64` runs VI and AI as closed-form device models that catch
up to the CPU's tick on a register access and at their events (ported from
ares, ISC; see its `NOTICE`), and hands their output to the core's window
and audio layers through `runtime/guests/n64/run`.

- VI: the registers, the line counter with its field bit and leap pattern,
  the line duration from `VI_H_SYNC` (the leap line from `VI_H_SYNC_LEAP`),
  the lines of a field from `VI_V_SYNC`, the interrupt at `VI_V_INTR` in
  progressive and interlaced modes, and the frame edge at the start line of
  `VI_V_VIDEO`. The video clock is 48681818 Hz (NTSC) or 49656530 Hz (PAL, a
  cartridge header region code of D, F, H, I, L, P, S, U, W, X, Y or Z); the
  frame rate is whatever the registers make it (about 59.83 Hz for the
  usual NTSC values) and a VI that is off makes a frame every 200 lines of
  0x800 clocks. The counter starts with a line at cycle 0, and a line is
  seen from the cycle after the one it starts at (ares runs its VI up to the CPU's clock before an access, not
  including it).
- Scan-out (`rl_n64_scanout`) is ares' software path, with no RDP: the
  output is 640 by 480 (NTSC) or 640 by 576 (PAL) RGB; the lines and columns
  inside `VI_V_VIDEO` and `VI_H_VIDEO` are drawn (clamped to the output
  window, 8 columns off the left edge and 7 off the right as ares does),
  each pixel read from RDRAM through `VI_X_SCALE` and `VI_Y_SCALE` (steps of
  1/1024 of a pixel in x and 1/2048 in y, with the subpixel offsets) as RGBA5551 or RGBA8888 (the
  alpha dropped, 5 bits widened by replication), and with `VI_CONTROL`'s
  serrate bit only the lines of the current field are drawn, the others
  keeping the last picture. `rl_n64_framebuffer` is the raw buffer: origin,
  width and the visible size, no scaling.
- Not modeled in the picture: anti-aliasing, dither, divot and gamma
  (`VI_CONTROL` bits 2 to 4 and 8 and 9 are kept and ignored, as in ares'
  software path), the color burst and sync timing registers (kept, never
  used).
- The program presents the picture with `rl_window_present_rgb` at each
  frame, paced to the guest's own clock (the CPU cycles between frames at
  93.75 MHz), so a 59.83 Hz picture is presented at 59.83 Hz and a headless
  run does not sleep. `--png FILE` is the picture of the last frame; `--raw-png
  FILE` the raw framebuffer; `--shown-png FILE` the last picture after the
  player's picture options (`picture.aa`), as the window shows it.
- AI: `AI_DRAM_ADDR` and `AI_LEN` fill a two-entry FIFO (a write with it
  full is ignored; a length is a multiple of 8 below 256 KiB), `AI_CONTROL`
  enables the DMA, a write of `AI_STATUS` clears the interrupt, and a read
  of any other register gives the length left in the playing buffer.
  `AI_STATUS` has full (bits 0 and 31), busy (bit 30), enabled (bit 25) and
  the two bits ares keeps at 1 (20 and 24). The interrupt rises when a
  buffer is queued into an empty FIFO and when the second buffer starts.
  The DAC takes a stereo word every (video clock / (`AI_DACRATE` + 1))
  clock, 44100 Hz until the rate is written; the DMA address wraps its low
  13 bits with the carry into the upper part delayed one sample, so the
  carry of the last sample of a buffer reaches the next buffer's address
  (the console's 8 KiB address bug). With nothing playing the DAC level
  decays to silence.
- The machine resamples the DAC's samples to `RL_N64_AUDIO_RATE` (44100 Hz,
  linear interpolation, fixed point) into a ring and the program hands
  them to `rl_audio_open` per frame; the frames made match the time that
  has passed, silence included. `--wav FILE` captures the sound (16-bit
  stereo) and `--no-audio` opens no output; headless runs capture without an
  output, and `--frames N` prints the frame count, peak and hash of the
  sound on stderr.

### The Z80 layer

`runtime/cpus/z80` (`relift/z80/abi.h`) is the CPU layer of the Z80
frontend (`cpus/z80`); the clock is in T-states. [verified: the layer by
`runtime/cpus/z80/tests`; lifted code on it by `relift/tests/z80_irq.rs`
(IM 1 taken while the CPU waits in HALT, and after the one instruction EI lets
through, with the T-state count and R) and `relift/tests/z80_zexdoc.rs`
(ZEXDOC, 67 of 67 groups, natively)]

- **Interrupt entries are lifted code.** The frontend lifts the entry of
  each interrupt (NMI, IM 0 with 0xFF on the bus, IM 1, IM 2) as a block at
  `RL_Z80_ENTRY` plus the kind that pushes `ret`, clears the flip-flops,
  charges the entry's T-states and jumps to the vector. The entry hook
  (`rl_irq_hook`) stores the address `rl_sync` gives it in `ctx->ret` and
  returns the entry's address, which dispatch runs like any function; so a
  program that can take interrupts must contain the four entries. The return
  hook reads the address the handler's return popped, as for the SM83.
- **The mask follows IFF1.** A block that sets IFF1 (EI, DI, RETI, RETN, an
  entry) calls the intrinsic `z80_iff(iff1)` before its sync point, which the
  frontend lowers to `rl_z80_iff`: the scheduler mask is the NMI line plus the
  INT line while IFF1 is 1, so a raise of INT while interrupts are disabled
  does not end the budget, and enabling them with INT asserted does. EI sets
  `ei` (EIDelay); the hook withholds a maskable interrupt while it is set and
  ends the budget, so the entry happens at the sync point after the next
  instruction. An NMI is edge latched by the machine (`rl_z80_ints.pending`)
  and is never withheld.
- **HALT** charges its 4 T-states and calls `z80_halt(iff1, r) -> r`: lifted
  code keeps registers in locals, so the values go in and the new R comes
  back. The layer waits through the scheduler for an NMI, or INT with IFF1
  set, in whole NOP cycles that count in R, and ends the budget so the sync
  point that closes the block enters the interrupt with the address after
  HALT. A HALT nothing can end stops the guest with a report.
- **Ports** are the intrinsics `z80_in(port)` and `z80_out(port, value)`,
  lowered to `rl_z80_in` and `rl_z80_out` over `rl_z80_ints.in` and `.out`;
  the 16-bit port is the whole address bus.
- A lifted `CALL` is a host call and `RET` a host return, so a host function
  standing in for guest code at a call target (a BDOS stub) is its own `RET`:
  it adds 2 to SP, which the pushed return address took. Conditional calls
  and returns are two blocks, the second at the instruction's address plus
  `RL_Z80_HALF`.

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
- **Ordered mode** (`rl_sched_config.ordered`, for machines whose CPUs
  share memory with cycle-exact timing, such as the NDS): the CPUs no
  longer bound each other by a quantum. Before each access another CPU may
  observe, the guest layer calls `rl_sched_order(cpu)`, which hands the run
  to whichever CPU is first in the order (tick, rank, index) until the
  caller is first; private code runs fused in between. A halted CPU with no
  raised line leaves the order and does not hold events back; the guest
  layer moves its clock to the wake tick (`rl_sched_stall`) when it raises
  the line. The result depends on the clocks only, so it is repeatable and
  matches a per-instruction lockstep run access for access; an interrupt is
  seen at the guest layer's next check (a block boundary), not between two
  instructions of a block.
  [verified: core_tests `sched_ordered_*`, nds_tests `q3_*`]
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
  call back). Returns, traps and indirect calls are not, and neither is a
  call or jump back to a lifted function that leaves only by returning (no
  call, tail call or computed jump, every instruction lifted): no cycle of
  transfers passes through such a function, and its own loops carry their
  own checks. Other frontends emit nothing.
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
  pending. `rl_preempt` is declared `RL_COLD_CALL` (`preserve_all` and
  `cold` on x86-64 and arm64): the caller keeps its live values in their
  registers across it, so a short function whose only call is the check
  needs no callee-saved registers and no spills for it. The registers stay in their locals: `rl_preempt` only waits for
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

## 14.2 Linux threads

A Linux guest thread is a host thread with a context of its own over the
one guest address space (`runtime/guests/linux/thread.c`). Lifted code
records the address of each `svc` in the context's pc before it traps
(`rl_svc_site`, the `arm64.svc_site` intrinsic), so the system call handler
knows the call site. `clone` with the flags of a thread starts the child
at the instruction after the `svc`, which is in the middle of the function
that made the call: the Linux guest makes the address after every `svc #0`
whose system call number is the constant 220 (set by the straight-line code
before it) a function entry, found structurally in `lift.rs`
(`clone_continuations`), never by library or symbol name [checked: musl and
glibc `pthread_create` in the corpus]. The child gets the parent's
registers with x0 = 0, the stack and the thread pointer `clone` named. The
futex is a wait queue over a host mutex and condition variable
(`lxsync.h`); the address map and the descriptor table take one lock around
the calls that change them. A signal sent to a thread by `tgkill` is
delivered when that thread next returns from a system call. A `clone`
without the thread flags (a fork) returns `ENOSYS`; fork-style `clone`,
`fork` and `exec` are not supported, and `wait4` returns `ECHILD`.

### Signals that interrupt blocking calls

Every guest thread has a wake handle (`hio_waker_*`: a self-pipe on POSIX
hosts, an event on Windows). A blocking call waits through `lx_wait`
(`hio_wait`: the descriptor, the timeout and the handle together), and the
futex waits on a condition variable that a kick also wakes. A signal sent to
a thread (`tgkill`, `tkill`, `kill` picking a thread that does not block it)
queues on that thread and kicks it; a signal that is blocked, ignored or
default-ignored does not interrupt anything. An interrupted call returns a
kernel restart code (`include/linux/errno.h`), and `rl_svc` decides as the
arm64 kernel does, before the handler runs: `ERESTARTSYS` (a read on a pipe,
terminal or socket; a futex wait without a timeout) runs the call again after
the handler when the handler has `SA_RESTART` and returns `EINTR` otherwise;
`ERESTARTNOHAND` (`ppoll`) and `ERESTART_RESTARTBLOCK` (`nanosleep`,
`clock_nanosleep`, a futex wait with a timeout) return `EINTR` whenever a
handler runs, `SA_RESTART` or not, and a relative sleep stores the time left
in its `rem` argument. A restarted call runs again with its original
arguments [checked: `sig_interrupt` in the corpus, recorded on arm64 Linux,
static, static-PIE and glibc]. A thread that runs guest code without making
a system call sees a signal at its next call.

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
  The AES S-box and its inverse are 256-byte tables held
  in code: each is a helper that picks one of 32 `i64` constants with a
  `br_table` on the byte's top five bits and shifts by the low three. The
  tables are computed at emit time from `relift_ir`'s definition and
  live in code, not in a data segment, so the module still writes
  nothing into the shared memory. Every module carries the cipher and
  binary16 helpers, used or not: an empty module grows from 953 to 3,157
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
  two; `Nop`, `Const` and `ZeroBlock` are inline. A64 carries `CNTPCT`, `WFE`
  and the SM4 S-box (`arm64.sm4_sbox`, one byte in and out; the host
  supplies the table) as imports, `CNTFRQ`, `CTR_EL0` and `DCZID_EL0` as the
  constants of `arm64/abi.h`, `DC ZVA` as a 64-byte zeroing, and cache maintenance and
  `SEV` as no-ops (one thread per memory, no instruction cache to keep
  coherent); `cpus/arm64` checks the constants against the header.
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
over the A64 lifter (`relift-cpu-arm64`), so it needs no GPL code. `--engine
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

## 16.1 Lane operations

`Op::Lane` and `Op::Permute` (`core/ir/src/lane.rs`) work on 128-bit values
read as 8, 16, 32 or 64-bit lanes, lane 0 being the least significant bits.
They are pure, have no guest-visible state, and belong to no guest: the RSP
vector unit, ARM NEON and wasm SIMD128 all lower to the same set. The module
documentation of `lane.rs` is the contract: the semantics of every kind
(saturation, shift amounts of the lane width or more, out-of-range shuffle
indices giving 0, mask lanes of all ones or zeros), the lane widths each kind
accepts, and a table of the NEON, SSE2 to SSE4.1 and wasm SIMD128
instructions an emitter uses for each. `relift_ir::lane::eval` is the
reference the interpreter runs; `core/ir/tests/lane.rs` checks every kind and
width against an independent per-lane reference. An emitter that does not
lower them yet refuses the block through its existing unsupported path.

`core/emit` lowers each to a call of a `static inline` helper in the prelude
(`rl_lane_KINDn`, `RL_LANE_PERM1` for one source and `RL_LANE_PERM2` for
two) on `rl_e_vq` values. With clang the helpers are vector-extension
expressions, so the target flags of the build pick the NEON or SSE2 to
SSE4.1 instruction; defining `RL_LANE_SCALAR`, or a compiler without the
extensions (gcc), gives per-lane loops. `relift/tests/emit_lanes.rs` checks
every kind, width and immediate, byte permutations and random chains
against the interpreter on SSE2, SSSE3, SSE4.1, the scalar path, `-O0` and
registers in the context. `core/emit-wasm` does not lower them yet.
Text mnemonics are `lane.KIND.WIDTH` (`lane.qadds.16 v1, v2`,
`lane.shli.32 v1, 5`), `lane.perm` and `lane.perm2`; the binary form is
`relift_ir::BIN_FORMAT` 2.

## Checks run for this contract

All with clang 23.1.1 on Linux unless noted. Probe sources are scratch, not
committed.

| check | result |
|---|---|
| `clang -std=c11 -Wall -Wextra -Werror -fsyntax-only -x c-header` on `arm64/abi.h` (with `abi_core.h`), default target, `--target=aarch64-linux-gnu`, `--target=x86_64-pc-windows-gnu` | clean |
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
