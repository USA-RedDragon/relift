# cpus/sm83

The SM83 frontend (the Game Boy and Game Boy Color CPU): an opcode table with
length, M-cycles taken and not taken, flags and the bus access of each
M-cycle, a decoder, and a lifter to `relift-ir` (`relift-cpu-sm83`). The
lifter has one template per opcode, generated from our own Sail model
`sail/sm83.sail` (`gen/README.md`); the generated file is committed.

The Game Boy runs in the exact mode: every bus access goes through the device
library and a sync point closes each instruction (`docs/abi.md` section 13).

```sh
cargo test -p relift-cpu-sm83
```

`check/` compares the lifted IR with rustyboi-core
(`relift-check --oracle rustyboi lift`). The C half is `runtime/cpus/sm83`.
