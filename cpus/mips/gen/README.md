# VR4300 lifting templates from Sail

`src/vr4300/decode/generated.rs` (the decode table) and `src/vr4300/lift/generated/*.rs`
(one Rust function per encoding, one file per class: `int`, `ldst`,
`branch`, `system`, `cop1`) are generated from the MIPS part of the Sail
CHERI-MIPS model (sail-cheri-mips, BSD-2-Clause, see `NOTICE-sail-cheri-mips`)
plus the VR4300 overlay `vr4300.sail` by the `relift-cpu-mips-gen` crate in
this folder. Neither the generator nor the Sail sources are needed to build;
`generated/mod.rs` and `decode/mod.rs` are hand-written.

The generator is the VR4300 layer over `relift-sail` (`core/sail`): `Mips`
in `src/cut.rs` implements `relift_sail::eval::Cpu` and
`relift_sail::stage::Stage`, and `src/main.rs` builds one decode group from
the model's `decode` clauses and implements `relift_sail::stage::Form` for
each. Every clause gets its VR4300 mnemonic from the table in `main.rs`; the
generator fails when the model's clauses and the table differ, or when two
clauses share a word.

## The overlay

`vr4300.sail` is loaded after the model with `Model::load_overlay`. It drops
the clauses that are not VR4300 (MIPS32/64 R2, BERI and CHERI additions),
patches LL and SC to the VR4300 link rule, and replaces the model where the
VR4300 differs: 32-bit operations on unextended registers, natural
alignment, SRA and SRAV, MULT, the divide-by-zero results of the DIV family,
LWR, CACHE and the TLB instructions as cuts, and the CP0 register file. It
adds all of COP1: the moves, BC1, the loads and stores, and the arithmetic,
conversions and compares, with the FR=0 and FR=1 register views.

The functions in `src/cut.rs` are written by hand: register access, the
delay-slot and branch state, address translation, the bus, exceptions, the
coprocessor usable checks, the FPU operations and the CP0 effects the
runtime owns. Their run-time side is `St` in `src/vr4300/lift/rt.rs`.

## Regenerate

From the repository root:

    cpus/mips/gen/fetch-sail.sh "$DIR"
    cpus/mips/gen/regen.sh "$DIR/lib" "$DIR/model"

`fetch-sail.sh` downloads the files the generator reads (listed with their
SHA-256 in `sources.txt`) from sail-cheri-mips at
`13a9c3fb0c3f8d320d4f4650a63d23d3a8b12724` and rems-project/sail at
`c44944f11b71e5c9bd4b30cdbf79ea32f6dcf456`. CI runs both and fails when the
committed files differ. `try SAIL_LIB MODEL_SRC OVERLAY NAME...` prints the
template of named encodings (by Rust name or mnemonic) or why it fails.
