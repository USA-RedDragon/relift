//! One template per encoding: finds the Sail decode clause of an AARCHMRS
//! encoding, runs the staged evaluator once per value of the bits it must
//! split on, and writes the Rust function.

use std::fmt::Write as _;

use relift_cpu_a64::decode::Encoding;

use crate::eval::{E, Gen};
use crate::model::Model;

/// One `__DecodeA64_*` clause's bit pattern.
#[derive(Clone, Debug)]
pub struct DecodeClause {
    /// The clause index within it.
    pub index: usize,
    /// The fixed bits.
    pub mask: u32,
    /// Their values.
    pub value: u32,
}

/// The decode group of a word, as in the Sail top-level decoder.
pub fn group_of(word: u32) -> Option<&'static str> {
    let op0 = (word >> 25) & 0xf;
    Some(match op0 {
        0 if word >> 31 == 1 => "__DecodeA64_SME",
        0 => "__DecodeA64_Reserved",
        1 => "__DecodeA64_Unallocated1",
        2 => "__DecodeA64_SVE",
        3 => "__DecodeA64_Unallocated2",
        8 | 9 => "__DecodeA64_DataProcImm",
        10 | 11 => "__DecodeA64_BranchExcSys",
        _ if op0 & 0b0101 == 0b0100 => "__DecodeA64_LoadStore",
        _ if op0 & 0b0111 == 0b0101 => "__DecodeA64_DataProcReg",
        _ if op0 & 0b0111 == 0b0111 => "__DecodeA64_DataProcFPSIMD",
        _ => return None,
    })
}

/// The bit patterns of every clause of `group`, in order.
pub fn clauses(m: &Model, group: &str) -> Vec<DecodeClause> {
    let mut out = Vec::new();
    for (index, raw) in m.funcs.get(group).into_iter().flatten().enumerate() {
        let Some(start) = raw.find("((pc, ([") else {
            continue;
        };
        let rest = &raw[start + 8..];
        let Some(end) = rest.find("] as __opcode") else {
            continue;
        };
        let bits: Vec<&str> = rest[..end].split(',').map(str::trim).collect();
        if bits.len() != 32 {
            continue;
        }
        let (mut mask, mut value) = (0u32, 0u32);
        for (i, b) in bits.iter().enumerate() {
            let pos = 31 - i as u32;
            match *b {
                "bitzero" => mask |= 1 << pos,
                "bitone" => {
                    mask |= 1 << pos;
                    value |= 1 << pos;
                }
                _ => {}
            }
        }
        out.push(DecodeClause { index, mask, value });
    }
    out
}

/// The first decode clause that takes the words whose `known` bits are
/// `value`: `Ok(None)` when none does, `Err(bits)` when the choice depends on
/// these further bits.
fn clause_for(cls: &[DecodeClause], known: u32, value: u32) -> Result<Option<DecodeClause>, u32> {
    for c in cls {
        let both = c.mask & known;
        if (c.value ^ value) & both != 0 {
            continue;
        }
        if c.mask & !known != 0 {
            return Err(c.mask & !known);
        }
        return Ok(Some(c.clone()));
    }
    Ok(None)
}

/// Whether some word with the `known` bits `value` decodes to `enc`.
fn reachable(enc: Encoding, known: u32, value: u32) -> bool {
    let mut x: u32 = 0x9e37_79b9 ^ value;
    for _ in 0..512 {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        let w = value | (x & !known);
        if relift_cpu_a64::decode::decode(w).map(|i| i.encoding) == Some(enc) {
            return true;
        }
    }
    false
}

/// A generated template.
pub struct Template {
    /// The function's Rust source.
    pub code: String,
}

/// The most bits a template may split on.
const MAX_SPLIT: u32 = 8;

/// Generates the template of `enc`.
pub fn template(m: &Model, enc: Encoding) -> Result<Template, String> {
    let group = group_of(enc.value()).ok_or("no decode group")?;
    let pats = clauses(m, group);
    let cls = match m.func(group) {
        Some(Ok(c)) => c,
        Some(Err(e)) => return Err(e),
        None => return Err("no decode function".into()),
    };
    let mut split = 0u32;
    'outer: loop {
        if split.count_ones() > MAX_SPLIT {
            return Err(format!("needs a split on {} bits", split.count_ones()));
        }
        let combos = 1u32 << split.count_ones();
        let mut arms = Vec::new();
        let mut failed: Vec<String> = Vec::new();
        let mut good = 0u32;
        let mut cut = std::collections::BTreeSet::new();
        for i in 0..combos {
            let val = deposit(i, split);
            let (known, value) = (enc.mask() | split, enc.value() | val);
            if split != 0 && !reachable(enc, known, value) {
                arms.push((
                    val,
                    vec!["return Err(LiftError::Undefined(ENC));".to_string()],
                ));
                continue;
            }
            let dc = match clause_for(&pats, known, value) {
                Ok(Some(c)) => c,
                Ok(None) => return Err(format!("no clause of {group} matches")),
                Err(more) => {
                    split |= more;
                    continue 'outer;
                }
            };
            let mut g = Gen::new(m, known, value);
            let r = g.run_decode(&cls[dc.index]);
            let excl = g.excl_pass.clone();
            cut.extend(g.used_cut.iter().copied());
            let mut lines = g.take();
            match r {
                Ok(()) => {
                    good += 1;
                    if excl.is_some() {
                        return Err("exclusive monitor check without a store".into());
                    }
                }
                Err(E::Throw(kind)) => lines.push(format!("return Err(LiftError::{kind}(ENC));")),
                Err(E::Split(d)) => {
                    let d = d & !(enc.mask() | split);
                    if d == 0 {
                        return Err("split on known bits".into());
                    }
                    split |= d;
                    continue 'outer;
                }
                Err(E::Fail(e)) if split == 0 => return Err(e),
                Err(E::Fail(e)) => {
                    failed.push(e);
                    lines = vec!["return Err(LiftError::Unsupported(ENC));".to_string()];
                }
                Err(E::NoMatch) => return Err("unmatched call".into()),
            }
            let lines = hoist_align(lines)?;
            arms.push((val, dead_lines(lines)));
        }
        if !failed.is_empty() && good == 0 {
            return Err(failed.swap_remove(0));
        }
        let mut code = String::new();
        let name = enc.name();
        let _ = writeln!(code, "/// `{name}`.");
        if split != 0 {
            let _ = writeln!(code, "///");
            let _ = writeln!(code, "/// Split on the word bits `{split:#010x}`.");
            if let Some(e) = failed.first() {
                let _ = writeln!(
                    code,
                    "/// {} of {combos} arms are unsupported: {}.",
                    failed.len(),
                    e.replace('`', "'")
                );
            }
        }
        let _ = writeln!(code, "///");
        let _ = writeln!(
            code,
            "/// Cut: {}.",
            cut.iter().copied().collect::<Vec<_>>().join(", ")
        );
        let _ = writeln!(
            code,
            "pub(super) fn {name}(word: u32, b: &mut Builder, st: &mut St) -> Result<(), LiftError> {{"
        );
        let _ = writeln!(code, "const ENC: Encoding = Encoding::{name};");
        let _ = writeln!(code, "let w = u128::from(word);");
        let _ = writeln!(code, "let pc = u128::from(b.pc());");
        if split == 0 {
            for l in &arms[0].1 {
                let _ = writeln!(code, "{l}");
            }
            let _ = writeln!(code, "Ok(())");
        } else {
            let _ = writeln!(code, "match word & {split:#010x} {{");
            for (val, lines) in &arms {
                let _ = writeln!(code, "{val:#010x} => {{");
                for l in lines {
                    let _ = writeln!(code, "{l}");
                }
                let _ = writeln!(code, "Ok(())");
                let _ = writeln!(code, "}}");
            }
            let _ = writeln!(code, "_ => unreachable!(),");
            let _ = writeln!(code, "}}");
        }
        let _ = writeln!(code, "}}");
        return Ok(Template { code });
    }
}

const PURE_CALLS: &[&str] = &[
    "b.konst(",
    "b.bin(",
    "b.cmp(",
    "b.un(",
    "b.not(",
    "b.and(",
    "b.or(",
    "b.select(",
    "b.zext(",
    "b.sext(",
    "b.extract(",
    "b.concat(",
    "b.trunc(",
    "st.x_read(",
    "st.v_read(",
    "st.sp_read(",
    "st.get(",
];

/// The name a removable line defines: a static `let` or a dynamic one whose
/// right side is a pure builder call or a select between values.
fn pure_def(line: &str) -> Option<&str> {
    let rest = line.strip_prefix("let ")?;
    let (lhs, rhs) = rest.split_once(" = ")?;
    let name = lhs.split(':').next()?.trim();
    if !name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
        return None;
    }
    if lhs.contains(':') {
        return Some(name);
    }
    let pure = PURE_CALLS.iter().any(|c| rhs.starts_with(c))
        || (rhs.starts_with("if ") && !EFFECTS.iter().any(|e| rhs.contains(e)));
    pure.then_some(name)
}

/// The builder calls with an effect on memory or the monitor.
const EFFECTS: &[&str] = &[
    "b.load",
    "b.store",
    "b.atomic_rmw(",
    "b.compare_swap(",
    "b.fence(",
    "b.clear_exclusive(",
];

/// Moves every alignment check (a guard) above the template's first memory
/// effect, with the pure definitions it reads: the IR wants every guard
/// before every effect, and a later access of a pair checks its own
/// address. Fails when a check reads a value only an effect defines.
fn hoist_align(lines: Vec<String>) -> Result<Vec<String>, String> {
    let is_effect = |l: &str| EFFECTS.iter().any(|e| l.contains(e)) && !l.contains("align::check");
    let Some(first) = lines.iter().position(|l| is_effect(l)) else {
        return Ok(lines);
    };
    let mut moved = vec![false; lines.len()];
    let def_at = |n: &str| -> Option<usize> {
        lines.iter().position(|l| {
            l.strip_prefix("let ")
                .and_then(|r| r.split_once(" = "))
                .is_some_and(|(lhs, _)| lhs.split(':').next().map(str::trim) == Some(n))
        })
    };
    for i in first..lines.len() {
        if !lines[i].contains("align::check") {
            continue;
        }
        let mut todo = vec![i];
        while let Some(j) = todo.pop() {
            if moved[j] {
                continue;
            }
            moved[j] = true;
            for t in idents(&lines[j]) {
                if !(t.starts_with('v') || t.starts_with('s'))
                    || !t[1..].chars().all(|c| c.is_ascii_digit())
                {
                    continue;
                }
                if let Some(k) = def_at(t)
                    && k > first
                    && !moved[k]
                {
                    if pure_def(&lines[k]).is_none() {
                        return Err(format!("alignment check reads {t}, defined by an effect"));
                    }
                    todo.push(k);
                }
            }
        }
    }
    let mut out: Vec<String> = lines[..first].to_vec();
    for (i, l) in lines.iter().enumerate().skip(first) {
        if moved[i] {
            out.push(l.clone());
        }
    }
    for (i, l) in lines.iter().enumerate().skip(first) {
        if !moved[i] {
            out.push(l.clone());
        }
    }
    Ok(out)
}

fn idents(line: &str) -> impl Iterator<Item = &str> {
    line.split(|c: char| !(c.is_ascii_alphanumeric() || c == '_'))
        .filter(|t| !t.is_empty())
}

/// Drops the lines that define a value nothing reads.
fn dead_lines(lines: Vec<String>) -> Vec<String> {
    let mut used = std::collections::HashSet::new();
    let mut keep = vec![true; lines.len()];
    for (i, l) in lines.iter().enumerate().rev() {
        if let Some(n) = pure_def(l)
            && !used.contains(n)
        {
            keep[i] = false;
            continue;
        }
        let def = pure_def(l);
        for t in idents(l) {
            if Some(t) != def {
                used.insert(t.to_string());
            }
        }
    }
    lines
        .into_iter()
        .zip(keep)
        .filter_map(|(l, k)| k.then_some(l))
        .collect()
}

/// Spreads the low bits of `x` over the set bits of `m`.
fn deposit(x: u32, m: u32) -> u32 {
    let mut out = 0;
    let mut k = 0;
    for b in 0..32 {
        if m >> b & 1 == 1 {
            if x >> k & 1 == 1 {
                out |= 1 << b;
            }
            k += 1;
        }
    }
    out
}
