# gpus/sm86

NVIDIA SM86 (Ampere, the Switch 2 GPU) shader code: a decoder for the 128-bit
SASS words that prints nvdisasm's text form, and a translator from SASS to
SPIR-V (`relift-gpu-sm86`). Field positions follow Mesa NAK's SM70+ encoder
(`src/nouveau/compiler/nak/sm70_encode.rs`, MIT, Collabora).

The translator takes vertex, fragment and compute programs: structured
control flow, the integer, float, half and conversion ALU, MUFU, stage
inputs and outputs, constant banks, global, shared and local memory,
atomics, warp operations, textures and surfaces, for a host profile (see
`translate::HostProfile`). What it cannot reproduce exactly on that host is
rejected with the reason or listed in `HostReport::inexact`.

```sh
cargo test -p relift-gpu-sm86
```

`examples/check.rs` compares the decoder with nvdisasm output on a list of
words; nvdisasm is a local tool and its output is never committed.
