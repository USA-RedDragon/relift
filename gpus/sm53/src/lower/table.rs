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
    /// Not translated: a program that uses the form stops with this reason.
    Stop { reason: &'static str },
}

/// The stop of every form no check has settled yet.
pub const UNCHECKED: Row = Row::Stop {
    reason: "no semantic check yet",
};

/// The row of `op` in the form `src`.
pub fn row(op: Op, src: Src) -> Row {
    let _ = (op, src);
    UNCHECKED
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
         lowered to exactly one SM86 form, handled as a pseudo-instruction, or\n\
         a stop. A form is lowered only with evidence that both behave the same\n\
         in every case.\n\n\
         | form | row | to | evidence or reason |\n|---|---|---|---|\n",
    );
    let (mut exact, mut pseudo, mut stop) = (0, 0, 0);
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
        "\n{} forms: {exact} exact, {pseudo} pseudo, {stop} stop.\n",
        exact + pseudo + stop
    );
    s
}
