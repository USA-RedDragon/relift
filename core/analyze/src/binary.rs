//! A binary form of a [`Layout`], so an earlier build's outlines can be
//! read back fast ([`to_bytes`], [`from_bytes`]).

use std::collections::{BTreeMap, BTreeSet};

use relift_guest::{LiftFailure, RootKind};
use relift_ir::{BinError, Reader, put_str, put_uvar};

use crate::{EntryKind, Layout, Outline};

const MAGIC: &[u8; 4] = b"RLLO";

/// The version of the layout [`to_bytes`] writes.
pub const FORMAT: u32 = 2;

fn kind_code(k: EntryKind) -> u8 {
    match k {
        EntryKind::Root(RootKind::Entry) => 0,
        EntryKind::Root(RootKind::Symbol) => 1,
        EntryKind::Root(RootKind::Unwind) => 2,
        EntryKind::Root(RootKind::Pointer) => 3,
        EntryKind::Root(RootKind::Hint) => 4,
        EntryKind::Called => 5,
        EntryKind::AddressTaken => 6,
    }
}

fn kind_of(c: u8) -> Result<EntryKind, BinError> {
    Ok(match c {
        0 => EntryKind::Root(RootKind::Entry),
        1 => EntryKind::Root(RootKind::Symbol),
        2 => EntryKind::Root(RootKind::Unwind),
        3 => EntryKind::Root(RootKind::Pointer),
        4 => EntryKind::Root(RootKind::Hint),
        5 => EntryKind::Called,
        6 => EntryKind::AddressTaken,
        _ => return Err(BinError("bad entry kind")),
    })
}

fn put_addrs(out: &mut Vec<u8>, addrs: impl ExactSizeIterator<Item = u64>) {
    put_uvar(out, addrs.len() as u64);
    let mut last = 0u64;
    for a in addrs {
        put_uvar(out, a.wrapping_sub(last));
        last = a;
    }
}

fn addrs(r: &mut Reader<'_>) -> Result<Vec<u64>, BinError> {
    let n = r.count()?;
    let mut out = Vec::with_capacity(n);
    let mut last = 0u64;
    for _ in 0..n {
        last = last.wrapping_add(r.uvar()?);
        out.push(last);
    }
    Ok(out)
}

fn put_outline(out: &mut Vec<u8>, o: &Outline) {
    out.push(kind_code(o.kind));
    match &o.name {
        None => out.push(0),
        Some(n) => {
            out.push(1);
            put_str(out, n);
        }
    }
    put_uvar(out, o.granule);
    put_uvar(out, o.runs.len() as u64);
    let mut last = o.entry;
    for &(first, n) in &o.runs {
        put_uvar(out, first.wrapping_sub(last));
        put_uvar(out, u64::from(n));
        last = first;
    }
    put_uvar(out, o.tables.len() as u64);
    for (j, ts) in &o.tables {
        put_uvar(out, *j);
        put_addrs(out, ts.iter().copied());
    }
    put_addrs(out, o.pads.iter().copied());
    put_uvar(out, o.failed.len() as u64);
    for (pc, e) in &o.failed {
        put_uvar(out, *pc);
        put_uvar(out, e.insn);
        put_str(out, &e.message);
    }
    put_uvar(out, o.calls.len() as u64);
    for &(t, ret) in &o.calls {
        put_uvar(out, t);
        put_uvar(out, ret.wrapping_sub(o.entry));
    }
    put_uvar(out, o.end.wrapping_sub(o.entry));
    put_uvar(out, o.reach.wrapping_sub(o.end));
}

fn outline(r: &mut Reader<'_>, entry: u64) -> Result<Outline, BinError> {
    let kind = kind_of(r.u8()?)?;
    let name = match r.u8()? {
        0 => None,
        1 => Some(r.str()?.to_string()),
        _ => return Err(BinError("bad name")),
    };
    let granule = r.uvar()?;
    let n = r.count()?;
    let mut runs = Vec::with_capacity(n);
    let mut last = entry;
    for _ in 0..n {
        let first = last.wrapping_add(r.uvar()?);
        let k = u32::try_from(r.uvar()?).map_err(|_| BinError("bad run"))?;
        runs.push((first, k));
        last = first;
    }
    let mut tables = BTreeMap::new();
    for _ in 0..r.count()? {
        let j = r.uvar()?;
        tables.insert(j, addrs(r)?);
    }
    let pads: BTreeSet<u64> = addrs(r)?.into_iter().collect();
    let n = r.count()?;
    let mut failed = Vec::with_capacity(n);
    for _ in 0..n {
        let pc = r.uvar()?;
        let insn = r.uvar()?;
        let message = r.str()?.to_string();
        failed.push((pc, LiftFailure { insn, message }));
    }
    let n = r.count()?;
    let mut calls = Vec::with_capacity(n);
    for _ in 0..n {
        let t = r.uvar()?;
        calls.push((t, entry.wrapping_add(r.uvar()?)));
    }
    let end = entry.wrapping_add(r.uvar()?);
    let reach = end.wrapping_add(r.uvar()?);
    Ok(Outline {
        entry,
        name,
        kind,
        runs,
        tables,
        pads,
        failed,
        calls,
        end,
        reach,
        granule,
    })
}

/// The bytes of `l`. `key` is written in the header; [`from_bytes`]
/// returns it, so a caller can refuse bytes written for other inputs.
pub fn to_bytes(l: &Layout, key: &str) -> Vec<u8> {
    let mut out = MAGIC.to_vec();
    put_uvar(&mut out, u64::from(FORMAT));
    put_str(&mut out, key);
    put_uvar(&mut out, l.functions.len() as u64);
    let mut last = 0;
    for o in l.functions.values() {
        put_uvar(&mut out, o.entry - last);
        last = o.entry;
        put_outline(&mut out, o);
    }
    out
}

/// Reads back the bytes of [`to_bytes`]. Returns the key and the layout.
///
/// # Errors
///
/// The bytes are not of [`to_bytes`] of this version.
pub fn from_bytes(bytes: &[u8]) -> Result<(String, Layout), BinError> {
    let mut r = Reader::new(bytes);
    if r.bytes(4)? != MAGIC || r.u32()? != FORMAT {
        return Err(BinError("not a layout of this version"));
    }
    let key = r.str()?.to_string();
    let n = r.count()?;
    let mut functions = BTreeMap::new();
    let mut entry = 0u64;
    for _ in 0..n {
        entry = entry
            .checked_add(r.uvar()?)
            .ok_or(BinError("address overflow"))?;
        functions.insert(entry, outline(&mut r, entry)?);
    }
    if !r.is_empty() {
        return Err(BinError("trailing bytes"));
    }
    Ok((key, Layout { functions }))
}
