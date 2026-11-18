//! The lowering table: one row per Maxwell form ([`Op`] and [`Src`]).

use std::fmt::Write as _;

use crate::form::{ENCODINGS, Op, Src};

/// How one Maxwell form reaches the translator.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Row {
    /// Lowered to the translator's instruction of exactly this SM86 form
    /// (nvdisasm's mnemonic), equal in behavior by the evidence named.
    Exact {
        to: &'static str,
        evidence: &'static str,
    },
    /// A pseudo-instruction the translator handles from the decoded
    /// operands, because no SM86 form behaves the same; the evidence names
    /// the difference.
    Pseudo {
        name: &'static str,
        evidence: &'static str,
    },
    /// Lowered to the SM86 form `to` in the cases the evidence covers and
    /// the pseudo-instruction `pseudo` in the rest.
    Split {
        to: &'static str,
        pseudo: &'static str,
        evidence: &'static str,
    },
    /// Not translated: a program that uses the form stops with this reason.
    Stop { reason: &'static str },
}

/// The stop of every form no check has settled yet.
pub const UNCHECKED: Row = Row::Stop {
    reason: "no semantic check yet",
};

/// The row of `op` in the form `src`.
pub fn row(op: Op, src: Src) -> Row {
    match (op, src) {
        (Op::Mov, Src::Imm32) => Row::Exact {
            to: "MOV",
            evidence: "Ryujinx's Mov32i copies Imm32 to Dest and the SM86 translator's MOV \
                       copies its immediate; lane mask 0xf only (every census word), other \
                       masks stop; operands equal envydis on every census word",
        },
        (Op::Mov, Src::CBuf) => Row::Exact {
            to: "MOV",
            evidence: "Ryujinx's MovC copies c[CbufSlot][4 * CbufOffset] and the SM86 \
                       translator's MOV reads the same bank and byte offset; lane mask 0xf \
                       only, and offsets of 0x8000 and up stop (SM86 reads them as \
                       negative); operands equal envydis on every census word",
        },
        (Op::Ald, _) => Row::Exact {
            to: "ALD",
            evidence: "same fields (offset, vertex register, size, .P, .O); Ryujinx's Ald and \
                       the SM86 translator read the same attribute map (generic 0x80 to \
                       0x27c, position 0x70, vertex id 0x2fc as VertexIndex, instance id \
                       0x2f8 as InstanceIndex - BaseInstance); a register in the address \
                       stops; operands equal envydis on every census word",
        },
        (Op::Ipa, _) => Row::Split {
            to: "IPA",
            pseudo: "IPA.MUL, IPA.SAT",
            evidence: "Ryujinx's Ipa and the SM86 translator's IPA both give a perspective \
                       input times FragCoord.w and take the mode from the SPH input map \
                       (words 6 to 13 in both headers); .CONSTANT of a non-flat input stops \
                       in the translator. .MUL (times SrcB) and .SAT (clamp, NaN to 0) have no \
                       SM86 IPA form: pseudo, as Ryujinx (round to nearest, denormals kept, \
                       inferred). Indexed, offset, sample and .SC forms and a source \
                       predicate other than PT stop; operands equal envydis on every census \
                       word",
        },
        (Op::Nop, _) => Row::Exact {
            to: "NOP",
            evidence: "Ryujinx's Nop emits nothing and the SM86 translator's NOP emits nothing",
        },
        (Op::Depbar, _) => Row::Exact {
            to: "NOP",
            evidence: "a scoreboard wait orders nothing a SPIR-V invocation can see: Ryujinx's \
                       Depbar emits nothing, as the SM86 translator does for DEPBAR",
        },
        (Op::Bra, _) => Row::Exact {
            to: "BRA",
            evidence: "Ryujinx's Bra branches to the same instruction under the guard; the \
                       target keeps its instruction (word k is Maxwell instruction k); a \
                       condition code test and .CA, .LMT, .U stop; synthetic programs \
                       translate whole (tests/flow.rs)",
        },
        (Op::Exit, _) => Row::Exact {
            to: "EXIT",
            evidence: "Ryujinx's Exit returns under the guard and the SM86 translator's EXIT \
                       writes the fragment outputs of the SPH and returns (Ryujinx does the \
                       same at its return); a condition code test and .KEEPREFCOUNT stop",
        },
        (Op::Kil, _) => Row::Exact {
            to: "KILL",
            evidence: "the same kill-thread instruction (envydis's kil, nvdisasm's KILL); the \
                       SM86 translator demotes, as for every KILL, and Ryujinx terminates \
                       (OpKill), which differs only in the quad's derivatives after the \
                       kill, undefined under OpKill. An unguarded KIL ends its path: the \
                       control-flow pass ends the invocation after it where nothing else \
                       reaches. A condition code test stops (tests/flow.rs)",
        },
        (Op::Ssy | Op::Pbk, _) => Row::Exact {
            to: "BSSY",
            evidence: "the control-flow pass resolves the reconvergence stack as Ryujinx's \
                       decoder does (pushes propagated into their pops); the push becomes a \
                       BSSY on the barrier of its stack depth with the same target, where the \
                       guest's lanes meet again. A push at two depths stops (tests/flow.rs)",
        },
        (Op::Sync | Op::Brk, _) => Row::Exact {
            to: "BRA",
            evidence: "for one thread a pop is a jump to the entry it pops (a BRK unwinds the \
                       SSY entries above its PBK, as Ryujinx's decoder does); Ryujinx emits a \
                       branch when the pop has one target, and the control-flow pass lowers \
                       only those, under the same guard. A pop with more than one target or \
                       none and a condition code test stop (tests/flow.rs)",
        },
        (Op::Fmul, _) => Row::Exact {
            to: "FMUL",
            evidence: "Ryujinx's Fmul multiplies the same operands (its .NEG negates the \
                       product) and scales by D2 to M8; the rounding mode, .FTZ, .SAT and \
                       scale become the same SM86 modifiers, whose behavior the sm86 known \
                       answers pin (fmul: every mode with and without FTZ, SAT, the scales, \
                       negation). FMUL32I has no mode bits but .FTZ and .SAT. .FMZ, the \
                       condition code and constant offsets from 0x8000 stop. Operands equal \
                       envydis on every census word; whole game programs without MUFU draw \
                       the same bits as ShaderTools' on lavapipe",
        },
        (Op::Fadd, _) => Row::Exact {
            to: "FADD",
            evidence: "Ryujinx's Fadd adds the operands with their .NEG and .ABS; the modes \
                       become SM86's modifiers (fadd known answers: every mode, FTZ, SAT, \
                       negation and absolute value); an immediate's .NEG and .ABS fold into \
                       its sign bit as the operand modifiers act. The condition code and \
                       constant offsets from 0x8000 stop. Operands equal envydis on every \
                       census word; game programs as for FMUL",
        },
        (Op::Ffma, _) => Row::Exact {
            to: "FFMA",
            evidence: "Ryujinx's Ffma is a * b + c with the product's and the addend's \
                       .NEG; FFMA32I adds its destination and the register-constant form \
                       takes the register as b (SM86 FFMA form 3, constant in c). Modes \
                       become SM86's modifiers (ffma known answers); .FMZ, the condition code and \
                       constant offsets from 0x8000 stop. Operands equal envydis on every \
                       census word; game programs as for FMUL",
        },
        (Op::Mufu, _) => Row::Split {
            to: "MUFU",
            pseudo: "MUFU.SAT",
            evidence: "same function numbers (COS, SIN, EX2, LG2, RCP, RSQ, SQRT) and \
                       operand .NEG and .ABS; the SM86 translator's MUFU reproduces the SM86 \
                       unit bit for bit (mufu known answers). Maxwell's unit precision is \
                       console-only: the SM86 model is the pick (inferred, \
                       s2-console-questions; Ryujinx uses the host's functions). .SAT has no \
                       SM86 form: pseudo, the clamp of every saturating float result, \
                       checked on the mufu known answers. RCP64H and RSQ64H stop (no program \
                       of the census uses them). Operands equal envydis on every census word",
        },
        (Op::Rro, _) => Row::Pseudo {
            name: "RRO.SINCOS, RRO.EX2",
            evidence: "SM86 has no range reduction: MUFU.SIN and MUFU.COS take turns and \
                       MUFU.EX2 converts its operand itself. RRO.SINCOS is the operand times \
                       1 / (2 pi) rounded toward zero, as NVIDIA's compiler emits for SM70 \
                       and later in its place, and RRO.EX2 the operand; with the MUFU after \
                       it this is Ryujinx's meaning (sine and cosine of radians, 2^x). The \
                       Maxwell format itself is console-only (inferred, s2-console-questions). \
                       Operands equal envydis on every census word",
        },
        (Op::Mov, Src::Reg) => Row::Exact {
            to: "MOV",
            evidence: "Ryujinx's MovR copies SrcA (bits 20 to 27) to Dest and the SM86 \
                       translator's MOV copies its register; lane mask 0xf only (every census \
                       word), other masks stop; operands equal envydis on every census word",
        },
        (Op::Fmnmx, _) => Row::Exact {
            to: "FMNMX",
            evidence: "Ryujinx's Fmnmx takes the minimum when the predicate holds and the \
                       maximum otherwise, after .NEG and .ABS of both operands; SM86 FMNMX \
                       has the same operands and predicate meaning (an immediate's .NEG and \
                       .ABS fold into its sign bit). The SM86 translator returns the other \
                       operand for one NaN and orders -0 below +0, as PTX defines min and \
                       max; Maxwell's own NaN and signed zero results are console-only \
                       (inferred, s2-console-questions). The condition code and constant \
                       offsets from 0x8000 stop. Operands equal envydis on every census word",
        },
        (Op::Fsetp, _) => Row::Exact {
            to: "FSETP",
            evidence: "Ryujinx's Fsetp compares the operands after .NEG and .ABS with the \
                       same sixteen tests (ordered and unordered), combines the result and \
                       its negation with the source predicate by AND, OR or XOR and writes \
                       both destinations, as the SM86 translator's FSETP does; bit 47 is \
                       .FTZ (envydis; Ryujinx's form also calls it the condition code bit, \
                       which FSETP never writes). An invalid boolean operation and constant \
                       offsets from 0x8000 stop. Operands equal envydis on every census word",
        },
        (Op::I2f, _) => Row::Split {
            to: "I2F",
            pseudo: "I2F.IABS, I2F.INEG",
            evidence: "same source sizes, signedness, byte or half selection and rounding \
                       modes (i2f known answers); Ryujinx's I2F extracts the selected byte or \
                       half and converts with the mode. Maxwell's integer .ABS and .NEG of a \
                       32-bit source have no SM86 form: pseudo, wrapping absolute value then \
                       negation before the conversion, as Ryujinx. F16 and F64 results, 64-bit \
                       sources, .ABS or .NEG of smaller sources and the condition code stop. \
                       Operands equal envydis on every census word",
        },
        (Op::F2i, _) => Row::Exact {
            to: "F2I",
            evidence: "same destination sizes and signedness, rounding (nearest even, floor, \
                       ceiling, toward zero), .FTZ and operand .NEG and .ABS; out of range \
                       values saturate on both (f2i known answers). A NaN gives 0: the SM86 \
                       word sets .NTZ, which ptxas adds for PTX's cvt (NaN to 0) on SM70 and \
                       later, while Maxwell's F2I has no such bit (envydis, Mesa NAK's SM50 \
                       encoder) and is taken to give 0 itself (inferred, \
                       s2-console-questions). F16 and F64 sources and the condition code \
                       stop. Operands equal envydis on every census word",
        },
        (Op::F2f, _) => Row::Exact {
            to: "FRND",
            evidence: "F2F.F32.F32 with an integer rounding (nearest even, floor, ceiling, \
                       toward zero) is SM86 FRND with the same rounding, .FTZ and operand \
                       .NEG and .ABS (frnd known answers); Ryujinx's F2f rounds the same way. \
                       Conversions between formats, F32 to F32 without an integer rounding, \
                       .SAT and the condition code stop. Operands equal envydis on every \
                       census word",
        },
        (Op::Ast, _) => Row::Exact {
            to: "AST",
            evidence: "same fields (offset, data register, vertex register, size, .P); \
                       Ryujinx's Ast and the SM86 translator write the same attribute map as \
                       for ALD; a register in the address stops. Operands equal envydis on \
                       every census word",
        },
        (Op::Texs, _) => Row::Exact {
            to: "TEX",
            evidence: "TEX.SCR on the bound handle c[2][TidB * 4] (every NVN1 driver writes 2 to the \
                       texture constant buffer index, 3D method 0x982; census TidB values are the \
                       even words from 0x20, the low words of NVN's 64-bit handles). Ryujinx's \
                       TEXS reads each target's sources from Ra, Ra + 1, Rb, Rb + 1 in the order \
                       the SM86 translator's .SCR split takes them (array index, coordinates, LOD, \
                       depth reference) and writes the components of its write-mask table to Rd, \
                       Rd + 1, Rd2, Rd2 + 1 as SM86 TEX does. Target and components equal envydis \
                       on every census word. An explicit LOD with a depth compare and the array \
                       depth compare (Ryujinx reads the reference before the LOD; no census word) \
                       and an empty write mask stop",
        },
        (Op::Tex | Op::TexB, _) => Row::Exact {
            to: "TEX",
            evidence: "SM86 TEX on the bound handle c[2][TidB * 4] (TEX) or a register handle \
                       (TEX.B), same dimension, LOD mode (none, LZ, LB, LL), offsets, depth \
                       compare and component mask. Ryujinx's TEX takes the array index and \
                       coordinates from Ra on and the handle, LOD or bias, offsets and depth \
                       reference from Rb on, the order the SM86 translator reads; Maxwell \
                       writes the components to Rd on, SM86 to Rd, Rd + 1, Rd2 = Rd + 2, \
                       Rd + 3. Operands equal envydis on every census word. .LC, the array LOD \
                       modes, .NDV, a sparse predicate and the 3D array stop",
        },
        (Op::Tld4s, _) => Row::Exact {
            to: "TLD4",
            evidence: "TLD4.SCR of a 2D texture on c[2][TidB * 4] with the same component, \
                       offsets (six bits per coordinate in both) and depth compare; Ryujinx's \
                       TLD4S reads coordinates, offsets and reference from Ra, Ra + 1, Rb, \
                       Rb + 1 in the order .SCR splits them and writes four components (two \
                       with Rd2 RZ). Operands equal envydis on every census word",
        },
        (Op::Tlds, _) => Row::Exact {
            to: "TLD",
            evidence: "TLD.SCR on c[2][TidB * 4] with each target's dimension, LOD mode, \
                       offsets and sample; Ryujinx's TLDS reads each target's sources from Ra, \
                       Ra + 1, Rb, Rb + 1 in the order .SCR splits them, except the 2D array \
                       target (layer in Ra after the coordinates in Rb), which stops. \
                       Operands equal envydis on every census word",
        },
        (Op::TldB, _) => Row::Exact {
            to: "TLD",
            evidence: "TLD.B with the same dimension, LOD mode (LZ, LL), offsets and sample; \
                       Ryujinx's TLD reads the layer and coordinates from Ra on and the \
                       handle, LOD, offsets and sample from Rb on, as the SM86 translator. \
                       Operands equal envydis on every census word. .CL, a sparse predicate \
                       and the 3D array stop",
        },
        (Op::Txq | Op::TxqB, _) => Row::Exact {
            to: "TXQ",
            evidence: "TXQ TEX_HEADER_DIMENSION on c[2][TidB * 4] or a register handle with \
                       the same component mask; Ryujinx's TXQ reads the handle (bindless) and \
                       the level from Ra on, as the SM86 translator. Operands equal envydis on \
                       every census word. Other queries stop",
        },
        (Op::Pcnt | Op::Cont, _) => Row::Stop {
            reason: "PCNT and CONT: no program of the census uses them",
        },
        (Op::Cal | Op::Ret | Op::Pret | Op::Jcal, _) => Row::Stop {
            reason: "subroutine call or return: the SM86 translator inlines CALL.REL through a \
                     return address register Maxwell does not have",
        },
        (Op::Brx | Op::Jmx | Op::Jmp, _) => Row::Stop {
            reason: "indirect jump: its targets are in the program's constant bank 1, which the \
                     scan does not carry",
        },
        _ => UNCHECKED,
    }
}

/// Every form once, in the order of [`ENCODINGS`].
pub fn forms() -> Vec<(Op, Src)> {
    let mut out: Vec<(Op, Src)> = Vec::new();
    for e in &ENCODINGS {
        if !out.contains(&(e.op, e.src)) {
            out.push((e.op, e.src));
        }
    }
    out
}

fn src_name(src: Src) -> &'static str {
    match src {
        Src::None => "",
        Src::Reg => "R",
        Src::Imm => "I",
        Src::CBuf => "C",
        Src::RegCBuf => "RC",
        Src::Imm32 => "32I",
    }
}

/// The table as Markdown: per form its row kind, target and evidence.
pub fn markdown() -> String {
    let mut s = String::from(
        "# Maxwell lowering table\n\n\
         Written by `RELIFT_BLESS=1 cargo test -p relift-gpu-sm53 --test lowering`\n\
         from `src/lower/table.rs`; do not edit by hand. Each Maxwell form is\n\
         lowered to exactly one SM86 form, handled as a pseudo-instruction,\n\
         split between the two by case, or a stop. A form is lowered only\n\
         with evidence that both behave the same in every case it lowers;\n\
         the cases the evidence does not cover stop with their own reason.\n\n\
         The lowering builds the translator's instruction of the SM86 form\n\
         directly, as the SM86 decoder gives it (no SM86 word is encoded), so\n\
         an exact row rests on three things: what the Maxwell form does\n\
         (Ryujinx's emitter, and envydis's operands on every census word);\n\
         that the built instruction has the same operands and modifiers,\n\
         which `tests/lowering.rs` pins as text in nvdisasm's spelling of the\n\
         SM86 form (each text was checked against nvdisasm when the lowering\n\
         still encoded SM86 words); and what the translator does with that\n\
         instruction, which the SM86 known answers (`gpus/sm86/kat`, an RTX\n\
         3090) pin. A pseudo row rests on the first and on the translator's\n\
         handling in `relift_gpu_nv::translate::pseudo`.\n\n\
         | form | row | to | evidence or reason |\n|---|---|---|---|\n",
    );
    let (mut exact, mut split, mut pseudo, mut stop) = (0, 0, 0, 0);
    for (op, src) in forms() {
        let name = match src_name(src) {
            "" => op.name().to_string(),
            k => format!("{} {k}", op.name()),
        };
        let line = match row(op, src) {
            Row::Exact { to, evidence } => {
                exact += 1;
                format!("| {name} | exact | {to} | {evidence} |")
            }
            Row::Split {
                to,
                pseudo: p,
                evidence,
            } => {
                split += 1;
                format!("| {name} | exact and pseudo | {to}; {p} | {evidence} |")
            }
            Row::Pseudo { name: p, evidence } => {
                pseudo += 1;
                format!("| {name} | pseudo | {p} | {evidence} |")
            }
            Row::Stop { reason } => {
                stop += 1;
                format!("| {name} | stop | | {reason} |")
            }
        };
        let _ = writeln!(s, "{line}");
    }
    let _ = write!(
        s,
        "\n{} forms: {exact} exact, {split} exact and pseudo, {pseudo} pseudo, {stop} stop.\n",
        exact + split + pseudo + stop
    );
    s
}
