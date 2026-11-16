# Z80 lifting templates from Sail

`src/lift/generated.rs` in `cpus/z80` holds one Rust function per opcode of
every page (base 252, CB 256, ED 256, DD 252, FD 252, DDCB 256, FDCB 256),
per interrupt entry (4) and for a lone DD/FD prefix (2), generated from the
Sail model in `cpus/z80/sail/z80.sail` (our own work, MIT, with ares' ISC
notice for the parts ported from its Z80 core) by this crate on the shared
Sail frontend `core/sail`. Neither the generator nor Sail is needed to build.

The functions declared in `z80.sail` with `val` and no body are the cut line
(`src/cut.rs`): the registers through `St`, the instruction's own bytes
(known at lift time), memory and port reads and writes at the T-state of
their machine cycle, the instruction's total, branches, the condition of a
conditional `CALL` or `RET`, and the per-CPU layer's wait. Their lift-time
side is `cpus/z80/src/lift/rt.rs`. In an interrupt entry PC is the return
address, read from the `ret` slot.

Regenerate from the repository root, with the Sail library that
`cpus/arm64/gen/fetch-sail.sh` downloads:

    cpus/arm64/gen/fetch-sail.sh "$DIR"
    cpus/z80/gen/regen.sh "$DIR/lib"

`try` prints the templates of named opcodes (`3c`, `cb7e`, `edb0`, `dd34`,
`ddcb06`, `irq2`, `lonedd`) or why one fails:

    relift-cpu-z80-gen try "$DIR/lib" cpus/z80/sail/z80.sail edb0 irq2
