# MIPS lifting templates from Sail

For each profile `P`, `src/P/decode/generated.rs` (the decode table) and
`src/P/lift/generated/*.rs` (one Rust function per encoding, one file per
class) are generated from the MIPS part of the Sail CHERI-MIPS model
(sail-cheri-mips, BSD-2-Clause, see `NOTICE-sail-cheri-mips`) plus the
profile's overlay `P.sail` by the `relift-cpu-mips-gen` crate in this
folder. Neither the generator nor the Sail sources are needed to build;
`generated/mod.rs` and `decode/mod.rs` are hand-written.

| Profile | Overlay | Classes |
|---|---|---|
| `vr4300` | `vr4300.sail` | `int`, `ldst`, `branch`, `system`, `cop1` |
| `r3000a` | `r3000a.sail` | `int` (a stub, see `../README.md`) |

An overlay names its profile with a `$relift profile NAME` line (`vr4300`
without one). `src/profile.rs` gives each profile its mnemonic table, its
template classes, the register file the cut checks register names against
and the Rust paths of its generated files.

The generator is the MIPS layer over `relift-sail` (`core/sail`): `Mips`
in `src/cut.rs` implements `relift_sail::eval::Cpu` and
`relift_sail::stage::Stage`, and `src/main.rs` builds one decode group from
the model's `decode` clauses and implements `relift_sail::stage::Form` for
each. Every clause gets its mnemonic from the profile's table in
`profile.rs`; the generator fails when the clauses the overlay keeps and
the table differ, or when two clauses share a word.

## The overlays

`vr4300.sail` is loaded after the model with `Model::load_overlay`. It drops
the clauses that are not VR4300 (MIPS32/64 R2, BERI and CHERI additions),
patches LL and SC to the VR4300 link rule, and replaces the model where the
VR4300 differs: 32-bit operations on unextended registers, natural
alignment, SRA and SRAV, MULT, the divide-by-zero results of the DIV family,
LWR, CACHE and the TLB instructions as cuts, and the CP0 register file. It
adds all of COP1: the moves, BC1, the loads and stores, and the arithmetic,
conversions and compares, with the FR=0 and FR=1 register views.

`r3000a.sail` is loaded the same way. It keeps the MIPS I clauses (no
MIPS II or III, no likely branches, no LL and SC, no TLB instructions) and
gives the `int` class the R3000A rules: 32-bit registers and the
divide-by-zero results of DIV and DIVU. RFE, the R3000A COP0 and the GTE
come with the rest of the profile.

The functions in `src/cut.rs` are written by hand: register access, the
delay-slot and branch state, address translation, the bus, exceptions, the
coprocessor usable checks, the FPU operations and the CP0 effects the
runtime owns. Their run-time side is each profile's `St` in
`src/P/lift/rt.rs`; a profile's `St` has the methods of the cuts its
templates use.

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
