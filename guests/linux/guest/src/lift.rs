//! A program's code lifted and emitted as one unit.

use std::collections::{BTreeMap, BTreeSet};

use relift_analyze::{Insn, discover};
use relift_cpu_arm64::frontend::Arm64;
use relift_emit::{Options, Output};
use relift_guest::{Frontend, LiftFailure, Root, RootKind};
use relift_ir::{Flow, Next, Op};

use crate::cpu;
use crate::program::{self, Placed};

/// The C symbol of the unit of module `index`.
pub fn unit_symbol(index: usize) -> String {
    format!("rl_unit_linux_{index}")
}

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
    /// Whether the program was emitted in trampoline mode.
    pub trampoline: bool,
    /// Why trampoline mode was chosen, when it was.
    pub reason: Option<String>,
    /// The hot functions as discovery and the escape analysis left them.
    pub analysis: relift_analyze::Analysis,
    /// The functions the superset roots add, for the cold copy.
    pub superset: relift_analyze::total::Superset,
    /// The program's code and data as the analysis reads them.
    pub module: relift_guest::Module,
    /// The entries inside the unit's functions that its table lists.
    pub inner: std::collections::BTreeSet<u64>,
}

/// `__NR_clone` on aarch64.
const NR_CLONE: u128 = 220;

/// `__NR_mmap` and `__NR_mprotect` on aarch64.
const NR_MAP: [u128; 2] = [222, 226];

/// The `PROT_EXEC` bit.
const PROT_EXEC: u128 = 4;

/// The constant `reg` holds at the `svc` at `pc`, set by the straight-line
/// code just before it (at most eight instructions back).
fn constant_before(f: &relift_analyze::Function, pc: u64, reg: relift_ir::Reg) -> Option<u128> {
    for k in 1..=8u64 {
        let Some(Insn::Lifted(p)) = f.insts.get(&pc.wrapping_sub(4 * k)) else {
            break;
        };
        if p.exit.flow != Flow::Jump || p.exit.next != Next::Const(pc.wrapping_sub(4 * (k - 1))) {
            break;
        }
        let set = p.insts.iter().find_map(|i| match i.op {
            Op::Set(r, v) if r == reg => Some(v),
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
            return konst(v);
        }
    }
    None
}

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
            if b.exit.flow == Flow::Trap(0) && constant_before(f, pc, nr) == Some(NR_CLONE) {
                out.insert(pc + 4);
            }
        }
    }
    out
}

/// The address of an `svc #0` that maps or protects memory with a
/// constant protection that includes `PROT_EXEC`: the program writes the
/// code it runs.
pub fn maps_executable(a: &relift_analyze::Analysis) -> Option<u64> {
    let (nr, prot) = (relift_cpu_arm64::regs::x(8), relift_cpu_arm64::regs::x(2));
    a.functions.values().find_map(|f| {
        f.insts.iter().find_map(|(&pc, i)| {
            let Insn::Lifted(b) = i else { return None };
            let maps = b.exit.flow == Flow::Trap(0)
                && constant_before(f, pc, nr).is_some_and(|n| NR_MAP.contains(&n));
            (maps && constant_before(f, pc, prot).is_some_and(|p| p & PROT_EXEC != 0)).then_some(pc)
        })
    })
}

/// Why the program needs trampoline mode (`docs/abi.md` section 5.5), or
/// `None` when direct returns are proven safe: the build forces it, a
/// segment is writable and executable, a system call maps executable memory,
/// or (in any function, the cold copy's superset included) a write of the stack pointer is not relative to the function's entry
/// value ([`relift_analyze::stack`]).
fn mode_reason(
    fe: &Arm64,
    p: &Placed,
    a: &relift_analyze::Analysis,
    force: bool,
) -> Option<String> {
    if force {
        return Some("forced by emit.trampoline".into());
    }
    if p.object
        .segments
        .iter()
        .any(|s| s.perm.write && s.perm.exec)
    {
        return Some("a segment is writable and executable".into());
    }
    if let Some(at) = maps_executable(a) {
        return Some(format!("the system call at {at:#x} maps executable memory"));
    }
    let w = relift_analyze::stack::stack_writes(fe, a);
    w.unproven.first().map(|u| {
        format!(
            "{} of {} stack pointer writes are not relative to the entry value, the first at {:#x} in the function at {:#x}",
            w.unproven.len(),
            w.writes,
            u.at,
            u.function
        )
    })
}

/// The frontend of the program's CPU `cpu`.
pub fn frontend(cpu: cpu::GuestCpu) -> Arm64 {
    Arm64 { cpu: cpu.cpu() }
}

/// Discovery of one module, then the escapes found with the other modules
/// in view: each module's analysis, the slots each is bound through and the
/// continuations the analysis added as roots.
struct Found {
    module: relift_guest::Module,
    analysis: relift_analyze::Analysis,
}

/// Lifts every function discovery finds in the modules `placed` of the
/// program, and emits each module as one unit; `image_id` identifies the
/// program to the runtime. The modules are analyzed together for what
/// crosses between them: a `setjmp` of one library makes the code after its
/// call in another a resume point. `force_trampoline` makes the program use
/// trampoline mode whatever the analysis says; one module that needs it puts
/// them all in it. `cold` adds the superset of each module for its cold copy.
pub fn lift_program(
    placed: &[Placed],
    cpu: cpu::GuestCpu,
    image_id: [u8; 32],
    force_trampoline: bool,
    cold: bool,
) -> Vec<Lifted> {
    let fe = frontend(cpu);
    let slots = program::bound_slots(placed);
    let mut found: Vec<Found> = relift_proc::workers::parallel(placed, |p| {
        let module = program::module(p);
        let analysis = discover(&fe, &module);
        Found { module, analysis }
    });
    let mut escapes = escapes_of(&fe, &found, &slots);
    for _ in 0..2 {
        let wanted: Vec<BTreeSet<u64>> = escapes
            .iter()
            .zip(&found)
            .map(|(e, f)| {
                let mut c = e.continuations.clone();
                c.extend(clone_continuations(&f.analysis));
                c
            })
            .collect();
        let mut changed = false;
        for (f, c) in found.iter_mut().zip(&wanted) {
            let have: BTreeSet<u64> = f.module.roots.iter().map(|r| r.addr).collect();
            let new: Vec<u64> = c.iter().copied().filter(|a| !have.contains(a)).collect();
            if new.is_empty() {
                continue;
            }
            changed = true;
            f.module.roots.extend(new.into_iter().map(|addr| Root {
                addr,
                kind: RootKind::Pointer,
                name: None,
            }));
            f.analysis = discover(&fe, &f.module);
        }
        if !changed {
            break;
        }
        escapes = escapes_of(&fe, &found, &slots);
    }
    for (f, e) in found.iter_mut().zip(&escapes) {
        relift_analyze::escape::apply(&mut f.analysis, e);
    }
    let indexed: Vec<usize> = (0..placed.len()).collect();
    let supersets: Vec<relift_analyze::total::Superset> =
        relift_proc::workers::parallel(&indexed, |&i| {
            let f = &found[i];
            if cold {
                let hot = relift_analyze::Layout::of(&f.analysis, fe.granule());
                relift_analyze::total::superset(&fe, &f.module, &hot)
            } else {
                relift_analyze::total::Superset::default()
            }
        });
    let reasons: Vec<Option<String>> = relift_proc::workers::parallel(&indexed, |&i| {
        let f = &found[i];
        let mut every = f.analysis.clone();
        every
            .functions
            .extend(supersets[i].layout.lift(&fe, &f.module).functions);
        mode_reason(&fe, &placed[i], &every, force_trampoline)
    });
    let reason = reasons.into_iter().flatten().next();
    relift_proc::workers::parallel(&indexed, |&i| {
        emit_module(
            &fe,
            &placed[i],
            i,
            (&found[i], &supersets[i]),
            image_id,
            &reason,
        )
    })
}

fn escapes_of(
    fe: &Arm64,
    found: &[Found],
    slots: &[BTreeMap<u64, u64>],
) -> Vec<relift_analyze::escape::Escapes> {
    let linked: Vec<relift_analyze::escape::Linked<'_>> = found
        .iter()
        .zip(slots)
        .map(|(f, s)| relift_analyze::escape::Linked {
            module: &f.module,
            analysis: &f.analysis,
            slots: s,
        })
        .collect();
    relift_analyze::escape::return_escapes_linked(fe, &linked)
}

fn emit_module(
    fe: &Arm64,
    p: &Placed,
    index: usize,
    (found, superset): (&Found, &relift_analyze::total::Superset),
    image_id: [u8; 32],
    reason: &Option<String>,
) -> Lifted {
    let module = &found.module;
    let analysis = &found.analysis;
    let trampoline = reason.is_some();
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
        fe,
        &fns,
        &[],
        &Options {
            unit_name: p.name.clone(),
            unit_symbol: unit_symbol(index),
            module_base: p.image.base,
            image_id,
            trampoline,
            preempt_resume: true,
            ..Options::default()
        },
    );
    let inner = output
        .files
        .iter()
        .filter(|f| f.name == "unit.c")
        .flat_map(|f| f.fns.iter().map(|(pc, _)| *pc))
        .collect();
    Lifted {
        output,
        functions: fns.len(),
        instructions,
        unlifted,
        trampoline,
        reason: reason.clone(),
        inner,
        analysis: analysis.clone(),
        superset: superset.clone(),
        module: module.clone(),
    }
}
