//! The local access chain conversion of parallel-rdp's SPIR-V.
//!
//! Some drivers (Adreno 7xx system drivers among them) fail to build a
//! compute pipeline whose shader stores or loads through a constant-index
//! `OpAccessChain` into a function-local composite. Rewriting each such
//! access as a load of the whole variable with `OpCompositeExtract`, or a
//! load, `OpCompositeInsert` and store of the whole variable, means the same
//! thing and every driver takes it; this is SPIRV-Tools'
//! `--convert-local-access-chains`. relift applies it to every shader of the
//! pinned tree's `slangmosh.hpp` bank, on every host, so all hosts run the
//! same SPIR-V.

use std::collections::{HashMap, HashSet};

const OP_NAME: u32 = 5;
const OP_TYPE_BOOL: u32 = 20;
const OP_TYPE_INT: u32 = 21;
const OP_TYPE_FLOAT: u32 = 22;
const OP_TYPE_VECTOR: u32 = 23;
const OP_TYPE_MATRIX: u32 = 24;
const OP_TYPE_ARRAY: u32 = 28;
const OP_TYPE_STRUCT: u32 = 30;
const OP_TYPE_POINTER: u32 = 32;
const OP_CONSTANT: u32 = 43;
const OP_FUNCTION: u32 = 54;
const OP_VARIABLE: u32 = 59;
const OP_LOAD: u32 = 61;
const OP_STORE: u32 = 62;
const OP_ACCESS_CHAIN: u32 = 65;
const OP_IN_BOUNDS_ACCESS_CHAIN: u32 = 66;
const OP_DECORATE: u32 = 71;
const OP_COMPOSITE_EXTRACT: u32 = 81;
const OP_COMPOSITE_INSERT: u32 = 82;
const OP_DECORATE_ID: u32 = 332;
const OP_DECORATE_STRING: u32 = 5632;

const STORAGE_FUNCTION: u32 = 7;

/// The operand words of the instructions in `words` after the header, each
/// as (opcode, the instruction's words).
fn insts(words: &[u32]) -> Result<Vec<(u32, &[u32])>, String> {
    let mut out = Vec::new();
    let mut at = 5;
    while at < words.len() {
        let n = (words[at] >> 16) as usize;
        if n == 0 || at + n > words.len() {
            return Err(format!("a bad instruction at word {at}"));
        }
        out.push((words[at] & 0xFFFF, &words[at..at + n]));
        at += n;
    }
    Ok(out)
}

fn inst(op: u32, operands: &[u32]) -> Vec<u32> {
    let mut v = Vec::with_capacity(operands.len() + 1);
    v.push(((operands.len() as u32 + 1) << 16) | op);
    v.extend_from_slice(operands);
    v
}

/// Whether the type `id` can be loaded, extracted from and inserted into
/// whole: scalars, vectors, matrices, and arrays and structs of them.
fn target_type(id: u32, types: &HashMap<u32, &[u32]>, seen: usize) -> bool {
    let Some(t) = types.get(&id) else {
        return false;
    };
    if seen > 32 {
        return false;
    }
    match t[0] & 0xFFFF {
        OP_TYPE_BOOL | OP_TYPE_INT | OP_TYPE_FLOAT | OP_TYPE_VECTOR | OP_TYPE_MATRIX => true,
        OP_TYPE_ARRAY => target_type(t[2], types, seen + 1),
        OP_TYPE_STRUCT => t[2..].iter().all(|&m| target_type(m, types, seen + 1)),
        _ => false,
    }
}

/// The words of an instruction that may name an id a function-local
/// variable or an access chain could be, past its result: every operand
/// except the literals of the instructions whose literals can be large.
fn id_operands(op: u32, w: &[u32]) -> &[u32] {
    match op {
        OP_NAME | OP_DECORATE | OP_DECORATE_ID | OP_DECORATE_STRING => &w[1..2],
        OP_LOAD => &w[3..4],
        OP_STORE => &w[1..3],
        OP_COMPOSITE_EXTRACT => &w[3..4],
        OP_COMPOSITE_INSERT => &w[3..5],
        OP_VARIABLE => &w[4..],
        OP_ACCESS_CHAIN | OP_IN_BOUNDS_ACCESS_CHAIN => &w[3..],
        1..=4 | 6..=8 | 10 | 11 | 14..=17 | 19 | 21 | 22 | 43 | 50 | 51 | 248 | 251 | 317 | 330 => {
            &[]
        }
        _ => &w[1..],
    }
}

/// `words` with every constant-index access chain into a function-local
/// variable that only loads, stores and such chains use replaced by whole
/// loads, extracts, inserts and stores; the module itself when it has none.
///
/// # Errors
///
/// `words` is not a SPIR-V module.
pub fn convert_local_access_chains(words: &[u32]) -> Result<Vec<u32>, String> {
    if words.len() < 5 || words[0] != 0x0723_0203 {
        return Err("not a SPIR-V module".into());
    }
    let list = insts(words)?;
    let mut types: HashMap<u32, &[u32]> = HashMap::new();
    let mut pointee: HashMap<u32, (u32, u32)> = HashMap::new();
    let mut consts: HashMap<u32, u32> = HashMap::new();
    let mut vars: HashMap<u32, u32> = HashMap::new();
    let mut chains: HashMap<u32, (u32, Vec<u32>)> = HashMap::new();
    let mut in_function = false;
    for &(op, w) in &list {
        match op {
            OP_FUNCTION => in_function = true,
            OP_TYPE_POINTER => {
                pointee.insert(w[1], (w[2], w[3]));
            }
            OP_TYPE_BOOL..=OP_TYPE_STRUCT => {
                types.insert(w[1], w);
            }
            OP_CONSTANT
                if (w.len() == 4 || (w.len() == 5 && w[4] == 0))
                    && types
                        .get(&w[1])
                        .is_some_and(|t| t[0] & 0xFFFF == OP_TYPE_INT) =>
            {
                consts.insert(w[2], w[3]);
            }
            OP_VARIABLE if in_function && w[3] == STORAGE_FUNCTION && w.len() == 4 => {
                if let Some(&(_, t)) = pointee.get(&w[1])
                    && target_type(t, &types, 0)
                {
                    vars.insert(w[2], t);
                }
            }
            OP_ACCESS_CHAIN | OP_IN_BOUNDS_ACCESS_CHAIN if vars.contains_key(&w[3]) => {
                let idx: Option<Vec<u32>> = w[4..].iter().map(|i| consts.get(i).copied()).collect();
                match idx {
                    Some(idx) if !idx.is_empty() => {
                        chains.insert(w[2], (w[3], idx));
                    }
                    _ => {
                        vars.remove(&w[3]);
                    }
                }
            }
            _ => {}
        }
    }
    let mut bad: HashSet<u32> = HashSet::new();
    for &(op, w) in &list {
        let ids = id_operands(op, w);
        let ok_pos = |k: usize| -> bool {
            match op {
                OP_NAME | OP_DECORATE | OP_DECORATE_ID | OP_DECORATE_STRING => true,
                OP_LOAD => k == 0,
                OP_STORE => k == 0,
                OP_ACCESS_CHAIN | OP_IN_BOUNDS_ACCESS_CHAIN => k == 0,
                _ => false,
            }
        };
        for (k, id) in ids.iter().enumerate() {
            if let Some(&(base, _)) = chains.get(id) {
                let fine = matches!(
                    op,
                    OP_NAME | OP_DECORATE | OP_DECORATE_ID | OP_DECORATE_STRING | OP_LOAD
                ) || (op == OP_STORE && k == 0);
                if !fine {
                    bad.insert(base);
                }
            } else if vars.contains_key(id) && !ok_pos(k) {
                bad.insert(*id);
            }
        }
    }
    for b in &bad {
        vars.remove(b);
    }
    chains.retain(|_, (base, _)| vars.contains_key(base));
    if chains.is_empty() {
        return Ok(words.to_vec());
    }
    let mut bound = words[3];
    let mut fresh = || {
        let id = bound;
        bound += 1;
        id
    };
    let mut out: Vec<u32> = words[..5].to_vec();
    for &(op, w) in &list {
        match op {
            OP_NAME | OP_DECORATE | OP_DECORATE_ID | OP_DECORATE_STRING
                if chains.contains_key(&w[1]) => {}
            OP_ACCESS_CHAIN | OP_IN_BOUNDS_ACCESS_CHAIN if chains.contains_key(&w[2]) => {}
            OP_LOAD if chains.contains_key(&w[3]) => {
                let (base, idx) = &chains[&w[3]];
                let whole = fresh();
                let mut l = vec![vars[base], whole, *base];
                l.extend_from_slice(&w[4..]);
                out.extend(inst(OP_LOAD, &l));
                let mut e = vec![w[1], w[2], whole];
                e.extend_from_slice(idx);
                out.extend(inst(OP_COMPOSITE_EXTRACT, &e));
            }
            OP_STORE if chains.contains_key(&w[1]) => {
                let (base, idx) = &chains[&w[1]];
                let t = vars[base];
                let whole = fresh();
                let new = fresh();
                out.extend(inst(OP_LOAD, &[t, whole, *base]));
                let mut i = vec![t, new, w[2], whole];
                i.extend_from_slice(idx);
                out.extend(inst(OP_COMPOSITE_INSERT, &i));
                let mut s = vec![*base, new];
                s.extend_from_slice(&w[3..]);
                out.extend(inst(OP_STORE, &s));
            }
            _ => out.extend_from_slice(w),
        }
    }
    out[3] = bound;
    Ok(out)
}

/// The text of a slangmosh header with every SPIR-V module of its
/// `spirv_bank` converted ([`convert_local_access_chains`]) and the bank's
/// offsets and sizes moved to match.
///
/// # Errors
///
/// The header has no `spirv_bank` or a module in it is not SPIR-V.
pub fn convert_bank(header: &str) -> Result<String, String> {
    const START: &str = "static const uint32_t spirv_bank[] =\n{\n";
    let open = header.find(START).ok_or("no spirv_bank")? + START.len();
    let close = open + header[open..].find("};").ok_or("spirv_bank does not end")?;
    let bank: Vec<u32> = header[open..close]
        .split(',')
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(|s| {
            let h = s.trim_start_matches("0x").trim_end_matches('u');
            u32::from_str_radix(h, 16).map_err(|e| format!("{s}: {e}"))
        })
        .collect::<Result<_, _>>()?;
    const REF: &str = "spirv_bank + ";
    let mut refs: Vec<(usize, usize)> = Vec::new();
    let mut at = close;
    while let Some(p) = header[at..].find(REF) {
        let s = at + p + REF.len();
        let rest = &header[s..];
        let (off, tail) = rest
            .split_once(", ")
            .ok_or("a bank reference without a size")?;
        let size: String = tail.chars().take_while(char::is_ascii_digit).collect();
        refs.push((
            off.parse().map_err(|e| format!("{off}: {e}"))?,
            size.parse().map_err(|e| format!("{size}: {e}"))?,
        ));
        at = s;
    }
    let mut order: Vec<(usize, usize)> = refs.clone();
    order.sort_unstable();
    order.dedup();
    let mut new_bank: Vec<u32> = Vec::with_capacity(bank.len());
    let mut moved: HashMap<(usize, usize), (usize, usize)> = HashMap::new();
    for &(off, size) in &order {
        let words = bank
            .get(off..off + size / 4)
            .ok_or_else(|| format!("bank + {off}, {size} is past the bank"))?;
        let conv = convert_local_access_chains(words)?;
        moved.insert((off, size), (new_bank.len(), conv.len() * 4));
        new_bank.extend(conv);
    }
    let mut text = String::with_capacity(header.len() + header.len() / 16);
    text.push_str(&header[..open]);
    for line in new_bank.chunks(8) {
        text.push('\t');
        let cells: Vec<String> = line.iter().map(|w| format!("0x{w:08x}u")).collect();
        text.push_str(&cells.join(", "));
        text.push_str(",\n");
    }
    let mut at = close;
    for &(off, size) in &refs {
        let p = at + header[at..].find(REF).expect("found above") + REF.len();
        text.push_str(&header[at..p]);
        let (o, s) = moved[&(off, size)];
        text.push_str(&format!("{o}, {s}"));
        at = p + off.to_string().len() + 2 + size.to_string().len();
    }
    text.push_str(&header[at..]);
    Ok(text)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A module with one function: `%v` (a struct of an int and a vec2 of
    /// ints) gets 5 stored into member 0, then member 1's element 1 loaded.
    fn module() -> Vec<u32> {
        let mut m = vec![0x0723_0203, 0x0001_0300, 0, 40, 0];
        let mut add = |op: u32, o: &[u32]| m.extend(inst(op, o));
        add(OP_NAME, &[22, 0x0000_6361]);
        add(OP_TYPE_INT, &[1, 32, 1]);
        add(OP_TYPE_VECTOR, &[2, 1, 2]);
        add(OP_TYPE_STRUCT, &[3, 1, 2]);
        add(OP_TYPE_POINTER, &[4, STORAGE_FUNCTION, 3]);
        add(OP_TYPE_POINTER, &[5, STORAGE_FUNCTION, 1]);
        add(OP_CONSTANT, &[1, 6, 0]);
        add(OP_CONSTANT, &[1, 7, 1]);
        add(OP_CONSTANT, &[1, 8, 5]);
        add(19, &[9]);
        add(33, &[10, 9]);
        add(OP_FUNCTION, &[9, 11, 0, 10]);
        add(248, &[12]);
        add(OP_VARIABLE, &[4, 20, STORAGE_FUNCTION]);
        add(OP_ACCESS_CHAIN, &[5, 22, 20, 6]);
        add(OP_STORE, &[22, 8]);
        add(OP_ACCESS_CHAIN, &[5, 23, 20, 7, 7]);
        add(OP_LOAD, &[1, 24, 23]);
        add(253, &[]);
        add(56, &[]);
        m
    }

    fn ops(m: &[u32]) -> Vec<u32> {
        insts(m)
            .expect("parses")
            .iter()
            .map(|(op, _)| *op)
            .collect()
    }

    #[test]
    fn chains_into_a_local_composite_become_whole_loads_and_stores() {
        let out = convert_local_access_chains(&module()).expect("converts");
        let o = ops(&out);
        assert!(
            !o.contains(&OP_ACCESS_CHAIN) && !o.contains(&OP_NAME),
            "{o:?}"
        );
        let list = insts(&out).expect("parses");
        let body: Vec<(u32, Vec<u32>)> = list
            .iter()
            .skip_while(|(op, _)| *op != OP_VARIABLE)
            .map(|(op, w)| (*op, w[1..].to_vec()))
            .collect();
        assert_eq!(
            body[1..6],
            [
                (OP_LOAD, vec![3, 40, 20]),
                (OP_COMPOSITE_INSERT, vec![3, 41, 8, 40, 0]),
                (OP_STORE, vec![20, 41]),
                (OP_LOAD, vec![3, 42, 20]),
                (OP_COMPOSITE_EXTRACT, vec![1, 24, 42, 1, 1]),
            ]
        );
        assert_eq!(out[3], 43);
    }

    #[test]
    fn a_variable_with_another_use_is_left_alone() {
        let mut m = module();
        let at = m.len() - 2;
        m.splice(at..at, inst(57, &[9, 25, 11, 20]));
        assert_eq!(convert_local_access_chains(&m).expect("converts"), m);
    }

    #[test]
    fn the_bank_moves_with_the_modules_it_holds() {
        let m = module();
        let mut header = String::from("#pragma once\nstatic const uint32_t spirv_bank[] =\n{\n");
        let empty = [0x0723_0203u32, 0x0001_0300, 0, 1, 0];
        for w in empty.iter().chain(&m) {
            header.push_str(&format!("\t0x{w:08x}u,\n"));
        }
        let size = m.len() * 4;
        header.push_str(&format!(
            "}};\nx(spirv_bank + 0, 20);\ny(spirv_bank + 5, {size});\nz(spirv_bank + 0, 20);\n"
        ));
        let out = convert_bank(&header).expect("converts");
        let conv = convert_local_access_chains(&m).expect("converts");
        assert!(out.contains("x(spirv_bank + 0, 20);\n"), "{out}");
        assert!(
            out.contains(&format!("y(spirv_bank + 5, {});\n", conv.len() * 4)),
            "{out}"
        );
        assert!(out.contains("z(spirv_bank + 0, 20);\n"), "{out}");
        assert!(
            out.contains(
                "\t0x07230203u, 0x00010300u, 0x00000000u, 0x00000001u, 0x00000000u, 0x07230203u,"
            ),
            "{out}"
        );
        assert_ne!(conv, m);
    }
}
