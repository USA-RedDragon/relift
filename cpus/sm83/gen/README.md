# SM83 lifting templates from Sail

`src/lift/sail/generated.rs` in `cpus/sm83` holds one Rust function per
defined opcode (244 base, 256 CB), generated from the Sail model in
`cpus/sm83/sail/sm83.sail` (our own work, MIT) by this crate on the shared
Sail frontend `core/sail`. Neither the generator nor Sail is needed to build.

The functions declared in `sm83.sail` with `val` and no body are the cut
line (`src/cut.rs`): the registers through `St`, instruction fetches (known
at lift time), the data reads and writes with their M-cycle, the
instruction's cycle total, branches, the branch condition, and the calls
into the per-CPU layer (EI, DI, RETI, HALT, STOP). Their lift-time side is
`cpus/sm83/src/lift/sail/rt.rs`. The block structure (the EI pair, the two
blocks of a conditional CALL or RET, the HALT bug view, sync points) is the
frontend's, shared with the hand-written lifter.

Regenerate from the repository root, with the Sail library that
`cpus/a64/gen/fetch-sail.sh` downloads:

    cpus/a64/gen/fetch-sail.sh "$DIR"
    cpus/sm83/gen/regen.sh "$DIR/lib"

`try` prints the templates of named opcodes (`3c`, `cb7e`) or why one
fails:

    relift-cpu-sm83-gen try "$DIR/lib" cpus/sm83/sail/sm83.sail 3c cb7e

`cargo test -p relift-cpu-sm83 sail` compares every template with the
hand-written lifter (the same IR after `dce` and `simplify`, else the same
registers, accesses, clock and exit in the interpreter on random states)
and checks that the comparison catches mutated templates. `relift-check
--oracle rustyboi lift --lifter sail` runs the generated lifter against
rustyboi.
