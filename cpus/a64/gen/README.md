# A64 decoder generator

`src/decode/generated.rs` is generated from Arm's AARCHMRS `Instructions.json`
(BSD-3-Clause, see `NOTICE-AARCHMRS`). The JSON is not needed to build.

Regenerate from the repository root (stdlib Python only):

    python3 -I cpus/a64/gen/aarchmrs_decoder.py <Instructions.json> cpus/a64/src/decode/generated.rs
    cargo fmt -p relift-cpu-a64

The generator prints the number of leaves it emitted (4429 for release
2026-09) and fails on any encodeset, value or condition shape it does not
understand. Every field constraint and feature test (`IsFeatureImplemented`)
becomes a predicate in `generated::condition` over the word and the CPU's
`Features`; the feature names become the `Feature` enum (plus `FEAT_PMULL`,
which only the Sail semantics test).

# A64 lifting templates from Sail

`src/lift/sail/generated/*.rs` holds one Rust function per encoding id, one
file per instruction class, generated from the Sail model of the Arm
architecture (sail-arm, BSD-3-Clause-Clear, see `NOTICE-sail-arm`) by the
`relift-cpu-a64-gen` crate in this folder. Neither the generator nor the Sail
sources are needed to build; `generated/mod.rs` is hand-written.

The generator parses the Sail subset the model uses, then runs the Sail
semantics of each encoding's decode clauses in a staged evaluator: fixed bits
are known while generating, the other instruction bits become Rust code run
at lift time, and guest state becomes `relift-ir` builder calls. A value that
must be known while generating (a width, a loop bound, the choice of decode
clause) splits the template on the bits it depends on. The functions in
`src/cut.rs` are not translated but written by hand: register access, the PC
and branches, memory and the exclusive monitor, configuration checks that are
no-ops at EL0 in the fixed Armv8.0-A configuration, `DecodeBitMasks`, and the
integer quotient of `SDIV` and `UDIV`. Their lift-time side is
`src/lift/sail/rt.rs`.

Regenerate from the repository root:

    cpus/a64/gen/fetch-sail.sh "$DIR"
    cpus/a64/gen/regen.sh "$DIR/lib" "$DIR/model"

`fetch-sail.sh` downloads the files the generator reads (listed with their
SHA-256 in `sources.txt`) from sail-arm at
`5416ef6b43e9527c38dc71fb752259c1d01a47e1` and rems-project/sail at
`c44944f11b71e5c9bd4b30cdbf79ea32f6dcf456`. CI runs both and fails when the
committed files differ. `regen.sh` first replaces each class file with
`placeholder.rs`, so the generator builds even when the old output no longer
compiles against `rt.rs`.

`forms.txt` lists the encodings to generate; the generator fails when any of
them does not generate. `try` prints the template of named encodings or why
it fails, and `parse-all` parses every model function. A form joins
`forms.txt` only after `relift-check forms` (see `check/`) passes it and
catches its mutation.
