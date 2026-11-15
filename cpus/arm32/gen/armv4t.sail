/* ARMv4T (ARM7TDMI) overlay for the sail-arm v9.4-a AArch32 model.
 *
 * Loaded after the model: every `function` here replaces the model's
 * function of that name, a `function clause` of a decode group goes before
 * the model's clauses of that group, and the `$relift` lines below edit
 * model functions in place (`patch`) or name the decode clauses that exist
 * on ARMv4T (`allow`; every other clause decodes as undefined).
 *
 * Functions named `__v4t_*` without a body are cut (cpus/arm32/gen/src/cut.rs):
 * one aligned bus access, internal cycles, the Thumb bit, and whether the
 * template is a register-shifted-register form.
 */

/* ---- Profile: the decode clauses that exist on ARMv4T ---- */

$relift allow ADC_i_A1 ADC_r_A1 ADC_rr_A1 ADD_i_A1 ADD_r_A1 ADD_rr_A1 ADD_SP_i_A1 ADD_SP_r_A1 ADR_A1 ADR_A2
$relift allow AND_i_A1 AND_r_A1 AND_rr_A1 BIC_i_A1 BIC_r_A1 BIC_rr_A1 EOR_i_A1 EOR_r_A1 EOR_rr_A1
$relift allow ORR_i_A1 ORR_r_A1 ORR_rr_A1 RSB_i_A1 RSB_r_A1 RSB_rr_A1 RSC_i_A1 RSC_r_A1 RSC_rr_A1
$relift allow SBC_i_A1 SBC_r_A1 SBC_rr_A1 SUB_i_A1 SUB_r_A1 SUB_rr_A1 SUB_SP_i_A1 SUB_SP_r_A1
$relift allow CMN_i_A1 CMN_r_A1 CMN_rr_A1 CMP_i_A1 CMP_r_A1 CMP_rr_A1 TEQ_i_A1 TEQ_r_A1 TEQ_rr_A1 TST_i_A1 TST_r_A1 TST_rr_A1
$relift allow MOV_i_A1 MOV_r_A1 MOV_rr_A1 MVN_i_A1 MVN_r_A1 MVN_rr_A1
$relift allow B_A1 BL_i_A1 BX_A1 SVC_A1
$relift allow MRS_A1 MSR_i_A1 MSR_r_A1
$relift allow LDM_A1 LDMDA_A1 LDMDB_A1 LDMIB_A1 STM_A1 STMDA_A1 STMDB_A1 STMIB_A1
$relift allow LDM_e_A1 LDM_u_A1 STM_u_A1
$relift allow LDR_i_A1 LDR_r_A1 LDRB_i_A1 LDRB_r_A1 LDRT_A1 LDRT_A2 LDRBT_A1 LDRBT_A2
$relift allow STR_i_A1 STR_r_A1 STRB_i_A1 STRB_r_A1 STRT_A1 STRT_A2 STRBT_A1 STRBT_A2
$relift allow LDRH_i_A1 LDRH_r_A1 STRH_i_A1 STRH_r_A1
$relift allow LDRSB_i_A1 LDRSB_r_A1 LDRSH_i_A1 LDRSH_r_A1
$relift allow MUL_A1 MLA_A1 UMULL_A1 UMLAL_A1 SMULL_A1 SMLAL_A1 SWP_A1
$relift allow ADC_r_T1 ADD_i_T1 ADD_i_T2 ADD_r_T1 ADD_r_T2 ADD_SP_i_T1 ADD_SP_i_T2 ADD_SP_r_T1 ADD_SP_r_T2 ADR_T1
$relift allow AND_r_T1 ASR_i_T1 ASR_r_T1 BIC_r_T1 CMN_r_T1 CMP_i_T1 CMP_r_T1 CMP_r_T2 EOR_r_T1
$relift allow LSL_i_T1 LSL_r_T1 LSR_i_T1 LSR_r_T1 MOV_i_T1 MOV_r_T1 MOV_r_T2 MOV_rr_T1 MUL_T1 MVN_r_T1 ORR_r_T1
$relift allow ROR_r_T1 RSB_i_T1 SBC_r_T1 SUB_i_T1 SUB_i_T2 SUB_r_T1 SUB_SP_i_T1 TST_r_T1
$relift allow B_T1 B_T2 BX_T1 BXV4_T1 BLPRE_T1 BLSUF_T1 SVC_T1
$relift allow LDM_T1 STM_T1 POP_T1 PUSH_T1
$relift allow LDR_i_T1 LDR_i_T2 LDR_l_T1 LDR_r_T1 LDRB_i_T1 LDRB_r_T1 LDRH_i_T1 LDRH_r_T1 LDRSB_r_T1 LDRSH_r_T1
$relift allow STR_i_T1 STR_i_T2 STR_r_T1 STRB_i_T1 STRB_r_T1 STRH_i_T1 STRH_r_T1

/* ---- Edits of model functions ---- */

/* Should-be-zero and should-be-one fields: sail-arm's patterns fix them,
 * ARM7TDMI ignores them. */
$relift patch __DecodeA32_* throw(Error_ConstrainedUnpredictable())\n    } else { ==> ()\n    };\n    {
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_TST_ 0x0000f000
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_TEQ_ 0x0000f000
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_CMP_ 0x0000f000
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_CMN_ 0x0000f000
$relift patch __DecodeA32_DataProMisc decode_aarch32_instrs_TST_i_A1enc_A_txt(cond, Rn, imm12) ==> if Slice(__opcode, 12, 4) == 0b1111 then __v4t_TestP(cond, false) else decode_aarch32_instrs_TST_i_A1enc_A_txt(cond, Rn, imm12)
$relift patch __DecodeA32_DataProMisc decode_aarch32_instrs_TST_r_A1enc_A_txt(cond, Rn, imm5, stype, Rm) ==> if Slice(__opcode, 12, 4) == 0b1111 then __v4t_TestP(cond, false) else decode_aarch32_instrs_TST_r_A1enc_A_txt(cond, Rn, imm5, stype, Rm)
$relift patch __DecodeA32_DataProMisc decode_aarch32_instrs_TST_rr_A1enc_A_txt(cond, Rn, Rs, stype, Rm) ==> if Slice(__opcode, 12, 4) == 0b1111 then __v4t_TestP(cond, true) else decode_aarch32_instrs_TST_rr_A1enc_A_txt(cond, Rn, Rs, stype, Rm)
$relift patch __DecodeA32_DataProMisc decode_aarch32_instrs_TEQ_i_A1enc_A_txt(cond, Rn, imm12) ==> if Slice(__opcode, 12, 4) == 0b1111 then __v4t_TestP(cond, false) else decode_aarch32_instrs_TEQ_i_A1enc_A_txt(cond, Rn, imm12)
$relift patch __DecodeA32_DataProMisc decode_aarch32_instrs_TEQ_r_A1enc_A_txt(cond, Rn, imm5, stype, Rm) ==> if Slice(__opcode, 12, 4) == 0b1111 then __v4t_TestP(cond, false) else decode_aarch32_instrs_TEQ_r_A1enc_A_txt(cond, Rn, imm5, stype, Rm)
$relift patch __DecodeA32_DataProMisc decode_aarch32_instrs_TEQ_rr_A1enc_A_txt(cond, Rn, Rs, stype, Rm) ==> if Slice(__opcode, 12, 4) == 0b1111 then __v4t_TestP(cond, true) else decode_aarch32_instrs_TEQ_rr_A1enc_A_txt(cond, Rn, Rs, stype, Rm)
$relift patch __DecodeA32_DataProMisc decode_aarch32_instrs_CMP_i_A1enc_A_txt(cond, Rn, imm12) ==> if Slice(__opcode, 12, 4) == 0b1111 then __v4t_TestP(cond, false) else decode_aarch32_instrs_CMP_i_A1enc_A_txt(cond, Rn, imm12)
$relift patch __DecodeA32_DataProMisc decode_aarch32_instrs_CMP_r_A1enc_A_txt(cond, Rn, imm5, stype, Rm) ==> if Slice(__opcode, 12, 4) == 0b1111 then __v4t_TestP(cond, false) else decode_aarch32_instrs_CMP_r_A1enc_A_txt(cond, Rn, imm5, stype, Rm)
$relift patch __DecodeA32_DataProMisc decode_aarch32_instrs_CMP_rr_A1enc_A_txt(cond, Rn, Rs, stype, Rm) ==> if Slice(__opcode, 12, 4) == 0b1111 then __v4t_TestP(cond, true) else decode_aarch32_instrs_CMP_rr_A1enc_A_txt(cond, Rn, Rs, stype, Rm)
$relift patch __DecodeA32_DataProMisc decode_aarch32_instrs_CMN_i_A1enc_A_txt(cond, Rn, imm12) ==> if Slice(__opcode, 12, 4) == 0b1111 then __v4t_TestP(cond, false) else decode_aarch32_instrs_CMN_i_A1enc_A_txt(cond, Rn, imm12)
$relift patch __DecodeA32_DataProMisc decode_aarch32_instrs_CMN_r_A1enc_A_txt(cond, Rn, imm5, stype, Rm) ==> if Slice(__opcode, 12, 4) == 0b1111 then __v4t_TestP(cond, false) else decode_aarch32_instrs_CMN_r_A1enc_A_txt(cond, Rn, imm5, stype, Rm)
$relift patch __DecodeA32_DataProMisc decode_aarch32_instrs_CMN_rr_A1enc_A_txt(cond, Rn, Rs, stype, Rm) ==> if Slice(__opcode, 12, 4) == 0b1111 then __v4t_TestP(cond, true) else decode_aarch32_instrs_CMN_rr_A1enc_A_txt(cond, Rn, Rs, stype, Rm)
$relift patch __DecodeT16 decode_aarch32_instrs_CMP_r_T2enc_A_txt(N, Rm, Rn) ==> if N @ Rn == 0b1111 then __v4t_RestoreCPSR(true) else decode_aarch32_instrs_CMP_r_T2enc_A_txt(N, Rm, Rn)
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_MOV_i_A1 0x000f0000
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_MOV_r_A1 0x000f0000
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_MOV_rr_A1 0x000f0000
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_MVN_ 0x000f0000
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_MUL_A1 0x0000f000
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_LDRH_r_A1 0x00000f00
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_STRH_r_A1 0x00000f00
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_LDRSB_r_A1 0x00000f00
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_LDRSH_r_A1 0x00000f00
$relift relax __DecodeA32_DataProMisc decode_aarch32_instrs_BX_A1 0x000fff00

/* The ARMv7 UNPREDICTABLE cases of these encodings have one ARM7TDMI
 * behavior, which the generic semantics plus this overlay give (R15 reads,
 * writeback order, empty register lists). */
$relift patch decode_aarch32_instrs_* throw(Error_Unpredictable()) ==> ()
/* ARMv4 has no LDRT-style P=0 W=1 halfword forms and no separate literal
 * forms: P=0 W=1 is post-indexed with writeback, and Rn=15 is an ordinary
 * base register (read as PC+8; writeback to it is a jump). */
$relift drop_clause __DecodeA32_LoadStoreImmLit LDR_l_A1enc
$relift drop_clause __DecodeA32_LoadStoreImmLit LDRB_l_A1enc
$relift drop_clause __DecodeA32_DataProMisc LDRH_l_A1enc
$relift drop_clause __DecodeA32_DataProMisc LDRSB_l_A1enc
$relift drop_clause __DecodeA32_DataProMisc LDRSH_l_A1enc
/* ARMv4T has no hints: MSR with an empty field mask writes nothing. */
$relift drop_clause __DecodeA32_DataProMisc decode_aarch32_instrs_DBG_A1enc
$relift drop_clause __DecodeA32_DataProMisc decode_aarch32_instrs_NOP_A1enc
$relift drop_clause __DecodeA32_DataProMisc decode_aarch32_instrs_SEV_A1enc
$relift drop_clause __DecodeA32_DataProMisc decode_aarch32_instrs_WFE_A1enc
$relift drop_clause __DecodeA32_DataProMisc decode_aarch32_instrs_WFI_A1enc
$relift drop_clause __DecodeA32_DataProMisc decode_aarch32_instrs_YIELD_A1enc
$relift patch decode_aarch32_instrs_MSR_i_A1enc_AS_txt throw(Error_See("Related encodings")) ==> ()
/* The Thumb conditional branch clause's guard leaves cond = 1111 to SVC;
 * the generator chooses clauses by their fixed bits, so SVC goes first. */
$relift drop_clause __DecodeT16 decode_aarch32_instrs_SVC_T1enc_A_txt(imm8)
$relift patch decode_aarch32_instrs_* throw(Error_See("LDRHT")) ==> ()
$relift patch decode_aarch32_instrs_* throw(Error_See("STRHT")) ==> ()
$relift patch decode_aarch32_instrs_* throw(Error_See("LDRSBT")) ==> ()
$relift patch decode_aarch32_instrs_* throw(Error_See("LDRSHT")) ==> ()
$relift patch decode_aarch32_instrs_*_A1enc_A_txt throw(Error_See("LDR (literal)")) ==> ()
$relift patch decode_aarch32_instrs_*_A1enc_A_txt throw(Error_See("LDRB (literal)")) ==> ()
$relift patch decode_aarch32_instrs_*_A1enc_A_txt throw(Error_See("LDRH (literal)")) ==> ()
$relift patch decode_aarch32_instrs_*_A1enc_A_txt throw(Error_See("LDRSB (literal)")) ==> ()
$relift patch decode_aarch32_instrs_*_A1enc_A_txt throw(Error_See("LDRSH (literal)")) ==> ()
/* A byte load into its own base register: the loaded value wins over the
 * writeback (sail-arm writes the base last). */
$relift patch execute_aarch32_instrs_LDRB* R_set(t) = ZeroExtend(MemU_read(address, 1), 32);\n    if wback then {\n        R_set(n) = offset_addr\n    }; ==> let data : bits(32) = ZeroExtend(MemU_read(address, 1), 32);\n    if wback then {\n        R_set(n) = offset_addr\n    };\n    R_set(t) = data;
$relift patch execute_aarch32_instrs_LDRSB* R_set(t) = SignExtend(MemU_read(address, 1), 32);\n    if wback then {\n        R_set(n) = offset_addr\n    }; ==> let data : bits(32) = SignExtend(MemU_read(address, 1), 32);\n    if wback then {\n        R_set(n) = offset_addr\n    };\n    R_set(t) = data;
$relift patch execute_aarch32_instrs_LDRBT_Op_A_txt R_set(t) = ZeroExtend(MemU_unpriv_read(address, 1), 32);\n    if postindex then {\n        R_set(n) = offset_addr\n    }; ==> let data : bits(32) = ZeroExtend(MemU_unpriv_read(address, 1), 32);\n    if postindex then {\n        R_set(n) = offset_addr\n    };\n    R_set(t) = data;
/* Long multiplies with RdHi = RdLo keep the high word. */
$relift patch execute_aarch32_instrs_*MULL_Op_A_txt R_set(dHi) = result[63 .. 32];\n    R_set(dLo) = result[31 .. 0] ==> R_set(dLo) = result[31 .. 0];\n    R_set(dHi) = result[63 .. 32]
$relift patch execute_aarch32_instrs_*MLAL_Op_A_txt R_set(dHi) = result[63 .. 32];\n    R_set(dLo) = result[31 .. 0] ==> R_set(dLo) = result[31 .. 0];\n    R_set(dHi) = result[63 .. 32]
/* The shift register Rs is read in the first cycle, so R15 there reads as
 * PC+8; Rn and Rm read in the second cycle give PC+12 (see R_read). */
$relift patch execute_aarch32_instrs_*_rr_Op_A_txt UInt(R_read(s)[7 .. 0]) ==> UInt(__v4t_ReadRs(s)[7 .. 0])
/* Multiply cycles: m internal cycles from the multiplier's top bytes, plus
 * one for an accumulate and one for a long result (GBATEK). */
$relift patch execute_aarch32_instrs_MUL_Op_A_txt (d, m, n, setflags) = { ==> (d, m, n, setflags) = {\n    __v4t_MulCycles(R_read(m), true, 0);
$relift patch execute_aarch32_instrs_MLA_Op_A_txt (a, d, m, n, setflags) = { ==> (a, d, m, n, setflags) = {\n    __v4t_MulCycles(R_read(m), true, 1);
$relift patch execute_aarch32_instrs_SMULL_Op_A_txt (dHi, dLo, m, n, setflags) = { ==> (dHi, dLo, m, n, setflags) = {\n    __v4t_MulCycles(R_read(m), true, 1);
$relift patch execute_aarch32_instrs_UMULL_Op_A_txt (dHi, dLo, m, n, setflags) = { ==> (dHi, dLo, m, n, setflags) = {\n    __v4t_MulCycles(R_read(m), false, 1);
$relift patch execute_aarch32_instrs_SMLAL_Op_A_txt (dHi, dLo, m, n, setflags) = { ==> (dHi, dLo, m, n, setflags) = {\n    __v4t_MulCycles(R_read(m), true, 2);
$relift patch execute_aarch32_instrs_UMLAL_Op_A_txt (dHi, dLo, m, n, setflags) = { ==> (dHi, dLo, m, n, setflags) = {\n    __v4t_MulCycles(R_read(m), false, 2);
/* Byte and halfword stores of R15 store PC+12, like STR. */
$relift patch execute_aarch32_instrs_STRB* R_read(t) ==> __v4t_StoreValue(t)
$relift patch execute_aarch32_instrs_STRH* R_read(t) ==> __v4t_StoreValue(t)
/* LDR to the PC from an unaligned address: the rotated word, aligned. */
$relift patch execute_aarch32_instrs_LDR_* throw(Error_Unpredictable()) ==> LoadWritePC(data)
/* LDRH and LDRSH from an odd address (see __v4t_LoadH and __v4t_LoadSH). */
$relift patch execute_aarch32_instrs_LDRH_* let data : bits(16) = MemU_read(address, 2); ==> let data : bits(32) = __v4t_LoadH(address);
$relift patch execute_aarch32_instrs_LDRH_* ZeroExtend(data, 32) ==> data
$relift patch execute_aarch32_instrs_LDRSH_* let data : bits(16) = MemU_read(address, 2); ==> let data : bits(32) = __v4t_LoadSH(address);
$relift patch execute_aarch32_instrs_LDRSH_* SignExtend(data, 32) ==> data

/* ---- Cut functions ---- */

val __v4t_read : forall 'size, 'size in {1, 2, 4}. (bits(32), int('size)) -> bits(8 * 'size)
val __v4t_write : forall 'size, 'size in {1, 2, 4}. (bits(32), int('size), bits(8 * 'size)) -> unit
val __v4t_internal : int -> unit
val __v4t_internal_if : bool -> unit
val __v4t_force_n : unit -> unit
val __v4t_SetT : bits(1) -> unit
val __v4t_RegShiftForm : unit -> bool
val __v4t_R : forall 'n, (0 <= 'n & 'n <= 15). int('n) -> bits(32)
val __v4t_RSet : forall 'n, (0 <= 'n & 'n <= 15). (int('n), bits(32)) -> unit
val __v4t_CPSR : unit -> bits(32)
val __v4t_SPSR : unit -> bits(32)
val __v4t_WriteCPSR : (bits(32), bits(4)) -> unit
val __v4t_WriteSPSR : (bits(32), bits(4)) -> unit
val __v4t_RestoreCPSR : bool -> unit
val __v4t_BranchAligned : bits(32) -> unit
val __v4t_SVC : unit -> unit

/* ---- Registers and the PC ---- */

/* R15 reads as the instruction address plus 8 (A32), plus 12 in the
 * register-shifted-register forms (the shift register is read in an extra
 * cycle, after the PC advanced), or plus 4 (Thumb). */
function R_read n = {
    if n == 15 then {
        let 'offset = if PSTATE.T == 0b1 then 4 else if __v4t_RegShiftForm() then 12 else 8;
        return(_PC[31 .. 0] + offset)
    } else {
        return(__v4t_R(n))
    }
}

function R_set (n, value_name) = {
    __v4t_RSet(n, value_name);
    return()
}

/* STR and STM of R15 store the instruction address plus 12; a Thumb STM
 * with an empty list stores the address plus 6. */
function PCStoreValue () = {
    return(_PC[31 .. 0] + (if PSTATE.T == 0b1 then 6 else 12))
}

val __v4t_ReadRs : forall 'n, (0 <= 'n & 'n <= 15). int('n) -> bits(32)
function __v4t_ReadRs n = {
    if n == 15 then _PC[31 .. 0] + 8 else R_read(n)
}

/* The multiplier's early termination: one internal cycle per byte above
 * the lowest that is not all zeros (or all ones, signed). */
val __v4t_MulCycles : (bits(32), bool, int) -> unit
function __v4t_MulCycles (rs, signed, extra) = {
    __v4t_internal(1 + extra);
    __v4t_internal_if(not_bool(IsZero(rs[31 .. 8]) | (signed & IsOnes(rs[31 .. 8]))));
    __v4t_internal_if(not_bool(IsZero(rs[31 .. 16]) | (signed & IsOnes(rs[31 .. 16]))));
    __v4t_internal_if(not_bool(IsZero(rs[31 .. 24]) | (signed & IsOnes(rs[31 .. 24]))))
}

val __v4t_StoreValue : forall 'n, (0 <= 'n & 'n <= 15). int('n) -> bits(32)
function __v4t_StoreValue n = {
    if n == 15 then PCStoreValue() else R_read(n)
}

/* No interworking but BX: an ALU or load write of the PC keeps the state
 * and ignores the low address bits. */
function BranchWritePC (address_in, branch_type) = {
    address : bits(32) = address_in;
    if PSTATE.T == 0b0 then {
        address[1 .. 0] = 0b00
    } else {
        address[0] = Bit(0b0)
    };
    BranchTo(address, branch_type, false)
}

function ALUWritePC address = {
    BranchWritePC(address, BranchType_INDIR)
}

function LoadWritePC address = {
    BranchWritePC(address, BranchType_INDIR)
}

function BXWritePC (address_in, branch_type) = {
    __v4t_SetT([address_in[0]]);
    let address : bits(32) = if [address_in[0]] == 0b1 then address_in[31 .. 1] @ 0b0 else address_in[31 .. 2] @ 0b00;
    BranchTo(address, branch_type, false)
}

/* ---- Modes and the PSRs ----
 * The mode is only known at run time: the cut keeps the CPSR in the
 * register file and swaps the banked registers on a mode change. Proven
 * on an AGB-001 (relift hardware probe, sb1 and p2 runs): MSR to the SPSR
 * forces mode bit 4 to 1; the test ops with Rd = 15 and S = 1, and Thumb
 * CMP with Rd = 15, copy the SPSR to the CPSR in a mode with an SPSR and
 * change nothing in user and system mode. */

/* MOVS PC and the other data-processing writes of the PC with S = 1:
 * CPSR := SPSR, then a branch aligned by the restored Thumb bit. */
function ALUExceptionReturn address = {
    __v4t_RestoreCPSR(false);
    __v4t_BranchAligned(address)
}

/* MRS: the whole PSR, bits 27 to 8 as stored. */
function execute_aarch32_instrs_MRS_Op_AS_txt (d, read_spsr) = {
    R_set(d) = if read_spsr then __v4t_SPSR() else __v4t_CPSR()
}

/* MSR: the f field writes the flags and c the control byte (not in user
 * mode for the CPSR); ARMv4T stores nothing of the x and s fields. */
function execute_aarch32_instrs_MSR_i_Op_AS_txt (imm32, mask, write_spsr) = {
    if write_spsr then __v4t_WriteSPSR(imm32, mask) else __v4t_WriteCPSR(imm32, mask)
}

function execute_aarch32_instrs_MSR_r_Op_AS_txt (mask, n, write_spsr) = {
    if write_spsr then __v4t_WriteSPSR(R_read(n), mask) else __v4t_WriteCPSR(R_read(n), mask)
}

/* TST, TEQ, CMP and CMN with Rd = 15 and S = 1 (the P forms), and Thumb
 * CMP with Rd = 15: no flags from the result; CPSR := SPSR where there is
 * one (keeping the Thumb bit set for the Thumb CMP). The PC moves on. The
 * decode clauses of the test ops ignore Rd (see the relax lines above), so
 * they test it here. */
val __v4t_TestP : (bits(4), bool) -> unit
function __v4t_TestP (cond, regshift) = {
    if ConditionPassed() then {
        if regshift then {
            __v4t_internal(1)
        };
        __v4t_RestoreCPSR(false)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _] as __opcode))) = {
    let imm8 = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_SVC_T1enc_A_txt(imm8)
}

/* SWI: the supervisor call, entered by the cut (SPSR_svc := CPSR, svc
 * mode, ARM state, IRQs masked, LR the next instruction, PC 0x08). */
function execute_aarch32_instrs_SVC_Op_A_txt imm32 = {
    __v4t_SVC()
}

/* The register-shifted-register forms (and Thumb shifts by register) take
 * one internal cycle to read the shift register. Only they decode a
 * register shift type. */
function DecodeRegShift srtype = {
    __v4t_internal(1);
    shift_t : SRType = undefined;
    match srtype {
      0b00 => { shift_t = SRType_LSL },
      0b01 => { shift_t = SRType_LSR },
      0b10 => { shift_t = SRType_ASR },
      0b11 => { shift_t = SRType_ROR }
    };
    return(shift_t)
}

/* ---- Memory ---- */

/* Single loads: the bus reads the aligned word or halfword; a word load
 * rotates it right by the byte offset. Every single load ends with one
 * internal cycle. */
function MemU_read (address, size) = {
    if size == 4 then {
        let data : bits(32) = __v4t_read(address[31 .. 2] @ 0b00, 4);
        __v4t_internal(1);
        return(ROR(data, 8 * UInt(address[1 .. 0])))
    } else if size == 2 then {
        let data : bits(16) = __v4t_read(address[31 .. 1] @ 0b0, 2);
        __v4t_internal(1);
        return(data)
    } else {
        let data : bits(8) = __v4t_read(address, 1);
        __v4t_internal(1);
        return(data)
    }
}

/* Single stores write the aligned word or halfword. */
function MemU_set (address, size, value_name) = {
    if size == 4 then {
        __v4t_write(address[31 .. 2] @ 0b00, 4, value_name)
    } else if size == 2 then {
        __v4t_write(address[31 .. 1] @ 0b0, 2, value_name)
    } else {
        __v4t_write(address, 1, value_name)
    }
}

function MemU_unpriv_read (address, size) = {
    return(MemU_read(address, size))
}

function MemU_unpriv_set (address, size, value_name) = {
    MemU_set(address, size) = value_name
}

/* LDRH from an odd address: the aligned halfword rotated right by 8. */
val __v4t_LoadH : bits(32) -> bits(32)
function __v4t_LoadH address = {
    let data : bits(16) = __v4t_read(address[31 .. 1] @ 0b0, 2);
    __v4t_internal(1);
    return(ROR(ZeroExtend(data, 32), 8 * UInt(address[0 .. 0])))
}

/* LDRSH from an odd address loads the byte there, sign-extended. The bus
 * reads the aligned halfword either way. */
val __v4t_LoadSH : bits(32) -> bits(32)
function __v4t_LoadSH address = {
    let data : bits(16) = __v4t_read(address[31 .. 1] @ 0b0, 2);
    __v4t_internal(1);
    return(if [address[0]] == 0b1 then SignExtend(data[15 .. 8], 32) else SignExtend(data, 32))
}

/* ---- Load and store multiple ----
 * ARM7TDMI: an empty list transfers R15 and moves the base by 0x40; LDM
 * with the base in the list does not write back; STM with the base in the
 * list stores the old base when it is the lowest register, else the new
 * one. Transfers are of aligned words, first N then S; LDM ends with one
 * internal cycle. */

val __v4t_count : bits(16) -> int
function __v4t_count registers = {
    if IsZero(registers) then 16 else BitCount(registers)
}

val __v4t_LDM : forall 'n, (0 <= 'n & 'n <= 15). (int('n), bits(16), bool, bits(32), bits(32)) -> unit
function __v4t_LDM (n, registers, wback, start, newbase) = {
    address : bits(32) = start;
    foreach (i from 0 to 14 by 1 in inc) {
        if [registers[i]] == 0b1 then {
            R_set(i) = __v4t_read(address[31 .. 2] @ 0b00, 4);
            address = address + 4
        };
        ()
    };
    let pc : bool = [registers[15]] == 0b1 | IsZero(registers);
    if pc then {
        let data : bits(32) = __v4t_read(address[31 .. 2] @ 0b00, 4);
        __v4t_internal(1);
        LoadWritePC(data)
    } else {
        __v4t_internal(1)
    };
    if wback & [registers[n]] == 0b0 then {
        R_set(n) = newbase
    };
    ()
}

val __v4t_STM : forall 'n, (0 <= 'n & 'n <= 15). (int('n), bits(16), bool, bits(32), bits(32)) -> unit
function __v4t_STM (n, registers, wback, start, newbase) = {
    address : bits(32) = start;
    foreach (i from 0 to 14 by 1 in inc) {
        if [registers[i]] == 0b1 then {
            let v : bits(32) = if (i == n & wback) & i != LowestSetBit(registers) then newbase else R_read(i);
            __v4t_write(address[31 .. 2] @ 0b00, 4, v);
            address = address + 4
        };
        ()
    };
    if [registers[15]] == 0b1 | IsZero(registers) then {
        __v4t_write(address[31 .. 2] @ 0b00, 4, PCStoreValue())
    };
    if wback then {
        R_set(n) = newbase
    };
    ()
}

/* LDM with the S bit and R15 in the list: the loads of the current mode,
 * then CPSR := SPSR and a branch aligned by the restored Thumb bit. */
val __v4t_LDMe : forall 'n, (0 <= 'n & 'n <= 15). (int('n), bits(15), bool, bits(32), bits(32)) -> unit
function __v4t_LDMe (n, registers, wback, start, newbase) = {
    address : bits(32) = start;
    foreach (i from 0 to 14 by 1 in inc) {
        if [registers[i]] == 0b1 then {
            R_set(i) = __v4t_read(address[31 .. 2] @ 0b00, 4);
            address = address + 4
        };
        ()
    };
    let data : bits(32) = __v4t_read(address[31 .. 2] @ 0b00, 4);
    __v4t_internal(1);
    if wback & [registers[n]] == 0b0 then {
        R_set(n) = newbase
    };
    __v4t_RestoreCPSR(false);
    __v4t_BranchAligned(data)
}

function execute_aarch32_instrs_LDM_e_Op_AS_txt (increment_name, n, registers, wback, wordhigher) = {
    let 'c = BitCount(registers) + 1;
    let base : bits(32) = R_read(n);
    let low : bits(32) = if increment_name then base else base - 4 * c;
    let start : bits(32) = if wordhigher then low + 4 else low;
    __v4t_LDMe(n, registers, wback, start, if increment_name then base + 4 * c else base - 4 * c)
}

/* LDM and STM with the S bit and no R15 loaded: the user bank's
 * registers (the current ones in user and system mode). An LDM with an
 * empty list loads R15 from the lowest word of a 16-word block and then
 * restores the CPSR, as the LDM with R15 in the list. */
function execute_aarch32_instrs_LDM_u_Op_AS_txt (increment_name, n, registers, wordhigher) = {
    let base : bits(32) = R_read(n);
    if IsZero(registers) then {
        let low : bits(32) = if increment_name then base else base - 64;
        __v4t_LDMe(n, registers, false, if wordhigher then low + 4 else low, base)
    } else {
        let 'c = BitCount(registers);
        let low : bits(32) = if increment_name then base else base - 4 * c;
        address : bits(32) = if wordhigher then low + 4 else low;
        foreach (i from 0 to 14 by 1 in inc) {
            if [registers[i]] == 0b1 then {
                Rmode_set(i, M32_User) = __v4t_read(address[31 .. 2] @ 0b00, 4);
                address = address + 4
            };
            ()
        };
        __v4t_internal(1)
    }
}

function execute_aarch32_instrs_STM_u_Op_AS_txt (increment_name, n, registers, wordhigher) = {
    let 'c = __v4t_count(registers);
    let base : bits(32) = R_read(n);
    let low : bits(32) = if increment_name then base else base - 4 * c;
    address : bits(32) = if wordhigher then low + 4 else low;
    foreach (i from 0 to 14 by 1 in inc) {
        if [registers[i]] == 0b1 then {
            __v4t_write(address[31 .. 2] @ 0b00, 4, Rmode_read(i, M32_User));
            address = address + 4
        };
        ()
    };
    if [registers[15]] == 0b1 | IsZero(registers) then {
        __v4t_write(address[31 .. 2] @ 0b00, 4, PCStoreValue())
    };
    ()
}

function execute_aarch32_instrs_LDM_Op_A_txt (n, registers, wback) = {
    let 'c = __v4t_count(registers);
    __v4t_LDM(n, registers, wback, R_read(n), R_read(n) + 4 * c)
}

function execute_aarch32_instrs_LDMIB_Op_A_txt (n, registers, wback) = {
    let 'c = __v4t_count(registers);
    __v4t_LDM(n, registers, wback, R_read(n) + 4, R_read(n) + 4 * c)
}

function execute_aarch32_instrs_LDMDA_Op_A_txt (n, registers, wback) = {
    let 'c = __v4t_count(registers);
    __v4t_LDM(n, registers, wback, R_read(n) - 4 * c + 4, R_read(n) - 4 * c)
}

function execute_aarch32_instrs_LDMDB_Op_A_txt (n, registers, wback) = {
    let 'c = __v4t_count(registers);
    __v4t_LDM(n, registers, wback, R_read(n) - 4 * c, R_read(n) - 4 * c)
}

function execute_aarch32_instrs_STM_Op_A_txt (n, registers, wback) = {
    let 'c = __v4t_count(registers);
    __v4t_STM(n, registers, wback, R_read(n), R_read(n) + 4 * c)
}

function execute_aarch32_instrs_STMIB_Op_A_txt (n, registers, wback) = {
    let 'c = __v4t_count(registers);
    __v4t_STM(n, registers, wback, R_read(n) + 4, R_read(n) + 4 * c)
}

function execute_aarch32_instrs_STMDA_Op_A_txt (n, registers, wback) = {
    let 'c = __v4t_count(registers);
    __v4t_STM(n, registers, wback, R_read(n) - 4 * c + 4, R_read(n) - 4 * c)
}

function execute_aarch32_instrs_STMDB_Op_A_txt (n, registers, wback) = {
    let 'c = __v4t_count(registers);
    __v4t_STM(n, registers, wback, R_read(n) - 4 * c, R_read(n) - 4 * c)
}

function execute_aarch32_instrs_POP_Op_A_txt (UnalignedAllowed, registers) = {
    let 'c = __v4t_count(registers);
    __v4t_LDM(13, registers, true, R_read(13), R_read(13) + 4 * c)
}

function execute_aarch32_instrs_PUSH_Op_A_txt (UnalignedAllowed, registers) = {
    let 'c = __v4t_count(registers);
    __v4t_STM(13, registers, true, R_read(13) - 4 * c, R_read(13) - 4 * c)
}

/* ---- Instructions sail-arm does not have ---- */

/* SWP and SWPB (removed after Armv7): a load like LDR (rotated) or LDRB,
 * then a nonsequential store, then one internal cycle. */
val decode_aarch32_instrs_SWP_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(4)) -> unit
function decode_aarch32_instrs_SWP_A1enc_A_txt (cond, B, Rn, Rt, Rt2) = {
    if ConditionPassed() then {
        let 'n = UInt(Rn);
        let 't = UInt(Rt);
        let 't2 = UInt(Rt2);
        let address : bits(32) = R_read(n);
        if B == 0b1 then {
            let data : bits(8) = __v4t_read(address, 1);
            __v4t_force_n();
            __v4t_write(address, 1, __v4t_StoreValue(t2)[7 .. 0]);
            R_set(t) = ZeroExtend(data, 32)
        } else {
            let data : bits(32) = __v4t_read(address[31 .. 2] @ 0b00, 4);
            __v4t_force_n();
            __v4t_write(address[31 .. 2] @ 0b00, 4, __v4t_StoreValue(t2));
            R_set(t) = ROR(data, 8 * UInt(address[1 .. 0]))
        };
        __v4t_internal(1)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, bitone, bitzero, bitzero, bitone, _, _, _, _] as __opcode))) = {
    let cond = Slice(__opcode, 28, 4);
    let B = Slice(__opcode, 22, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rt = Slice(__opcode, 12, 4);
    let Rt2 = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_SWP_A1enc_A_txt(cond, B, Rn, Rt, Rt2)
}

/* Thumb BX: ARM7TDMI ignores bit 7 (BLX from Armv5) and bits 2 to 0. */
val decode_aarch32_instrs_BXV4_T1enc_A_txt : bits(4) -> unit
function decode_aarch32_instrs_BXV4_T1enc_A_txt Rm = {
    BXWritePC(R_read(UInt(Rm)), BranchType_INDIR)
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitone, _, _, _, _, _, _, _, _] as __opcode))) = {
    let Rm = Slice(__opcode, 3, 4);
    decode_aarch32_instrs_BXV4_T1enc_A_txt(Rm)
}

/* Thumb BL as two 16-bit instructions: the first adds the high offset to
 * the PC into LR, the second branches to LR plus the low offset and sets
 * LR to the next instruction with bit 0 set. */
val decode_aarch32_instrs_BLPRE_T1enc_A_txt : bits(11) -> unit
function decode_aarch32_instrs_BLPRE_T1enc_A_txt imm11 = {
    R_set(14) = R_read(15) + SignExtend(imm11 @ Zeros(12), 32);
    ()
}

val decode_aarch32_instrs_BLSUF_T1enc_A_txt : bits(11) -> unit
function decode_aarch32_instrs_BLSUF_T1enc_A_txt imm11 = {
    let next : bits(32) = _PC[31 .. 0] + 2;
    let target : bits(32) = R_read(14) + ZeroExtend(imm11 @ 0b0, 32);
    R_set(14) = next[31 .. 1] @ 0b1;
    BranchWritePC(target, BranchType_DIRCALL)
}

function clause __DecodeT16 ((pc, ([bitone, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _] as __opcode))) = {
    let imm11 = Slice(__opcode, 0, 11);
    decode_aarch32_instrs_BLPRE_T1enc_A_txt(imm11)
}

function clause __DecodeT16 ((pc, ([bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode))) = {
    let imm11 = Slice(__opcode, 0, 11);
    decode_aarch32_instrs_BLSUF_T1enc_A_txt(imm11)
}
