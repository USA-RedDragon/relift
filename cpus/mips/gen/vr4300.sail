/* VR4300 (the Nintendo 64 CPU) overlay for the sail-cheri-mips MIPS model.
 *
 * Copyright (c) relift contributors, MIT. Behavior references: the NEC
 * VR4300 manual and ares (ISC) n64/cpu.
 *
 * Loaded after the model: every `function` here replaces the model's
 * function of that name, a `function clause` goes before the model's
 * clauses of its function, and the `$relift` lines edit the model in place
 * (`patch`) or remove decode clauses that are not VR4300 (`drop_clause`).
 *
 * Functions named `MIPS_*` without a body, and `rFPR`/`wFPR`, are cut
 * (cpus/mips/gen/src/cut.rs): branches, the FPU, the TLB, CACHE, Random
 * and the interrupt check after a CP0 write.
 */

/* ---- not VR4300 (MIPS32/64 R2, BERI, CHERI additions) */
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

/* ---- VR4300 differences inside model text */
/* SC stores whenever LLbit is set (no LLAddr compare); LL keeps PA >> 4 */
$relift patch execute CP0LLBit[0] & (CP0LLAddr == pAddr) ==> CP0LLBit[0]
$relift patch execute CP0LLAddr = pAddr; ==> CP0LLAddr = pAddr >> 4;

/* ---- cut functions (the lifter's St and the runtime) */
val MIPS_Branch : bits(64) -> unit
val MIPS_Cache : (bits(5), bits(64)) -> unit
val MIPS_Tlb : bits(2) -> unit
val MIPS_Random : unit -> bits(64)
val MIPS_Cp0Sync : unit -> unit
val MIPS_Cop1Usable : unit -> unit
val MIPS_Fp : (bits(6), bits(5), bits(64), bits(64)) -> bits(64)
val MIPS_FpCmp : (bits(4), bits(5), bits(64), bits(64)) -> unit
val MIPS_FpUnimpl : unit -> unit
val rFPR : bits(5) -> bits(64)
val wFPR : (bits(5), bits(64)) -> unit

function execute_branch(pc) = MIPS_Branch(pc)

val mips_run : option(ast) -> unit
function mips_run(x) = match x {
  Some(a) => execute(a),
  None() => SignalException(ResI)
}

/* 32-bit operations read the low word of any 64-bit value */
function NotWordVal (word) = false

/* natural alignment (BERI relaxes it) */
function isAddressAligned (addr, wordType) = match wordType {
  B => true,
  H => addr[0] == bitzero,
  W => addr[1 .. 0] == 0b00,
  D => addr[2 .. 0] == 0b000
}

enum Exception = {
   Interrupt, TLBMod, TLBL, TLBS, AdEL, AdES, Sys, Bp, ResI, CpU, Ov, Tr, C2E, C2Trap,
   XTLBRefillL, XTLBRefillS, XTLBInvL, XTLBInvS, MCheck, FPE
}

bitfield StatusReg : bits(32) = {
  CU : 31 .. 28,
  RP : 27,
  FR : 26,
  RE : 25,
  DS : 24 .. 16,
  BEV : 22,
  IM : 15 .. 8,
  KX : 7,
  SX : 6,
  UX : 5,
  KSU : 4 .. 3,
  ERL : 2,
  EXL : 1,
  IE : 0
}

/* SRA/SRAV shift the whole 64-bit register, then sign-extend the low word */
function clause execute (SRA(rt, rd, sa)) = {
  wGPR(rd) = sign_extend((rGPR(rt) >>_s sa)[31 .. 0])
}
function clause execute (SRAV(rs, rt, rd)) = {
  wGPR(rd) = sign_extend((rGPR(rt) >>_s (rGPR(rs)[4 .. 0]))[31 .. 0])
}

/* MULT: a 64 x 35-bit product (ares: hardware with unextended operands) */
function clause execute (MULT(rs, rt)) = {
  let t : bits(64) = sign_extend(rGPR(rt)[34 .. 0]);
  let p : bits(128) = rGPR(rs) *_s t;
  LO = sign_extend(p[31 .. 0]);
  HI = sign_extend(p[63 .. 32])
}

/* DIV family: VR4300 results for a zero divisor */
function clause execute (DIV(rs, rt)) = {
  let a = rGPR(rs);
  let n = signed(a[31 .. 0]);
  let d = signed(rGPR(rt));
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
function clause execute (DDIV(rs, rt)) = {
  let a = rGPR(rs);
  let n = signed(a);
  let d = signed(rGPR(rt));
  if d == 0 then {
    LO = if n < 0 then 0x0000000000000001 else 0xFFFFFFFFFFFFFFFF;
    HI = a
  } else {
    LO = get_slice_int(64, quot_round_zero(n, d), 0);
    HI = get_slice_int(64, rem_round_zero(n, d), 0)
  }
}
function clause execute (DDIVU(rs, rt)) = {
  let a = rGPR(rs);
  let n = unsigned(a);
  let d = unsigned(rGPR(rt));
  if d == 0 then {
    LO = 0xFFFFFFFFFFFFFFFF;
    HI = a
  } else {
    LO = get_slice_int(64, quot_round_zero(n, d), 0);
    HI = get_slice_int(64, rem_round_zero(n, d), 0)
  }
}

/* LWR: a partial word keeps bits 63..32 (VR4300, as ares), a whole word
   sign-extends */
function clause execute(LWR(base, rt, offset)) = {
  let (vAddr, size) = addrWrapperUnaligned(sign_extend(offset) + rGPR(base), LoadData, WR);
  let pAddr = (TLBTranslate(vAddr, LoadData)) in {
    reg_val = rGPR(rt);
    match size {
      1 => wGPR(rt) = reg_val[63 .. 8] @ MEMr_wrapper(pAddr, 1),
      2 => wGPR(rt) = reg_val[63 .. 16] @ MEMr_wrapper(pAddr, 2),
      3 => wGPR(rt) = reg_val[63 .. 24] @ MEMr_wrapper(pAddr, 3),
      _ => wGPR(rt) = sign_extend(MEMr_wrapper(pAddr, 4))
    }
  }
}

/* CACHE: a cut (the runtime keeps no caches, but code invalidation and
   cache-line effects are its business) */
function clause execute (CACHE(base, op, imm)) = {
  checkCP0Access();
  MIPS_Cache(op, rGPR(base) + sign_extend(imm))
}

/* TLB instructions: cut (32-entry VR4300 TLB in the runtime) */
function clause execute (TLBR()) = { checkCP0Access(); MIPS_Tlb(0b00) }
function clause execute (TLBWI()) = { checkCP0Access(); MIPS_Tlb(0b01) }
function clause execute (TLBWR()) = { checkCP0Access(); MIPS_Tlb(0b10) }
function clause execute (TLBP()) = { checkCP0Access(); MIPS_Tlb(0b11) }

/* ---- CP0 register file of the VR4300 */
register CP0Index : bits(64)
register CP0EntryLo0 : bits(64)
register CP0EntryLo1 : bits(64)
register CP0Context : bits(64)
register CP0PageMask : bits(64)
register CP0Wired : bits(64)
register CP0EntryHi : bits(64)
register CP0Config : bits(64)
register CP0WatchLo : bits(64)
register CP0WatchHi : bits(64)
register CP0XContext : bits(64)
register CP0ParityErr : bits(64)
register CP0TagLo : bits(64)
register CP0Latch : bits(64)

val cp0_read : bits(5) -> bits(64)
function cp0_read (rd) = match rd {
  0b00000 => CP0Index,
  0b00001 => MIPS_Random(),
  0b00010 => CP0EntryLo0,
  0b00011 => CP0EntryLo1,
  0b00100 => CP0Context,
  0b00101 => CP0PageMask,
  0b00110 => CP0Wired,
  0b01000 => CP0BadVAddr,
  0b01001 => zero_extend(CP0Count),
  0b01010 => CP0EntryHi,
  0b01011 => zero_extend(CP0Compare),
  0b01100 => zero_extend(CP0Status.bits),
  0b01101 => zero_extend(CP0Cause.bits),
  0b01110 => CP0EPC,
  0b01111 => 0x0000000000000B22,
  0b10000 => (CP0Config & 0x000000007F00800F) | 0x0000000000066460,
  0b10001 => CP0LLAddr,
  0b10010 => CP0WatchLo,
  0b10011 => CP0WatchHi,
  0b10100 => CP0XContext,
  0b11010 => CP0ParityErr,
  0b11011 => 0x0000000000000000,
  0b11100 => CP0TagLo,
  0b11101 => 0x0000000000000000,
  0b11110 => CP0ErrorEPC,
  _ => CP0Latch
}

val cp0_write : (bits(5), bits(64)) -> unit
function cp0_write (rd, v) = {
  CP0Latch = v;
  match rd {
    0b00000 => CP0Index = v & 0x000000008000003F,
    0b00010 => CP0EntryLo0 = v & 0x000000003FFFFFFF,
    0b00011 => CP0EntryLo1 = v & 0x000000003FFFFFFF,
    0b00100 => CP0Context = (v & 0xFFFFFFFFFF800000) | (CP0Context & 0x00000000007FFFF0),
    0b00101 => CP0PageMask = v & 0x0000000001FFE000,
    0b00110 => CP0Wired = v & 0x000000000000003F,
    0b01001 => CP0Count = v[31 .. 0],
    0b01010 => CP0EntryHi = v & 0xC00000FFFFFFE0FF,
    0b01011 => {
      CP0Compare = v[31 .. 0];
      CP0Cause = Mk_CauseReg(CP0Cause.bits & 0xFFFF7FFF);
      MIPS_Cp0Sync()
    },
    0b01100 => {
      CP0Status = Mk_StatusReg((v[31 .. 0] & 0xFF57FFFF) | (CP0Status.bits & 0x00A80000));
      MIPS_Cp0Sync()
    },
    0b01101 => {
      CP0Cause = Mk_CauseReg((CP0Cause.bits & 0xFFFFFCFF) | (v[31 .. 0] & 0x00000300));
      MIPS_Cp0Sync()
    },
    0b01110 => CP0EPC = v,
    0b10000 => CP0Config = (CP0Config & 0x0000000070000000) | (v & 0x000000000F00800F),
    0b10001 => CP0LLAddr = (CP0LLAddr & 0xFFFFFFFF00000000) | (v & 0x00000000FFFFFFFF),
    0b10010 => CP0WatchLo = v & 0x00000000FFFFFFFB,
    0b10011 => CP0WatchHi = v & 0x000000000000000F,
    0b10100 => CP0XContext = (v & 0xFFFFFFFE00000000) | (CP0XContext & 0x00000001FFFFFFF0),
    0b11010 => CP0ParityErr = v & 0x00000000000000FF,
    0b11100 => CP0TagLo = v & 0x00000000FFFFFFFF,
    0b11110 => CP0ErrorEPC = v,
    _ => ()
  }
}

function clause execute (MFC0(rt, rd, sel, double)) = {
  checkCP0Access();
  let r = cp0_read(rd);
  wGPR(rt) = if double then r else sign_extend(r[31 .. 0])
}
function clause execute (MTC0(rt, rd, sel, double)) = {
  checkCP0Access();
  cp0_write(rd, rGPR(rt))
}

/* ---- COP1 (FPU) */
register FCR31 : bits(32)

val fr_mode : unit -> bool
function fr_mode () = CP0Status.FR() == 0b1

/* the move view (MTC1, MFC1, LWC1, SWC1): in FR=0 an odd register is the
   high word of the even one */
val fpr_get32 : bits(5) -> bits(32)
function fpr_get32 (n) =
  if fr_mode() then rFPR(n)[31 .. 0]
  else if n[0] == bitone then rFPR(n & 0b11110)[63 .. 32]
  else rFPR(n)[31 .. 0]

val fpr_set32 : (bits(5), bits(32)) -> unit
function fpr_set32 (n, v) =
  if fr_mode() then wFPR(n, rFPR(n)[63 .. 32] @ v)
  else if n[0] == bitone then wFPR(n & 0b11110, v @ rFPR(n & 0b11110)[31 .. 0])
  else wFPR(n, rFPR(n)[63 .. 32] @ v)

val fpr_get64 : bits(5) -> bits(64)
function fpr_get64 (n) = if fr_mode() then rFPR(n) else rFPR(n & 0b11110)

val fpr_set64 : (bits(5), bits(64)) -> unit
function fpr_set64 (n, v) = if fr_mode() then wFPR(n, v) else wFPR(n & 0b11110, v)

/* the arithmetic source views (as ares, from hardware tests): fs is the
   even register in FR=0, ft and fd are the register named */
val fp_src : (bits(5), bits(5)) -> bits(64)
function fp_src (fmt, n) = {
  let x : bits(64) = if fr_mode() then rFPR(n) else rFPR(n & 0b11110);
  if (fmt == 0b10000) | (fmt == 0b10100) then zero_extend(x[31 .. 0]) else x
}
val fp_src_t : (bits(5), bits(5)) -> bits(64)
function fp_src_t (fmt, n) = {
  let x : bits(64) = rFPR(n);
  if (fmt == 0b10000) | (fmt == 0b10100) then zero_extend(x[31 .. 0]) else x
}

/* the arithmetic destination: register fd itself, a word clears the high half */
val fp_dst : (bits(5), bits(5), bits(64)) -> unit
function fp_dst (fmt, n, v) =
  if (fmt == 0b10000) | (fmt == 0b10100) then wFPR(n, 0x00000000 @ v[31 .. 0]) else wFPR(n, v)

union clause ast = COP1_Move : (bits(5), regno, regno)
function clause decode (0b010001 @ 0b00000 @ rt : regno @ fs : regno @ 0b00000000000) = Some(COP1_Move(0b00000, rt, fs))
function clause decode (0b010001 @ 0b00001 @ rt : regno @ fs : regno @ 0b00000000000) = Some(COP1_Move(0b00001, rt, fs))
function clause decode (0b010001 @ 0b00010 @ rt : regno @ fs : regno @ 0b00000000000) = Some(COP1_Move(0b00010, rt, fs))
function clause decode (0b010001 @ 0b00100 @ rt : regno @ fs : regno @ 0b00000000000) = Some(COP1_Move(0b00100, rt, fs))
function clause decode (0b010001 @ 0b00101 @ rt : regno @ fs : regno @ 0b00000000000) = Some(COP1_Move(0b00101, rt, fs))
function clause decode (0b010001 @ 0b00110 @ rt : regno @ fs : regno @ 0b00000000000) = Some(COP1_Move(0b00110, rt, fs))
function clause execute (COP1_Move(op, rt, fs)) = {
  MIPS_Cop1Usable();
  match op {
    0b00000 => wGPR(rt) = sign_extend(fpr_get32(fs)),
    0b00001 => wGPR(rt) = fpr_get64(fs),
    0b00010 => wGPR(rt) = sign_extend(if fs == 0b11111 then FCR31 else if fs == 0b00000 then 0x00000A00 else 0x00000000),
    0b00100 => fpr_set32(fs, rGPR(rt)[31 .. 0]),
    0b00101 => fpr_set64(fs, rGPR(rt)),
    _ => if fs == 0b11111 then {
      let v = rGPR(rt)[31 .. 0] & 0x0183FFFF;
      FCR31 = v;
      if (v[17] == bitone) | ((v[16 .. 12] & v[11 .. 7]) != 0b00000) then SignalException(FPE)
    }
  }
}

union clause ast = COP1_BC : (bit, bool, imm16)
function clause decode (0b010001 @ 0b01000 @ 0b00000 @ imm : imm16) = Some(COP1_BC(bitzero, false, imm))
function clause decode (0b010001 @ 0b01000 @ 0b00001 @ imm : imm16) = Some(COP1_BC(bitone, false, imm))
function clause decode (0b010001 @ 0b01000 @ 0b00010 @ imm : imm16) = Some(COP1_BC(bitzero, true, imm))
function clause decode (0b010001 @ 0b01000 @ 0b00011 @ imm : imm16) = Some(COP1_BC(bitone, true, imm))
function clause execute (COP1_BC(tf, likely, imm)) = {
  MIPS_Cop1Usable();
  if (InBranchDelay) then SignalException(ResI);
  if (FCR31[23] == tf) then {
    let offset : bits(64) = (sign_extend(imm @ 0b00) + 4) in
    execute_branch(PC + offset)
  } else {
    if (likely) then NextPC = PC + 8 else NextInBranchDelay = 0b1
  }
}

union clause ast = COP1_Arith : (bits(6), bits(5), regno, regno, regno)
function clause decode (0b010001 @ 0b10000 @ ft : regno @ fs : regno @ fd : regno @ funct : bits(6)) = Some(COP1_Arith(funct, 0b10000, ft, fs, fd))
function clause decode (0b010001 @ 0b10001 @ ft : regno @ fs : regno @ fd : regno @ funct : bits(6)) = Some(COP1_Arith(funct, 0b10001, ft, fs, fd))
function clause decode (0b010001 @ 0b10100 @ ft : regno @ fs : regno @ fd : regno @ funct : bits(6)) = Some(COP1_Arith(funct, 0b10100, ft, fs, fd))
function clause decode (0b010001 @ 0b10101 @ ft : regno @ fs : regno @ fd : regno @ funct : bits(6)) = Some(COP1_Arith(funct, 0b10101, ft, fs, fd))

/* the format of an operation's result, 0b00000 when funct and fmt are
   not a VR4300 operation */
val fp_res_fmt : (bits(6), bits(5)) -> bits(5)
function fp_res_fmt (funct, fmt) = {
  let fl = (fmt == 0b10000) | (fmt == 0b10001);
  let n = unsigned(funct);
  if n <= 7 then (if fl then fmt else 0b00000)
  else if n <= 11 then (if fl then 0b10101 else 0b00000)
  else if n <= 15 then (if fl then 0b10100 else 0b00000)
  else if n == 32 then (if fmt != 0b10000 then 0b10000 else 0b00000)
  else if n == 33 then (if fmt != 0b10001 then 0b10001 else 0b00000)
  else if n == 36 then (if fl then 0b10100 else 0b00000)
  else if n == 37 then (if fl then 0b10101 else 0b00000)
  else 0b00000
}

function clause execute (COP1_Arith(funct, fmt, ft, fs, fd)) = {
  MIPS_Cop1Usable();
  let a = fp_src(fmt, fs);
  let b = fp_src_t(fmt, ft);
  let n = unsigned(funct);
  if n >= 48 then {
    if (fmt == 0b10000) | (fmt == 0b10001) then MIPS_FpCmp(funct[3 .. 0], fmt, a, b)
    else MIPS_FpUnimpl()
  } else {
    let rf = fp_res_fmt(funct, fmt);
    if rf == 0b00000 then MIPS_FpUnimpl()
    else if n == 6 then wFPR(fd, fpr_get64(fs))
    else fp_dst(rf, fd, MIPS_Fp(funct, fmt, a, b))
  }
}

union clause ast = COP1_Mem : (bool, bool, regno, regno, imm16)
function clause decode (0b110001 @ base : regno @ ft : regno @ offset : imm16) = Some(COP1_Mem(false, false, base, ft, offset))
function clause decode (0b110101 @ base : regno @ ft : regno @ offset : imm16) = Some(COP1_Mem(false, true, base, ft, offset))
function clause decode (0b111001 @ base : regno @ ft : regno @ offset : imm16) = Some(COP1_Mem(true, false, base, ft, offset))
function clause decode (0b111101 @ base : regno @ ft : regno @ offset : imm16) = Some(COP1_Mem(true, true, base, ft, offset))
function clause execute (COP1_Mem(store, dbl, base, ft, offset)) = {
  MIPS_Cop1Usable();
  let vAddr : bits(64) = sign_extend(offset) + rGPR(base);
  if ~(isAddressAligned(vAddr, if dbl then D else W)) then
    SignalExceptionBadAddr(if store then AdES else AdEL, vAddr)
  else if store then {
    let pAddr = TLBTranslate(vAddr, StoreData) in
    if dbl then MEMw_wrapper(pAddr, 8, fpr_get64(ft)) else MEMw_wrapper(pAddr, 4, fpr_get32(ft))
  } else {
    let pAddr = TLBTranslate(vAddr, LoadData) in
    if dbl then fpr_set64(ft, MEMr_wrapper(pAddr, 8)) else fpr_set32(ft, MEMr_wrapper(pAddr, 4))
  }
}
