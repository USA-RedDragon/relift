//! A program's code lifted and emitted as one unit.

use std::collections::{BTreeMap, BTreeSet};

use relift_analyze::{Insn, discover};
use relift_cpu_arm64::frontend::Arm64;
use relift_emit::{Options, Output};
use relift_guest::{Frontend, LiftFailure, Root, RootKind};
use relift_ir::{Flow, Next, Op};

use crate::cpu;
use crate::program::{self, Placed};

/// The unit's C symbol.
pub const UNIT_SYMBOL: &str = "rl_unit_linux";

/// An instruction that did not lift.
#[derive(Clone, Debug)]
pub struct Unlifted {
    /// Where.
    pub at: u64,
    /// Why.
    pub failure: LiftFailure,
}

/// Everything a program lifts to.
#[derive(Clone, Debug)]
pub struct Lifted {
    /// The emitted files of the unit.
    pub output: Output,
    /// The functions in the unit.
    pub functions: usize,
    /// The instructions lifted, counted once per function that holds them.
    pub instructions: usize,
    /// The instructions that did not lift.
    pub unlifted: Vec<Unlifted>,
}

/// `__NR_clone` on aarch64.
const NR_CLONE: u128 = 220;

/// The addresses after each `svc #0` whose system call number is the
/// constant `clone`, set by the straight-line code just before it. A thread
/// that `clone` creates starts at the instruction after the `svc`, in the
/// middle of the function that made the call, so that address needs a
/// function of its own (a continuation, like the one after an escaper's
/// inner call).
pub fn clone_continuations(a: &relift_analyze::Analysis) -> BTreeSet<u64> {
    let nr = relift_cpu_arm64::regs::x(8);
    let mut out = BTreeSet::new();
    for f in a.functions.values() {
        for (&pc, i) in &f.insts {
            let Insn::Lifted(b) = i else { continue };
            if b.exit.flow != Flow::Trap(0) {
                continue;
            }
            for k in 1..=8u64 {
                let Some(Insn::Lifted(p)) = f.insts.get(&pc.wrapping_sub(4 * k)) else {
                    break;
                };
                if p.exit.flow != Flow::Jump
                    || p.exit.next != Next::Const(pc.wrapping_sub(4 * (k - 1)))
                {
                    break;
                }
                let set = p.insts.iter().find_map(|i| match i.op {
                    Op::Set(r, v) if r == nr => Some(v),
                    _ => None,
                });
                if let Some(v) = set {
                    let konst = |v: relift_ir::Value| match p.insts[v.index()].op {
                        Op::Const(c) => Some(c.get()),
                        Op::Ext(_, w) => match p.insts[w.index()].op {
                            Op::Const(c) => Some(c.get()),
                            _ => None,
                        },
                        _ => None,
                    };
                    if konst(v) == Some(NR_CLONE) {
                        out.insert(pc + 4);
                    }
                    break;
                }
            }
        }
    }
    out
}

/// The frontend of the program's CPU.
pub fn frontend() -> Arm64 {
    Arm64 { cpu: cpu::cpu() }
}

/// Lifts every function discovery finds in `p` and emits them as one unit;
/// `image_id` identifies the program to the runtime.
pub fn lift(p: &Placed, image_id: [u8; 32]) -> Lifted {
    let fe = frontend();
    let mut module = program::module(p);
    let mut analysis = discover(&fe, &module);
    let mut found = relift_analyze::escape::return_escapes(&fe, &module, &analysis);
    found.continuations.extend(clone_continuations(&analysis));
    if !found.continuations.is_empty() {
        module
            .roots
            .extend(found.continuations.iter().map(|&addr| Root {
                addr,
                kind: RootKind::Pointer,
                name: None,
            }));
        analysis = discover(&fe, &module);
    }
    let found = relift_analyze::escape::return_escapes(&fe, &module, &analysis);
    relift_analyze::escape::apply(&mut analysis, &found);
    let granule = fe.granule();
    let raw = |pc: u64| {
        module.bytes_at(pc).and_then(|b| b.get(..4)).map_or(0, |w| {
            u64::from(u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
        })
    };
    let mut unlifted = Vec::new();
    let mut instructions = 0;
    let fns: Vec<relift_emit::Function<'_>> = analysis
        .functions
        .values()
        .map(|f| {
            let mut insts = BTreeMap::new();
            for (&pc, i) in &f.insts {
                match i {
                    Insn::Lifted(b) => {
                        instructions += 1;
                        insts.insert(
                            pc,
                            relift_emit::Insn::Lifted {
                                block: b,
                                raw: raw(pc),
                            },
                        );
                    }
                    Insn::Failed(e) => {
                        unlifted.push(Unlifted {
                            at: pc,
                            failure: e.clone(),
                        });
                        insts.insert(pc, relift_emit::Insn::Failed(e));
                    }
                }
            }
            relift_emit::Function {
                entry: f.entry,
                name: program::host_name(f.entry),
                guest_name: f.name.clone(),
                span: f.span(granule),
                insts,
                tables: f.tables.clone(),
                pads: f.pads.clone(),
                guesses: BTreeMap::new(),
                entries: BTreeMap::new(),
            }
        })
        .collect();
    unlifted.sort_by_key(|u| u.at);
    unlifted.dedup_by_key(|u| u.at);
    let output = relift_emit::emit(
        &fe,
        &fns,
        &[],
        &Options {
            unit_name: p.name.clone(),
            unit_symbol: UNIT_SYMBOL.into(),
            module_base: p.image.base,
            image_id,
            ..Options::default()
        },
    );
    Lifted {
        output,
        functions: fns.len(),
        instructions,
        unlifted,
    }
}
