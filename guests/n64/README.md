# guests/n64

The Nintendo 64 guest. State: ROM format pieces and static code discovery;
`relift import` does not take an N64 image yet.

| Crate | Folder | Does |
|---|---|---|
| `relift-n64-formats` | `formats` | Byte orders and normalization, the header, the boot variant by hash, CRC1/CRC2, Yay0, Yaz0 and MIO0 decoders and a stream scan |
| `relift-n64-analyze` | `analyze` | Code discovery: the VR4300 IR over abstract values from the boot entry, the PI DMA fixed point, the ROM-wide region scan with the link address vote, relocation masks, the sweep and the RSP image list |
| `relift-n64-tools` | `tools` | `relift-n64rom`: `info`, `streams`, `normalize`, `functions` |

```sh
cargo build --release -p relift-n64-tools
cargo test -p relift-n64-formats -p relift-n64-analyze -p relift-n64-tools
relift-n64rom info rom.z64
relift-n64rom info --json rom.v64
relift-n64rom streams rom.n64
relift-n64rom normalize rom.v64 rom.z64
relift-n64rom functions rom.z64
relift-n64rom functions --json --cic 6102 rom.z64
```

`functions` prints the entry point, the counts, the constant PI DMAs, the RSP
images and one line per function entry (address, source, source offset,
origin), then the transfers left unknown. With `--json` it also lists the
segments and the code units: each unit is a run of code words with its
source (the ROM or a decoded stream), offset, link address and a hash of the
words with the relocation fields cleared, the key the runtime binds code by
when it finds those bytes in RDRAM. The analysis reads no boot code and uses
no per-game table.

The boot code area (`0x40..0x1000`) is only hashed to name the boot variant;
relift never reads or interprets it. The checksum check is available for the
variants whose checksum is computed from the game's own data (6101, 6102,
6103, 6106); for the others it reports `not checked`. `info --cic 6102`
checks as that variant, for images with a custom boot code.

Tests build their input with `synth`; no game data is used.
