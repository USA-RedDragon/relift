# Known-answer vectors

`tests/kat/*.kat` hold what an RTX 3090 (CUDA, sm_86) computed for small SASS
programs, one file per family of instructions. `tests/kat.rs` translates each
program for several hosts, validates the SPIR-V with `spirv-val` (when it is
on the path) and runs it in a small SPIR-V interpreter
(`tests/kat/spvi.rs`) under each denormal behavior the host may have; every
output must equal the device's, bit for bit.

## Programs

Every program is the same wrapper around a few instructions (`sass.py`):
thread `i` loads four words from `in[4i..]` into R4 to R7, runs the
variant's instructions, and stores up to sixteen registers to `out[16i..]`.
A family shares its input rows across its variants. The rows mix edge
values (signed zeros, denormals, the normal and overflow edges, infinities,
quiet and signaling NaNs with payloads, integer limits) with inputs built to
hit halfway cases, cancellation and the denormal and overflow edges of a
product, plus random values. `gen.py` lists the families:

| file | instructions |
|---|---|
| fadd, fmul, ffma | every rounding mode with and without FTZ, SAT, the FMUL scales, operand negation and absolute value |
| fcmp | FMNMX (min and max, FTZ), FSETP with every comparison, plain and FTZ |
| f2i, f2i_f16 | F2I from F32 and from either F16 half to S8..S64 and U8..U64 in every rounding mode, FTZ, NTZ |
| i2f | I2F from 8 to 64-bit integers (and byte and half selectors) to F32 and F16 in every rounding mode, I2FP |
| f2f, frnd | F2F F32 to F16 (every mode, FTZ) and F16 to F32, FRND on F32 and F16 |
| f2fp | F2FP.PACK_AB to F16 and BF16, RZ, RELU, SATFINITE |
| half | HADD2, HMUL2, HFMA2 (FTZ, SAT, RELU, FMZ, lane selects, negation), HADD2.F32, HMNMX2 |
| int, shf | IADD3 and IADD3.X carries, IMAD (wide, high, with carry), LEA (HI, X, SX32, negated), IMNMX, IABS, FLO, POPC, BREV, ISETP chains, PRMT in every mode, every SHF form |
| mufu_* | MUFU.RCP, RSQ, SQRT, EX2, LG2, SIN, COS on 2600 inputs each (every table index, the exponent range, the special values); the .F16 forms; RCP64H and RSQ64H |

On SM86 the device rejects F2F.F32.F32, and HADD2, HMUL2 and HFMA2 with
`.F32` except `HADD2.F32 d, -RZ, b`; those have no vectors.

## Recording

    python3 -I kat/gen.py <absolute out dir> [family ...]

needs nvcc and the device. It compiles `skeleton.cu` (a kernel that only
serves as a container) and `runner.cpp`, which loads the skeleton with
each program's words in place of its code through the CUDA driver API and
runs one thread per row. The inputs are seeded per family, so a rerun on
the same device writes the same files. Copy the files to `tests/kat/` and
add a new family to the `families!` list in `tests/kat.rs`.

The file layout is in `tests/kat/file.rs`: the program words, the inputs,
then each output column XORed with the closest earlier column and
run-length coded, which keeps the whole set under 1 MB.

## MUFU tables

`src/translate/mufu/tables.rs` is written by

    python3 -I kat/mufu_tables.py <absolute work dir> <absolute path of tables.rs>

which runs MUFU on every input of each table's range on the device (about
80 million inputs, a few seconds) and solves each row, with numpy and scipy,
in about fifteen seconds. The mufu_* vectors check the translated model
against independent device outputs.

## Task and mesh programs

    CARGO_TARGET_DIR=<dir> python3 -I kat/mesh/kat.py <absolute work dir> [--record tests/kat/mesh.txt] [case ...]

needs the NVIDIA device with `VK_EXT_mesh_shader` and `VK_NV_mesh_shader`,
glslangValidator, spirv-val, zstd, the Vulkan headers and a release build of
`relift-shaders` and the `mesh` example. Each case under `kat/mesh/cases`
(GLSL task, mesh and fragment shaders and `case.json`: the pipeline state,
the draw and the blend) is drawn by the driver, which also leaves its SASS
in the pipeline cache; `relift-shaders` carves the programs from it, both
back ends translate them, and `mrun.c` draws the native translation and the
compute back end. All three images must agree word for word (blending makes
the primitive order visible). `--record` writes the SASS and pipelines that
`tests/mesh.rs` translates for every host.

    KAT_DEV=llvmpipe KAT_HOST=lavapipe python3 -I kat/mesh/kat.py <absolute work dir> --replay tests/kat/mesh.txt

translates the recorded programs for a host preset and draws both back ends
on that device, which must agree word for word; it needs no NVIDIA device.
CI runs it on a pinned lavapipe build (`scripts/ci/mesh-kat.sh`).

Cost on a 3090, for draws of 4000 and 16000 tasks of 1 to 4 meshes of 49
to 64 triangles (10000 and 40000 meshes), one command buffer, steady state:
native task and mesh 74 and 248 microseconds; compute back end 133 and 469
(task pass 9 and 16, task scan 14 and 34, mesh pass 53 and 195, indexed
draw 67 and 235), 1.8 and 1.9 times the native time; with two mesh
workgroups per compute workgroup 125 and 444. On an Adreno 740 (subgroups
of 64 to 128, GPU at 680 MHz) the compute back end takes 5.6 and 22.5
milliseconds with one guest workgroup per workgroup and 4.0 and 16.1 with
two in full subgroups of 64 (four in subgroups of 128: 4.2 and 16.9); the
mesh pass is most of it.

`KAT_SLOTS=<n>` translates the compute back end for `n` guest workgroups per
workgroup and `KAT_SUBGROUP=<size>` runs its passes in full subgroups of
that size.
