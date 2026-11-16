# cpus/mips

The VR4300 frontend (the Nintendo 64 CPU, MIPS III with its FPU): a decoder
and a lifter to `relift-ir` (`relift-cpu-mips`). The decode table and the
lifting templates are generated from the MIPS part of sail-cheri-mips and a
VR4300 overlay (`gen/README.md`). The generated files are committed and
never edited by hand. A lifted instruction reads and writes the register
file of `src/vr4300/regs.rs`; a branch or jump is lifted with its delay slot into
one block, and exceptions, the TLB and the FPU's slow path leave the block
through guards before any effect (`src/vr4300/lift/rt.rs`).

```sh
cargo test -p relift-cpu-mips
```

State: no guest crate links this frontend yet, there is no runtime CPU
layer for it, and the lifted-versus-oracle check is not in `check/` yet.
