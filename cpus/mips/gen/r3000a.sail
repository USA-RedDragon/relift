/* R3000A (the PlayStation CPU) overlay for the sail-cheri-mips MIPS model.
 *
 * Copyright (c) relift contributors, MIT. Behavior references: the LSI
 * LR33300 and R3000 manuals and psx-spx.
 *
 * A stub that proves the profile seam: it keeps the MIPS I decode clauses
 * and gives the `int` class its R3000A rules. The other classes, RFE, the
 * R3000A COP0 and the GTE (COP2) come later (cpus/mips/README.md).
 *
 * Loaded after the model like vr4300.sail: every `function` here replaces
 * the model's function of that name, a `function clause` goes before the
 * model's clauses of its function, and the `$relift` lines edit the model
 * in place or remove decode clauses that are not MIPS I.
 */

$relift profile r3000a

/* ---- not MIPS I (MIPS32/64 R2, BERI and CHERI additions) */
$relift drop_clause decode Some(MUL(
$relift drop_clause decode Some(MADD(
$relift drop_clause decode Some(MADDU(
$relift drop_clause decode Some(MSUB(
$relift drop_clause decode Some(MSUBU(
$relift drop_clause decode Some(MOVN(
$relift drop_clause decode Some(MOVZ(
$relift drop_clause decode Some(WAIT(
$relift drop_clause decode Some(RDHWR(
$relift drop_clause decode Some(HCF(
$relift drop_clause decode Some(PREF(

/* ---- not MIPS I (MIPS II and III) */
$relift drop_clause decode Some(DADDIU(
$relift drop_clause decode Some(DADDU(
$relift drop_clause decode Some(DADDI(
$relift drop_clause decode Some(DADD(
$relift drop_clause decode Some(DSUBU(
$relift drop_clause decode Some(DSUB(
$relift drop_clause decode Some(DSLL(
$relift drop_clause decode Some(DSLL32(
$relift drop_clause decode Some(DSLLV(
$relift drop_clause decode Some(DSRA(
$relift drop_clause decode Some(DSRA32(
$relift drop_clause decode Some(DSRAV(
$relift drop_clause decode Some(DSRL(
$relift drop_clause decode Some(DSRL32(
$relift drop_clause decode Some(DSRLV(
$relift drop_clause decode Some(DMULT(
$relift drop_clause decode Some(DMULTU(
$relift drop_clause decode Some(DDIV(
$relift drop_clause decode Some(DDIVU(
$relift drop_clause decode Some(BEQ(rs, rt, imm, false, true))
$relift drop_clause decode Some(BEQ(rs, rt, imm, true , true))
$relift drop_clause decode Some(BCMPZ(rs, imm, LT, false, true))
$relift drop_clause decode Some(BCMPZ(rs, imm, LT, true, true))
$relift drop_clause decode Some(BCMPZ(rs, imm, GE, false, true))
$relift drop_clause decode Some(BCMPZ(rs, imm, GE, true, true))
$relift drop_clause decode Some(BCMPZ(rs, imm, GT, false, true))
$relift drop_clause decode Some(BCMPZ(rs, imm, LE, false, true))
$relift drop_clause decode Some(TRAPREG(
$relift drop_clause decode Some(TRAPIMM(
$relift drop_clause decode Some(Load(W, false, false
$relift drop_clause decode Some(Load(D,
$relift drop_clause decode true, base, rt, offset))
$relift drop_clause decode Some(Store(D,
$relift drop_clause decode Some(LDL(
$relift drop_clause decode Some(LDR(
$relift drop_clause decode Some(SDL(
$relift drop_clause decode Some(SDR(
$relift drop_clause decode Some(CACHE(
$relift drop_clause decode Some(SYNC(
$relift drop_clause decode sel, true))
$relift drop_clause decode Some(ERET(

/* ---- no TLB on the console: the TLB instructions are reserved */
$relift drop_clause decode Some(TLBWI(
$relift drop_clause decode Some(TLBWR(
$relift drop_clause decode Some(TLBR(
$relift drop_clause decode Some(TLBP(

val mips_run : option(ast) -> unit
function mips_run(x) = match x {
  Some(a) => execute(a),
  None() => SignalException(ResI)
}

/* the registers are always sign-extended 32-bit values */
function NotWordVal (word) = false

enum Exception = {
   Interrupt, TLBMod, TLBL, TLBS, AdEL, AdES, Sys, Bp, ResI, CpU, Ov, Tr, C2E, C2Trap,
   XTLBRefillL, XTLBRefillS, XTLBInvL, XTLBInvS, MCheck, FPE
}

/* DIV family: results for a zero divisor (psx-spx) */
function clause execute (DIV(rs, rt)) = {
  let a = rGPR(rs);
  let n = signed(a[31 .. 0]);
  let d = signed(rGPR(rt)[31 .. 0]);
  if d == 0 then {
    LO = if n < 0 then 0x0000000000000001 else 0xFFFFFFFFFFFFFFFF;
    HI = sign_extend(a[31 .. 0])
  } else {
    LO = sign_extend(get_slice_int(32, quot_round_zero(n, d), 0));
    HI = sign_extend(get_slice_int(32, rem_round_zero(n, d), 0))
  }
}
function clause execute (DIVU(rs, rt)) = {
  let a = rGPR(rs);
  let n = unsigned(a[31 .. 0]);
  let d = unsigned(rGPR(rt)[31 .. 0]);
  if d == 0 then {
    LO = 0xFFFFFFFFFFFFFFFF;
    HI = sign_extend(a[31 .. 0])
  } else {
    LO = sign_extend(get_slice_int(32, quot_round_zero(n, d), 0));
    HI = sign_extend(get_slice_int(32, rem_round_zero(n, d), 0))
  }
}
