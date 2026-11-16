/* RSP (the Nintendo 64 Reality Signal Processor) overlay for the
 * sail-cheri-mips MIPS model.
 *
 * Copyright (c) relift contributors, MIT. The scalar unit is a 32-bit
 * subset of the MIPS R4000 ISA; the vector unit (COP2) follows ares
 * n64/rsp/interpreter-vpu.cpp, whose SSE formulations of the vector
 * instructions are written here over the IR's lane operations. That part
 * is derived from ares:
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *   WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *   ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *   OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Loaded after the model like cpus/mips/gen/vr4300.sail. Functions without
 * a body are cut (cpus/rsp/gen/src/cut.rs): branches, DMEM, the SP
 * registers, BREAK, the divider ROMs and the lane operations.
 *
 * Vector values: a register is bits(128) with element 0 (the RSP's first)
 * in bits 127 .. 112, so element n is IR lane 7 - n and byte k of the
 * register (0 the most significant) is IR byte 15 - k. The accumulator
 * slices ACCH, ACCM and ACCL hold one 16-bit lane per element the same
 * way; the flag registers VCOL, VCOH, VCCL, VCCH and VCE hold all ones in
 * an element's lane when its flag is set.
 */

/* ---- not on the RSP: 64-bit, multiply/divide, likely branches, traps,
   LL/SC, unaligned-pair loads and stores, CACHE, TLB, ERET, R2 additions */
$relift drop_clause decode Some(DADD
$relift drop_clause decode Some(DSUB
$relift drop_clause decode Some(DSLL
$relift drop_clause decode Some(DSRL
$relift drop_clause decode Some(DSRA
$relift drop_clause decode Some(DMULT
$relift drop_clause decode Some(DDIV
$relift drop_clause decode Some(MFHI
$relift drop_clause decode Some(MFLO
$relift drop_clause decode Some(MTHI
$relift drop_clause decode Some(MTLO
$relift drop_clause decode sel, true))
$relift drop_clause decode Some(MUL
$relift drop_clause decode Some(DIV
$relift drop_clause decode Some(MADD
$relift drop_clause decode Some(MSUB
$relift drop_clause decode Some(MOV
$relift drop_clause decode true))
$relift drop_clause decode Some(SYSCALL
$relift drop_clause decode Some(WAIT
$relift drop_clause decode Some(TRAP
$relift drop_clause decode Some(Load(D
$relift drop_clause decode Some(Load(W, true,  true
$relift drop_clause decode Some(Store(D
$relift drop_clause decode Some(Store(W, true
$relift drop_clause decode Some(LWL
$relift drop_clause decode Some(LWR
$relift drop_clause decode Some(SWL
$relift drop_clause decode Some(SWR
$relift drop_clause decode Some(LDL
$relift drop_clause decode Some(LDR
$relift drop_clause decode Some(SDL
$relift drop_clause decode Some(SDR
$relift drop_clause decode Some(CACHE
$relift drop_clause decode Some(SYNC
$relift drop_clause decode Some(HCF
$relift drop_clause decode Some(RDHWR
$relift drop_clause decode Some(ERET
$relift drop_clause decode Some(TLB

/* links are 12-bit IMEM addresses */
$relift patch execute = PC + 8; ==> = (PC + 8) & 0x0000000000000FFF;

/* ---- cuts */
val MIPS_Branch : bits(64) -> unit
val RSP_Cop0Read : bits(5) -> bits(32)
val RSP_Cop0Write : (bits(5), bits(32)) -> unit
val RSP_Break : unit -> unit
val rVPR : bits(5) -> bits(128)
val wVPR : (bits(5), bits(128)) -> unit

/* DMEM for the vector unit: the 16-byte line that holds an address, the 16
   bytes from an address (wrapping at 4 KiB), and their writes of the bytes
   a mask selects; the byte at the lowest address is the most significant */
val rDMEM_line : bits(12) -> bits(128)
val rDMEM16 : bits(12) -> bits(128)
val wDMEM_line : (bits(12), bits(128), bits(128)) -> unit
val wDMEM16 : (bits(12), bits(128), bits(128)) -> unit

/* the divider ROMs: entry i (9 bits) */
val RSP_RcpRom : bits(9) -> bits(16)
val RSP_RsqRom : bits(9) -> bits(16)

/* the byte pattern (lane_permute) of an element selector */
val RSP_ElemPattern : bits(4) -> bits(128)

/* lane operations, each one IR operation (relift_ir::lane). The int is the
   lane width in bits; lane_imm's immediate and the byte patterns of
   lane_permute and lane_permute2 are known when lifting */
enum lane_kind = {
  L_Add, L_Sub, L_AddSatS, L_AddSatU, L_SubSatS, L_SubSatU, L_MulLo,
  L_MulHiS, L_MulHiU, L_MulHiSU, L_And, L_Or, L_Xor, L_AndNot, L_Not,
  L_MinS, L_MaxS, L_Eq, L_Ne, L_Slt, L_Ult, L_Ule, L_ShlImm, L_LShrImm,
  L_AShrImm, L_NarrowSS
}
val lane1 : (lane_kind, int, bits(128)) -> bits(128)
val lane2 : (lane_kind, int, bits(128), bits(128)) -> bits(128)
val lane_imm : (lane_kind, int, int, bits(128)) -> bits(128)
val lane_select : (bits(128), bits(128), bits(128)) -> bits(128)
val lane_permute : (bits(128), bits(128)) -> bits(128)
val lane_permute2 : (bits(128), bits(128), bits(128)) -> bits(128)
val lane_extract16 : (int, bits(128)) -> bits(16)
val lane_insert16 : (bits(128), int, bits(16)) -> bits(128)
val lane_splat16 : bits(16) -> bits(128)
/* rotates left by whole bytes, modulo 16 */
val vrotl : (bits(128), int) -> bits(128)

function execute_branch(pc) = MIPS_Branch(pc)

val mips_run : option(ast) -> unit
function mips_run(x) = match x {
  Some(a) => execute(a),
  None() => ()
}

/* no 64-bit check, no alignment: the 32-bit register file holds
   sign-extended values */
function NotWordVal (word) = false
function isAddressAligned (addr, wordType) = true

/* TLBTranslate and the MEM wrappers stay cuts: St masks the address to
   12 bits and reads and writes DMEM byte by byte, wrapping */

/* no overflow traps on the RSP */
function clause execute (ADD(rs, rt, rd)) = wGPR(rd) = sign_extend(rGPR(rs)[31 .. 0] + rGPR(rt)[31 .. 0])
function clause execute (ADDI(rs, rt, imm)) = wGPR(rt) = sign_extend(rGPR(rs)[31 .. 0] + sign_extend(imm))
function clause execute (SUB(rs, rt, rd)) = wGPR(rd) = sign_extend(rGPR(rs)[31 .. 0] - rGPR(rt)[31 .. 0])
function clause execute (BREAK()) = RSP_Break()

/* MFC0/MTC0: the SP and DP (RDP command) registers, a cut */
function clause execute (MFC0(rt, rd, sel, double)) = wGPR(rt) = sign_extend(RSP_Cop0Read(rd))
function clause execute (MTC0(rt, rd, sel, double)) = RSP_Cop0Write(rd, rGPR(rt)[31 .. 0])

/* ---- COP2: the vector unit */
register ACCH : bits(128)
register ACCM : bits(128)
register ACCL : bits(128)
register VCOL : bits(128)
register VCOH : bits(128)
register VCCL : bits(128)
register VCCH : bits(128)
register VCE : bits(128)
register DIVIN : bits(16)
register DIVOUT : bits(16)
register DIVDP : bits(1)

/* 16-bit lanes */
val v1 : (lane_kind, bits(128)) -> bits(128)
function v1 (k, a) = lane1(k, 16, a)

val v2 : (lane_kind, bits(128), bits(128)) -> bits(128)
function v2 (k, a, b) = lane2(k, 16, a, b)

val vi : (lane_kind, int, bits(128)) -> bits(128)
function vi (k, n, a) = lane_imm(k, 16, n, a)

val vsel : (bits(128), bits(128), bits(128)) -> bits(128)
function vsel (m, a, b) = lane_select(m, a, b)

val vand : (bits(128), bits(128)) -> bits(128)
function vand (a, b) = lane2(L_And, 64, a, b)

val vor : (bits(128), bits(128)) -> bits(128)
function vor (a, b) = lane2(L_Or, 64, a, b)

val vxor : (bits(128), bits(128)) -> bits(128)
function vxor (a, b) = lane2(L_Xor, 64, a, b)

/* a & ~b */
val vandn : (bits(128), bits(128)) -> bits(128)
function vandn (a, b) = lane2(L_AndNot, 64, a, b)

val vnot : bits(128) -> bits(128)
function vnot (a) = lane1(L_Not, 64, a)

/* x in every element */
val rep16 : bits(16) -> bits(128)
function rep16 (x) = x @ x @ x @ x @ x @ x @ x @ x

val rep32 : bits(32) -> bits(128)
function rep32 (x) = x @ x @ x @ x

/* the lane of element n */
val elem_mask : int -> bits(128)
function elem_mask (n) = sail_shiftleft(0x0000000000000000000000000000FFFF, 16 * (7 - n))

/* the top n bytes */
val top_bytes : int -> bits(128)
function top_bytes (n) = sail_shiftleft(ones() : bits(128), 128 - 8 * n)

/* toward higher register bytes (lower addresses) by n bytes, and back */
val bdown : (bits(128), int) -> bits(128)
function bdown (v, n) = sail_shiftright(v, 8 * n)

val bup : (bits(128), int) -> bits(128)
function bup (v, n) = sail_shiftleft(v, 8 * n)

/* vt with the element selector e applied */
val vte : (bits(5), bits(4)) -> bits(128)
function vte (vt, e) = lane_permute(RSP_ElemPattern(e), rVPR(vt))

/* m and h as one 32-bit lane per element, the lower four elements (IR
   lanes 0 to 3) first, then the upper four */
val acc32_lo : (bits(128), bits(128)) -> bits(128)
function acc32_lo (m, h) = lane_permute2(0x17160706151405041312030211100100, m, h)

val acc32_hi : (bits(128), bits(128)) -> bits(128)
function acc32_hi (m, h) = lane_permute2(0x1F1E0F0E1D1C0D0C1B1A0B0A19180908, m, h)

/* the 32-bit lanes of lo and hi back as 16-bit low and high halves */
val half16_lo : (bits(128), bits(128)) -> bits(128)
function half16_lo (lo, hi) = lane_permute2(0x1D1C1918151411100D0C090805040100, lo, hi)

val half16_hi : (bits(128), bits(128)) -> bits(128)
function half16_hi (lo, hi) = lane_permute2(0x1F1E1B1A171613120F0E0B0A07060302, lo, hi)

/* h:m (32 bits per element) shifted right by sh, clamped to 16 bits */
val clamp32 : (bits(128), bits(128), int) -> bits(128)
function clamp32 (m, h, sh) = {
  let lo = acc32_lo(m, h);
  let hi = acc32_hi(m, h);
  if sh == 0 then lane2(L_NarrowSS, 32, lo, hi)
  else lane2(L_NarrowSS, 32, lane_imm(L_AShrImm, 32, sh, lo), lane_imm(L_AShrImm, 32, sh, hi))
}

/* accumulatorSaturate(n, 1, 0x8000, 0x7FFF): the middle slice clamped */
val sat_mid : (bits(128), bits(128)) -> bits(128)
function sat_mid (h, m) = clamp32(m, h, 0)

/* accumulatorSaturate(n, 0, 0x0000, 0xFFFF): the low slice, 0 below and
   0xFFFF above the signed 32-bit range of h:m */
val sat_low : (bits(128), bits(128), bits(128)) -> bits(128)
function sat_low (h, m, l) = {
  let nhi = vi(L_AShrImm, 15, h);
  let nmd = vi(L_AShrImm, 15, m);
  let fits = vand(v2(L_Eq, nhi, nmd), v2(L_Eq, nhi, h));
  vsel(fits, l, vnot(nhi))
}

/* VMULF, VMULU: (vs * vte * 2 + 0x8000) to the accumulator */
val vmulf : (bool, bits(128), bits(128)) -> bits(128)
function vmulf (u, a, b) = {
  let lo = v2(L_MulLo, a, b);
  let hi = v2(L_MulHiS, a, b);
  let lo2 = v2(L_Add, lo, lo);
  let s = v2(L_Add, vi(L_LShrImm, 15, lo), vi(L_LShrImm, 15, lo2));
  ACCL = v2(L_Add, lo2, rep16(0x8000));
  let m = v2(L_Add, vi(L_ShlImm, 1, hi), s);
  ACCM = m;
  let neg = vi(L_AShrImm, 15, m);
  let same = v2(L_Eq, a, b);
  let h = vandn(neg, same);
  ACCH = h;
  if u then vandn(vor(m, neg), h) else v2(L_Add, m, vand(same, neg))
}

/* VMACF, VMACU: the accumulator plus vs * vte * 2 */
val vmacf : (bool, bits(128), bits(128)) -> bits(128)
function vmacf (u, a, b) = {
  let lo = v2(L_MulLo, a, b);
  let hi = v2(L_MulHiS, a, b);
  let pl = vi(L_ShlImm, 1, lo);
  let pm = vor(vi(L_ShlImm, 1, hi), vi(L_LShrImm, 15, lo));
  let ph = vi(L_AShrImm, 15, hi);
  let l = v2(L_Add, ACCL, pl);
  let c0 = v2(L_Ult, l, pl);
  let pm2 = v2(L_Sub, pm, c0);
  let ph2 = v2(L_Sub, ph, vand(c0, v2(L_Eq, pm2, zeros())));
  let m = v2(L_Add, ACCM, pm2);
  let c1 = v2(L_Ult, m, pm2);
  let h = v2(L_Sub, v2(L_Add, ACCH, ph2), c1);
  ACCL = l; ACCM = m; ACCH = h;
  if u then
    vor(v2(L_Slt, zeros(), h), vandn(vor(vi(L_AShrImm, 15, m), m), vi(L_AShrImm, 15, h)))
  else sat_mid(h, m)
}

/* VMADM (signed vs) and VMADN (signed vte): the accumulator plus the
   32-bit product; hi is its upper half */
val vmad_mn : (bool, bits(128), bits(128)) -> bits(128)
function vmad_mn (n, a, b) = {
  let lo = v2(L_MulLo, a, b);
  let hi = if n then v2(L_MulHiSU, b, a) else v2(L_MulHiSU, a, b);
  let l = v2(L_Add, ACCL, lo);
  let c0 = v2(L_Ult, l, lo);
  let hi2 = v2(L_Sub, hi, c0);
  let m = v2(L_Add, ACCM, hi2);
  let c1 = v2(L_Ult, m, hi2);
  let h = v2(L_Sub, v2(L_Add, ACCH, vi(L_AShrImm, 15, hi2)), c1);
  ACCL = l; ACCM = m; ACCH = h;
  if n then sat_low(h, m, l) else sat_mid(h, m)
}

/* VMADL: the accumulator plus the high half of the unsigned product */
val vmadl : (bits(128), bits(128)) -> bits(128)
function vmadl (a, b) = {
  let p = v2(L_MulHiU, a, b);
  let l = v2(L_Add, ACCL, p);
  let c0 = v2(L_Ult, l, p);
  let m = v2(L_Sub, ACCM, c0);
  let h = v2(L_Sub, ACCH, vand(c0, v2(L_Eq, m, zeros())));
  ACCL = l; ACCM = m; ACCH = h;
  sat_low(h, m, l)
}

/* VMADH: the upper 32 bits of the accumulator plus the signed product */
val vmadh : (bits(128), bits(128)) -> bits(128)
function vmadh (a, b) = {
  let lo = v2(L_MulLo, a, b);
  let hi = v2(L_MulHiS, a, b);
  let m = v2(L_Add, ACCM, lo);
  let c = v2(L_Ult, m, lo);
  let h = v2(L_Add, ACCH, v2(L_Sub, hi, c));
  ACCM = m; ACCH = h;
  sat_mid(h, m)
}

/* VMULQ: the signed product, rounded toward zero by 31 when negative */
val vmulq : (bits(128), bits(128)) -> bits(128)
function vmulq (a, b) = {
  let lo = v2(L_MulLo, a, b);
  let hi = v2(L_MulHiS, a, b);
  let lo2 = v2(L_Add, lo, vand(vi(L_AShrImm, 15, hi), rep16(0x001F)));
  let hi2 = v2(L_Sub, hi, v2(L_Ult, lo2, lo));
  ACCL = zeros(); ACCM = lo2; ACCH = hi2;
  vand(clamp32(lo2, hi2, 1), rep16(0xFFF0))
}

/* VMACQ: the upper 32 bits of the accumulator moved by 32 toward zero
   when bit 5 is clear */
val vmacq_step : bits(128) -> bits(128)
function vmacq_step (p) = {
  let b5 = v2(L_Eq, lane2(L_And, 64, p, rep32(0x00000020)), zeros());
  let neg = lane_imm(L_AShrImm, 32, 31, p);
  let big = lane2(L_Slt, 32, rep32(0x0000001F), p);
  let up = vand(b5, neg);
  let down = vand(b5, big);
  lane2(L_Sub, 32, lane2(L_Add, 32, p, vand(up, rep32(0x00000020))), vand(down, rep32(0x00000020)))
}

val vmacq : unit -> bits(128)
function vmacq () = {
  let p0 = vmacq_step(acc32_lo(ACCM, ACCH));
  let p1 = vmacq_step(acc32_hi(ACCM, ACCH));
  ACCM = half16_lo(p0, p1);
  ACCH = half16_hi(p0, p1);
  vand(lane2(L_NarrowSS, 32, lane_imm(L_AShrImm, 32, 1, p0), lane_imm(L_AShrImm, 32, 1, p1)), rep16(0xFFF0))
}

/* VRNDP (positive accumulators) and VRNDN (negative ones): the accumulator
   plus vte, or vte << 16 when bit 0 of the vs field is set */
val vrnd : (bool, bits(5), bits(128)) -> bits(128)
function vrnd (p, vs, x) = {
  let h0 = ACCH;
  let m0 = ACCM;
  let l0 = ACCL;
  let ext = vi(L_AShrImm, 15, x);
  let neg = vi(L_AShrImm, 15, h0);
  let take = if p then vnot(neg) else neg;
  if vs[0] == bitone then {
    let m = v2(L_Add, m0, x);
    let h = v2(L_Sub, v2(L_Add, h0, ext), v2(L_Ult, m, x));
    ACCM = vsel(take, m, m0);
    ACCH = vsel(take, h, h0)
  } else {
    let l = v2(L_Add, l0, x);
    let d = v2(L_Sub, ext, v2(L_Ult, l, x));
    let m = v2(L_Add, m0, d);
    let h = v2(L_Sub, v2(L_Add, h0, vi(L_AShrImm, 15, d)), v2(L_Ult, m, d));
    ACCL = vsel(take, l, l0);
    ACCM = vsel(take, m, m0);
    ACCH = vsel(take, h, h0)
  };
  sat_mid(ACCH, ACCM)
}

/* VADD, VSUB: with the carry (VCOL), signed-clamped */
val vadd : (bits(128), bits(128)) -> bits(128)
function vadd (a, b) = {
  let co = VCOL;
  ACCL = v2(L_Sub, v2(L_Add, a, b), co);
  VCOL = zeros(); VCOH = zeros();
  v2(L_AddSatS, v2(L_SubSatS, v2(L_MinS, a, b), co), v2(L_MaxS, a, b))
}

val vsub : (bits(128), bits(128)) -> bits(128)
function vsub (a, b) = {
  let co = VCOL;
  let ud = v2(L_Sub, b, co);
  let sd = v2(L_SubSatS, b, co);
  ACCL = v2(L_Sub, a, ud);
  VCOL = zeros(); VCOH = zeros();
  v2(L_AddSatS, v2(L_SubSatS, a, sd), v2(L_Slt, ud, sd))
}

/* VABS: vte with the sign of vs, 0 where vs is 0 */
val vabs : (bits(128), bits(128)) -> bits(128)
function vabs (a, b) = {
  let s = vi(L_AShrImm, 15, a);
  let x = vxor(vandn(b, v2(L_Eq, a, zeros())), s);
  ACCL = v2(L_Sub, x, s);
  v2(L_SubSatS, x, s)
}

/* VADDC, VSUBC: unsigned, the carry or borrow to VCOL */
val vaddc : (bits(128), bits(128)) -> bits(128)
function vaddc (a, b) = {
  let s = v2(L_Add, a, b);
  ACCL = s;
  VCOL = v2(L_Ult, s, a); VCOH = zeros();
  s
}

val vsubc : (bits(128), bits(128)) -> bits(128)
function vsubc (a, b) = {
  let d = v2(L_Sub, a, b);
  ACCL = d;
  VCOL = v2(L_Ult, a, b); VCOH = v2(L_Ne, a, b);
  d
}

/* the operations the RSP does not have (VSUT, VADDB, ... VINSN) */
val vzero : (bits(128), bits(128)) -> bits(128)
function vzero (a, b) = {
  ACCL = v2(L_Add, a, b);
  zeros()
}

/* VLT, VEQ, VNE, VGE: VCCL is the condition; vs where it holds, else vte */
val vcmp : (bits(2), bits(128), bits(128)) -> bits(128)
function vcmp (op, a, b) = {
  let eq = v2(L_Eq, a, b);
  let c : bits(128) = match op {
    0b00 => vor(v2(L_Slt, a, b), vand(vand(VCOH, VCOL), eq)),
    0b01 => vandn(eq, VCOH),
    0b10 => vor(vand(VCOH, eq), vnot(eq)),
    _ => vor(v2(L_Slt, b, a), vandn(eq, vand(VCOH, VCOL)))
  };
  VCCL = c;
  VCCH = zeros(); VCOL = zeros(); VCOH = zeros();
  let r = vsel(c, a, b);
  ACCL = r;
  r
}

/* VCL: clip low (the second step of a double-precision clip) */
val vcl : (bits(128), bits(128)) -> bits(128)
function vcl (a, b) = {
  let col = VCOL;
  let coh = VCOH;
  let ce = VCE;
  let nvt = v2(L_Sub, vxor(b, col), col);
  let diff = v2(L_Sub, a, nvt);
  let ncarry = v2(L_Eq, diff, v2(L_AddSatU, a, b));
  let diff0 = v2(L_Eq, diff, zeros());
  let lec1 = vandn(vand(diff0, ncarry), ce);
  let lec2 = vand(ce, vor(diff0, ncarry));
  let le = vsel(vandn(col, coh), vor(lec1, lec2), VCCL);
  let ge = vsel(vor(col, coh), VCCH, v2(L_Ule, b, a));
  let r = vsel(vsel(col, le, ge), nvt, a);
  VCCH = ge; VCCL = le;
  VCOL = zeros(); VCOH = zeros(); VCE = zeros();
  ACCL = r;
  r
}

/* VCH: clip high (the first step of a double-precision clip) */
val vch : (bits(128), bits(128)) -> bits(128)
function vch (a, b) = {
  let sign = vi(L_AShrImm, 15, vxor(a, b));
  let nvt = v2(L_Sub, vxor(b, sign), sign);
  let diff = v2(L_Sub, a, nvt);
  let diff0 = v2(L_Eq, diff, zeros());
  let vtn = vi(L_AShrImm, 15, b);
  let gt = v2(L_Slt, zeros(), diff);
  let cch = vsel(sign, vtn, vor(gt, diff0));
  let ccl = vsel(sign, vnot(gt), vtn);
  let ce = vand(v2(L_Eq, diff, sign), sign);
  let r = vsel(vsel(sign, ccl, cch), nvt, a);
  VCOL = sign; VCOH = vnot(vor(diff0, ce));
  VCCH = cch; VCCL = ccl; VCE = ce;
  ACCL = r;
  r
}

/* VCR: clip with ones' complement negation */
val vcr : (bits(128), bits(128)) -> bits(128)
function vcr (a, b) = {
  let sign = vi(L_AShrImm, 15, vxor(a, b));
  let ccl = vi(L_AShrImm, 15, v2(L_Add, vand(a, sign), b));
  let cch = v2(L_Eq, v2(L_MinS, vor(a, sign), b), b);
  let r = vsel(vsel(sign, ccl, cch), vxor(b, sign), a);
  VCCL = ccl; VCCH = cch;
  VCOL = zeros(); VCOH = zeros(); VCE = zeros();
  ACCL = r;
  r
}

/* VMRG: vs where VCCL is set, else vte */
val vmrg : (bits(128), bits(128)) -> bits(128)
function vmrg (a, b) = {
  let r = vsel(VCCL, a, b);
  VCOL = zeros(); VCOH = zeros();
  ACCL = r;
  r
}

/* VAND ... VNXOR */
val vlogic : (bits(3), bits(128), bits(128)) -> bits(128)
function vlogic (op, a, b) = {
  let r : bits(128) = match op {
    0b000 => vand(a, b),
    0b001 => vnot(vand(a, b)),
    0b010 => vor(a, b),
    0b011 => vnot(vor(a, b)),
    0b100 => vxor(a, b),
    _ => vnot(vxor(a, b))
  };
  ACCL = r;
  r
}

/* VRCP, VRCPL, VRSQ, VRSQL: the reciprocal (rsq false) or inverse square
   root of element e & 7 of vt, or with the high half from VRCPH or VRSQH
   (low: true, after one), to element de of vd and DIVOUT */
val vdiv : (bool, bool, bits(4), bits(5), bits(3), bits(5)) -> unit
function vdiv (rsq, low, e, vt, de, vd) = {
  let x = lane_extract16(7 - unsigned(e[2 .. 0]), rVPR(vt));
  let input : bits(32) = if low & (DIVDP == 0b1) then DIVIN @ x else sign_extend(x);
  let mask : bits(32) = sail_arith_shiftright(input, 31);
  let data0 = input ^ mask;
  let data = if signed(input) > negate(32768) then data0 - mask else data0;
  let shift = count_leading_zeros(data);
  let index : bits(32) = sail_shiftright(sail_shiftleft(data, shift) & 0x7FC00000, 22);
  let r : bits(32) = if rsq then {
    let rom = RSP_RsqRom(index[8 .. 1] @ get_slice_int(1, shift, 0));
    sail_shiftright(sail_shiftleft(0x00010000 | zero_extend(rom), 14), (31 - shift) / 2) ^ mask
  } else {
    let rom = RSP_RcpRom(index[8 .. 0]);
    sail_shiftright(sail_shiftleft(0x00010000 | zero_extend(rom), 14), 31 - shift) ^ mask
  };
  let result : bits(32) = if data == 0x00000000 then 0x7FFFFFFF
    else if input == 0xFFFF8000 then 0xFFFF0000
    else r;
  DIVDP = 0b0;
  DIVOUT = result[31 .. 16];
  ACCL = vte(vt, e);
  wVPR(vd, lane_insert16(rVPR(vd), 7 - unsigned(de), result[15 .. 0]))
}

/* VRCPH, VRSQH: DIVOUT to element de of vd; element e & 7 of vt is the
   high half of the next VRCPL or VRSQL */
val vdivh : (bits(4), bits(5), bits(3), bits(5)) -> unit
function vdivh (e, vt, de, vd) = {
  let x = lane_extract16(7 - unsigned(e[2 .. 0]), rVPR(vt));
  let out = DIVOUT;
  ACCL = vte(vt, e);
  DIVDP = 0b1;
  DIVIN = x;
  wVPR(vd, lane_insert16(rVPR(vd), 7 - unsigned(de), out))
}

/* VMOV: element de of vte to element de of vd */
val vmov : (bits(4), bits(5), bits(3), bits(5)) -> unit
function vmov (e, vt, de, vd) = {
  let b = vte(vt, e);
  ACCL = b;
  wVPR(vd, vsel(elem_mask(unsigned(de)), b, rVPR(vd)))
}

/* VSAR: a slice of the accumulator (e 8, 9, 10), else 0 */
val vsar : bits(4) -> bits(128)
function vsar (e) = match e {
  0x8 => ACCH,
  0x9 => ACCM,
  0xA => ACCL,
  _ => zeros()
}

/* every computational instruction: COP2 with bit 25 set, by function */
union clause ast = VOP : (bits(6), bits(4), regno, regno, regno)

val vop : (bits(6), bits(4), bits(5), bits(5), bits(5)) -> unit
function vop (f, e, vt, vs, vd) = {
  let de = vs[2 .. 0];
  match f {
    0b110000 => vdiv(false, false, e, vt, de, vd),
    0b110001 => vdiv(false, true, e, vt, de, vd),
    0b110010 => vdivh(e, vt, de, vd),
    0b110011 => vmov(e, vt, de, vd),
    0b110100 => vdiv(true, false, e, vt, de, vd),
    0b110101 => vdiv(true, true, e, vt, de, vd),
    0b110110 => vdivh(e, vt, de, vd),
    0b110111 => (),
    0b111111 => (),
    0b011101 => wVPR(vd, vsar(e)),
    0b001011 => wVPR(vd, vmacq()),
    0b000010 => wVPR(vd, vrnd(true, vs, vte(vt, e))),
    0b001010 => wVPR(vd, vrnd(false, vs, vte(vt, e))),
    _ => {
      let a = rVPR(vs);
      let b = vte(vt, e);
      let r : bits(128) = match f {
        0b000000 => vmulf(false, a, b),
        0b000001 => vmulf(true, a, b),
        0b000011 => vmulq(a, b),
        0b000100 => {
          let l = v2(L_MulHiU, a, b);
          ACCL = l; ACCM = zeros(); ACCH = zeros();
          l
        },
        0b000101 => {
          let m = v2(L_MulHiSU, a, b);
          ACCL = v2(L_MulLo, a, b); ACCM = m; ACCH = vi(L_AShrImm, 15, m);
          m
        },
        0b000110 => {
          let l = v2(L_MulLo, a, b);
          let m = v2(L_MulHiSU, b, a);
          ACCL = l; ACCM = m; ACCH = vi(L_AShrImm, 15, m);
          l
        },
        0b000111 => {
          let m = v2(L_MulLo, a, b);
          let h = v2(L_MulHiS, a, b);
          ACCL = zeros(); ACCM = m; ACCH = h;
          sat_mid(h, m)
        },
        0b001000 => vmacf(false, a, b),
        0b001001 => vmacf(true, a, b),
        0b001100 => vmadl(a, b),
        0b001101 => vmad_mn(false, a, b),
        0b001110 => vmad_mn(true, a, b),
        0b001111 => vmadh(a, b),
        0b010000 => vadd(a, b),
        0b010001 => vsub(a, b),
        0b010011 => vabs(a, b),
        0b010100 => vaddc(a, b),
        0b010101 => vsubc(a, b),
        0b100000 => vcmp(0b00, a, b),
        0b100001 => vcmp(0b01, a, b),
        0b100010 => vcmp(0b10, a, b),
        0b100011 => vcmp(0b11, a, b),
        0b100100 => vcl(a, b),
        0b100101 => vch(a, b),
        0b100110 => vcr(a, b),
        0b100111 => vmrg(a, b),
        0b101000 => vlogic(0b000, a, b),
        0b101001 => vlogic(0b001, a, b),
        0b101010 => vlogic(0b010, a, b),
        0b101011 => vlogic(0b011, a, b),
        0b101100 => vlogic(0b100, a, b),
        0b101101 => vlogic(0b101, a, b),
        _ => vzero(a, b)
      };
      wVPR(vd, r)
    }
  }
}

function clause execute (VOP(f, e, vt, vs, vd)) = vop(f, e, vt, vs, vd)

function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b000000) = Some(VOP(0b000000, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b000001) = Some(VOP(0b000001, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b000010) = Some(VOP(0b000010, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b000011) = Some(VOP(0b000011, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b000100) = Some(VOP(0b000100, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b000101) = Some(VOP(0b000101, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b000110) = Some(VOP(0b000110, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b000111) = Some(VOP(0b000111, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b001000) = Some(VOP(0b001000, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b001001) = Some(VOP(0b001001, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b001010) = Some(VOP(0b001010, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b001011) = Some(VOP(0b001011, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b001100) = Some(VOP(0b001100, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b001101) = Some(VOP(0b001101, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b001110) = Some(VOP(0b001110, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b001111) = Some(VOP(0b001111, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b010000) = Some(VOP(0b010000, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b010001) = Some(VOP(0b010001, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b010010) = Some(VOP(0b010010, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b010011) = Some(VOP(0b010011, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b010100) = Some(VOP(0b010100, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b010101) = Some(VOP(0b010101, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b010110) = Some(VOP(0b010110, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b010111) = Some(VOP(0b010111, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b011000) = Some(VOP(0b011000, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b011001) = Some(VOP(0b011001, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b011010) = Some(VOP(0b011010, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b011011) = Some(VOP(0b011011, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b011100) = Some(VOP(0b011100, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b011101) = Some(VOP(0b011101, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b011110) = Some(VOP(0b011110, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b011111) = Some(VOP(0b011111, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b100000) = Some(VOP(0b100000, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b100001) = Some(VOP(0b100001, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b100010) = Some(VOP(0b100010, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b100011) = Some(VOP(0b100011, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b100100) = Some(VOP(0b100100, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b100101) = Some(VOP(0b100101, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b100110) = Some(VOP(0b100110, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b100111) = Some(VOP(0b100111, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b101000) = Some(VOP(0b101000, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b101001) = Some(VOP(0b101001, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b101010) = Some(VOP(0b101010, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b101011) = Some(VOP(0b101011, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b101100) = Some(VOP(0b101100, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b101101) = Some(VOP(0b101101, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b101110) = Some(VOP(0b101110, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b101111) = Some(VOP(0b101111, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b110000) = Some(VOP(0b110000, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b110001) = Some(VOP(0b110001, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b110010) = Some(VOP(0b110010, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b110011) = Some(VOP(0b110011, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b110100) = Some(VOP(0b110100, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b110101) = Some(VOP(0b110101, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b110110) = Some(VOP(0b110110, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b110111) = Some(VOP(0b110111, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b111000) = Some(VOP(0b111000, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b111001) = Some(VOP(0b111001, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b111010) = Some(VOP(0b111010, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b111011) = Some(VOP(0b111011, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b111100) = Some(VOP(0b111100, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b111101) = Some(VOP(0b111101, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b111110) = Some(VOP(0b111110, e, vt, vs, vd))
function clause decode (0b010010 @ 0b1 @ e : bits(4) @ vt : regno @ vs : regno @ vd : regno @ 0b111111) = Some(VOP(0b111111, e, vt, vs, vd))

/* the bit of each element's flag in CFC2 and CTC2: element n is bit n of
   the low byte and bit n of the high byte */
val flags_lo : unit -> bits(128)
function flags_lo () = 0x00010002000400080010002000400080

val flags_hi : unit -> bits(128)
function flags_hi () = 0x01000200040008001000200040008000

/* MFC2, MTC2 (e is a byte of the register), CFC2, CTC2 (VCO, VCC, VCE) */
union clause ast = C2MOVE : (bits(2), regno, regno, bits(4))

function clause execute (C2MOVE(op, rt, r, e)) = {
  let k = unsigned(e);
  match op {
    0b00 => wGPR(rt) = sign_extend(vrotl(rVPR(r), k)[127 .. 112]),
    0b01 => {
      let x = rGPR(rt);
      let d = sail_shiftright(sail_shiftleft(zero_extend(x[15 .. 0]) : bits(128), 112), 8 * k);
      wVPR(r, vsel(bdown(top_bytes(2), k), d, rVPR(r)))
    },
    0b10 => {
      let lo : bits(128) = if r[1 .. 0] == 0b00 then VCOL else if r[1 .. 0] == 0b01 then VCCL else VCE;
      let hi : bits(128) = if r[1 .. 0] == 0b00 then VCOH else if r[1 .. 0] == 0b01 then VCCH else zeros();
      let x0 = (lo & flags_lo()) | (hi & flags_hi());
      let x1 = x0 | sail_shiftright(x0, 64);
      let x2 = x1 | sail_shiftright(x1, 32);
      let x3 = x2 | sail_shiftright(x2, 16);
      wGPR(rt) = sign_extend(x3[15 .. 0])
    },
    _ => {
      let s = lane_splat16(rGPR(rt)[15 .. 0]);
      let lo = v2(L_Ne, vand(s, flags_lo()), zeros());
      let hi = v2(L_Ne, vand(s, flags_hi()), zeros());
      if r[1 .. 0] == 0b00 then { VCOL = lo; VCOH = hi }
      else if r[1 .. 0] == 0b01 then { VCCL = lo; VCCH = hi }
      else VCE = lo
    }
  }
}

function clause decode (0b010010 @ 0b00000 @ rt : regno @ vs : regno @ e : bits(4) @ _ : bits(7)) = Some(C2MOVE(0b00, rt, vs, e))
function clause decode (0b010010 @ 0b00010 @ rt : regno @ rd : regno @ _ : bits(11)) = Some(C2MOVE(0b10, rt, rd, 0x0))
function clause decode (0b010010 @ 0b00100 @ rt : regno @ vs : regno @ e : bits(4) @ _ : bits(7)) = Some(C2MOVE(0b01, rt, vs, e))
function clause decode (0b010010 @ 0b00110 @ rt : regno @ rd : regno @ _ : bits(11)) = Some(C2MOVE(0b11, rt, rd, 0x0))

/* the address of a vector load or store: the 7-bit offset counts units of
   2^sh bytes */
val vaddr : (bits(5), bits(7), int) -> bits(12)
function vaddr (base, off, sh) = rGPR(base)[11 .. 0] + sail_shiftleft(sign_extend(off) : bits(12), sh)

/* the address with its low three bits clear */
val align8 : bits(12) -> bits(12)
function align8 (a) = a & 0xFF8

/* LBV ... LTV (LWC2; LWV is not on the RSP) */
union clause ast = VLOAD : (bits(5), regno, regno, bits(4), bits(7))

function clause execute (VLOAD(op, base, vt, e, off)) = {
  let k = unsigned(e);
  let v = rVPR(vt);
  match op {
    0b00000 => {
      let a = vaddr(base, off, 0);
      wVPR(vt, vsel(bdown(top_bytes(1), k), bdown(rDMEM16(a), k), v))
    },
    0b00001 => {
      let a = vaddr(base, off, 1);
      wVPR(vt, vsel(bdown(top_bytes(2), k), bdown(rDMEM16(a), k), v))
    },
    0b00010 => {
      let a = vaddr(base, off, 2);
      wVPR(vt, vsel(bdown(top_bytes(4), k), bdown(rDMEM16(a), k), v))
    },
    0b00011 => {
      let a = vaddr(base, off, 3);
      wVPR(vt, vsel(bdown(top_bytes(8), k), bdown(rDMEM16(a), k), v))
    },
    0b00100 => {
      /* LQV: from the address to the end of its line */
      let a = vaddr(base, off, 4);
      let s = unsigned(a[3 .. 0]);
      let d = bdown(bup(rDMEM_line(a), s), k);
      wVPR(vt, vsel(bdown(bup(ones(), s), k), d, v))
    },
    0b00101 => {
      /* LRV: from the start of the line to the address, into the end of
         the register */
      let a = vaddr(base, off, 4);
      let start = 16 + k - unsigned(a[3 .. 0]);
      wVPR(vt, vsel(bdown(ones(), start), bdown(rDMEM_line(a), start), v))
    },
    0b01011 => {
      /* LTV: element i of register (vt & ~7) + ((e / 2 + i) & 7) */
      let a = vaddr(base, off, 4);
      let r = vrotl(rDMEM16(align8(a)), (k + unsigned(a[3 .. 3]) * 8) % 16);
      let g = unsigned(vt) - unsigned(vt) % 8;
      foreach (i from 0 to 7) {
        let n : bits(5) = get_slice_int(5, g + (k / 2 + i) % 8, 0);
        wVPR(n, vsel(elem_mask(i), r, rVPR(n)))
      }
    },
    _ => {
      /* LPV, LUV, LHV, LFV: bytes of the 16 at the address rounded down to
         8, from byte (address & 7) - e on, to the upper bits of elements */
      let a = vaddr(base, off, if op == 0b00110 | op == 0b00111 then 3 else 4);
      let r = vrotl(rDMEM16(align8(a)), unsigned(a[2 .. 0]) - k);
      match op {
        0b00110 => wVPR(vt, lane_permute(0x0F800E800D800C800B800A8009800880, r)),
        0b00111 => wVPR(vt, vi(L_LShrImm, 1, lane_permute(0x0F800E800D800C800B800A8009800880, r))),
        0b01000 => wVPR(vt, vi(L_LShrImm, 1, vand(r, rep16(0xFF00)))),
        _ => {
          let t = vi(L_LShrImm, 1, lane_permute(0x0F800B8007800380078003800F800B80, r));
          wVPR(vt, vsel(bdown(top_bytes(8), k), t, v))
        }
      }
    }
  }
}

function clause decode (0b110010 @ base : regno @ vt : regno @ 0b00000 @ e : bits(4) @ off : bits(7)) = Some(VLOAD(0b00000, base, vt, e, off))
function clause decode (0b110010 @ base : regno @ vt : regno @ 0b00001 @ e : bits(4) @ off : bits(7)) = Some(VLOAD(0b00001, base, vt, e, off))
function clause decode (0b110010 @ base : regno @ vt : regno @ 0b00010 @ e : bits(4) @ off : bits(7)) = Some(VLOAD(0b00010, base, vt, e, off))
function clause decode (0b110010 @ base : regno @ vt : regno @ 0b00011 @ e : bits(4) @ off : bits(7)) = Some(VLOAD(0b00011, base, vt, e, off))
function clause decode (0b110010 @ base : regno @ vt : regno @ 0b00100 @ e : bits(4) @ off : bits(7)) = Some(VLOAD(0b00100, base, vt, e, off))
function clause decode (0b110010 @ base : regno @ vt : regno @ 0b00101 @ e : bits(4) @ off : bits(7)) = Some(VLOAD(0b00101, base, vt, e, off))
function clause decode (0b110010 @ base : regno @ vt : regno @ 0b00110 @ e : bits(4) @ off : bits(7)) = Some(VLOAD(0b00110, base, vt, e, off))
function clause decode (0b110010 @ base : regno @ vt : regno @ 0b00111 @ e : bits(4) @ off : bits(7)) = Some(VLOAD(0b00111, base, vt, e, off))
function clause decode (0b110010 @ base : regno @ vt : regno @ 0b01000 @ e : bits(4) @ off : bits(7)) = Some(VLOAD(0b01000, base, vt, e, off))
function clause decode (0b110010 @ base : regno @ vt : regno @ 0b01001 @ e : bits(4) @ off : bits(7)) = Some(VLOAD(0b01001, base, vt, e, off))
function clause decode (0b110010 @ base : regno @ vt : regno @ 0b01011 @ e : bits(4) @ off : bits(7)) = Some(VLOAD(0b01011, base, vt, e, off))

/* SFV: the elements written, by e; others write zeros */
val sfv_pattern : bits(4) -> bits(128)
function sfv_pattern (e) = {
  let (x0, x1, x2, x3) : (int, int, int, int) = match e {
    0x0 => (0, 1, 2, 3),
    0xF => (0, 1, 2, 3),
    0x1 => (6, 7, 4, 5),
    0x4 => (1, 2, 3, 0),
    0x5 => (7, 4, 5, 6),
    0x8 => (4, 5, 6, 7),
    0xB => (3, 0, 1, 2),
    0xC => (5, 6, 7, 4),
    _ => (8, 8, 8, 8)
  };
  /* window byte 4k takes the low byte of element xk (IR byte 14 - 2 xk) */
  r : bits(128) = 0x80808080808080808080808080808080;
  r[127 .. 120] = get_slice_int(8, if x0 == 8 then 128 else 14 - 2 * x0, 0);
  r[95 .. 88] = get_slice_int(8, if x1 == 8 then 128 else 14 - 2 * x1, 0);
  r[63 .. 56] = get_slice_int(8, if x2 == 8 then 128 else 14 - 2 * x2, 0);
  r[31 .. 24] = get_slice_int(8, if x3 == 8 then 128 else 14 - 2 * x3, 0);
  r
}

/* SBV ... STV (SWC2) */
union clause ast = VSTORE : (bits(5), regno, regno, bits(4), bits(7))

function clause execute (VSTORE(op, base, vt, e, off)) = {
  let k = unsigned(e);
  let v = rVPR(vt);
  match op {
    0b00000 => wDMEM16(vaddr(base, off, 0), vrotl(v, k), top_bytes(1)),
    0b00001 => wDMEM16(vaddr(base, off, 1), vrotl(v, k), top_bytes(2)),
    0b00010 => wDMEM16(vaddr(base, off, 2), vrotl(v, k), top_bytes(4)),
    0b00011 => wDMEM16(vaddr(base, off, 3), vrotl(v, k), top_bytes(8)),
    0b00100 => {
      /* SQV: to the end of the line */
      let a = vaddr(base, off, 4);
      let s = unsigned(a[3 .. 0]);
      wDMEM_line(a, bdown(vrotl(v, k), s), bdown(ones(), s))
    },
    0b00101 => {
      /* SRV: from the start of the line to the address */
      let a = vaddr(base, off, 4);
      let s = unsigned(a[3 .. 0]);
      wDMEM_line(a, vrotl(v, k + 16 - s), bup(ones(), 16 - s))
    },
    0b00110 => {
      /* SPV: the upper bytes of the elements, then the elements >> 7 */
      let x = lane_permute2(0x0F0D0B09070503011E1C1A1816141210, v, vi(L_LShrImm, 7, v));
      wDMEM16(vaddr(base, off, 3), vrotl(x, k), top_bytes(8))
    },
    0b00111 => {
      /* SUV: the elements >> 7, then the upper bytes */
      let x = lane_permute2(0x1E1C1A18161412100F0D0B0907050301, v, vi(L_LShrImm, 7, v));
      wDMEM16(vaddr(base, off, 3), vrotl(x, k), top_bytes(8))
    },
    0b01000 => {
      /* SHV: bits 14 .. 7 of each element from byte e on, to every other
         byte of the 16 at the address rounded down to 8 */
      let a = vaddr(base, off, 4);
      let x = vand(vi(L_ShlImm, 1, vrotl(v, k)), rep16(0xFF00));
      let n = 16 - unsigned(a[2 .. 0]);
      wDMEM16(align8(a), vrotl(x, n), vrotl(rep16(0xFF00), n))
    },
    0b01001 => {
      /* SFV: elements >> 7 to every fourth byte */
      let a = vaddr(base, off, 4);
      let x = lane_permute(sfv_pattern(e), vi(L_LShrImm, 7, v));
      let n = 16 - unsigned(a[2 .. 0]);
      wDMEM16(align8(a), vrotl(x, n), vrotl(0xFF000000FF000000FF000000FF000000, n))
    },
    0b01010 => {
      /* SWV: the register from byte e on, rotated to the address */
      let a = vaddr(base, off, 4);
      wDMEM16(align8(a), vrotl(v, k + 16 - unsigned(a[2 .. 0])), ones())
    },
    _ => {
      /* STV: element (i - e / 2) & 7 of register (vt & ~7) + i, as
         element i, rotated to the address */
      let a = vaddr(base, off, 4);
      let g = unsigned(vt) - unsigned(vt) % 8;
      let h = k / 2;
      u : bits(128) = rVPR(get_slice_int(5, g + h % 8, 0));
      foreach (m from 1 to 7) {
        u = vsel(elem_mask(m), rVPR(get_slice_int(5, g + (m + h) % 8, 0)), u)
      };
      wDMEM16(align8(a), vrotl(u, 16 - unsigned(a[2 .. 0])), ones())
    }
  }
}

function clause decode (0b111010 @ base : regno @ vt : regno @ 0b00000 @ e : bits(4) @ off : bits(7)) = Some(VSTORE(0b00000, base, vt, e, off))
function clause decode (0b111010 @ base : regno @ vt : regno @ 0b00001 @ e : bits(4) @ off : bits(7)) = Some(VSTORE(0b00001, base, vt, e, off))
function clause decode (0b111010 @ base : regno @ vt : regno @ 0b00010 @ e : bits(4) @ off : bits(7)) = Some(VSTORE(0b00010, base, vt, e, off))
function clause decode (0b111010 @ base : regno @ vt : regno @ 0b00011 @ e : bits(4) @ off : bits(7)) = Some(VSTORE(0b00011, base, vt, e, off))
function clause decode (0b111010 @ base : regno @ vt : regno @ 0b00100 @ e : bits(4) @ off : bits(7)) = Some(VSTORE(0b00100, base, vt, e, off))
function clause decode (0b111010 @ base : regno @ vt : regno @ 0b00101 @ e : bits(4) @ off : bits(7)) = Some(VSTORE(0b00101, base, vt, e, off))
function clause decode (0b111010 @ base : regno @ vt : regno @ 0b00110 @ e : bits(4) @ off : bits(7)) = Some(VSTORE(0b00110, base, vt, e, off))
function clause decode (0b111010 @ base : regno @ vt : regno @ 0b00111 @ e : bits(4) @ off : bits(7)) = Some(VSTORE(0b00111, base, vt, e, off))
function clause decode (0b111010 @ base : regno @ vt : regno @ 0b01000 @ e : bits(4) @ off : bits(7)) = Some(VSTORE(0b01000, base, vt, e, off))
function clause decode (0b111010 @ base : regno @ vt : regno @ 0b01001 @ e : bits(4) @ off : bits(7)) = Some(VSTORE(0b01001, base, vt, e, off))
function clause decode (0b111010 @ base : regno @ vt : regno @ 0b01010 @ e : bits(4) @ off : bits(7)) = Some(VSTORE(0b01010, base, vt, e, off))
function clause decode (0b111010 @ base : regno @ vt : regno @ 0b01011 @ e : bits(4) @ off : bits(7)) = Some(VSTORE(0b01011, base, vt, e, off))
