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
