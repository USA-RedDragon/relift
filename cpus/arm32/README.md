# cpus/arm32

The ARMv4T frontend (the ARM7TDMI of the Game Boy Advance, ARM and Thumb
states): a decoder and a lifter to `relift-ir` (`relift-cpu-arm32`). The
decode tables and the lifting templates are generated from the AArch32 part
of sail-arm and an ARMv4T overlay that carries the ARM7TDMI rules where the
Armv8 model differs, including rules measured on a console
(`gen/README.md`). The generated files are committed and never edited by
hand. A lifted instruction reads and writes the register file of `src/regs.rs`
(the current mode's registers, the banked registers and the PSRs), so a mode
change is IR like any other write.

```sh
cargo test -p relift-cpu-arm32
cargo run -p relift-cpu-arm32 --example step    # one instruction per input line, for model checks
```

State: no guest crate links this frontend yet, and there is no runtime CPU
layer for it (planned).
