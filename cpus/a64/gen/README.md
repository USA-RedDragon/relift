# A64 decoder generator

`src/decode/generated.rs` is generated from Arm's AARCHMRS `Instructions.json`
(BSD-3-Clause, see `NOTICE-AARCHMRS`). The JSON is not needed to build.

Regenerate from the repository root (stdlib Python only):

    python3 -I cpus/a64/gen/aarchmrs_decoder.py <Instructions.json> cpus/a64/src/decode/generated.rs
    cargo fmt -p relift-cpu-a64

The generator prints the number of leaves it emitted (4429 for release
2026-09) and fails on any encodeset, value or condition shape it does not
understand. `IsFeatureImplemented` is treated as true; every other field
constraint becomes a predicate in `generated::condition`.
