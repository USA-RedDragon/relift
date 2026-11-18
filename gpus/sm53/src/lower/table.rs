//! The lowering table: one row per Maxwell form ([`Op`] and [`Src`]).

use std::fmt::Write as _;

use crate::form::{ENCODINGS, Op, Src};

/// How one Maxwell form reaches the translator.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Row {
    /// Lowered to exactly this SM86 form (nvdisasm's mnemonic), equal in
    /// behavior by the evidence named.
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
