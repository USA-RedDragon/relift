# gpus/sm86

NVIDIA SM86 (Ampere, the Switch 2 GPU) shader code: a decoder for the 128-bit
SASS words that prints nvdisasm's text form, and a translator from SASS to
SPIR-V (`relift-gpu-sm86`). Field positions follow Mesa NAK's SM70+ encoder
(`src/nouveau/compiler/nak/sm70_encode.rs`, MIT, Collabora).

The translator covers a first subset: straight-line vertex and fragment
programs with float ALU, MUFU, attribute loads and stores, interpolation and
user constant banks (`c[3+n]` is uniform binding `n`). Anything else is
rejected with the reason.

```sh
cargo test -p relift-gpu-sm86
```

`examples/check.rs` compares the decoder with nvdisasm output on a list of
words; nvdisasm is a local tool and its output is never committed.
