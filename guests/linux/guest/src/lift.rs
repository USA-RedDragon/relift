//! A program's code lifted and emitted as one unit.

use std::collections::BTreeMap;

use relift_analyze::{Insn, discover};
use relift_cpu_arm64::frontend::Arm64;
use relift_emit::{Options, Output};
use relift_guest::{Frontend, LiftFailure};

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

/// The frontend of the program's CPU.
pub fn frontend() -> Arm64 {
    Arm64 { cpu: cpu::cpu() }
}

/// Lifts every function discovery finds in `p` and emits them as one unit;
/// `image_id` identifies the program to the runtime.
pub fn lift(p: &Placed, image_id: [u8; 32]) -> Lifted {
    let fe = frontend();
    let module = program::module(p);
    let analysis = discover(&fe, &module);
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
