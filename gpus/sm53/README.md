# gpus/sm53

NVIDIA SM53 (Maxwell GM20B, the Switch GPU) shader code as NVN programs hold
it (`relift-gpu-sm53`): the 0x30-byte code header, the 0x50-byte shader
program header, code in 32-byte bundles (a scheduling word, then three
64-bit instructions), the instruction forms and their operand fields, and a
text form.

The opcode patterns are Ryujinx's `InstTable` (MIT, Copyright (c) Ryujinx
Team and Contributors) plus three encodings from envytools' `gm107.c` (MIT); the scheduling
fields follow Mesa NAK's SM50 encoder (MIT, Collabora). `gen.py <Ryujinx
checkout>` writes `src/form.rs` and `src/field.rs` from Ryujinx's
`InstTable.cs` and `InstDecoders.cs`. The text form uses the spelling of
envytools' `envydis -m gm107 -V sm52` so the decoder can be compared with it
word for word; envydis is a local tool and its output is never committed.
See NOTICE.

[LOWERING.md](LOWERING.md) lists how each Maxwell form reaches the
translator of `relift-gpu-nv`: lowered to exactly one SM86 form, a pseudo-instruction, both
by case, or a stop, each with its evidence. It is written from
`src/lower/table.rs` by `RELIFT_BLESS=1 cargo test -p relift-gpu-sm53 --test
lowering`; the test fails while it is stale. `lower::lower` turns one
instruction into its SM86 word, its pseudo-instruction (see
`relift_gpu_nv::translate::pseudo`) or a named stop, and `lower::program` a
whole program into a `relift_gpu_nv` `Program` the translator runs, after the control-flow pass (`lower::flow`) has resolved
the SSY/SYNC and PBK/BRK reconvergence stack into BSSY and branches.

```sh
cargo test -p relift-gpu-sm53
```
