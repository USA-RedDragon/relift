# ARMv4T lifting templates from Sail

`src/decode/generated.rs` (the decode tables) and `src/lift/generated/a32.rs`
and `t16.rs` (one Rust function per encoding) are generated from the AArch32
part of the Sail model of the Arm architecture (sail-arm, BSD-3-Clause-Clear,
see `NOTICE-sail-arm`) plus the ARMv4T overlay `armv4t.sail` by the
`relift-cpu-arm32-gen` crate in this folder. Neither the generator nor the
Sail sources are needed to build; `generated/mod.rs` and `decode/mod.rs` are
hand-written.

The generator is the ARMv4T layer over `relift-sail` (`core/sail`): `Arm32`
in `src/cut.rs` implements `relift_sail::eval::Cpu` and
`relift_sail::stage::Stage`, and `src/main.rs` implements
`relift_sail::stage::Form` for every decode clause of the A32 and T16 groups
that the overlay allows.

## The overlay

`armv4t.sail` is loaded after the model with `Model::load_overlay`: its
functions replace the model's, its `function clause` items of a decode group
go before the model's clauses, and its `$relift patch`, `$relift relax` and
`$relift drop_clause` lines edit model text (the generator prints how many
texts each `patch` and `relax` line changed, so one that stops matching after
a model update shows as 0). `$relift allow` names the encodings that exist on
ARMv4T; every other clause of the groups decodes as undefined.

Where sail-arm (an Armv8 model) and the ARM7TDMI differ, the overlay carries
the ARM7TDMI rule: R15 reads, PC writes without interworking, unaligned
loads, the LDM and STM edge cases, SWP, the Thumb BL halves, multiply cycles,
the modes and PSRs. The rules measured on a Game Boy Advance (AGB-001) with
relift's hardware probe are marked in the overlay: MSR to the SPSR forces
mode bit 4 to 1, and TST, TEQ, CMP and CMN with Rd = 15 and S = 1 (and Thumb
CMP with Rd = 15) copy the SPSR to the CPSR in a mode with an SPSR and leave
the CPSR unchanged in user and system mode.

The functions in `src/cut.rs` are written by hand: register access in the
current mode and the user bank, the PSRs (the mode is a run-time value; a
mode change swaps the banked registers in the IR, see `rt.rs`), the PC and
branches, the condition guard, the software interrupt, and the bus accesses
and internal cycles that make the cycle cost. A read of `PSTATE.M` is refused,
so a model function that tests the mode must be replaced in the overlay.

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
