# cpus/rsp

The RSP frontend (the Nintendo 64 Reality Signal Processor: a 32-bit MIPS
scalar unit and a vector unit of eight 16-bit lanes): a decoder and a
lifter to `relift-ir` (`relift-cpu-rsp`). The decode table and the lifting
templates are generated from the MIPS part of sail-cheri-mips and an RSP
overlay (`gen/README.md`); the vector unit is written over the IR's lane
operations. The generated files are committed and never edited by hand. A
lifted instruction reads and writes the register file of `src/regs.rs` and
the 4 KiB of DMEM; a branch or jump is lifted with its delay slot into one
block (`src/lift/rt.rs`).

```sh
cargo test -p relift-cpu-rsp
```

The tests run every COP2 instruction's template on the IR interpreter
against a per-element reference written from ares, on random and edge
states, and replay hardware known answers from n64-systemtest.

State: no guest crate links this frontend yet, and there is no runtime
layer for the RSP (DMEM, the SP registers, the RDP interface).
