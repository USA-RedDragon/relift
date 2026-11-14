//! One template per encoding: finds the Sail decode clause of an encoding,
//! runs the staged evaluator once per value of the bits it must split on,
//! and writes the Rust function. What is per CPU (the decode group of a
//! word, which encoding a word decodes to, which clauses are allowed) comes
//! from the [`Form`] the caller gives.

use std::fmt::Write as _;

use crate::ast::{Expr, Lit, Pat, Typ, TypArg};
use crate::eval::{Cpu, E, Gen};
use crate::model::Model;

/// One encoding to generate, as the CPU layer describes it.
pub trait Form {
    /// The encoding's name; the template is a function of this name and
    /// `Encoding::<name>` is its id.
    fn name(&self) -> String;
    /// The word bits fixed for every word of the encoding, plus any bits the
    /// CPU layer fixes while generating.
    fn mask(&self) -> u32;
    /// Their values.
    fn value(&self) -> u32;
    /// The width of the instruction word in bits (32 or 16).
    fn word_bits(&self) -> u32 {
        32
    }
    /// The Sail decode function whose clauses take the encoding's words.
    fn group(&self) -> Option<&'static str>;
    /// Whether some word with the `known` bits `value` decodes to this
    /// encoding.
    fn reachable(&self, known: u32, value: u32) -> bool;
    /// Checks the decode clause chosen for the words (by its raw text);
    /// a CPU that restricts decoding to a profile refuses clauses outside it.
    fn accept_clause(&self, _raw: &str) -> Result<(), String> {
        Ok(())
    }
}

/// Builder calls and state reads without effects, which the dead-line pass
/// may drop; `extra` adds the CPU layer's own.
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
    "st.get(",
];

/// The per-CPU hooks of the staging loop that are not on the evaluator.
pub trait Stage: Cpu {
    /// State reads of the CPU's `St` without effects.
    const PURE_READS: &'static [&'static str] = &[];
    /// Checks a finished run; a CPU layer refuses a run that left work undone.
    fn finish_run(_g: &Gen<'_, Self>) -> Result<(), String> {
        Ok(())
    }
    /// Sets up the evaluator before the decode clause runs.
    fn setup(_g: &mut Gen<'_, Self>) {}
}

/// One decode clause's bit pattern.
#[derive(Clone, Debug)]
pub struct DecodeClause {
    /// The clause index within it.
    pub index: usize,
    /// The fixed bits.
    pub mask: u32,
    /// Their values.
    pub value: u32,
}

/// The bit patterns of every clause of `group`, in order, for words of
/// `bits` bits.
pub fn clauses(m: &Model, group: &str, bits: u32) -> Vec<DecodeClause> {
    let parsed = match m.func(group) {
        Some(Ok(c)) => Some(c),
        _ => None,
    };
    let mut out = Vec::new();
    for (index, raw) in m.funcs.get(group).into_iter().flatten().enumerate() {
        let from_ast = || {
            let c = parsed.as_ref()?.get(index)?;
            let p = match &c.params {
                Pat::Tuple(ps) if ps.len() == 2 => &ps[1],
                p => p,
            };
            pat_bits(p, bits)
        };
        if let Some((mask, value)) = clause_bits(raw, bits).or_else(from_ast) {
            out.push(DecodeClause { index, mask, value });
        }
    }
    out
}

/// The fixed bits and their values of an opcode pattern of `nbits` bits
/// written as a literal (`0x3F`, `0b0110`), a bit list, or a concatenation
/// of literals and typed names (`0b01 @ r : bits(3) @ 0b110`); `None` for
/// a pattern that fixes no bits or whose widths are unknown.
fn pat_bits(p: &Pat, nbits: u32) -> Option<(u32, u32)> {
    let (w, mask, value) = pat_parts(p)?;
    (w == nbits && mask != 0).then_some((mask, value))
}

/// The width, fixed bits and values of a pattern.
fn pat_parts(p: &Pat) -> Option<(u32, u32, u32)> {
    match p {
        Pat::Lit(Lit::Bits { width, value }) if *width <= 32 => {
            let all = if *width == 32 {
                u32::MAX
            } else {
                (1u32 << width) - 1
            };
            Some((*width, all, u32::try_from(*value).ok()?))
        }
        Pat::Lit(Lit::Bit(b)) => Some((1, 1, u32::from(*b))),
        Pat::As(q, _) => pat_parts(q),
        Pat::Typed(q, t) => {
            let w = bits_of(t)?;
            match pat_parts(q) {
                Some((qw, m, v)) => (qw == w).then_some((w, m, v)),
                None => matches!(q.as_ref(), Pat::Id(_) | Pat::Wild).then_some((w, 0, 0)),
            }
        }
        Pat::Vec(ps) | Pat::Concat(ps) => {
            let (mut w, mut mask, mut value) = (0u32, 0u32, 0u32);
            for q in ps {
                let (qw, m, v) = match (p, q) {
                    (Pat::Vec(_), Pat::Wild | Pat::Id(_)) => (1, 0, 0),
                    _ => pat_parts(q)?,
                };
                w += qw;
                if w > 32 {
                    return None;
                }
                mask = mask.checked_shl(qw).unwrap_or(0) | m;
                value = value.checked_shl(qw).unwrap_or(0) | v;
            }
            Some((w, mask, value))
        }
        _ => None,
    }
}

/// The width of `bits(N)`.
fn bits_of(t: &Typ) -> Option<u32> {
    match t {
        Typ::App(n, args) if n == "bits" => match args.as_slice() {
            [TypArg::Expr(Expr::Lit(Lit::Int(w)))] => u32::try_from(*w).ok(),
            _ => None,
        },
        _ => None,
    }
}

/// The fixed bits and their values of a decode clause's opcode pattern,
/// written either as a list of `bitzero`, `bitone` and `_` or as one binary
/// literal; `None` for the catch-all clause.
fn clause_bits(raw: &str, nbits: u32) -> Option<(u32, u32)> {
    if let Some(start) = raw.find("((pc, ([") {
        let rest = &raw[start + 8..];
        let end = rest.find("] as __opcode")?;
        let bits: Vec<&str> = rest[..end].split(',').map(str::trim).collect();
        if bits.len() != nbits as usize {
            return None;
        }
        let (mut mask, mut value) = (0u32, 0u32);
        for (i, b) in bits.iter().enumerate() {
            let pos = nbits - 1 - i as u32;
            match *b {
                "bitzero" => mask |= 1 << pos,
                "bitone" => {
                    mask |= 1 << pos;
                    value |= 1 << pos;
                }
                _ => {}
            }
        }
        return Some((mask, value));
    }
    let start = raw.find("((pc, (0b")?;
    let rest = &raw[start + 9..];
    let end = rest.find(" as __opcode")?;
    let lit = &rest[..end];
    if lit.len() != nbits as usize {
        return None;
    }
    let all = if nbits == 32 {
        u32::MAX
    } else {
        (1u32 << nbits) - 1
    };
    Some((all, u32::from_str_radix(lit, 2).ok()?))
}

/// The position in `cls` of the first decode clause from `from` on that
/// takes the words whose `known` bits are `value`: `Ok(None)` when none
/// does, `Err(bits)` when the choice depends on these further bits.
fn clause_for(
    cls: &[DecodeClause],
    from: usize,
    known: u32,
    value: u32,
) -> Result<Option<usize>, u32> {
    for (i, c) in cls.iter().enumerate().skip(from) {
        let both = c.mask & known;
        if (c.value ^ value) & both != 0 {
            continue;
        }
        if c.mask & !known != 0 {
            return Err(c.mask & !known);
        }
        return Ok(Some(i));
    }
    Ok(None)
}

/// A generated template.
pub struct Template {
    /// The function's Rust source.
    pub code: String,
}

/// The most bits a template may split on.
const MAX_SPLIT: u32 = 8;

/// Generates the template of `enc`.
pub fn template<C: Stage>(m: &Model, enc: &dyn Form) -> Result<Template, String> {
    let group = enc.group().ok_or("no decode group")?;
    let nbits = enc.word_bits();
    let pats = clauses(m, group, nbits);
    let cls = match m.func(group) {
        Some(Ok(c)) => c,
        Some(Err(e)) => return Err(e),
        None => return Err("no decode function".into()),
    };
    let raws = &m.funcs[group];
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
            if split != 0 && !enc.reachable(known, value) {
                arms.push((
                    val,
                    vec!["return Err(LiftError::Undefined(ENC));".to_string()],
                ));
                continue;
            }
            // Sail's decoder retries a word once after an `Error_See`, with
            // only the clauses after the one that threw it.
            let mut from = 0;
            let (g, r) = loop {
                let at = match clause_for(&pats, from, known, value) {
                    Ok(Some(at)) => at,
                    Ok(None) if from == 0 => return Err(format!("no clause of {group} matches")),
                    Ok(None) => return Err("Error_See and no later clause takes the word".into()),
                    Err(more) => {
                        split |= more;
                        continue 'outer;
                    }
                };
                enc.accept_clause(&raws[pats[at].index])?;
                let mut g: Gen<'_, C> = Gen::new(m, known, value);
                C::setup(&mut g);
                let r = g.run_decode(&cls[pats[at].index], nbits);
                if !matches!(r, Err(E::See)) {
                    break (g, r);
                }
                if from != 0 {
                    return Err("Error_See on the retry".into());
                }
                from = at + 1;
            };
            let fin = C::finish_run(&g);
            cut.extend(g.used_cut.iter().copied());
            let mut lines = g.take();
            match r {
                Ok(()) => {
                    good += 1;
                    fin?;
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
                Err(E::See) => unreachable!("the decode loop retries Error_See"),
            }
            let lines = hoist_align::<C>(lines)?;
            arms.push((val, dead_lines::<C>(lines)));
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

/// The name a removable line defines: a static `let` or a dynamic one whose
/// right side is a pure builder call or a select between values.
fn pure_def<C: Stage>(line: &str) -> Option<&str> {
    let rest = line.strip_prefix("let ")?;
    let (lhs, rhs) = rest.split_once(" = ")?;
    let name = lhs.split(':').next()?.trim();
    if !name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
        return None;
    }
    if lhs.contains(':') {
        return Some(name);
    }
    let pure = PURE_CALLS
        .iter()
        .chain(C::PURE_READS)
        .any(|c| rhs.starts_with(c))
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
fn hoist_align<C: Stage>(lines: Vec<String>) -> Result<Vec<String>, String> {
    let is_effect = |l: &str| EFFECTS.iter().any(|e| l.contains(e)) && !l.contains("align::");
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
        if !lines[i].contains("align::") {
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
                    if pure_def::<C>(&lines[k]).is_none() {
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
fn dead_lines<C: Stage>(lines: Vec<String>) -> Vec<String> {
    let mut used = std::collections::HashSet::new();
    let mut keep = vec![true; lines.len()];
    for (i, l) in lines.iter().enumerate().rev() {
        if let Some(n) = pure_def::<C>(l)
            && !used.contains(n)
        {
            keep[i] = false;
            continue;
        }
        let def = pure_def::<C>(l);
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
pub fn deposit(x: u32, m: u32) -> u32 {
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

#[cfg(test)]
mod tests {
    use super::*;

    fn bits(text: &str, n: u32) -> Option<(u32, u32)> {
        let p = crate::parse::Parser::new(text)
            .and_then(|mut p| p.pat())
            .expect("pattern parses");
        pat_bits(&p, n)
    }

    #[test]
    fn typed_literal_patterns_give_their_fixed_bits() {
        assert_eq!(bits("0x3F", 8), Some((0xFF, 0x3F)));
        assert_eq!(
            bits("0b01 @ d : bits(3) @ 0b110", 8),
            Some((0b1100_0111, 0b0100_0110))
        );
        assert_eq!(
            bits("(0b001 @ cc : bits(2) @ 0b000)", 8),
            Some((0xE7, 0x20))
        );
        assert_eq!(bits("[bitone, _, bitzero, _]", 4), Some((0b1010, 0b1000)));
        assert_eq!(bits("op", 8), None);
        assert_eq!(bits("0b01 @ x", 8), None);
        assert_eq!(bits("0x3F", 16), None);
    }
}
