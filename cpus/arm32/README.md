# cpus/arm32

The 32-bit Arm frontend, ARM and Thumb states, in two profiles: ARMv4T (the
ARM7TDMI of the Game Boy Advance and the NDS ARM7) and ARMv5TE (the
ARM946E-S of the NDS ARM9): a decoder and a lifter to `relift-ir`
(`relift-cpu-arm32`). The decode tables and the lifting templates are
generated from the AArch32 part of sail-arm and one overlay per profile
that carries the CPU's rules where the Armv8 model differs, including rules
measured on a console (`gen/README.md`). The generated files are committed and never edited by
hand. A lifted instruction reads and writes the register file of `src/regs.rs`
(the current mode's registers, the banked registers and the PSRs), so a mode
change is IR like any other write.

```sh
cargo test -p relift-cpu-arm32
cargo run -p relift-cpu-arm32 --example step    # one instruction per input line, for model checks (--v5te)
```

State: the Game Boy Advance guest links the ARMv4T profile; the ARMv5TE
profile is checked against qemu-arm (`-cpu arm946`) but no guest links it
yet.
