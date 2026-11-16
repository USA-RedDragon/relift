# cpus/mips

The MIPS frontends (`relift-cpu-mips`): a decoder and a lifter to
`relift-ir` for each MIPS CPU relift supports, one profile per CPU. The
decode tables and the lifting templates are generated from the MIPS part of
sail-cheri-mips plus one overlay per profile (`gen/README.md`). The
generated files are committed and never edited by hand.

| Profile | CPU | Overlay | State |
|---|---|---|---|
| `vr4300` | Nintendo 64 CPU: MIPS III, 64-bit registers, FPU, TLB | `gen/vr4300.sail` | complete |
| `r3000a` | PlayStation CPU: MIPS I, 32-bit registers, load delay slots, no FPU, no TLB, GTE as COP2 | `gen/r3000a.sail` | stub: MIPS I decode table and the `int` class |

```sh
cargo test -p relift-cpu-mips
```

State: `relift-n64-analyze` runs the VR4300's lifted IR for code discovery;
no guest crate emits it yet. The VR4300 has a runtime CPU layer
(`runtime/cpus/mips`), the R3000A has none, and the lifted-versus-oracle
check covers the VR4300 only (`relift-check mips`).

## Layout

```
src/
  lib.rs              re-exports vr4300::{decode, lift, regs} at the crate root
  common/             shared by every profile, hand-written
    rt.rs             Core: pending register writes, delay-slot and branch
                      state, guards and the static exception, memory as I/O
    lift.rs           Profile trait and the block driver: one instruction,
                      or a branch with its delay slot, each with its tick,
                      then the exit
  vr4300/, r3000a/    one module per profile
    regs.rs           the register file (slot order, names the cut checks)
    decode/           generated.rs (the table) and mod.rs (Encoding, decode)
    lift/
      generated/      one file per template class, plus a hand-written mod.rs
      rt.rs           St: the profile's state over Core and the cut calls
      mod.rs          LiftError, Lifted, lift and the Profile impl
tests/
  lift.rs, ctx_layout.rs    VR4300
  r3000a.rs                 the R3000A stub
```

A profile owns everything that differs between CPUs: the register file and
its widths, the decode table, the templates, address translation, the
coprocessors and the effects the runtime owns. `common` owns what all MIPS
CPUs share: a branch or jump lifts together with its delay slot into one
block, exceptions and the slow paths leave the block through guards before
any effect, register writes stay pending until the instruction (and its
slot) cannot fault, and loads and stores are I/O accesses in the profile's
byte order. A new profile adds an overlay, an entry in `gen/src/profile.rs`
(mnemonic table, template classes, register file), its module here and its
line in `gen/regen.sh` and `scripts/ci/generated.sh`.

## Cycles

Each instruction ticks one issue cycle plus what the overlay's `mips_run`
gives the cut `MIPS_Cycles` for it (`cycles` and `fp_cycles` in
`gen/vr4300.sail`: the multiplier, the divider and the FPU, as ares counts
them). A template that ticks under a condition of its own does it at once
(`TickIf`); the rest is added to the instruction's tick, which is
conditional only in a likely branch's slot (an annulled slot takes no
cycle). The block driver puts a branch's tick after the block's last guard,
so the slot's accesses and calls see it and guards stay before effects.
The memory system's cycles are not in the templates: `St` calls the
intrinsic `mips_dcache` after every load and store, and the guest frontend
adds the instruction cache fetches and the bus waits (`docs/abi.md`, the
N64 section).

The VR4300 modules stay at the crate root (`relift_cpu_mips::lift`,
`decode`, `regs`), so code written before the split keeps working.

## Load delay slots (R3000A, not modeled yet)

On the R3000A the value of a load (LB, LBU, LH, LHU, LW, LWL, LWR, and
MFC0, MFC2, CFC2 and LWC2) reaches its register one instruction late: the
instruction after the load still reads the old value. When that instruction
writes the same register itself, its write wins and the loaded value is
dropped. LWL and LWR after a load of the same register merge with the
pending value, not the old one, so the usual unaligned pair works. The
sail-cheri-mips model has no load delay, so relift models it in the
profile's `St`, not in Sail:

- The overlay routes the register write of every delayed load through a cut
  of its own (a `wGPR_load`), so `St` knows which write is delayed. The
  other templates are unchanged.
- Within a block the delay is lift-time state: `St` holds one pending
  (register, value) pair. The next instruction's reads of that register see
  the old slot; after its own writes the pending pair is committed unless
  that instruction wrote the register. When the next instruction does not
  read the register, which is most code, this is the same as an immediate
  write and costs nothing.
- A load can be the last instruction of a block: a load in a branch's delay
  slot, or a block that ends at the load. The pair then crosses the block
  boundary in two context slots of the register file (pending register,
  0 for none, and its value). The block writes them at its exit, and a
  block entered with a pending load applies it after its first
  instruction, under the same rule. For a constant target the lifter knows
  the first instruction and commits at the exit whenever that instruction
  neither reads nor writes the register, so only computed targets and
  interrupts at block boundaries pay for the slots.
- A guard that leaves inside the instruction after a load (an exception or
  a slow path) commits the pending pair first, since the load's write-back
  has happened by then; the oracle check against a console decides the
  corner cases, as for the VR4300.

Branch delay slots are shared with the VR4300 (`common/lift.rs`). The
R3000A has no likely branches, so its slot always runs.
