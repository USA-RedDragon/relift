//! A binary form of an [`Analysis`], so an earlier build's result can be
//! read back fast ([`to_bytes`], [`from_bytes`]).

use std::collections::{BTreeMap, BTreeSet};
use std::sync::Arc;

use relift_guest::{LiftFailure, RootKind};
use relift_ir::{
    BIN_FORMAT, BinError, Reader, RegFile, put_str, put_uvar, read_block, write_block,
};

use crate::{Analysis, EntryKind, Function, Insn};

const MAGIC: &[u8; 4] = b"RLAN";

/// The version of the layout [`to_bytes`] writes: the one of this module
/// and of the blocks in it.
pub const FORMAT: u32 = 1;

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

/// The bytes of `a`: every instruction once, then the functions by the
/// positions of their instructions. `key` is written in the header;
/// [`from_bytes`] returns it, so a caller can refuse bytes written for
/// other inputs. `files` are the register files of the blocks.
///
/// # Panics
///
/// Panics when a block's register file is not in `files`.
pub fn to_bytes(a: &Analysis, key: &str, files: &[RegFile]) -> Vec<u8> {
    let mut insts: BTreeMap<u64, &Insn> = BTreeMap::new();
    for f in a.functions.values() {
        for (pc, i) in &f.insts {
            insts.entry(*pc).or_insert(i);
        }
    }
    let index: BTreeMap<u64, usize> = insts.keys().enumerate().map(|(n, &p)| (p, n)).collect();
    let mut out = MAGIC.to_vec();
    put_uvar(&mut out, u64::from(FORMAT));
    put_uvar(&mut out, u64::from(BIN_FORMAT));
    put_str(&mut out, key);
    put_uvar(&mut out, files.len() as u64);
    for f in files {
        put_str(&mut out, f.name());
    }
    put_uvar(&mut out, insts.len() as u64);
    let mut last = 0;
    for (&pc, i) in &insts {
        put_uvar(&mut out, pc - last);
        last = pc;
        match i {
            Insn::Lifted(b) => {
                out.push(0);
                write_block(&mut out, b, files);
            }
            Insn::Failed(e) => {
                out.push(1);
                put_uvar(&mut out, e.insn);
                put_str(&mut out, &e.message);
            }
        }
    }
    put_uvar(&mut out, a.functions.len() as u64);
    let mut last = 0;
    for f in a.functions.values() {
        put_uvar(&mut out, f.entry - last);
        last = f.entry;
        out.push(kind_code(f.kind));
        match &f.name {
            None => out.push(0),
            Some(n) => {
                out.push(1);
                put_str(&mut out, n);
            }
        }
        put_uvar(&mut out, f.insts.len() as u64);
        let mut next = 0;
        for pc in f.insts.keys() {
            let n = index[pc];
            put_uvar(&mut out, (n - next) as u64);
            next = n + 1;
        }
        put_uvar(&mut out, f.tables.len() as u64);
        for (j, ts) in &f.tables {
            put_uvar(&mut out, *j);
            put_uvar(&mut out, ts.len() as u64);
            for t in ts {
                put_uvar(&mut out, *t);
            }
        }
        put_uvar(&mut out, f.pads.len() as u64);
        let mut last = 0;
        for p in &f.pads {
            put_uvar(&mut out, p - last);
            last = *p;
        }
    }
    out
}

/// Reads back the bytes of [`to_bytes`], with the blocks' register files
/// looked up in `files`. Returns the key and the analysis.
///
/// # Errors
///
/// The bytes are not of [`to_bytes`] of this version, or `files` differ
/// from the ones written.
pub fn from_bytes(bytes: &[u8], files: &[RegFile]) -> Result<(String, Analysis), BinError> {
    let mut r = Reader::new(bytes);
    if r.bytes(4)? != MAGIC || r.u32()? != FORMAT || r.u32()? != BIN_FORMAT {
        return Err(BinError("not an analysis of this version"));
    }
    let key = r.str()?.to_string();
    if r.count()? != files.len() {
        return Err(BinError("other register files"));
    }
    for f in files {
        if r.str()? != f.name() {
            return Err(BinError("other register files"));
        }
    }
    let n = r.count()?;
    let mut table: Vec<(u64, Insn)> = Vec::with_capacity(n);
    let mut pc = 0u64;
    for _ in 0..n {
        pc = pc
            .checked_add(r.uvar()?)
            .ok_or(BinError("address overflow"))?;
        let i = match r.u8()? {
            0 => Insn::Lifted(Arc::new(read_block(&mut r, files)?)),
            1 => {
                let insn = r.uvar()?;
                let message = r.str()?.to_string();
                Insn::Failed(LiftFailure { insn, message })
            }
            _ => return Err(BinError("bad instruction")),
        };
        table.push((pc, i));
    }
    let n = r.count()?;
    let mut functions = BTreeMap::new();
    let mut entry = 0u64;
    for _ in 0..n {
        entry = entry
            .checked_add(r.uvar()?)
            .ok_or(BinError("address overflow"))?;
        let kind = kind_of(r.u8()?)?;
        let name = match r.u8()? {
            0 => None,
            1 => Some(r.str()?.to_string()),
            _ => return Err(BinError("bad name")),
        };
        let mut f = Function {
            entry,
            name,
            kind,
            insts: BTreeMap::new(),
            tables: BTreeMap::new(),
            pads: BTreeSet::new(),
        };
        let k = r.count()?;
        let mut next = 0usize;
        for _ in 0..k {
            let at = usize::try_from(r.uvar()?)
                .ok()
                .and_then(|d| next.checked_add(d))
                .ok_or(BinError("bad instruction index"))?;
            let (pc, i) = table.get(at).ok_or(BinError("bad instruction index"))?;
            f.insts.insert(*pc, i.clone());
            next = at + 1;
        }
        for _ in 0..r.count()? {
            let j = r.uvar()?;
            let m = r.count()?;
            let mut ts = Vec::with_capacity(m);
            for _ in 0..m {
                ts.push(r.uvar()?);
            }
            f.tables.insert(j, ts);
        }
        let mut last = 0u64;
        for _ in 0..r.count()? {
            last = last
                .checked_add(r.uvar()?)
                .ok_or(BinError("address overflow"))?;
            f.pads.insert(last);
        }
        functions.insert(entry, f);
    }
    if !r.is_empty() {
        return Err(BinError("trailing bytes"));
    }
    Ok((key, Analysis { functions }))
}
