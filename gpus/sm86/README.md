# gpus/sm86

NVIDIA SM86 (Ampere, the Switch 2 GPU) shader code: a decoder for the 128-bit
SASS words that prints nvdisasm's text form, and a translator from SASS to
SPIR-V (`relift-gpu-sm86`). Field positions follow Mesa NAK's SM70+ encoder
(`src/nouveau/compiler/nak/sm70_encode.rs`, MIT, Collabora).

The translator takes vertex, tessellation control, tessellation evaluation,
geometry, fragment, task, mesh and compute programs: structured control flow, the
integer, float, half and conversion ALU, MUFU, stage inputs and outputs,
constant banks, global, shared and local memory, atomics, warp operations,
textures and surfaces, for a host profile (see `translate::HostProfile`).
The pipeline state the tessellation and geometry SPH does not carry
(tessellation domain, spacing and output primitives, the geometry input
primitive, the patch size) comes from `translate::Pipeline`. What it cannot
reproduce exactly on that host is rejected with the reason or listed in
`HostReport::inexact`.

```sh
cargo test -p relift-gpu-sm86
```

Task and mesh programs have two back ends from one translation
(`translate::io::mesh`): `SPV_EXT_mesh_shader` where the host's mesh shader
limits fit the pipeline (`HostProfile::mesh_shader`), and compute passes
plus an indirect draw elsewhere (`translate::mesh_fallback`).
`kat/mesh/kat.py` draws known-answer cases on an RTX 3090 with NVIDIA's own
compilation, both translations of its SASS, and compares the images word
for word; `tests/mesh.rs` translates the recorded programs for every host.

`Program::pseudo` holds pseudo-instructions in place of words: instructions
of another GPU (Maxwell's `IPA.MUL`, for one) that no SM86 form matches,
which `translate::pseudo` handles from their operands.

`examples/check.rs` compares the decoder with nvdisasm output on a list of
words; nvdisasm is a local tool and its output is never committed.
