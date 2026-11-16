# guests/n64

The Nintendo 64 guest. State: ROM format pieces only; `relift import` does not
take an N64 image yet.

| Crate | Folder | Does |
|---|---|---|
| `relift-n64-formats` | `formats` | Byte orders and normalization, the header, the boot variant by hash, CRC1/CRC2, Yay0, Yaz0 and MIO0 decoders and a stream scan |
| `relift-n64-tools` | `tools` | `relift-n64rom`: `info`, `streams`, `normalize` |

```sh
cargo build --release -p relift-n64-tools
cargo test -p relift-n64-formats -p relift-n64-tools
relift-n64rom info rom.z64
relift-n64rom info --json rom.v64
relift-n64rom streams rom.n64
relift-n64rom normalize rom.v64 rom.z64
```

The boot code area (`0x40..0x1000`) is only hashed to name the boot variant;
relift never reads or interprets it. The checksum check is available for the
variants whose checksum is computed from the game's own data (6101, 6102,
6103, 6106); for the others it reports `not checked`. `info --cic 6102`
checks as that variant, for images with a custom boot code.

Tests build their input with `synth`; no game data is used.
