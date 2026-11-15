//! Emitted functions kept between builds ([`Store`]): a function whose
//! input and whose direct-call targets are as they were takes its C from
//! the store instead of being emitted again.

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet};

use relift_guest::Frontend;
use relift_ir::{BinError, IoMap, Reader, put_str, put_uvar, write_block};

use crate::func::{Emitted1, Piece};
use crate::{Function, Insn, Target};

/// The key of one function's input, as the store hashes it.
pub type Key = [u8; 32];

/// Where [`crate::emit_reusing`] keeps the C of every function it emits,
/// and finds the C of functions emitted before.
pub trait Store: Sync {
    /// The key of the input bytes `input`.
    fn key(&self, input: &[u8]) -> Key;
    /// The bytes [`Store::keep`] kept under `key`, if any.
    fn get(&self, key: &Key) -> Option<Vec<u8>>;
    /// Keeps `output` under `key` for the function at `entry`; called once
    /// per function, in entry order. `reused` says the bytes are those
    /// [`Store::get`] gave.
    fn keep(&self, entry: u64, key: &Key, output: Vec<u8>, reused: bool);
}

/// The direct-call targets of a module, recording every address one
/// function looks up: those decide its C as much as its code does.
pub(crate) struct Targets<'a> {
    all: &'a BTreeMap<u64, Target>,
    seen: RefCell<BTreeSet<u64>>,
}

impl<'a> Targets<'a> {
    pub(crate) fn new(all: &'a BTreeMap<u64, Target>) -> Targets<'a> {
        Targets {
            all,
            seen: RefCell::new(BTreeSet::new()),
        }
    }

    pub(crate) fn get(&self, t: &u64) -> Option<&'a Target> {
        self.seen.borrow_mut().insert(*t);
        self.all.get(t)
    }

    fn seen(&self) -> Vec<(u64, Option<&'a Target>)> {
        self.seen
            .borrow()
            .iter()
            .map(|&t| (t, self.all.get(&t)))
            .collect()
    }
}

/// The bytes that decide what the emitter makes of the functions of one
/// emit apart from their own code.
pub(crate) fn salt(fe: &dyn Frontend, piece_bytes: usize, io: &IoMap) -> Vec<u8> {
    let mut out = Vec::new();
    put_str(&mut out, fe.regs().name());
    put_uvar(&mut out, piece_bytes as u64);
    put_str(&mut out, &format!("{io:?}"));
    out
}

/// The input bytes of `f`: `salt`, then everything of the function the
/// emitter reads.
pub(crate) fn input(fe: &dyn Frontend, salt: &[u8], f: &Function<'_>, test_fault: bool) -> Vec<u8> {
    let files = [fe.regs()];
    let mut out = salt.to_vec();
    put_uvar(&mut out, f.entry);
    put_str(&mut out, &f.name);
    out.push(u8::from(test_fault));
    put_uvar(&mut out, f.insts.len() as u64);
    for (&pc, i) in &f.insts {
        put_uvar(&mut out, pc);
        match i {
            Insn::Lifted { block, raw } => {
                out.push(0);
                put_uvar(&mut out, *raw);
                write_block(&mut out, block, &files);
            }
            Insn::Failed(e) => {
                out.push(1);
                put_uvar(&mut out, e.insn);
                put_str(&mut out, &e.message);
            }
        }
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
    for p in &f.pads {
        put_uvar(&mut out, *p);
    }
    out
}

const FLAGS: [&str; 2] = ["RL_FN_MAY_UNWIND", "RL_FN_MAY_UNWIND | RL_FN_LANDING_PADS"];

fn put_target(out: &mut Vec<u8>, t: Option<&Target>) {
    match t {
        None => out.push(0),
        Some(t) => {
            out.push(1);
            put_str(out, &t.name);
            out.push(u8::from(t.lifted) | u8::from(t.resume_point) << 1 | u8::from(t.declare) << 2);
        }
    }
}

fn target(r: &mut Reader<'_>) -> Result<Option<Target>, BinError> {
    Ok(match r.u8()? {
        0 => None,
        1 => {
            let name = r.str()?.to_string();
            let b = r.u8()?;
            Some(Target {
                name,
                lifted: b & 1 != 0,
                resume_point: b & 2 != 0,
                declare: b & 4 != 0,
            })
        }
        _ => return Err(BinError("bad target")),
    })
}

/// The bytes of `e`, emitted with the lookups `targets` recorded.
pub(crate) fn output(e: &Emitted1, targets: &Targets<'_>) -> Option<Vec<u8>> {
    let flags = FLAGS.iter().position(|f| *f == e.flags)?;
    let mut out = Vec::new();
    let seen = targets.seen();
    put_uvar(&mut out, seen.len() as u64);
    for (t, v) in seen {
        put_uvar(&mut out, t);
        put_target(&mut out, v);
    }
    put_uvar(&mut out, e.pieces.len() as u64);
    for p in &e.pieces {
        put_str(&mut out, &p.name);
        put_str(&mut out, &p.text);
        match p.first_pc {
            None => out.push(0),
            Some(pc) => {
                out.push(1);
                put_uvar(&mut out, pc);
            }
        }
    }
    put_uvar(&mut out, e.callees.len() as u64);
    for c in &e.callees {
        put_uvar(&mut out, *c);
    }
    put_uvar(&mut out, e.undefined as u64);
    out.push(u8::try_from(flags).ok()?);
    Some(out)
}

/// The function `output` holds, when every target it looked up is still
/// as it was in `all`.
pub(crate) fn reuse(bytes: &[u8], all: &BTreeMap<u64, Target>) -> Option<Emitted1> {
    read(bytes, all).ok().flatten()
}

fn read(bytes: &[u8], all: &BTreeMap<u64, Target>) -> Result<Option<Emitted1>, BinError> {
    let mut r = Reader::new(bytes);
    for _ in 0..r.count()? {
        let t = r.uvar()?;
        if target(&mut r)?.as_ref() != all.get(&t) {
            return Ok(None);
        }
    }
    let n = r.count()?;
    let mut pieces = Vec::with_capacity(n);
    for _ in 0..n {
        let name = r.str()?.to_string();
        let text = r.str()?.to_string();
        let first_pc = match r.u8()? {
            0 => None,
            1 => Some(r.uvar()?),
            _ => return Err(BinError("bad piece")),
        };
        pieces.push(Piece {
            name,
            text,
            first_pc,
        });
    }
    let mut callees = BTreeSet::new();
    for _ in 0..r.count()? {
        callees.insert(r.uvar()?);
    }
    let undefined = usize::try_from(r.uvar()?).map_err(|_| BinError("bad count"))?;
    let flags = *FLAGS
        .get(usize::from(r.u8()?))
        .ok_or(BinError("bad flags"))?;
    if !r.is_empty() {
        return Err(BinError("trailing bytes"));
    }
    Ok(Some(Emitted1 {
        pieces,
        callees,
        undefined,
        flags,
    }))
}
