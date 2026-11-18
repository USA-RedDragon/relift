# gpus/sm86

NVIDIA SM86 (Ampere, the Switch 2 GPU) shader code: a decoder for the 128-bit
SASS words into the instructions of `relift-gpu-nv`, whose text form is
nvdisasm's (`relift-gpu-sm86`), and the parse of program bytes
(`program::parse`). Field positions follow Mesa NAK's SM70+ encoder
(`src/nouveau/compiler/nak/sm70_encode.rs`, MIT, Collabora). The translator
to SPIR-V is in [gpus/nv](../nv/README.md); its tests are here, since they
run SM86 programs.

```sh
cargo test -p relift-gpu-sm86
```

`kat/mesh/kat.py` draws known-answer cases on an RTX 3090 with NVIDIA's own
compilation, both translations of its SASS (mesh shader and compute
fallback), and compares the images word for word; `tests/mesh.rs`
translates the recorded programs for every host.

`examples/check.rs` compares the decoder with nvdisasm output on a list of
words; nvdisasm is a local tool and its output is never committed.
