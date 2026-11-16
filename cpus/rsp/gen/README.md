# RSP lifting templates from Sail

`src/decode/generated.rs` (the decode table) and `src/lift/generated/*.rs`
(one Rust function per encoding, one file per class: `int`, `ldst`,
`branch`, `system`, `vector`, `vmem`) are generated from the MIPS part of
the Sail CHERI-MIPS model (sail-cheri-mips, BSD-2-Clause, see
`NOTICE-sail-cheri-mips`) plus the RSP overlay `rsp.sail` by the
`relift-cpu-rsp-gen` crate in this folder. Neither the generator nor the
Sail sources are needed to build; `generated/mod.rs` and `decode/mod.rs`
are hand-written.

The generator is the RSP layer over `relift-sail` (`core/sail`), built like
the VR4300 one (`cpus/mips/gen`): `Rsp` in `src/cut.rs` implements
`relift_sail::eval::Cpu` and `relift_sail::stage::Stage`, and `src/main.rs`
builds one decode group from the model's `decode` clauses and gives every
clause its RSP mnemonic. It fails when the model's clauses and the
mnemonic table differ, or when two clauses share a word.

## The overlay

`rsp.sail` is loaded after the model with `Model::load_overlay`. For the
scalar unit it drops every clause the RSP does not have (64-bit operations,
multiply and divide, likely branches, traps, LL/SC, the unaligned loads and
stores, CACHE, the TLB and ERET), masks links to 12-bit IMEM addresses,
removes overflow traps and alignment checks, and makes MFC0, MTC0 and
BREAK cuts (the SP and DP registers are the runtime's).

It adds all of COP2, with the behavior of ares (ISC, notice in the file):
the 64 computational functions (the ones the RSP does not have write 0 and
the sum to the accumulator), MFC2, MTC2, CFC2 and CTC2, LBV to LTV and SBV
to STV. LWV is not on the RSP, so its word decodes to nothing.

### Vector values

A vector register is `bits(128)` with element 0 (the RSP's first, its
lowest address) in bits 127 to 112. The RSP's big-endian element `n` is
therefore IR lane `7 - n` (`relift_ir::lane` numbers lanes from the least
significant bits), and byte `k` of a register is IR byte `15 - k`. The
accumulator is three registers of one 16-bit lane per element (`ACCH`,
`ACCM`, `ACCL`); the flags are five registers of masks (`VCOL`, `VCOH`,
`VCCL`, `VCCH`, `VCE`), all ones in an element's lane when the flag is set,
so they feed selects directly. `CFC2` and `CTC2` convert between masks and
the packed 16-bit form.

### Lane builtins

The vector instructions are written in Sail over builtins that the cut
maps one to one to IR lane operations, so a template is a short sequence of
`Op::Lane` and `Op::Permute` with no per-element loop:

- `lane1(kind, width, a)`, `lane2(kind, width, a, b)` and
  `lane_imm(kind, width, imm, a)`: one `LaneOp`. `kind` is a member of the
  overlay's `lane_kind` enum, named `L_` plus the `LaneKind` variant;
  `width` is the lane width in bits (64 for the bitwise kinds); `imm` must
  be known when lifting.
- `lane_select(mask, a, b)`, `lane_extract16(i, v)`,
  `lane_insert16(v, i, x)` and `lane_splat16(x)`.
- `lane_permute(pattern, v)` and `lane_permute2(pattern, a, b)`: a byte
  permutation whose pattern (the `Permute` pattern as a 128-bit value) is
  known when lifting. `RSP_ElemPattern(e)` gives the pattern of an element
  selector, computed when lifting; the identity costs nothing.
- `vrotl(v, n)`: a rotation by whole bytes, one IR rotate.

The formulations of the multiply-accumulate family, the clips and the
compares follow ares' SSE paths: carries between the 16-bit accumulator
slices are unsigned compares, and the clamps of the 32-bit middle of the
accumulator are a signed narrowing of the slices interleaved to 32-bit
lanes. Vector loads and stores read and write whole 16-byte DMEM lines
(`rDMEM_line`, `rDMEM16`, `wDMEM_line`, `wDMEM16`) with masks, so no access
is split into bytes.

### The divider

VRCP and VRSQ read the RSP's reciprocal and inverse square root ROMs. No
table is stored: `RSP_RcpRom` and `RSP_RsqRom` compute an entry from the
formula that defines the ROM, in IR (`St::rcp_rom`, `St::rsq_rom`), and
`rt::rcp_rom` and `rt::rsq_rom` give the same tables at lift time; the
tests compare both with the tables built the way ares builds them.

## Regenerate

From the repository root, with the files of the VR4300 generator:

    cpus/mips/gen/fetch-sail.sh "$DIR"
    cpus/rsp/gen/regen.sh "$DIR/lib" "$DIR/model"

`fetch-sail.sh` and its `sources.txt` are shared with `cpus/mips/gen`. CI
runs both and fails when the committed files differ. `try SAIL_LIB
MODEL_SRC OVERLAY NAME...` prints the template of named encodings (by Rust
name or mnemonic) or why it fails, and `list SAIL_LIB MODEL_SRC OVERLAY -`
prints every decode clause's constructor and fixed bits.
