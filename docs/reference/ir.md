# The IR

`relift-ir` (`core/ir`) is the intermediate representation that every CPU frontend lifts to and that the C emitter reads. It names no guest CPU. This page describes the model, the device extensions, the operations, the text and binary forms and the checks. The doc comments in `core/ir/src` are the source of truth; the excerpts below come from them.

The IR lives in memory between lifting and emitting: a frontend lifts guest instructions to blocks, the passes in `core/ir/src/opt.rs` rewrite them, and the emitter reads them. No file holds IR. The human-readable form is the text form (`core/ir/src/text.rs`), a printer and a parser that round-trip exactly; `relift-ir` and the tests use it. Every listing on this page is real output of `relift-ir` (A64), `relift-gbrom ir` (Game Boy SM83) or `relift-n64rom ir` (MIPS and RSP), embedded by docgen, so it follows the lifters. In a listing, `vN:iW` is value `N` of `W` bits, `get` and `set` read and write register slots, and the `;` lines are the tools' own headers: address, instruction word and, for A64, the encoding.

## An example

Two A64 instructions, `ldr x0, [x1, #16]` and `cbz x0, #8`, one block each:

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--dce 0xf9400820 0xb4000040" -->
```text
; 0x1000 f9400820 LDR_64_ldst_pos
block 0x1000 len 4 regs arm64
  v0:i64 = get x1
  v1:i64 = const 0x10
  v2:i64 = add v0, v1
  v3:i64 = load.le.plain v2
  set x0, v3
  exit jump 0x1004
; 0x1004 b4000040 CBZ_64_compbranch
block 0x1004 len 4 regs arm64
  v0:i64 = get x0
  v1:i64 = const 0x0
  v2:i1 = cmp.eq v0, v1
  v3:i1 = const 0x1
  v4:i1 = cmp.eq v2, v3
  exit jump select v4, 0x100c, 0x1008
```
<!-- /docgen:output -->

The first block reads `x1`, adds 16, loads 8 bytes little-endian with plain ordering and writes `x0`. The second compares `x0` to zero and exits through a select between the taken and the fall-through address. Each block ends in an exit that names where control goes next.

## Why an IR

relift lifts each guest instruction to this IR and emits C from it. The reasons for the extra layer, each checked against the code:

1. **N CPUs plus M outputs, not N times M.** `cpus/` holds six frontends (`arm32`, `arm64`, `mips`, `rsp`, `sm83`, `z80`). The outputs are C (`core/emit`) and WebAssembly (`core/emit-wasm`, which `relift` does not link yet). Each output reads blocks and a register file and names no CPU. The semantics of most instructions come from Sail models, lowered to IR once: each CPU's generator (`cpus/*/gen`) runs the shared Sail frontend in `core/sail`, which turns the semantics of one encoding into the body of a lifting template that builds IR. No CPU has a C generator of its own.
2. **The same lift drives discovery.** The shared Arm analyzer (`cpus/arm32/analyze`, under the GBA and NDS analyzers in `guests/gba/analyze` and `guests/nds/analyze`) and the Nintendo 64 analyzer (`guests/n64/analyze`) lift each instruction and run the lifted IR over abstract values: constants, partly known bits, a register plus an offset, table indexes. Discovery and emission therefore agree on what each instruction does, with no second CPU model and no byte patterns. Code that discovery misses stops the program with a named report (`CONTRIBUTING.md`), so the goal of zero interpreted guest instructions at run time depends on that agreement. The Game Boy analyzer (`guests/gb/analyze`) works from decoded SM83 instructions and does not use the IR.
3. **Per-instruction checking without a C compiler.** `interp::run` executes a block directly. `relift-check insn` and `relift-check forms <name>|all` run lifted A64 words on it against an oracle (Unicorn, QEMU user mode, QEMU `max`, or an Apple M2 over ssh) on random states, and `--mutate` breaks the first live operation of each block and expects a mismatch. For the pointer authentication forms the Sail model, evaluated concretely, is an oracle too. `relift-diff` (`core/diff`) compares a whole function in a live run with a reference engine.
4. **Optimizations on values the IR makes explicit.** The emitter drops a register write that no later instruction of the function can see, by backward liveness over the function (`core/emit/src/func.rs`; every register counts as live where the C stores the context: calls, traps, returns, indirect jumps, guards and sync points). Flags are 1-bit slots, so this removes dead flags without the C compiler having to prove a store to the context dead. `classify_io` turns an I/O access at a constant address into a plain load or store, or a device access with no range check; the SM83 and Z80 lifters apply it. `Tick` and `Sync` put cycle accounting in the IR, so the emitter charges the clock one way for every guest (`core/emit/src/time.rs`).
5. **The costs.** The IR is an extra layer of about 6,500 lines (`core/ir/src`). Some operations exist for one CPU's features: lanes (the RSP's vector unit; the A64 lifter writes NEON with extracts and concats), crypto and pointer authentication (A64) and half-precision floating point. The emitted C maps less directly onto each guest instruction than C written for one CPU would. A recompiler with one CPU and one output is simpler when it lifts straight to C; relift's case rests on points 1 to 4.

### When an op belongs in the IR

An operation is added to `Op` only when at least two CPUs need it, or when the optimizer, discovery or the emitter gains from seeing inside it. Anything that computes an address or a branch target counts under the second test. `Pac` is an op for that reason: pointer authentication produces values that are later used as addresses and branch targets, and as a pure op its result stays visible to the passes after it, where an intrinsic's result would be opaque. The operation is pure, so the interpreter and the emitter (`core/emit/src/value.rs`) both evaluate it.

Otherwise the operation is an `Op::Intrinsic`: a named call that the CPU crate and the runtime define (`Frontend::intrinsic`). The IR cannot see into an intrinsic, so every intrinsic counts as an effect, but the interpreter still runs it through the `Intrinsics` trait (`core/ir/src/interp.rs`), so an instruction lifted to an intrinsic stays checkable against an oracle.

Where an operation is needed, it models the primitive under the instruction, not the instruction. AES is the example. `CryptoOp` has the steps (`AesSubBytes`, `AesMixColumns` and their inverses), and A64 `AESE` is an `xor`, a byte permutation and `AesSubBytes` (see the crypto listing below, which keeps only the last), while `AESMC` is one `AesMixColumns` (`crypto.aes.mixcolumns` in the `relift-ir` lift of `aesmc`). An x86 `AESENC` is the same steps in another order, with a round-key `xor`, so it would reuse them and add no operation.

The known case the policy will be tested on is x87: 80-bit floating-point values and a register stack. `Fp` works on 16, 32 and 64 bits, and `Get` and `Set` name a slot by a constant index, so a stack addressed relative to a top-of-stack register needs indexed register access that the IR does not have.

## The model

- **Blocks.** A `Block` is the lifted form of one guest instruction, or a short run of them: straight-line SSA over bit-vector values of 1 to 128 bits. Value `n` is the result of instruction `n`, and an operand names an earlier instruction of the same block only. An `Inst` is an `Op` and its result width, which is `None` for an operation that gives no value (`Set`, stores, fences).
- **Exits.** A block ends with an `Exit`: where control goes (`Next`) and what kind of transfer it is (`Flow`). `Next` is a constant address, a select between two constant addresses on a one-bit condition, or a computed address of the block's address width. `Flow` is `Jump`, `Call` (with the return address), `Return` or `Trap` (with the frontend's code, and `next` is where execution resumes). The interpreter treats every flow as a jump; analysis uses the kind.
- **Register files.** A frontend declares its state as a `RegFile`: a named, ordered list of slots, each with a name and a width. `Get` reads the value a slot held when the block was entered. `Set` writes a slot, and all writes take effect when the block exits, so a later `Get` of the same slot still reads the entry value. The IR gives no slot a meaning.
- **Flags.** Condition flags are ordinary 1-bit slots. The frontend computes them with ordinary operations, so the IR has no flag-setting operations.
- **Memory.** `Load` and `Store` carry a size (the value width, whole bytes), an `Endian` (little or big) and an `Order` (plain, acquire, release, acquire-release, sequentially consistent, as in C11). Exclusive loads and stores pair through one monitor, and atomic read-modify-write and compare-and-swap are single operations.
- **Address width.** The frontend gives the address width when it creates the block (`Builder::new`). Every address operand has exactly that width, so 32-bit and big-endian guests need no change in the IR.
- **Verification.** `Block::verify` checks every width and operand reference and the exit. `Builder` checks each operation as it adds it and panics on a width error. A guard must come before every operation with an effect (a load is the one exception), and a sync must be the last instruction.
- **Guards.** `Guard(cond, code)` leaves the block before it has any effect when `cond` is 1, and the runtime runs the instruction on its slow path. Frontends use it for assumptions such as the floating-point control register being zero.

### The model, printed

Each concept above as relift prints it, in A64.

**Flags.** `adds w0, w1, #1` computes the sum, then builds `n`, `z`, `c` and `v` from ordinary operations on 1-bit values; the four `set` lines write the four 1-bit slots:

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--dce 0x31000420" -->
```text
; 0x1000 31000420 ADDS_32S_addsub_imm
block 0x1000 len 4 regs arm64
  v0:i64 = get x1
  v1:i32 = extract v0, 0
  v2:i128 = zext v1
  v3:i128 = const 0x1
  v4:i128 = add v2, v3
  v5:i128 = const 0x0
  v6:i128 = add v4, v5
  v7:i128 = sext v1
  v8:i128 = const 0x1
  v9:i128 = add v7, v8
  v10:i128 = const 0x0
  v11:i128 = add v9, v10
  v12:i32 = extract v6, 0
  v13:i1 = extract v12, 31
  v14:i32 = const 0x0
  v15:i1 = cmp.eq v12, v14
  v16:i1 = const 0x1
  v17:i1 = const 0x0
  v18:i1 = select v15, v16, v17
  v19:i128 = zext v12
  v20:i1 = cmp.eq v19, v6
  v21:i1 = const 0x0
  v22:i1 = const 0x1
  v23:i1 = select v20, v21, v22
  v24:i128 = sext v12
  v25:i1 = cmp.eq v24, v11
  v26:i1 = const 0x0
  v27:i1 = const 0x1
  v28:i1 = select v25, v26, v27
  v29:i2 = concat v13, v18
  v30:i3 = concat v29, v23
  v31:i4 = concat v30, v28
  v32:i1 = extract v31, 3
  v33:i1 = extract v31, 2
  v34:i1 = extract v31, 1
  v35:i1 = extract v31, 0
  v36:i64 = zext v12
  set x0, v36
  set n, v32
  set z, v33
  set c, v34
  set v, v35
  exit jump 0x1004
```
<!-- /docgen:output -->

**Memory ordering.** `ldar` and `stlr` put `acquire` and `release` in the `load` and `store`; the check before each is a guard on the low address bits (the code is the ASCII bytes `ALNG`):

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--dce 0xc8dffc20 0xc89ffc20" -->
```text
; 0x1000 c8dffc20 LDAR_LR64_ldstord
block 0x1000 len 4 regs arm64
  v0:i64 = get x1
  v1:i64 = const 0x0
  v2:i64 = const 0x7
  v3:i64 = and v0, v2
  v4:i1 = cmp.ne v3, v1
  guard v4, 0x414c4e47
  v6:i64 = load.le.acquire v0
  set x0, v6
  exit jump 0x1004
; 0x1004 c89ffc20 STLR_SL64_ldstord
block 0x1004 len 4 regs arm64
  v0:i64 = get x1
  v1:i64 = const 0x0
  v2:i64 = add v0, v1
  v3:i64 = get x0
  v4:i64 = const 0x0
  v5:i64 = const 0x7
  v6:i64 = and v2, v5
  v7:i1 = cmp.ne v6, v4
  guard v7, 0x414c4e47
  store.le.release v2, v3
  exit jump 0x1008
```
<!-- /docgen:output -->

**Exclusive pairs.** `ldaxr` is `load.excl` and arms the monitor. `stxr` is `store.excl`, which gives a bit; the lifter turns it into the status register, 0 when it stored and 1 when it did not:

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--dce 0xc85ffc20 0xc8027c20" -->
```text
; 0x1000 c85ffc20 LDAXR_LR64_ldstexclr
block 0x1000 len 4 regs arm64
  v0:i64 = get x1
  v1:i64 = const 0x0
  v2:i64 = const 0x7
  v3:i64 = and v0, v2
  v4:i1 = cmp.ne v3, v1
  guard v4, 0x414c4e47
  v6:i64 = load.excl.le.acquire v0
  set x0, v6
  exit jump 0x1004
; 0x1004 c8027c20 STXR_SR64_ldstexclr
block 0x1004 len 4 regs arm64
  v0:i64 = get x1
  v1:i64 = get x0
  v2:i64 = const 0x0
  v3:i64 = const 0x7
  v4:i64 = and v0, v3
  v5:i1 = cmp.ne v4, v2
  guard v5, 0x414c4e47
  v7:i1 = store.excl.le.plain v0, v1
  v8:i1 = const 0x0
  v9:i1 = const 0x1
  v10:i1 = select v7, v8, v9
  v11:i32 = zext v10
  v12:i64 = zext v11
  set x2, v12
  exit jump 0x1008
```
<!-- /docgen:output -->

**Atomics.** With `FEAT_LSE`, `ldadd` is one `atomic.add` that gives the old value, and `cas` is one `cas`:

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--features cortex-a57,FEAT_LSE --dce 0xf8200041 0xc8a07c41" -->
```text
; 0x1000 f8200041 LDADD_64_memop
block 0x1000 len 4 regs arm64
  v0:i64 = get x0
  v1:i64 = get x2
  v2:i64 = const 0x0
  v3:i64 = const 0x7
  v4:i64 = and v1, v3
  v5:i1 = cmp.ne v4, v2
  guard v5, 0x414c4e47
  v7:i64 = atomic.add.le.plain v1, v0
  set x1, v7
  exit jump 0x1004
; 0x1004 c8a07c41 CAS_C64_comswap
block 0x1004 len 4 regs arm64
  v0:i64 = get x0
  v1:i64 = get x1
  v2:i64 = get x2
  v3:i64 = const 0x0
  v4:i64 = const 0x7
  v5:i64 = and v2, v4
  v6:i1 = cmp.ne v5, v3
  guard v6, 0x414c4e47
  v8:i64 = cas.le.plain v2, v0, v1
  set x0, v8
  exit jump 0x1008
```
<!-- /docgen:output -->

**Computed exits.** `br` ends in `jump dynamic` on a value and `ret` in `return dynamic`. The interpreter treats both as jumps; analysis uses the kind:

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--dce 0xd61f0200 0xd65f03c0" -->
```text
; 0x1000 d61f0200 BR_64_branch_reg
block 0x1000 len 4 regs arm64
  v0:i64 = get x16
  exit jump dynamic v0
; 0x1004 d65f03c0 RET_64R_branch_reg
block 0x1004 len 4 regs arm64
  v0:i64 = get x30
  exit return dynamic v0
```
<!-- /docgen:output -->

**Call exits and traps.** `bl` writes the return address to `x30` and exits with `call`, which names the return address and the target. `svc` exits with `trap`, whose code is the immediate and whose `next` is where execution resumes:

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--dce 0x94000010 0xd4001001" -->
```text
; 0x1000 94000010 BL_only_branch_imm
block 0x1000 len 4 regs arm64
  v0:i64 = const 0x1004
  set x30, v0
  exit call 0x1004 0x1040
; 0x1004 d4001001 SVC_EX_exception
block 0x1004 len 4 regs arm64
  exit trap 0x80 0x1008
```
<!-- /docgen:output -->

**Guards.** The A64 floating-point templates assume `fpcr` is zero. `fcvtzs x0, d1` starts with a guard on `fpcr != 0` that leaves the block before any effect, so the runtime runs the instruction on its slow path (the code is the ASCII bytes `FPCR`):

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--dce 0x9e780020" -->
```text
; 0x1000 9e780020 FCVTZS_64D_float2int
block 0x1000 len 4 regs arm64
  v0:i32 = get fpcr
  v1:i32 = const 0x0
  v2:i1 = cmp.ne v0, v1
  guard v2, 0x46504352
  v4:i128 = get v1
  v5:i64 = extract v4, 0
  v6:i64 = fp.tosint.z v5
  set x0, v6
  exit jump 0x1004
```
<!-- /docgen:output -->

## The device extensions

Guests that drive devices through memory-mapped registers add four things. Guests without devices never emit them. The emitter and runtime contract is `docs/abi.md` section 13.

- **Ticks.** `Tick(n)` advances the guest clock by `n` cycles, in the unit the CPU frontend defines. `TickIf(cond, n)` does so only when `cond` is 1, for a taken branch or a data-dependent step. The clock moves at the position of the operation: an I/O access or sync after it sees the new time, one before it does not.
- **I/O accesses.** `IoLoad` and `IoStore` read and write where a device register may live. The guest's `IoMap` is an ordered list of address ranges, each with a class for reads and one for writes (`Memory` or `Device`), and a default class; the first range holding a byte gives its class. An access whose bytes are all memory reads or writes guest memory directly, and any other goes to the device hook, which first brings the devices up to the current cycle. An access that straddles a boundary goes to the hook as a whole. The access kind (`IoKind`) is `Any` (decided at run time by the map) or `Device` (always the hook). A device read is never removed, even when its value is unused.
- **`classify_io`.** The pass `classify_io(block, map)` rewrites an `Any` access at a constant address: to a plain `Load` or `Store` when every byte is memory, and to kind `Device` otherwise, so the emitter needs no range check for it.
- **Sync points.** `Sync` makes the exit a sync point. After the exit's register writes are committed and its target is known, the runtime may run the devices up to the current cycle and deliver an interrupt. A taken interrupt continues at the handler entry, and the exit's target becomes the pc the handler returns to. A block has at most one `Sync`, and it is the last instruction.
- **Code banks.** `Block::bank` names which mapping of the guest bytes at `pc` was lifted, for code that changes at one address (a cartridge ROM bank, an overlay, code copied to RAM). `(pc, bank)` is the block's identity. It is `None` for guests without banking. Exit targets are plain addresses; the mapping at run time, or an analysis that tracks bank-switch writes, decides which bank one reaches.

### The device extensions, printed

These come from the Game Boy (SM83), through its frontend and its real address map, where every address reaches the device library. `relift-gbrom ir` takes instruction bytes in hex. The first line of each block names the register file and, when it is not 64 bits, the address width (`addr i16`).

**Ticks, I/O loads and sync points.** `ldh a, (0x40)` loads from the I/O page and `ldh (0x41), a` stores to it. The clock moves at the position of the `tick`: the cycles before the access come before it and the rest after. The accesses have kind `dev` because the addresses are constants. Each block ends with `sync`:

<!-- docgen:output crate=relift-gb-tools bin=relift-gbrom args="ir f0 40 e0 41" -->
```text
; 0x150 f040 Ld
block 0x150 len 2 regs sm83 addr i16
  v0:i16 = const 0xff40
  tick 8
  v2:i8 = io.load.le.dev v0
  set a, v2
  tick 4
  sync
  exit jump 0x152
; 0x152 e041 Ld
block 0x152 len 2 regs sm83 addr i16
  v0:i8 = get a
  v1:i16 = const 0xff41
  tick 8
  io.store.le.dev v1, v0
  tick 4
  sync
  exit jump 0x154
```
<!-- /docgen:output -->

**Accesses of kind `any`.** When the address is computed, as in `ld a, (hl)`, the access has kind `any` and the guest's map decides at run time:

<!-- docgen:output crate=relift-gb-tools bin=relift-gbrom args="ir 7e" -->
```text
; 0x150 7e Ld
block 0x150 len 1 regs sm83 addr i16
  v0:i8 = get h
  v1:i8 = get l
  v2:i16 = concat v0, v1
  tick 4
  v4:i8 = io.load.le.any v2
  set a, v4
  tick 4
  sync
  exit jump 0x151
```
<!-- /docgen:output -->

**Conditional ticks.** `jr nz, +5` costs 8 cycles, and 4 more only when the branch is taken, so the extra cost is a `tick.if` on the branch condition, which the exit's select also uses:

<!-- docgen:output crate=relift-gb-tools bin=relift-gbrom args="ir 20 05" -->
```text
; 0x150 2005 Jr
block 0x150 len 2 regs sm83 addr i16
  tick 8
  v1:i1 = get fz
  v2:i1 = not v1
  tick.if v2, 4
  sync
  exit jump select v2, 0x157, 0x152
```
<!-- /docgen:output -->

**Interrupt enable and halt.** The Game Boy guest runs `ei` and `halt` (and `di` and `stop`) on the device library's own CPU, so they lift to an empty block that exits with a `trap`; the code is the opcode above the resume address:

<!-- docgen:output crate=relift-gb-tools bin=relift-gbrom args="ir fb 76" -->
```text
; 0x150 fb Ei
block 0x150 len 1 regs sm83 addr i16
  exit trap 0xfb0151 0x151
; 0x151 76 Halt
block 0x151 len 1 regs sm83 addr i16
  exit trap 0x760152 0x152
```
<!-- /docgen:output -->

**Code banks.** A block of code in a switchable ROM bank carries its bank in the header, and `(pc, bank)` is its identity. This is `jp 0x4010` at `0x4000` in bank 3:

<!-- docgen:output crate=relift-gb-tools bin=relift-gbrom args="ir --bank 3 --pc 0x4000 c3 10 40" -->
```text
; 0x4000 c31040 Jp bank 3
block 0x4000 len 3 regs sm83 addr i16 bank 0x3
  tick 16
  sync
  exit jump 0x4010
```
<!-- /docgen:output -->

## Operations

Every operation is a variant of `Op`. Operands are earlier values. The excerpt is the enum as written, with the doc comment of each variant:

<!-- docgen:code file=core/ir/src/ops.rs symbol=Op raw=true -->
```rust
/// One IR operation. Operands are earlier [`Value`]s of the same block.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum Op {
    /// A constant of the instruction's width.
    Const(Konst),
    /// The value a slot held when the block was entered.
    Get(Reg),
    /// Writes a slot. Writes take effect when the block exits, so a later
    /// `Get` of the same slot still reads the entry value.
    Set(Reg, Value),
    /// A one-operand operation.
    Un(UnOp, Value),
    /// A two-operand operation.
    Bin(BinOp, Value, Value),
    /// A comparison, giving one bit.
    Cmp(CmpOp, Value, Value),
    /// `if cond { a } else { b }`; `cond` is one bit.
    Select(Value, Value, Value),
    /// Widens to the instruction's width.
    Ext(ExtKind, Value),
    /// The instruction's width of bits starting at bit `lo`.
    Extract(Value, u8),
    /// `hi` above `lo`; the width is the sum.
    Concat(Value, Value),
    /// Reads the instruction's width (whole bytes) at `addr`.
    Load {
        /// The address, of the block's address width.
        addr: Value,
        /// The byte order.
        endian: Endian,
        /// The ordering.
        order: Order,
    },
    /// Writes `value` (whole bytes) at `addr`.
    Store {
        /// The address, of the block's address width.
        addr: Value,
        /// The value written.
        value: Value,
        /// The byte order.
        endian: Endian,
        /// The ordering.
        order: Order,
    },
    /// A load that also arms the exclusive monitor for its address and size.
    LoadExclusive {
        /// The address.
        addr: Value,
        /// The byte order.
        endian: Endian,
        /// The ordering.
        order: Order,
    },
    /// A store that happens only when the monitor is armed for exactly this
    /// address and size. Gives one bit, 1 when it stored. The monitor is
    /// cleared either way.
    StoreExclusive {
        /// The address.
        addr: Value,
        /// The value written.
        value: Value,
        /// The byte order.
        endian: Endian,
        /// The ordering.
        order: Order,
    },
    /// Clears the exclusive monitor.
    ClearExclusive,
    /// A memory fence.
    Fence(Order),
    /// A floating-point operation of the instruction's width (16, 32 or 64).
    Fp(FpOp, FpArgs),
    /// Leaves the block before it has any effect when `cond` (one bit) is 1:
    /// the block's assumption does not hold, and the runtime runs the
    /// instruction on its slow path, the one named by `code`. A guard comes
    /// before every operation with an effect, so leaving discards nothing.
    Guard(Value, u32),
    /// A call to a named intrinsic the frontend and runtime define, giving
    /// the instruction's width, or nothing when the instruction has none.
    Intrinsic(Box<Call>),
    /// Advances the guest clock by this many cycles, in the unit the CPU
    /// frontend defines (Game Boy: T-cycles). The clock moves at this
    /// position: an I/O access or sync point after it sees the time
    /// including it, one before it does not. A frontend gives each guest
    /// instruction its cost here and may split it around the instruction's
    /// I/O accesses so that each access happens at its own cycle. Guests
    /// without a clock never emit it. See docs/abi.md section 13.
    Tick(u32),
    /// Advances the guest clock by this many cycles when `cond` (one bit) is 1 and
    /// does nothing otherwise: the extra cost of a taken branch, or of one
    /// data-dependent step.
    TickIf(Value, u32),
    /// Reads the instruction's width (whole bytes) at `addr`, where a device
    /// register may live: an address the guest's [`crate::IoMap`] calls
    /// memory reads like [`Op::Load`] with plain ordering, any other goes to
    /// the device hook, which first brings the devices up to the current
    /// cycle. A device read may change device state, so it is never
    /// removed even when its value is unused.
    IoLoad {
        /// The address, of the block's address width.
        addr: Value,
        /// The byte order.
        endian: Endian,
        /// What the access is known to reach.
        kind: IoKind,
    },
    /// Writes `value` (whole bytes) at `addr`, where a device register may
    /// live; routed like [`Op::IoLoad`], with the map's write classes.
    IoStore {
        /// The address, of the block's address width.
        addr: Value,
        /// The value written.
        value: Value,
        /// The byte order.
        endian: Endian,
        /// What the access is known to reach.
        kind: IoKind,
    },
    /// Makes the block's exit a sync point: once the exit's register writes
    /// are committed and its target is known, the runtime may run the
    /// devices up to the current cycle and deliver an interrupt. A taken
    /// interrupt redirects control: execution continues at the handler
    /// entry the guest's interrupt logic gives, and the exit's target
    /// becomes the pc the handler returns to; the entry's guest-visible
    /// effects (a pushed return address, a cleared interrupt enable, the
    /// entry's cycles) are the guest runtime's. When no interrupt is taken
    /// the sync has no guest-visible effect besides device time. At most one
    /// per block, and it is the block's last instruction.
    Sync,
    /// A cipher step of the width [`CryptoOp::width`].
    Crypto(CryptoOp, Value),
    /// The 64-bit pointer authentication code of `data` under the 64-bit
    /// `modifier` and the key `key0:key1` (Arm `ComputePAC` with QARMA5,
    /// see [`crate::interp::eval_pac`]). Pure; all operands are 64 bits.
    Pac {
        /// The pointer or value signed.
        data: Value,
        /// The tweak.
        modifier: Value,
        /// The key's high half.
        key0: Value,
        /// The key's low half.
        key1: Value,
    },
    /// One single-copy-atomic read-modify-write of the instruction's width
    /// (whole bytes) at `addr`: memory becomes `op(old, value)`; gives `old`.
    AtomicRmw {
        /// The update.
        op: AtomicOp,
        /// The address.
        addr: Value,
        /// The operand.
        value: Value,
        /// The byte order.
        endian: Endian,
        /// The ordering.
        order: Order,
    },
    /// One single-copy-atomic compare-and-swap of the instruction's width:
    /// memory becomes `value` when it equals `expected`; gives the old
    /// value either way.
    CompareSwap {
        /// The address.
        addr: Value,
        /// The value compared.
        expected: Value,
        /// The value stored on a match.
        value: Value,
        /// The byte order.
        endian: Endian,
        /// The ordering.
        order: Order,
    },
    /// A per-lane operation on 128-bit values, see [`crate::lane`]. The
    /// operands are one to three values; no effect.
    Lane(LaneOp, LaneArgs),
    /// A constant byte permutation of one or two 128-bit values giving 128
    /// bits, see [`Permute`]. No effect.
    Permute(Box<Permute>),
}
```
<!-- /docgen:code -->

The sub-enums that the variants take, by family:

### Integer

`Un` takes a `UnOp` and gives the operand's width. `Bin` takes a `BinOp`; arithmetic and bitwise operations take two operands of one width, modulo 2^width, and shifts and rotates take an amount of any width read as unsigned. Division by zero is defined, not a trap. `Cmp` takes a `CmpOp` and gives one bit.

<!-- docgen:code file=core/ir/src/ops.rs symbol=UnOp raw=true -->
```rust
/// One-operand integer operations. The result has the operand's width.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum UnOp {
    /// Bitwise complement.
    Not,
    /// Two's complement negation.
    Neg,
    /// Count of leading zero bits; the width for zero.
    Clz,
    /// Count of trailing zero bits; the width for zero.
    Ctz,
    /// Count of one bits.
    Popcount,
    /// Bit order reversed.
    BitReverse,
    /// Byte order reversed; the width must be a multiple of 8.
    ByteSwap,
}
```
<!-- /docgen:code -->

<!-- docgen:code file=core/ir/src/ops.rs symbol=BinOp raw=true -->
```rust
/// Two-operand integer operations.
///
/// Arithmetic and bitwise operations take two operands of one width and give
/// that width, modulo 2^width. Shifts and rotates take an amount of any width,
/// read as unsigned, and give the first operand's width.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum BinOp {
    /// Wrapping sum.
    Add,
    /// Wrapping difference.
    Sub,
    /// Low half of the product.
    Mul,
    /// Unsigned quotient, rounded toward zero; 0 when dividing by zero.
    UDiv,
    /// Signed quotient, rounded toward zero; 0 when dividing by zero and the
    /// minimum value for the minimum value divided by -1.
    SDiv,
    /// Unsigned remainder; the dividend when dividing by zero.
    URem,
    /// Signed remainder with the dividend's sign; the dividend when dividing
    /// by zero and 0 for the minimum value divided by -1.
    SRem,
    /// Bitwise and.
    And,
    /// Bitwise or.
    Or,
    /// Bitwise exclusive or.
    Xor,
    /// Left shift; 0 once the amount reaches the width.
    Shl,
    /// Logical right shift; 0 once the amount reaches the width.
    LShr,
    /// Arithmetic right shift; all sign bits once the amount reaches the width.
    AShr,
    /// Right rotate by the amount modulo the width.
    RotR,
}
```
<!-- /docgen:code -->

<!-- docgen:code file=core/ir/src/ops.rs symbol=CmpOp raw=true -->
```rust
/// Comparisons of two operands of one width, giving one bit.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum CmpOp {
    /// Equal.
    Eq,
    /// Not equal.
    Ne,
    /// Unsigned less than.
    Ult,
    /// Unsigned less than or equal.
    Ule,
    /// Signed less than.
    Slt,
    /// Signed less than or equal.
    Sle,
}
```
<!-- /docgen:code -->

`Const`, `Select`, `Ext` (zero or sign, to the instruction's width), `Extract` (the instruction's width of bits from bit `lo`) and `Concat` (high above low, the width is the sum) complete the integer family.

**Integer arithmetic and shifts.** `mul` is a 128-bit multiply of zero-extended operands whose low 64 bits an `extract` takes. `udiv` zero-extends both operands to 128 bits so that one signed `sdiv` gives the unsigned quotient, and a select on a zero divisor gives 0 (division by zero is defined). `lsl` masks the amount to 6 bits and shifts:

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--dce 0x9b027c20 0x9ac20820 0x9ac22020" -->
```text
; 0x1000 9b027c20 MADD_64A_dp_3src
block 0x1000 len 4 regs arm64
  v0:i64 = get x1
  v1:i64 = get x2
  v2:i64 = const 0x0
  v3:i128 = zext v2
  v4:i128 = zext v0
  v5:i128 = zext v1
  v6:i128 = mul v4, v5
  v7:i128 = add v3, v6
  v8:i64 = extract v7, 0
  set x0, v8
  exit jump 0x1004
; 0x1004 9ac20820 UDIV_64_dp_2src
block 0x1004 len 4 regs arm64
  v0:i64 = get x1
  v1:i64 = get x2
  v2:i64 = const 0x0
  v3:i1 = cmp.eq v1, v2
  v4:i128 = zext v0
  v5:i128 = zext v1
  v6:i128 = sdiv v4, v5
  v7:i128 = const 0x0
  v8:i128 = select v3, v7, v6
  v9:i64 = extract v8, 0
  set x0, v9
  exit jump 0x1008
; 0x1008 9ac22020 LSLV_64_dp_2src
block 0x1008 len 4 regs arm64
  v0:i64 = get x2
  v1:i128 = zext v0
  v2:i128 = const 0x3f
  v3:i128 = and v1, v2
  v4:i64 = get x1
  v5:i64 = shl v4, v3
  set x0, v5
  exit jump 0x100c
```
<!-- /docgen:output -->

**Compare and select.** `csel x0, x1, x2, eq` compares the `z` flag with 1 and selects between two registers:

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--dce 0x9a820020" -->
```text
; 0x1000 9a820020 CSEL_64_condsel
block 0x1000 len 4 regs arm64
  v0:i1 = get z
  v1:i1 = const 0x1
  v2:i1 = cmp.eq v0, v1
  v3:i64 = get x1
  v4:i64 = get x2
  v5:i64 = select v2, v3, v4
  set x0, v5
  exit jump 0x1004
```
<!-- /docgen:output -->

**Extend, extract and concat.** `uxth w0, w1` extracts the low 16 bits and zero-extends them. `ubfx x0, x1, #4, #8` is built from shifts, masks and `or`. `extr x0, x1, x1, #8` (a rotate right) concatenates two values and shifts the pair:

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--dce 0x53003c20 0xd3442c20 0x93c12020" -->
```text
; 0x1000 53003c20 UBFM_32M_bitfield
block 0x1000 len 4 regs arm64
  v0:i64 = get x1
  v1:i32 = extract v0, 0
  v2:i32 = const 0xffff
  v3:i32 = and v1, v2
  v4:i32 = const 0x0
  v5:i32 = or v4, v3
  v6:i32 = const 0xffff
  v7:i32 = and v5, v6
  v8:i32 = const 0x0
  v9:i32 = or v8, v7
  v10:i64 = zext v9
  set x0, v10
  exit jump 0x1004
; 0x1004 d3442c20 UBFM_64M_bitfield
block 0x1004 len 4 regs arm64
  v0:i64 = get x1
  v1:i128 = const 0x4
  v2:i64 = lshr v0, v1
  v3:i128 = const 0x3c
  v4:i64 = shl v0, v3
  v5:i64 = or v2, v4
  v6:i64 = const 0xf0000000000000ff
  v7:i64 = and v5, v6
  v8:i64 = const 0x0
  v9:i64 = or v8, v7
  v10:i64 = const 0xff
  v11:i64 = and v9, v10
  v12:i64 = const 0x0
  v13:i64 = or v12, v11
  set x0, v13
  exit jump 0x1008
; 0x1008 93c12020 EXTR_64_extract
block 0x1008 len 4 regs arm64
  v0:i64 = get x1
  v1:i64 = get x1
  v2:i128 = concat v0, v1
  v3:i128 = const 0x8
  v4:i128 = lshr v2, v3
  v5:i64 = extract v4, 0
  set x0, v5
  exit jump 0x100c
```
<!-- /docgen:output -->

### Memory, atomics and exclusives

`Load`, `Store`, `LoadExclusive`, `StoreExclusive` (gives 1 when it stored; the monitor is cleared either way), `ClearExclusive`, `Fence`, `AtomicRmw` and `CompareSwap` (gives the old value either way). Printed examples of each are under [The model, printed](#the-model-printed).

<!-- docgen:code file=core/ir/src/ops.rs symbol=AtomicOp raw=true -->
```rust
/// The update of an [`Op::AtomicRmw`]: the new memory value from the old
/// one and the operand.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum AtomicOp {
    /// `old + v`.
    Add,
    /// `old & !v`.
    Clear,
    /// `old ^ v`.
    Xor,
    /// `old | v`.
    Or,
    /// The signed maximum.
    SMax,
    /// The signed minimum.
    SMin,
    /// The unsigned maximum.
    UMax,
    /// The unsigned minimum.
    UMin,
    /// `v`.
    Swap,
}
```
<!-- /docgen:code -->

### Floating point

`Fp` operates on IEEE 754 binary16, binary32 or binary64 bit patterns. Each `FpOp` is one primitive with one meaning: round to nearest even, subnormals kept, no exception flags. A NaN result may be any NaN. The conversions to integers saturate and give 0 for a NaN, and an emitter must not use a raw x86 `cvttsd2si`, which gives the integer indefinite value. Guest semantics (NaN propagation, the default NaN, maximum and minimum rules) are built by the frontend from ordinary operations around these primitives.

<!-- docgen:code file=core/ir/src/ops.rs symbol=FpOp raw=true -->
```rust
/// Floating-point operations on IEEE 754 binary16, binary32 or binary64 bit
/// patterns.
///
/// Each is an IEEE primitive with one fixed meaning, so an emitter can map it
/// to a host instruction or a libm call: round to nearest even, subnormals
/// kept (no flush), no exception flags. A NaN result may be any NaN; the
/// conversions are exact about their edge cases instead: `ToSInt` and
/// `ToUInt` saturate and give 0 for a NaN (an emitter must not use a raw
/// x86 `cvttsd2si`, which gives the integer indefinite value).
///
/// Guest semantics are built by the frontend around these primitives, from
/// ordinary IR operations. For A64 (`cpus/arm64/src/lift/sail/rt.rs`) that is
/// NaN propagation and quieting, the default NaN, FMAX/FMIN/FMAXNM/FMINNM
/// with signed zeros, the FMA invalid case and FCVT NaN payloads.
///
/// The A64 templates are generated for FPCR zero (round to nearest, no FZ,
/// FZ16, DN, AH or AHP). Every block whose semantics assume that starts with
/// an [`Op::Guard`] on `FPCR != 0`, so with any other FPCR it leaves before
/// any effect for the runtime's slow path (`runtime/cpus/arm64/fp.c`); an
/// emitter may hoist that check to function entry and after each write of
/// FPCR. Exception flags are not values in the IR: a frontend that needs
/// them maps each operation to a host operation that raises the host's
/// sticky flags (the A64 frontend does so for programs that read FPSR, see
/// `docs/abi.md` section 10.1).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FpOp {
    /// `a + b`.
    Add,
    /// `a - b`.
    Sub,
    /// `a * b`.
    Mul,
    /// `a / b`.
    Div,
    /// The square root of `a`.
    Sqrt,
    /// `a * b + c` with one rounding.
    MulAdd,
    /// `-a`, a sign flip.
    Neg,
    /// `|a|`, a sign clear.
    Abs,
    /// `a` rounded to an integral value in the same format; NaN stays NaN.
    RoundInt(FpRound),
    /// `a` rounded to a signed integer, saturating; NaN gives 0.
    ToSInt(FpRound),
    /// `a` rounded to an unsigned integer, saturating; NaN gives 0.
    ToUInt(FpRound),
    /// The signed integer `a`, rounded to nearest even.
    FromSInt,
    /// The unsigned integer `a`, rounded to nearest even.
    FromUInt,
    /// `a` in the other format, rounded to nearest even.
    Convert,
}
```
<!-- /docgen:code -->

<!-- docgen:code file=core/ir/src/ops.rs symbol=FpRound raw=true -->
```rust
/// A rounding direction for [`FpOp`] conversions to integral values.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FpRound {
    /// To nearest, ties to even.
    TiesEven,
    /// To nearest, ties away from zero.
    TiesAway,
    /// Toward minus infinity.
    Floor,
    /// Toward plus infinity.
    Ceil,
    /// Toward zero.
    Zero,
}
```
<!-- /docgen:code -->

**Floating point.** `fadd d0, d1, d2` is one `fp.add`; the lifter builds the Arm NaN rules around it from selects and masks, which `--grep` leaves out here. `fcvtzu w0, s1` is one `fp.touint.z`; the saturation and the 0 for a NaN are in the meaning of the operation, so nothing more is printed:

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--dce --grep fp. 0x1e622820 0x1e390020" -->
```text
; 0x1000 1e622820 FADD_D_floatdp2
block 0x1000 len 4 regs arm64
  v8:i64 = fp.add v5, v7
  exit jump 0x1004
; 0x1004 1e390020 FCVTZU_32S_float2int
block 0x1004 len 4 regs arm64
  v6:i32 = fp.touint.z v5
  exit jump 0x1008
```
<!-- /docgen:output -->

**Half precision.** With `FEAT_FP16`, `fadd h0, h1, h2` is an `fp.add` on 16-bit values (again only that line). `fneg h0, h1` flips the sign bit with an extract and a concat:

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--features cortex-a57,FEAT_FP16 --dce --grep fp. 0x1ee22820" -->
```text
; 0x1000 1ee22820 FADD_H_floatdp2
block 0x1000 len 4 regs arm64
  v10:i16 = fp.add v7, v9
  exit jump 0x1004
```
<!-- /docgen:output -->

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--features cortex-a57,FEAT_FP16 --dce 0x1ee14020" -->
```text
; 0x1000 1ee14020 FNEG_H_floatdp1
block 0x1000 len 4 regs arm64
  v0:i128 = get v1
  v1:i16 = extract v0, 0
  v2:i1 = extract v1, 15
  v3:i1 = not v2
  v4:i15 = extract v1, 0
  v5:i16 = concat v3, v4
  v6:i112 = const 0x0
  v7:i128 = concat v6, v5
  set v0, v7
  exit jump 0x1004
```
<!-- /docgen:output -->

### Lanes and permutes

`Lane` runs a `LaneOp` (a kind, a lane width of 8, 16, 32 or 64 bits, and an immediate) on 128-bit values. Lane 0 holds the least significant bits. The verifier rejects a lane width a kind does not support. The module doc of `core/ir/src/lane.rs` holds the semantics of each kind and the contract an emitter follows on each host:

<!-- docgen:doc crate=relift-ir name=lane -->
Per-lane operations on 128-bit values.

A 128-bit value is read as a vector of equal lanes of 8, 16, 32 or 64
bits (`LaneWidth`). **Lane 0 holds the least significant bits** of the
value, lane `n - 1` the most significant, so the value is what a
little-endian load of the lanes in memory order gives. Byte `i` of a value
is bits `8i .. 8i + 8`.

The operations are generic: nothing here belongs to one guest. They are
chosen so that each has a direct or a two-instruction form in ARM NEON,
x86 SSE2 to SSE4.1 (SSSE3 for `Permute`) and WebAssembly SIMD128; the
table below is the contract for an emitter. All are pure.

### Semantics

Every operation reads its operands and writes its result lane by lane
unless it says otherwise. "Mask" lanes are all ones (true) or all zeros
(false). `lw` is the lane width in bits.

| Kind | Operands | Result lane | Lane widths |
|------|----------|-------------|-------------|
| `Add`, `Sub` | `a`, `b` | wrapping sum, difference | 8 16 32 64 |
| `AddSatS`, `SubSatS` | `a`, `b` | signed saturating | 8 16 |
| `AddSatU`, `SubSatU` | `a`, `b` | unsigned saturating (difference clamps at 0) | 8 16 |
| `MulLo` | `a`, `b` | low `lw` bits of the product | 16 32 |
| `MulHiS`, `MulHiU`, `MulHiSU` | `a`, `b` | high `lw` bits of the `2 lw`-bit product; signed x signed, unsigned x unsigned, signed `a` x unsigned `b` | 16 32 |
| `And`, `Or`, `Xor`, `AndNot`, `Not` | `a`, `b` | `a & b`, `a \| b`, `a ^ b`, `a & !b`, `!a` (one operand) | bitwise, see below |
| `MinS`, `MinU`, `MaxS`, `MaxU` | `a`, `b` | signed or unsigned minimum, maximum | 8 16 32 |
| `Abs` | `a` | wrapping absolute value (the minimum stays) | 8 16 32 |
| `Neg` | `a` | wrapping negation | 8 16 32 64 |
| `Eq`, `Ne` | `a`, `b` | mask | 8 16 32 64 |
| `Slt`, `Sle`, `Ult`, `Ule` | `a`, `b` | mask, signed or unsigned `<`, `<=` | 8 16 32 |
| `ShlImm`, `LShrImm`, `AShrImm` | `a`, immediate `0 .. lw` | shift left, logical right, arithmetic right | 8 16 32 64 |
| `Shl`, `LShr`, `AShr` | `a`, `b` | shift by the unsigned lane of `b`; an amount of `lw` or more gives 0 (`AShr`: the sign fill) | 8 16 32 64 |
| `Select` | `m`, `a`, `b` | bitwise `m ? a : b`: `(m & a) \| (!m & b)` | bitwise |
| `WidenLoS`, `WidenLoU` | `a` | lane `i` of `a`, sign or zero extended to `2 lw` | 8 16 32 |
| `WidenHiS`, `WidenHiU` | `a` | lane `n/2 + i` of `a` (the upper half), extended | 8 16 32 |
| `NarrowTrunc` | `a`, `b` | the low `lw/2` bits of each source lane; `a` fills the low half of the result, `b` the high half | 16 32 64 |
| `NarrowSS` | `a`, `b` | signed source clamped to the signed `lw/2` range | 16 32 |
| `NarrowUU` | `a`, `b` | unsigned source clamped to the unsigned `lw/2` range | 16 32 |
| `NarrowSU` | `a`, `b` | signed source clamped to the unsigned `lw/2` range (negative gives 0) | 16 32 |
| `MulWideLo*`, `MulWideHi*` | `a`, `b` | full `2 lw`-bit product of lane `i` (low half) or `n/2 + i` (high half); suffix `SS`, `UU`, `SU` as for `MulHi` | 8 16 32 |
| `Broadcast(i)` | `a` | every lane is lane `i` of `a` | 8 16 32 64 |
| `Extract(i)` | `a` | **scalar** of `lw` bits, zero extended from lane `i` | 8 16 32 64 |
| `Insert(i)` | `a`, scalar `s` of `lw` bits | `a` with lane `i` replaced by `s` | 8 16 32 64 |
| `Splat` | scalar `s` of `lw` bits | every lane is `s` | 8 16 32 64 |

The bitwise kinds (and `Select`) do not depend on the lane width; they
carry `LaneWidth::L64` and the verifier requires it, so that equal
operations compare equal.

The lane-width columns are the widths that map to a single instruction or
a short fixed sequence on all three targets; the verifier rejects the
rest, and a frontend composes them from the others.

### Host mapping

Direct means one instruction; "2" and "3" count instructions on the
worst target. Widths not listed for a target use the same pattern.

| Kind | NEON | SSE2 to SSE4.1 | wasm SIMD128 |
|------|------|----------------|--------------|
| `Add`, `Sub`, `Neg` | `add`, `sub`, `neg` | `padd{b,w,d,q}`, `psub*`, `psub*` from zero | `iNxM.add`, `.sub`, `.neg` |
| `AddSat*`, `SubSat*` | `sqadd uqadd sqsub uqsub` | `padds{b,w}`, `paddus*`, `psubs*`, `psubus*` | `iNxM.add_sat_{s,u}`, `.sub_sat_{s,u}` |
| `MulLo` | `mul` | `pmullw`; 32: `pmulld` (SSE4.1) | `i16x8.mul`, `i32x4.mul` |
| `MulHiS`, `MulHiU` (16) | `smull`+`smull2`, `uzp2` (2) | `pmulhw`, `pmulhuw` | `i32x4.extmul_{low,high}_i16x8_s`+shuffle (3) |
| `MulHiSU` (16) | `sshll`, `ushll` of both halves, `mul`, `shrn` (about 5) | `pmulhuw(a, b) - (b & psraw(a, 15))` (4) | extend both, `mul`, shift, narrow (about 5) |
| `And`, `Or`, `Xor`, `Not` | `and orr eor mvn` | `pand por pxor`, `pxor` with ones | `v128.and or xor not` |
| `AndNot` | `bic` | `pandn` (operands swapped) | `v128.andnot` |
| `Min*`, `Max*` | `smin umin smax umax` | `pmin{sb,sw,sd,ub,uw,ud}` (SSE4.1; SSE2 has `pminsw`, `pminub`, `pmaxsw`, `pmaxub` only, others 2 to 3) | `iNxM.min_{s,u}`, `.max_{s,u}` |
| `Abs` | `abs` | `pabs{b,w,d}` (SSSE3) | `iNxM.abs` |
| `Eq` | `cmeq` | `pcmpeq{b,w,d}`, 64: `pcmpeqq` (SSE4.1) | `iNxM.eq` |
| `Ne` | `cmeq`, `mvn` (2) | `pcmpeq`, `pxor` (2) | `iNxM.ne` |
| `Slt`, `Sle` | `cmgt`, `cmge` with swapped operands | `pcmpgt` swapped; `Sle` is `pcmpgt` + `pxor` (2) | `iNxM.lt_s`, `.le_s` |
| `Ult`, `Ule` | `cmhi`, `cmhs` swapped | `pminu`+`pcmpeq` (2), or sign-bias xor + `pcmpgt` (3) | `iNxM.lt_u`, `.le_u` |
| `ShlImm`, `LShrImm`, `AShrImm` | `shl ushr sshr` (8-bit shifts, SSE: 2 to 3) | `psll{w,d,q}`, `psrl*`, `psra{w,d}`; 8-bit and 64-bit `AShr`: 2 to 3 | `iNxM.shl`, `.shr_u`, `.shr_s` |
| `Shl`, `LShr`, `AShr` | `ushl` (negated amount for right shifts, `sshl`), amounts above `lw` need `umin` (2) | AVX2 `psllv` is not in the base; SSE2 to SSE4.1: scalarized through 16 or 32-bit shifts, or a lookup (about 4 to 8); an emitter may extract lanes | scalarized (about 4 to 8) |
| `Select` | `bsl` | `pand`, `pandn`, `por` (3) or `pblendvb` (SSE4.1) | `v128.bitselect` |
| `Widen*` | `sxtl`, `uxtl`, `sxtl2`, `uxtl2` | `pmovsx*`, `pmovzx*` (SSE4.1); high: `psrldq 8` first (2) | `iNxM.extend_{low,high}_..._{s,u}` |
| `NarrowTrunc` | `xtn`, `xtn2` (2) | `pshufb` per source and `punpcklqdq` (3) | `i8x16.shuffle` (1) |
| `NarrowSS` | `sqxtn`, `sqxtn2` (2) | `packsswb`, `packssdw` | `i8x16.narrow_i16x8_s`, `i16x8.narrow_i32x4_s` |
| `NarrowUU` | `uqxtn`, `uqxtn2` (2) | `pminu` with the maximum then `packuswb` / `packusdw` (SSE4.1) (3) | `pmin_u` then `narrow_*_u` after masking (3) |
| `NarrowSU` | `sqxtun`, `sqxtun2` (2) | `packuswb`, `packusdw` (SSE4.1) | `i8x16.narrow_i16x8_u`, `i16x8.narrow_i32x4_u` |
| `MulWide*` | `smull`, `umull`, `smull2`, `umull2`; `SU`: widen both, `mul` (3) | `pmovsx`/`pmovzx` both, `pmul*` (3), 32: `pmuldq`/`pmuludq` (2 to 3) | `iNxM.extmul_{low,high}_..._{s,u}`; `SU`: extend both, `mul` (3) |
| `Broadcast(i)` | `dup` (lane) | `pshufd`, `pshuflw`+`pshufd` (16), `pshufb` (8) | `i8x16.shuffle` / `iNxM.splat` of `extract_lane` |
| `Extract(i)` | `umov` | `movd`, `pextr{b,w,d,q}` (SSE4.1; `pextrw` SSE2) | `iNxM.extract_lane_u` |
| `Insert(i)` | `ins` | `pinsr{b,d,q}` (SSE4.1), `pinsrw` (SSE2) | `iNxM.replace_lane` |
| `Splat` | `dup` (general) | `movd` + `pshufd`/`pshufb` | `iNxM.splat` |
| `Permute` | `tbl` / `tbl2` (out of range gives 0 as defined) | `pshufb` with the constant pattern remapped so that out-of-range bytes have bit 7 set; two sources: two `pshufb`, `por` (3) | `i8x16.swizzle`; two sources: `i8x16.shuffle` of both with a zero vector |

Constants that an emitter derives from an immediate or a `Permute`
pattern are known at translation time and cost nothing at run time.
<!-- /docgen:doc -->

<!-- docgen:doc crate=relift-ir name=lane::Permute -->
A constant byte permutation of one or two 128-bit sources, the payload of
`crate::Op::Permute`.

Result byte `i` is source byte `pattern[i]`: index 0 to 15 selects a byte
of `a`, index 16 to 31 a byte of `b` when there is one, and any other
index (including 16 to 31 with a single source) gives 0.
<!-- /docgen:doc --> `Permute` is a constant byte permutation of one or two 128-bit values: result byte `i` is source byte `pattern[i]`, and an index outside the sources gives 0.

**Lanes and permutes, printed.** Only the RSP lifter emits these operations; the A64 lifter writes NEON with extracts and concats. `vadd` on the RSP is built from `lane.add.16`, `lane.sub.16` and the saturating kinds, with the carry in the `VCOL` slot:

<!-- docgen:output crate=relift-n64-tools bin=relift-n64rom args="ir --rsp --dce 0x4a031050" -->
```text
; 0x80001000 4a031050
block 0x0 len 4 regs rsp addr i32
  v0:i128 = get v2
  v1:i128 = get v3
  v2:i128 = get VCOL
  v3:i128 = lane.add.16 v0, v1
  v4:i128 = lane.sub.16 v3, v2
  v5:i128 = const 0x0
  v6:i128 = const 0x0
  v7:i128 = lane.mins.16 v0, v1
  v8:i128 = lane.qsubs.16 v7, v2
  v9:i128 = lane.maxs.16 v0, v1
  v10:i128 = lane.qadds.16 v8, v9
  set v1, v10
  set ACCL, v4
  set VCOL, v5
  set VCOH, v6
  tick 1
  sync
  exit jump 0x4
```
<!-- /docgen:output -->

`vmudh` with element selector 8 (one element broadcast) first builds that operand with `lane.perm`, a permutation of one source whose pattern is a 128-bit hex value of 16 byte indexes. It then forms the accumulator with `lane.mul.16` and `lane.mulhs.16` and the result with `lane.perm2` (two sources) and `lane.narrowss.32`:

<!-- docgen:output crate=relift-n64-tools bin=relift-n64rom args="ir --rsp --dce 0x4b031047" -->
```text
; 0x80001000 4b031047
block 0x0 len 4 regs rsp addr i32
  v0:i128 = get v2
  v1:i128 = get v3
  v2:i128 = lane.perm v1, 0xf0e0f0e0f0e0f0e0f0e0f0e0f0e0f0e
  v3:i128 = lane.mul.16 v0, v2
  v4:i128 = lane.mulhs.16 v0, v2
  v5:i128 = const 0x0
  v6:i128 = lane.perm2 v3, v4, 0x17160706151405041312030211100100
  v7:i128 = lane.perm2 v3, v4, 0x1f1e0f0e1d1c0d0c1b1a0b0a19180908
  v8:i128 = lane.narrowss.32 v6, v7
  set v1, v8
  set ACCH, v4
  set ACCM, v3
  set ACCL, v5
  tick 1
  sync
  exit jump 0x4
```
<!-- /docgen:output -->

The VR4300 lifter (`lw t0, 4(a0)`) shows a big-endian `io.load.be.any`, two guards on the address (alignment, and a check of its top bits) and an `intrinsic` call, `mips_dcache`:

<!-- docgen:output crate=relift-n64-tools bin=relift-n64rom args="ir --dce 0x8c880004" -->
```text
; 0x80001000 8c880004
block 0x80001000 len 4 regs vr4300
  v0:i64 = get r4
  v1:i64 = const 0x4
  v2:i64 = add v1, v0
  v3:i2 = extract v2, 0
  v4:i2 = const 0x0
  v5:i1 = cmp.eq v3, v4
  v6:i1 = not v5
  guard v6, 0x1004
  v8:i34 = extract v2, 30
  v9:i34 = const 0x3fffffffe
  v10:i1 = cmp.eq v8, v9
  v11:i1 = not v10
  guard v11, 0x1002
  v13:i64 = const 0x1fffffff
  v14:i64 = and v2, v13
  v15:i32 = io.load.be.any v14
  v16:i32 = const 0x0
  v17:i1 = const 0x1
  intrinsic "mips_dcache" v2, v16, v17
  v19:i64 = sext v15
  tick 1
  set r8, v19
  sync
  exit jump 0x80001004
```
<!-- /docgen:output -->

### Crypto and pointer authentication

`Crypto` is one table-driven cipher step, kept whole so an emitter can use a table or a host instruction. The tables are computed from the field arithmetic the standards define:

<!-- docgen:doc crate=relift-ir name=crypto -->
The meaning of each `CryptoOp`, computed from the field arithmetic the
standards define (no copied tables).
<!-- /docgen:doc -->

<!-- docgen:code file=core/ir/src/ops.rs symbol=CryptoOp raw=true -->
```rust
/// Fixed table-driven steps of standard ciphers, kept whole so an emitter
/// can map each to a table or a host instruction instead of the gate-level
/// circuit the tables stand for. Each is pure and has one meaning.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum CryptoOp {
    /// AES `SubBytes` (FIPS-197 5.1.1) on each byte of a 128-bit value.
    AesSubBytes,
    /// AES `InvSubBytes` (FIPS-197 5.3.2) on each byte of a 128-bit value.
    AesInvSubBytes,
    /// AES `MixColumns` (FIPS-197 5.1.3) on a 128-bit state, column `c` in
    /// bytes `4c .. 4c + 4`, row 0 lowest.
    AesMixColumns,
    /// AES `InvMixColumns` (FIPS-197 5.3.3), same layout.
    AesInvMixColumns,
    /// The SM4 S-box (GB/T 32907) on one byte.
    Sm4Sbox,
}
```
<!-- /docgen:code -->

**Crypto, printed.** `aese v0.16b, v1.16b` is an `xor` of the two registers, a byte shuffle built from extracts and concats (ShiftRows), and one `crypto.aes.subbytes`; `--grep` keeps only the crypto line. (The emitter lifts these instructions to one intrinsic each, so the host's AES instructions can run them; `relift-ir` prints the portable lift.)

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--dce --grep crypto 0x4e284820" -->
```text
; 0x1000 4e284820 AESE_B_cryptoaes
block 0x1000 len 4 regs arm64
  v34:i128 = crypto.aes.subbytes v33
  exit jump 0x1004
```
<!-- /docgen:output -->

`Pac` is the 64-bit pointer authentication code of `data` under the 64-bit `modifier` and the key `key0:key1`: QARMA5, as the Arm `ComputePAC` pseudocode defines it. It is pure. The cipher:

<!-- docgen:doc crate=relift-ir name=pac -->
The meaning of [`crate::Op::Pac`]: QARMA5 (QARMA-64 with five rounds,
S-box σ2), the cipher of the Arm architecture's `ComputePACQARMA`, from
the round structure the QARMA paper and the Arm pseudocode define.
<!-- /docgen:doc -->

**Pointer authentication, printed.** With `FEAT_PAuth`, `pacia x0, x1` signs `x0` with modifier `x1` and the `apiakey` pair. The lift is about 100 lines, mostly the concats that build the pointer's sign extension, so `--grep` keeps the one `pac` line. Its operands are the pointer with its extension bits, the modifier and the two key halves:

<!-- docgen:output crate=relift-switch-tools bin=relift-ir args="--features cortex-a57,FEAT_PAuth --dce --grep pac 0xdac10020" -->
```text
; 0x1000 dac10020 PACIA_64P_dp_1src
block 0x1000 len 4 regs arm64
  v74:i64 = pac v71, v1, v72, v73
  exit jump 0x1004
```
<!-- /docgen:output -->

The interpreter evaluates a binary16 operation in binary64 and rounds once more, which is correctly rounded for sums, products, quotients and square roots; a fused multiply-add is computed exactly in integers (`core/ir/src/half.rs`).

### Effects and intrinsics

`Intrinsic` calls a named function that the frontend and runtime define. The IR cannot see into it, so every intrinsic counts as an effect. `Op::has_effect` lists the operations that dead-code elimination keeps even when their value is unused.

## Text format

`Display` for `Block` prints a block as text, and `text::parse` reads it back; the two round-trip exactly. Each line is one instruction, written `vN:iW = op operands`, with the result prefix left out for an operation that gives no value. The first line names the pc, the lifted length, the register file and, when they are not the defaults, the address width and the bank. The last line is the exit.

The listings on this page are in this form; [the first one](#an-example) is two A64 words. The grammar and the mnemonic of every operation are in the module doc of `core/ir/src/text.rs`:

<!-- docgen:code file=core/ir/src/text.rs symbol=crate raw=true -->
````rust
//! The textual form of a [`Block`]: a printer (`Display`) and a parser
//! ([`parse`]) that round-trip exactly.
//!
//! ```text
//! block 0x1000 len 4 regs arm64
//!   v0:i64 = get x1
//!   v1:i64 = const 0x10
//!   v2:i64 = add v0, v1
//!   v3:i64 = load.le.plain v2
//!   v4:i1 = cmp.eq v3, v1
//!   set x0, v3
//!   store.le.release v2, v3
//!   exit jump select v4, 0x1040, 0x1004
//! ```
//!
//! # Grammar
//!
//! Lines are separated by newlines; blank lines are ignored; tokens are
//! separated by spaces, and `,` is a token of its own. Instructions are
//! printed indented by two spaces, the parser accepts any indentation.
//!
//! ```text
//! block  := header inst* exit
//! header := "block" HEX "len" DEC "regs" NAME [ "addr" TYPE ] [ "bank" HEX ]
//! inst   := [ "v" N ":" TYPE "=" ] op
//! exit   := "exit" flow next
//! flow   := "jump" | "call" HEX | "return" | "trap" HEX
//! next   := HEX | "select" value "," HEX "," HEX | "dynamic" value
//! value  := "v" N
//! TYPE   := "i" WIDTH           (1 to 128 bits)
//! ```
//!
//! `NAME` is the register file's name ([`RegFile::name`]); [`parse`] looks it
//! up in the files it is given. The address width is omitted when it is 64
//! bits, otherwise it is printed as `addr i32` and so on; the bank
//! ([`Block::bank`]) is printed only when the block has one. `HEX` is `0x` and
//! hex digits. The exit puts the flow first so the common case reads
//! `exit jump 0x1004`; `call` names the return address, `trap` the
//! frontend's code, then comes where control goes next. A `select` taken
//! address comes first: `select vC, taken, not_taken`.
//!
//! Values are numbered by instruction index: `v0` is the first instruction,
//! and instructions with no result (`set`, `store`, fences) take an index
//! too, so a result prefix `vN:` must match the instruction's position. An
//! operand names an earlier instruction only.
//!
//! Operations, with operands separated by `, `:
//!
//! | Form | Meaning |
//! |------|---------|
//! | `const HEX` | [`Op::Const`] |
//! | `get REG` / `set REG, v` | [`Op::Get`] / [`Op::Set`], by declared slot name |
//! | `not neg clz ctz popcount bitreverse bswap v` | [`UnOp`] |
//! | `add sub mul udiv sdiv urem srem and or xor shl lshr ashr rotr a, b` | [`BinOp`] |
//! | `cmp.eq cmp.ne cmp.ult cmp.ule cmp.slt cmp.sle a, b` | [`CmpOp`] |
//! | `select c, a, b` | [`Op::Select`] |
//! | `zext v` / `sext v` | [`Op::Ext`] to the result width |
//! | `extract v, LO` | [`Op::Extract`], `LO` decimal |
//! | `concat hi, lo` | [`Op::Concat`] |
//! | `load.E.O addr` / `store.E.O addr, value` | memory, `E` is `le` or `be` |
//! | `load.excl.E.O addr` / `store.excl.E.O addr, value` | exclusive memory |
//! | `atomic.U.E.O addr, value` | [`Op::AtomicRmw`], `U` is `add clr eor set smax smin umax umin swp` |
//! | `cas.E.O addr, expected, value` | [`Op::CompareSwap`] |
//! | `crypto.C v` | [`Op::Crypto`], `C` is `aes.subbytes aes.invsubbytes aes.mixcolumns aes.invmixcolumns sm4.sbox` |
//! | `pac data, modifier, key0, key1` | [`Op::Pac`] |
//! | `lane.K.W args` | [`Op::Lane`]: `K` is a [`LaneKind`] mnemonic (`add qadds mulhs shli widenlos narrow mulwloss bcast extract insert splat` and so on, see `LANE` in this file), `W` the lane width 8, 16, 32 or 64 (omitted for the bitwise kinds `and or xor andn not select`); shift, broadcast, extract and insert end in `, N`, a decimal immediate |\n//! | `lane.perm v, PAT` / `lane.perm2 a, b, PAT` | [`Op::Permute`]; `PAT` is the 16 byte indices as a 128-bit hex value, byte `i` of the value giving result byte `i` |\n//! | `fence.O` / `clear.excl` | fence, clear the monitor |
//! | `fp.add fp.sub fp.mul fp.div fp.sqrt fp.muladd fp.neg fp.abs args` | [`FpOp`] |
//! | `fp.rint.R fp.tosint.R fp.touint.R fp.fromsint fp.fromuint fp.convert v` | [`FpOp`] conversions; `R` is `n`, `a`, `m`, `p` or `z` ([`FpRound`]) |
//! | `intrinsic "name" args` | [`Op::Intrinsic`], args may be empty |
//! | `guard c, HEX` | [`Op::Guard`]: leave to slow path `HEX` when `c` is 1 |
//! | `tick DEC` / `tick.if c, DEC` | [`Op::Tick`] / [`Op::TickIf`]: guest cycles, decimal |
//! | `sync` | [`Op::Sync`], the last instruction |
//! | `io.load.E.K addr` / `io.store.E.K addr, value` | [`Op::IoLoad`] / [`Op::IoStore`]; `K` is `any` or `dev` ([`IoKind`]) |
//!
//! The ordering `O` is `plain`, `acquire`, `release`, `acqrel` or `seqcst`.
//! Intrinsic names are quoted with `\"` and `\\` escapes. A parsed
//! intrinsic name is interned in a process-wide table because [`Op`] holds a
//! `&'static str`; the table grows by one entry per distinct name.
//!
//! The parser checks that every reference is to an earlier value, that
//! registers exist, and finally runs [`Block::verify`], mapping its error to
//! the offending line. Errors carry a 1-based line and column.
````
<!-- /docgen:code -->

## Binary format

`write_block` and `read_block` give a compact byte form of one block. The IR is never written to disk as IR. The one user of `write_block` is the emitter's reuse code (`core/emit/src/reuse.rs`), which serializes a function's blocks, with the rest of what the emitter reads about it, only as the input that is hashed into the function's emit-cache key (`store.key(&reuse::input(...))` in `core/emit/src/lib.rs`; the key also covers a hash of the running `relift`). The store keeps the emitted C of each function under that key, not the IR, and nothing reads the bytes back into blocks outside the tests of `core/ir`. Integers are LEB128. A block is written as:

1. The index of the register file in the list the caller passes (`files`).
2. One byte for the address width in bits.
3. The pc, the length and the bank, where the bank is a flag byte (0 for none, 1 for present) and then the value when present.
4. The number of instructions, then for each one a byte for its result width (0 for no result), a tag byte for the operation and its operands. An operand is an instruction index.
5. The exit: a tag for the next address (constant, select or dynamic) with its operands, then a tag for the flow.

The layout is private to a version of the crate, and `BIN_FORMAT` is the version of the layout:

<!-- docgen:code file=core/ir/src/bin.rs symbol=FORMAT -->
```rust
pub const FORMAT: u32 = 2;
```
<!-- /docgen:code -->

A byte string that is not a block of the given files is a `BinError`.

## How the IR is checked

- **The interpreter.** `interp::run` is the reference: its semantics are the definition of every operation, and the C emitter must agree with it. It runs a block on a `State` (one value per slot and the exclusive monitor) and a `GuestMemory`, with `Intrinsics` and `Devices` supplied by the caller. A fault discards the block's register writes; stores before it have happened.
- **Tests.** `core/ir/tests` runs every operation on boundary values (`interp.rs`), the lane kinds against a reference written in the test (`lane.rs`), the text and binary round trips (`text.rs`) and the device extensions (`hw.rs`). The crypto tables carry known-answer checks in `crypto.rs`.
- **Against other implementations.** `relift-check` runs lifted blocks on the interpreter against an oracle (Unicorn, QEMU, a reference emulator) on random states, and `--mutate` breaks each block and expects a mismatch (see [the differential checker](../../check/README.md)). `relift-diff` (`core/diff`) is the protocol behind `relift diff`: it runs one guest function from a live process's state in a reference engine and compares registers and pages with what the lifted code left.
- **Optimizer passes** (`core/ir/src/opt.rs`), all block-local and all behavior-preserving in the interpreter:

<!-- docgen:doc crate=relift-ir name=opt::dce -->
Removes instructions whose values nothing observable uses.

An instruction is kept when it has an effect ([`Op::has_effect`]: `Set`,
every load and store including exclusive ones, `ClearExclusive`, `Fence`,
every intrinsic, the clock ticks `Tick` and `TickIf`, the I/O
accesses `IoLoad` and `IoStore`, and `Sync`) or when a kept instruction or the exit uses its
value, transitively. Plain loads are kept even when the loaded value is
unused, because a load may fault and the fault is observable. Intrinsics
are all treated as effectful, since the IR cannot see into them.

The surviving instructions keep their order and are renumbered densely, so
the result passes `Block::verify` whenever the input does and behaves
the same in the interpreter.
<!-- /docgen:doc -->

<!-- docgen:doc crate=relift-ir name=opt::simplify -->
Folds constants and drops operations that give one of their operands.

A pure integer operation whose operands are all constant
(`constants`) becomes `Op::Const`. An operation that gives one of
its operands unchanged is replaced by that operand at every use: `x + 0`,
`x - 0`, `x | 0`, `x ^ 0`, `x & ones`, `x * 1`, shifts and rotates by 0,
an extension or extract that keeps the width, a select on a constant
condition or with equal arms, a one-bit `x == 1` or `x != 0`, the low or
high part of a concat, a zero extension's low bits, and a double
complement. An extract reads through the concats, extracts and zero
extensions it falls inside of, and a one-bit select between 1 and 0 is
its condition or the complement. An operation that gives a constant whatever its other
operand is (`x & 0`, `x * 0`, `x | ones`, a shift left or logical right
by at least the width) becomes that constant, and a select exit whose
condition is constant becomes a constant exit.

Instructions with effects stay in place with their operands replaced,
so the result behaves the same in the interpreter as the input. The
replaced instructions stay too (unused); `dce` removes them.
<!-- /docgen:doc -->

<!-- docgen:doc crate=relift-ir name=opt::classify_io -->
Classifies the I/O accesses of `block` whose address is constant
(`constants`) with the guest's `map`.

An `Op::IoLoad` or `Op::IoStore` of kind `IoKind::Any` at a
constant address becomes a plain `Op::Load` or `Op::Store` (ordering
`Order::Plain`) when every byte it touches is memory in `map`, and
kind `IoKind::Device` otherwise, so the emitter needs no range check
for it. Other instructions, and accesses whose address is not constant,
are unchanged. The instructions keep their positions, and the result
behaves the same in the interpreter as the input, given devices whose
`crate::interp::Devices::map` is `map`.
<!-- /docgen:doc -->

<!-- docgen:doc crate=relift-ir name=opt::fuse -->
The block that runs `head` and then `tail`: `head`'s address and length
with `tail`'s exit, for a `head` that always goes on to `tail`.

A read of a register after a write of it becomes the written value, and
only the last write of each register stays, so the result behaves in
the interpreter as `head` followed by `tail`.
<!-- /docgen:doc -->

<!-- docgen:doc crate=relift-ir name=opt::constants -->
The values of `block` that are constant: built from `Op::Const` by
pure integer operations only. Entry `n` is the value of instruction `n`
when it is constant, with the semantics of `crate::interp`.
<!-- /docgen:doc -->
