# cpus/z80

The Z80 (NMOS) frontend (`relift-cpu-z80`): the decoder that picks an
instruction's template (base, CB, ED, DD, FD, DDCB or FDCB page, or a lone
DD/FD prefix) and the lifter to `relift-ir`. Every template is generated from
our own Sail model `sail/z80.sail` (`gen/README.md`); the generated file is
committed.

Each bus access is an `IoLoad`/`IoStore` or a port intrinsic (`z80_in`,
`z80_out`) placed after the T-states that come before its machine cycle, so a
device sees the exact cycle of every access. A conditional `CALL` or `RET`
lifts as two blocks (the taken half at the address plus `HALF`); the four
interrupt entries (NMI, IM 0 with 0xFF on the bus, IM 1, IM 2) are blocks at
`ENTRY` plus the entry. `HALT` calls the per-CPU layer through the intrinsic
`HALT` names.

With `Options::live_operands` (code in RAM whose operands change) an
instruction lifts from its live template: the opcode and prefix bytes are
the lifted ones (`opcode_mask` gives their offsets) and every operand byte
(immediate, address, displacement) is a memory read in its own machine
cycle when the instruction runs, so a jump to an operand address is
computed. Every opcode that reads an operand has a live template (718 of
1786); the test `live_operands_read_the_bytes_in_memory` runs both forms of
every opcode on the IR interpreter and finds them equal, with the bytes
lifted and with other operand bytes in memory.

```sh
cargo test -p relift-cpu-z80
```

The model passes SingleStepTests z80 v1 (1,604,000 of 1,604,000 cases with
WZ, Q, R, the undocumented flags, ports and per-access T-states) and
ZEXDOC/ZEXALL (67 of 67 groups each), measured on the IR interpreter by the
checker of spike 31-sega; `relift-check` runs the same checks.
