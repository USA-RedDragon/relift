# ARMv4T and ARMv5TE lifting templates from Sail

`src/decode/generated.rs` (the decode tables of both profiles, one
`Encoding` for both) and `src/lift/generated/a32.rs` and `t16.rs` (ARMv4T)
and `a32_v5te.rs` and `t16_v5te.rs` (ARMv5TE), one Rust function per
encoding, are generated from the AArch32 part of the Sail model of the Arm
architecture (sail-arm, BSD-3-Clause-Clear, see `NOTICE-sail-arm`) plus the
profile's overlay, `armv4t.sail` or `armv5te.sail`, by the
`relift-cpu-arm32-gen` crate in this folder. Neither the generator nor the
Sail sources are needed to build; `generated/mod.rs` and `decode/mod.rs` are
hand-written.

The generator is the 32-bit Arm layer over `relift-sail` (`core/sail`): `Arm32`
in `src/cut.rs` implements `relift_sail::eval::Cpu` and
`relift_sail::stage::Stage`, and `src/main.rs` implements
`relift_sail::stage::Form` for every decode clause of the A32 and T16 groups
that the overlay allows.

## The overlays

An overlay is loaded after the model with `Model::load_overlay`: its
functions replace the model's, its `function clause` items of a decode group
go before the model's clauses, and its `$relift patch`, `$relift relax` and
`$relift drop_clause` lines edit model text (the generator prints how many
texts each `patch` and `relax` line changed, so one that stops matching after
a model update shows as 0). `$relift allow` names the encodings that exist on
the profile; every other clause of the groups decodes as undefined.
`$relift profile v5te` marks the ARMv5TE overlay: the generator then reads
the unconditional group (condition 1111), gives a word with condition 1111
only to clauses that fix it (BLX immediate), and cuts the Q flag.

Where sail-arm (an Armv8 model) and the ARM7TDMI differ, the overlay carries
the ARM7TDMI rule: R15 reads, PC writes without interworking, unaligned
loads, the LDM and STM edge cases, SWP, the Thumb BL halves, multiply cycles,
the modes and PSRs. The rules measured on a Game Boy Advance (AGB-001) with
relift's hardware probe are marked in the overlay: MSR to the SPSR forces
mode bit 4 to 1, and TST, TEQ, CMP and CMN with Rd = 15 and S = 1 (and Thumb
CMP with Rd = 15) copy the SPSR to the CPSR in a mode with an SPSR and leave
the CPSR unchanged in user and system mode.

`armv5te.sail` is `armv4t.sail` with the ARM946E-S changes, marked "v5:" in
it: the v5TE encodings (CLZ, BLX, BKPT, the saturating and 16-bit multiplies,
LDRD and STRD, PLD, MCR and MRC), loads of the PC interworking, the v5 LDM
and STM rules, forced alignment of odd halfword loads, and no ARM7 multiply
cycles. Rules from GBATEK's ARM9 notes are marked [GBATEK] and are not yet
measured on a console. MCR and MRC become `arm.cp_write` and `arm.cp_read`
intrinsics with the register key (coprocessor bit, opc1, CRn, CRm, opc2)
known at lift time; an MCR ends its block at a sync point.

The functions in `src/cut.rs` are written by hand: register access in the
current mode and the user bank, the PSRs (the mode is a run-time value; a
mode change swaps the banked registers in the IR, see `rt.rs`), the PC and
branches, the condition guard, the software interrupt, and the bus accesses
and internal cycles that make the cycle cost. A read of `PSTATE.M` is refused,
so a model function that tests the mode must be replaced in the overlay, and
so is a write of a `PSTATE` field with no cut (it would vanish from the
template); the model's `PSTATE.T` writes are dropped because the Thumb bit's
changes go through the overlay's `__v4t_SetT`.

## Regenerate

From the repository root:

    cpus/arm32/gen/fetch-sail.sh "$DIR"
    cpus/arm32/gen/regen.sh "$DIR/lib" "$DIR/model"

`fetch-sail.sh` downloads the files the generator reads (listed with their
SHA-256 in `sources.txt`) from sail-arm at
`5416ef6b43e9527c38dc71fb752259c1d01a47e1` and rems-project/sail at
`c44944f11b71e5c9bd4b30cdbf79ea32f6dcf456`. CI runs both and fails when the
committed files differ. `try SAIL_LIB MODEL_SRC OVERLAY NAME...` prints the
template of named encodings or why it fails.
