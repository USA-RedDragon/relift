# gpus/sm53

NVIDIA SM53 (Maxwell GM20B, the Switch GPU) shader code as NVN programs hold
it (`relift-gpu-sm53`): the 0x30-byte code header, the 0x50-byte shader
program header, code in 32-byte bundles (a scheduling word, then three
64-bit instructions), and the instruction forms.

The opcode patterns are Ryujinx's `InstTable` (MIT, Copyright (c) Ryujinx
Team and Contributors) plus three encodings from envytools' `gm107.c` (MIT); the scheduling
fields follow Mesa NAK's SM50 encoder (MIT, Collabora). See NOTICE.

```sh
cargo test -p relift-gpu-sm53
```
