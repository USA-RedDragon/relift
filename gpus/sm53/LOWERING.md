# Maxwell lowering table

Written by `RELIFT_BLESS=1 cargo test -p relift-gpu-sm53 --test lowering`
from `src/lower/table.rs`; do not edit by hand. Each Maxwell form is
lowered to exactly one SM86 form, handled as a pseudo-instruction,
split between the two by case, or a stop. A form is lowered only
with evidence that both behave the same in every case it lowers;
the cases the evidence does not cover stop with their own reason.

| form | row | to | evidence or reason |
|---|---|---|---|
| AL2P | stop | | no semantic check yet |
| ALD | exact | ALD | same fields (offset, vertex register, size, .P, .O); Ryujinx's Ald and the SM86 translator read the same attribute map (generic 0x80 to 0x27c, position 0x70, vertex id 0x2fc as VertexIndex, instance id 0x2f8 as InstanceIndex - BaseInstance); a register in the address stops; operands equal envydis on every census word |
| AST | stop | | no semantic check yet |
| ATOM | stop | | no semantic check yet |
| ATOM_CAS | stop | | no semantic check yet |
| ATOMS | stop | | no semantic check yet |
| ATOMS_CAS | stop | | no semantic check yet |
| B2R | stop | | no semantic check yet |
| BAR | stop | | no semantic check yet |
| BFE R | stop | | no semantic check yet |
| BFE I | stop | | no semantic check yet |
| BFE C | stop | | no semantic check yet |
| BFI R | stop | | no semantic check yet |
| BFI I | stop | | no semantic check yet |
| BFI C | stop | | no semantic check yet |
| BFI RC | stop | | no semantic check yet |
| BPT | stop | | no semantic check yet |
| BRA | exact | BRA | Ryujinx's Bra branches to the same instruction under the guard; the target keeps its instruction (word k is Maxwell instruction k); a condition code test and .CA, .LMT, .U stop; synthetic programs translate whole (tests/flow.rs) |
| BRK | exact | BRA | for one thread a pop is a jump to the entry it pops (a BRK unwinds the SSY entries above its PBK, as Ryujinx's decoder does); Ryujinx emits a branch when the pop has one target, and the control-flow pass lowers only those, under the same guard. A pop with more than one target or none and a condition code test stop (tests/flow.rs) |
| BRX | stop | | indirect jump: its targets are in the program's constant bank 1, which the scan does not carry |
| CAL | stop | | subroutine call or return: the SM86 translator inlines CALL.REL through a return address register Maxwell does not have |
| CCTL | stop | | no semantic check yet |
| CCTLL | stop | | no semantic check yet |
| CCTLT | stop | | no semantic check yet |
| CONT | stop | | PCNT and CONT: no program of the census uses them |
| CSET | stop | | no semantic check yet |
| CSETP | stop | | no semantic check yet |
| CS2R | stop | | no semantic check yet |
| DADD R | stop | | no semantic check yet |
| DADD I | stop | | no semantic check yet |
| DADD C | stop | | no semantic check yet |
| DEPBAR | exact | NOP | a scoreboard wait orders nothing a SPIR-V invocation can see: Ryujinx's Depbar emits nothing, as the SM86 translator does for DEPBAR |
| DFMA R | stop | | no semantic check yet |
| DFMA I | stop | | no semantic check yet |
| DFMA C | stop | | no semantic check yet |
| DFMA RC | stop | | no semantic check yet |
| DMNMX R | stop | | no semantic check yet |
| DMNMX I | stop | | no semantic check yet |
| DMNMX C | stop | | no semantic check yet |
| DMUL R | stop | | no semantic check yet |
| DMUL I | stop | | no semantic check yet |
| DMUL C | stop | | no semantic check yet |
| DSET R | stop | | no semantic check yet |
| DSET I | stop | | no semantic check yet |
| DSET C | stop | | no semantic check yet |
| DSETP R | stop | | no semantic check yet |
| DSETP I | stop | | no semantic check yet |
| DSETP C | stop | | no semantic check yet |
| EXIT | exact | EXIT | Ryujinx's Exit returns under the guard and the SM86 translator's EXIT writes the fragment outputs of the SPH and returns (Ryujinx does the same at its return); a condition code test and .KEEPREFCOUNT stop |
| F2F R | stop | | no semantic check yet |
| F2F I | stop | | no semantic check yet |
| F2F C | stop | | no semantic check yet |
| F2I R | stop | | no semantic check yet |
| F2I I | stop | | no semantic check yet |
| F2I C | stop | | no semantic check yet |
| FADD R | exact | FADD | Ryujinx's Fadd adds the operands with their .NEG and .ABS; the mode fields go to SM86's (fadd known answers: every mode, FTZ, SAT, negation and absolute value); an immediate's .NEG and .ABS fold into its sign bit as the operand modifiers act. The condition code and constant offsets from 0x8000 stop. Operands equal envydis on every census word; game programs as for FMUL |
| FADD I | exact | FADD | Ryujinx's Fadd adds the operands with their .NEG and .ABS; the mode fields go to SM86's (fadd known answers: every mode, FTZ, SAT, negation and absolute value); an immediate's .NEG and .ABS fold into its sign bit as the operand modifiers act. The condition code and constant offsets from 0x8000 stop. Operands equal envydis on every census word; game programs as for FMUL |
| FADD C | exact | FADD | Ryujinx's Fadd adds the operands with their .NEG and .ABS; the mode fields go to SM86's (fadd known answers: every mode, FTZ, SAT, negation and absolute value); an immediate's .NEG and .ABS fold into its sign bit as the operand modifiers act. The condition code and constant offsets from 0x8000 stop. Operands equal envydis on every census word; game programs as for FMUL |
| FADD 32I | exact | FADD | Ryujinx's Fadd adds the operands with their .NEG and .ABS; the mode fields go to SM86's (fadd known answers: every mode, FTZ, SAT, negation and absolute value); an immediate's .NEG and .ABS fold into its sign bit as the operand modifiers act. The condition code and constant offsets from 0x8000 stop. Operands equal envydis on every census word; game programs as for FMUL |
| FCHK R | stop | | no semantic check yet |
| FCHK I | stop | | no semantic check yet |
| FCHK C | stop | | no semantic check yet |
| FCMP R | stop | | no semantic check yet |
| FCMP I | stop | | no semantic check yet |
| FCMP C | stop | | no semantic check yet |
| FCMP RC | stop | | no semantic check yet |
| FFMA R | exact | FFMA | Ryujinx's Ffma is a * b + c with the product's and the addend's .NEG; FFMA32I adds its destination and the register-constant form takes the register as b (SM86 FFMA form 3, constant in c). Modes go to SM86's fields (ffma known answers); .FMZ, the condition code and constant offsets from 0x8000 stop. Operands equal envydis on every census word; game programs as for FMUL |
| FFMA I | exact | FFMA | Ryujinx's Ffma is a * b + c with the product's and the addend's .NEG; FFMA32I adds its destination and the register-constant form takes the register as b (SM86 FFMA form 3, constant in c). Modes go to SM86's fields (ffma known answers); .FMZ, the condition code and constant offsets from 0x8000 stop. Operands equal envydis on every census word; game programs as for FMUL |
| FFMA C | exact | FFMA | Ryujinx's Ffma is a * b + c with the product's and the addend's .NEG; FFMA32I adds its destination and the register-constant form takes the register as b (SM86 FFMA form 3, constant in c). Modes go to SM86's fields (ffma known answers); .FMZ, the condition code and constant offsets from 0x8000 stop. Operands equal envydis on every census word; game programs as for FMUL |
| FFMA RC | exact | FFMA | Ryujinx's Ffma is a * b + c with the product's and the addend's .NEG; FFMA32I adds its destination and the register-constant form takes the register as b (SM86 FFMA form 3, constant in c). Modes go to SM86's fields (ffma known answers); .FMZ, the condition code and constant offsets from 0x8000 stop. Operands equal envydis on every census word; game programs as for FMUL |
| FFMA 32I | exact | FFMA | Ryujinx's Ffma is a * b + c with the product's and the addend's .NEG; FFMA32I adds its destination and the register-constant form takes the register as b (SM86 FFMA form 3, constant in c). Modes go to SM86's fields (ffma known answers); .FMZ, the condition code and constant offsets from 0x8000 stop. Operands equal envydis on every census word; game programs as for FMUL |
| FLO R | stop | | no semantic check yet |
| FLO I | stop | | no semantic check yet |
| FLO C | stop | | no semantic check yet |
| FMNMX R | stop | | no semantic check yet |
| FMNMX I | stop | | no semantic check yet |
| FMNMX C | stop | | no semantic check yet |
| FMUL R | exact | FMUL | Ryujinx's Fmul multiplies the same operands (its .NEG negates the product) and scales by D2 to M8; the rounding mode, .FTZ, .SAT and scale go to the same SM86 fields, whose behavior the sm86 known answers pin (fmul: every mode with and without FTZ, SAT, the scales, negation). FMUL32I has no mode bits but .FTZ and .SAT. .FMZ, the condition code and constant offsets from 0x8000 stop. Operands equal envydis on every census word; whole game programs without MUFU draw the same bits as ShaderTools' on lavapipe |
| FMUL I | exact | FMUL | Ryujinx's Fmul multiplies the same operands (its .NEG negates the product) and scales by D2 to M8; the rounding mode, .FTZ, .SAT and scale go to the same SM86 fields, whose behavior the sm86 known answers pin (fmul: every mode with and without FTZ, SAT, the scales, negation). FMUL32I has no mode bits but .FTZ and .SAT. .FMZ, the condition code and constant offsets from 0x8000 stop. Operands equal envydis on every census word; whole game programs without MUFU draw the same bits as ShaderTools' on lavapipe |
| FMUL C | exact | FMUL | Ryujinx's Fmul multiplies the same operands (its .NEG negates the product) and scales by D2 to M8; the rounding mode, .FTZ, .SAT and scale go to the same SM86 fields, whose behavior the sm86 known answers pin (fmul: every mode with and without FTZ, SAT, the scales, negation). FMUL32I has no mode bits but .FTZ and .SAT. .FMZ, the condition code and constant offsets from 0x8000 stop. Operands equal envydis on every census word; whole game programs without MUFU draw the same bits as ShaderTools' on lavapipe |
| FMUL 32I | exact | FMUL | Ryujinx's Fmul multiplies the same operands (its .NEG negates the product) and scales by D2 to M8; the rounding mode, .FTZ, .SAT and scale go to the same SM86 fields, whose behavior the sm86 known answers pin (fmul: every mode with and without FTZ, SAT, the scales, negation). FMUL32I has no mode bits but .FTZ and .SAT. .FMZ, the condition code and constant offsets from 0x8000 stop. Operands equal envydis on every census word; whole game programs without MUFU draw the same bits as ShaderTools' on lavapipe |
| FSET R | stop | | no semantic check yet |
| FSET I | stop | | no semantic check yet |
| FSET C | stop | | no semantic check yet |
| FSETP R | stop | | no semantic check yet |
| FSETP I | stop | | no semantic check yet |
| FSETP C | stop | | no semantic check yet |
| FSWZADD | stop | | no semantic check yet |
| GETCRSPTR | stop | | no semantic check yet |
| GETLMEMBASE | stop | | no semantic check yet |
| HADD2 R | stop | | no semantic check yet |
| HADD2 I | stop | | no semantic check yet |
| HADD2 C | stop | | no semantic check yet |
| HADD2 32I | stop | | no semantic check yet |
| HFMA2 R | stop | | no semantic check yet |
| HFMA2 I | stop | | no semantic check yet |
| HFMA2 C | stop | | no semantic check yet |
| HFMA2 RC | stop | | no semantic check yet |
| HFMA2 32I | stop | | no semantic check yet |
| HMUL2 R | stop | | no semantic check yet |
| HMUL2 I | stop | | no semantic check yet |
| HMUL2 C | stop | | no semantic check yet |
| HMUL2 32I | stop | | no semantic check yet |
| HSET2 R | stop | | no semantic check yet |
| HSET2 I | stop | | no semantic check yet |
| HSET2 C | stop | | no semantic check yet |
| HSETP2 R | stop | | no semantic check yet |
| HSETP2 I | stop | | no semantic check yet |
| HSETP2 C | stop | | no semantic check yet |
| I2F R | stop | | no semantic check yet |
| I2F I | stop | | no semantic check yet |
| I2F C | stop | | no semantic check yet |
| I2I R | stop | | no semantic check yet |
| I2I I | stop | | no semantic check yet |
| I2I C | stop | | no semantic check yet |
| IADD R | stop | | no semantic check yet |
| IADD I | stop | | no semantic check yet |
| IADD C | stop | | no semantic check yet |
| IADD 32I | stop | | no semantic check yet |
| IADD3 R | stop | | no semantic check yet |
| IADD3 I | stop | | no semantic check yet |
| IADD3 C | stop | | no semantic check yet |
| ICMP R | stop | | no semantic check yet |
| ICMP I | stop | | no semantic check yet |
| ICMP C | stop | | no semantic check yet |
| ICMP RC | stop | | no semantic check yet |
| IDE | stop | | no semantic check yet |
| IDP R | stop | | no semantic check yet |
| IDP C | stop | | no semantic check yet |
| IMAD R | stop | | no semantic check yet |
| IMAD I | stop | | no semantic check yet |
| IMAD C | stop | | no semantic check yet |
| IMAD RC | stop | | no semantic check yet |
| IMAD 32I | stop | | no semantic check yet |
| IMADSP R | stop | | no semantic check yet |
| IMADSP I | stop | | no semantic check yet |
| IMADSP C | stop | | no semantic check yet |
| IMADSP RC | stop | | no semantic check yet |
| IMNMX R | stop | | no semantic check yet |
| IMNMX I | stop | | no semantic check yet |
| IMNMX C | stop | | no semantic check yet |
| IMUL R | stop | | no semantic check yet |
| IMUL I | stop | | no semantic check yet |
| IMUL C | stop | | no semantic check yet |
| IMUL 32I | stop | | no semantic check yet |
| IPA | exact and pseudo | IPA; IPA.MUL, IPA.SAT | Ryujinx's Ipa and the SM86 translator's IPA both give a perspective input times FragCoord.w and take the mode from the SPH input map (words 6 to 13 in both headers); .CONSTANT of a non-flat input stops in the translator. .MUL (times SrcB) and .SAT (clamp, NaN to 0) have no SM86 IPA form: pseudo, as Ryujinx (round to nearest, denormals kept, inferred). Indexed, offset, sample and .SC forms and a source predicate other than PT stop; operands equal envydis on every census word |
| ISBERD | stop | | no semantic check yet |
| ISCADD R | stop | | no semantic check yet |
| ISCADD I | stop | | no semantic check yet |
| ISCADD C | stop | | no semantic check yet |
| ISCADD 32I | stop | | no semantic check yet |
| ISET R | stop | | no semantic check yet |
| ISET I | stop | | no semantic check yet |
| ISET C | stop | | no semantic check yet |
| ISETP R | stop | | no semantic check yet |
| ISETP I | stop | | no semantic check yet |
| ISETP C | stop | | no semantic check yet |
| JCAL | stop | | subroutine call or return: the SM86 translator inlines CALL.REL through a return address register Maxwell does not have |
| JMP | stop | | indirect jump: its targets are in the program's constant bank 1, which the scan does not carry |
| JMX | stop | | indirect jump: its targets are in the program's constant bank 1, which the scan does not carry |
| KIL | exact | KILL | the same kill-thread instruction (envydis's kil, nvdisasm's KILL); the SM86 translator demotes, as for every KILL, and Ryujinx terminates (OpKill), which differs only in the quad's derivatives after the kill, undefined under OpKill. An unguarded KIL ends its path: the control-flow pass ends the invocation after it where nothing else reaches. A condition code test stops (tests/flow.rs) |
| LD | stop | | no semantic check yet |
| LDC | stop | | no semantic check yet |
| LDG | stop | | no semantic check yet |
| LDL | stop | | no semantic check yet |
| LDS | stop | | no semantic check yet |
| LEA R | stop | | no semantic check yet |
| LEA I | stop | | no semantic check yet |
| LEA C | stop | | no semantic check yet |
| LEA_HI R | stop | | no semantic check yet |
| LEA_HI C | stop | | no semantic check yet |
| LEPC | stop | | no semantic check yet |
| LONGJMP | stop | | no semantic check yet |
| LOP R | stop | | no semantic check yet |
| LOP I | stop | | no semantic check yet |
| LOP C | stop | | no semantic check yet |
| LOP3 R | stop | | no semantic check yet |
| LOP3 I | stop | | no semantic check yet |
| LOP3 C | stop | | no semantic check yet |
| LOP 32I | stop | | no semantic check yet |
| MEMBAR | stop | | no semantic check yet |
| MOV R | stop | | no semantic check yet |
| MOV I | stop | | no semantic check yet |
| MOV C | exact | MOV | Ryujinx's MovC copies c[CbufSlot][4 * CbufOffset] and the SM86 translator's MOV reads the same bank and byte offset; lane mask 0xf only, and offsets of 0x8000 and up stop (SM86 reads them as negative); operands equal envydis on every census word |
| MOV 32I | exact | MOV | Ryujinx's Mov32i copies Imm32 to Dest and the SM86 translator's MOV copies its immediate; lane mask 0xf only (every census word), other masks stop; operands equal envydis on every census word |
| MUFU | exact and pseudo | MUFU; MUFU.SAT | same function numbers (COS, SIN, EX2, LG2, RCP, RSQ, SQRT) and operand .NEG and .ABS; the SM86 translator's MUFU reproduces the SM86 unit bit for bit (mufu known answers). Maxwell's unit precision is console-only: the SM86 model is the pick (inferred, s2-console-questions; Ryujinx uses the host's functions). .SAT has no SM86 form: pseudo, the clamp of every saturating float result, checked on the mufu known answers. RCP64H and RSQ64H stop (no program of the census uses them). Operands equal envydis on every census word |
| NOP | exact | NOP | Ryujinx's Nop emits nothing and the SM86 translator's NOP emits nothing |
| OUT R | stop | | no semantic check yet |
| OUT I | stop | | no semantic check yet |
| OUT C | stop | | no semantic check yet |
| P2R R | stop | | no semantic check yet |
| P2R I | stop | | no semantic check yet |
| P2R C | stop | | no semantic check yet |
| PBK | exact | BSSY | the control-flow pass resolves the reconvergence stack as Ryujinx's decoder does (pushes propagated into their pops); the push becomes a BSSY on the barrier of its stack depth with the same target, where the guest's lanes meet again. A push at two depths stops (tests/flow.rs) |
| PCNT | stop | | PCNT and CONT: no program of the census uses them |
| PEXIT | stop | | no semantic check yet |
| PIXLD | stop | | no semantic check yet |
| PLONGJMP | stop | | no semantic check yet |
| POPC R | stop | | no semantic check yet |
| POPC I | stop | | no semantic check yet |
| POPC C | stop | | no semantic check yet |
| PRET | stop | | subroutine call or return: the SM86 translator inlines CALL.REL through a return address register Maxwell does not have |
| PRMT R | stop | | no semantic check yet |
| PRMT I | stop | | no semantic check yet |
| PRMT C | stop | | no semantic check yet |
| PRMT RC | stop | | no semantic check yet |
| PSET | stop | | no semantic check yet |
| PSETP | stop | | no semantic check yet |
| R2B | stop | | no semantic check yet |
| R2P R | stop | | no semantic check yet |
| R2P I | stop | | no semantic check yet |
| R2P C | stop | | no semantic check yet |
| RAM | stop | | no semantic check yet |
| RED | stop | | no semantic check yet |
| RET | stop | | subroutine call or return: the SM86 translator inlines CALL.REL through a return address register Maxwell does not have |
| RRO R | pseudo | RRO.SINCOS, RRO.EX2 | SM86 has no range reduction: MUFU.SIN and MUFU.COS take turns and MUFU.EX2 converts its operand itself. RRO.SINCOS is the operand times 1 / (2 pi) rounded toward zero, as NVIDIA's compiler emits for SM70 and later in its place, and RRO.EX2 the operand; with the MUFU after it this is Ryujinx's meaning (sine and cosine of radians, 2^x). The Maxwell format itself is console-only (inferred, s2-console-questions). Operands equal envydis on every census word |
| RRO I | pseudo | RRO.SINCOS, RRO.EX2 | SM86 has no range reduction: MUFU.SIN and MUFU.COS take turns and MUFU.EX2 converts its operand itself. RRO.SINCOS is the operand times 1 / (2 pi) rounded toward zero, as NVIDIA's compiler emits for SM70 and later in its place, and RRO.EX2 the operand; with the MUFU after it this is Ryujinx's meaning (sine and cosine of radians, 2^x). The Maxwell format itself is console-only (inferred, s2-console-questions). Operands equal envydis on every census word |
| RRO C | pseudo | RRO.SINCOS, RRO.EX2 | SM86 has no range reduction: MUFU.SIN and MUFU.COS take turns and MUFU.EX2 converts its operand itself. RRO.SINCOS is the operand times 1 / (2 pi) rounded toward zero, as NVIDIA's compiler emits for SM70 and later in its place, and RRO.EX2 the operand; with the MUFU after it this is Ryujinx's meaning (sine and cosine of radians, 2^x). The Maxwell format itself is console-only (inferred, s2-console-questions). Operands equal envydis on every census word |
| RTT | stop | | no semantic check yet |
| S2R | stop | | no semantic check yet |
| SAM | stop | | no semantic check yet |
| SEL R | stop | | no semantic check yet |
| SEL I | stop | | no semantic check yet |
| SEL C | stop | | no semantic check yet |
| SETCRSPTR | stop | | no semantic check yet |
| SETLMEMBASE | stop | | no semantic check yet |
| SHF_L R | stop | | no semantic check yet |
| SHF_R R | stop | | no semantic check yet |
| SHF_L I | stop | | no semantic check yet |
| SHF_R I | stop | | no semantic check yet |
| SHFL | stop | | no semantic check yet |
| SHL R | stop | | no semantic check yet |
| SHL I | stop | | no semantic check yet |
| SHL C | stop | | no semantic check yet |
| SHR R | stop | | no semantic check yet |
| SHR I | stop | | no semantic check yet |
| SHR C | stop | | no semantic check yet |
| SSY | exact | BSSY | the control-flow pass resolves the reconvergence stack as Ryujinx's decoder does (pushes propagated into their pops); the push becomes a BSSY on the barrier of its stack depth with the same target, where the guest's lanes meet again. A push at two depths stops (tests/flow.rs) |
| ST | stop | | no semantic check yet |
| STG | stop | | no semantic check yet |
| STL | stop | | no semantic check yet |
| STP | stop | | no semantic check yet |
| STS | stop | | no semantic check yet |
| SUATOM_B | stop | | no semantic check yet |
| SUATOM | stop | | no semantic check yet |
| SUATOM_B2 | stop | | no semantic check yet |
| SUATOM_CAS_B | stop | | no semantic check yet |
| SUATOM_CAS | stop | | no semantic check yet |
| SULD_DB | stop | | no semantic check yet |
| SULD_D | stop | | no semantic check yet |
| SULD_B | stop | | no semantic check yet |
| SULD | stop | | no semantic check yet |
| SURED_B | stop | | no semantic check yet |
| SURED | stop | | no semantic check yet |
| SUST_DB | stop | | no semantic check yet |
| SUST_D | stop | | no semantic check yet |
| SUST_B | stop | | no semantic check yet |
| SUST | stop | | no semantic check yet |
| SYNC | exact | BRA | for one thread a pop is a jump to the entry it pops (a BRK unwinds the SSY entries above its PBK, as Ryujinx's decoder does); Ryujinx emits a branch when the pop has one target, and the control-flow pass lowers only those, under the same guard. A pop with more than one target or none and a condition code test stop (tests/flow.rs) |
| TEX | stop | | no semantic check yet |
| TEX_B | stop | | no semantic check yet |
| TEXS | stop | | no semantic check yet |
| TEXS_F16 | stop | | no semantic check yet |
| TLD | stop | | no semantic check yet |
| TLD_B | stop | | no semantic check yet |
| TLDS | stop | | no semantic check yet |
| TLDS_F16 | stop | | no semantic check yet |
| TLD4 | stop | | no semantic check yet |
| TLD4_B | stop | | no semantic check yet |
| TLD4S | stop | | no semantic check yet |
| TLD4S_F16 | stop | | no semantic check yet |
| TMML | stop | | no semantic check yet |
| TMML_B | stop | | no semantic check yet |
| TXA | stop | | no semantic check yet |
| TXD | stop | | no semantic check yet |
| TXD_B | stop | | no semantic check yet |
| TXQ | stop | | no semantic check yet |
| TXQ_B | stop | | no semantic check yet |
| VABSDIFF | stop | | no semantic check yet |
| VABSDIFF4 | stop | | no semantic check yet |
| VADD | stop | | no semantic check yet |
| VMAD | stop | | no semantic check yet |
| VMNMX | stop | | no semantic check yet |
| VOTE | stop | | no semantic check yet |
| VOTEVTG | stop | | no semantic check yet |
| VSET | stop | | no semantic check yet |
| VSETP | stop | | no semantic check yet |
| VSHL | stop | | no semantic check yet |
| VSHR | stop | | no semantic check yet |
| XMAD R | stop | | no semantic check yet |
| XMAD I | stop | | no semantic check yet |
| XMAD C | stop | | no semantic check yet |
| XMAD RC | stop | | no semantic check yet |

293 forms: 25 exact, 2 exact and pseudo, 3 pseudo, 263 stop.
