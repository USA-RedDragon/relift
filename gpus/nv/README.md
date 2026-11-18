# gpus/nv

What relift's two NVIDIA shader front ends share (`relift-gpu-nv`): decoded
instructions (`Inst` and its operands, with nvdisasm's text form), the
values of an instruction the translator reads beside its operands
(`fields::Fields`), programs and their shader program headers (`program`),
and the translator from those instructions to SPIR-V (`translate`).

The SM86 decoder (`gpus/sm86`) produces these instructions from Ampere
words, and the SM53 lowering (`gpus/sm53`) produces them from Maxwell
instructions. Where no SM86 form behaves like a Maxwell form, the lowering
uses a pseudo-instruction (Maxwell's `IPA.MUL`, for one), which
`translate::pseudo` handles from its operands.

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

Task and mesh programs have two back ends from one translation
(`translate::io::mesh`): `SPV_EXT_mesh_shader` where the host's mesh shader
limits fit the pipeline (`HostProfile::mesh_shader`), and compute passes
plus an indirect draw elsewhere (`translate::mesh_fallback`).

The translator's tests run SM86 programs and live with the SM86 decoder
(`gpus/sm86/tests`, `gpus/sm86/kat`):

```sh
cargo test -p relift-gpu-sm86
```
