//! Short straight-line functions run in place at the transfers to them.
//!
//! A function that is a few instructions in a line (a PLT stub, a getter,
//! a jump to another function) costs more at its edges than in its body:
//! the caller stores its registers and calls, the callee loads what it
//! reads, stores what it wrote and returns or jumps on. [`inline_body`]
//! picks such functions and [`inline_transfers`] appends their
//! instructions to the block that calls or jumps to them
//! ([`relift_ir::fuse`]), so the transfer goes where the short function
//! would have gone: on to its target, or, for a call to one that returns,
//! straight to the return address.

use std::collections::BTreeMap;
use std::sync::Arc;

use relift_ir::{Block, Exit, Flow, Next, Op, opt};

use crate::{Function, Insn};

/// The most instructions run in place at one transfer.
const BUDGET: usize = 16;

/// The blocks of `f` in the order they run, when `f` is at most `max`
/// instructions that run in a line from the entry (no branch, call, trap,
/// table or landing pad) to one return or one jump out of `f`, and have
/// only register, integer and memory operations: no floating point, guard,
/// intrinsic, exclusive or atomic access, clock or device operation.
pub fn inline_body(f: &Function, max: usize) -> Option<Vec<Arc<Block>>> {
    if f.insts.len() > max || !f.tables.is_empty() || !f.pads.is_empty() {
        return None;
    }
    let mut body = Vec::with_capacity(f.insts.len());
    let mut pc = f.entry;
    loop {
        let Some(Insn::Lifted(b)) = f.insts.get(&pc) else {
            return None;
        };
        let plain = b.bank.is_none()
            && b.insts.iter().all(|i| {
                matches!(
                    i.op,
                    Op::Const(_)
                        | Op::Get(_)
                        | Op::Set(..)
                        | Op::Un(..)
                        | Op::Bin(..)
                        | Op::Cmp(..)
                        | Op::Select(..)
                        | Op::Ext(..)
                        | Op::Extract(..)
                        | Op::Concat(..)
                        | Op::Load { .. }
                        | Op::Store { .. }
                )
            });
        if !plain {
            return None;
        }
        body.push(b.clone());
        match (b.exit.flow, b.exit.next) {
            (Flow::Jump, Next::Const(t)) if t > pc && f.insts.contains_key(&t) => pc = t,
            (Flow::Jump, Next::Const(t)) if !f.insts.contains_key(&t) => break,
            (Flow::Jump | Flow::Return, Next::Dynamic(_)) => break,
            _ => return None,
        }
    }
    (body.len() == f.insts.len()).then_some(body)
}

/// `f` with the bodies of `bodies` ([`inline_body`], by entry) run in
/// place at its direct calls and jumps to them, or `None` when it has no
/// such transfer. A call to a body that returns to the call's return
/// address becomes a jump there; a call to one that jumps on becomes a
/// call of where it jumps; a jump to a body takes the body's exit. A body
/// whose exit leads to another body runs in place too, up to [`BUDGET`]
/// instructions per transfer.
pub fn inline_transfers(f: &Function, bodies: &BTreeMap<u64, Vec<Arc<Block>>>) -> Option<Function> {
    let mut out: Option<Function> = None;
    for (&pc, insn) in &f.insts {
        let Insn::Lifted(b) = insn else {
            continue;
        };
        let mut cur: Option<Block> = None;
        let mut left = BUDGET;
        loop {
            let at = cur.as_ref().unwrap_or(b);
            let (ret, t) = match (at.exit.flow, at.exit.next) {
                (Flow::Call { ret }, Next::Const(t)) => (Some(ret), t),
                (Flow::Jump, Next::Const(t)) => (None, t),
                _ => break,
            };
            if f.insts.contains_key(&t) {
                break;
            }
            let Some(body) = bodies.get(&t).filter(|body| body.len() <= left) else {
                break;
            };
            let ret = ret.map(|r| (r, f.insts.contains_key(&r)));
            let Some(fused) = run_in_place(at, body, ret) else {
                break;
            };
            left -= body.len();
            cur = Some(fused);
        }
        if let Some(fused) = cur {
            out.get_or_insert_with(|| f.clone())
                .insts
                .insert(pc, Insn::Lifted(Arc::new(fused)));
        }
    }
    out
}

/// `at` with `body` after it; `ret` is the return address when `at` calls
/// the body, and whether the function holds it. `None` when the body
/// returns from a call to an address that is not known to be `ret`, or
/// that the function does not hold.
fn run_in_place(at: &Block, body: &[Arc<Block>], ret: Option<(u64, bool)>) -> Option<Block> {
    let mut fused = body.iter().fold(at.clone(), |acc, b| opt::fuse(&acc, b));
    let exit = fused.exit;
    fused.exit = match (ret, exit.flow, exit.next) {
        (None, ..) => exit,
        (Some((ret, inside)), Flow::Return, Next::Dynamic(v)) => {
            let to = opt::constants(&fused).get(v.index()).copied().flatten();
            if !inside || to != Some(u128::from(ret)) {
                return None;
            }
            Exit {
                next: Next::Const(ret),
                flow: Flow::Jump,
            }
        }
        (Some((ret, _)), Flow::Jump, next) => Exit {
            next,
            flow: Flow::Call { ret },
        },
        (Some(_), ..) => return None,
    };
    Some(opt::dce(&fused))
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;

    use relift_guest::Frontend;
    use relift_guest::toy::Toy;
    use relift_ir::interp::{self, FlatMemory, NoIntrinsics, State};
    use relift_ir::{Builder, Endian, Order, Reg, Width};

    use super::*;
    use crate::EntryKind;

    const R0: Reg = Reg(0);
    const R1: Reg = Reg(1);
    const LR: Reg = Reg(3);
    const SLOT: u64 = 0x1010;

    fn b(pc: u64) -> Builder {
        Builder::new(Toy.regs(), Width::W64, pc)
    }

    fn func(blocks: Vec<Block>) -> Function {
        Function {
            entry: blocks[0].pc,
            name: None,
            kind: EntryKind::Called,
            insts: blocks
                .into_iter()
                .map(|b| (b.pc, Insn::Lifted(b.into())))
                .collect(),
            tables: BTreeMap::new(),
            pads: BTreeSet::new(),
        }
    }

    /// `r1 = r1 + r0 + n`, then on to the next instruction.
    fn add(pc: u64, n: u128) -> Block {
        let mut x = b(pc);
        let (a, c) = (x.get(R1), x.get(R0));
        let k = x.konst(Width::W64, n);
        let s = x.add(a, c);
        let s = x.add(s, k);
        x.set(R1, s);
        x.jump(4, pc + 4)
    }

    /// `r0 = [SLOT]`, then on to the next instruction.
    fn load(pc: u64) -> Block {
        let mut x = b(pc);
        let a = x.konst(Width::W64, u128::from(SLOT));
        let v = x.load(a, Width::W64, Endian::Little, Order::Plain);
        x.set(R0, v);
        x.jump(4, pc + 4)
    }

    /// `lr = pc + 4` and a call of `t`.
    fn call(pc: u64, t: u64) -> Block {
        let mut x = b(pc);
        let k = x.konst(Width::W64, u128::from(pc + 4));
        x.set(LR, k);
        x.finish(
            4,
            Exit {
                next: Next::Const(t),
                flow: Flow::Call { ret: pc + 4 },
            },
        )
    }

    fn to(pc: u64, r: Reg, flow: Flow) -> Block {
        let mut x = b(pc);
        let v = x.get(r);
        x.finish(
            4,
            Exit {
                next: Next::Dynamic(v),
                flow,
            },
        )
    }

    fn jump(pc: u64, t: u64) -> Block {
        b(pc).jump(4, t)
    }

    /// The registers, the address reached and the memory after running
    /// the functions `fs` from `pc` until it leaves them.
    fn run(fs: &[&Function], mut pc: u64) -> (Vec<u128>, Vec<u8>) {
        let mut s = State::new(Toy.regs().len());
        s.regs[0] = 5;
        s.regs[1] = 7;
        let mut m = FlatMemory {
            base: 0x1000,
            bytes: (0..64u8).collect(),
        };
        while let Some(Insn::Lifted(blk)) = fs.iter().find_map(|f| f.insts.get(&pc)) {
            pc = interp::run(blk, &mut s, &mut m, &mut NoIntrinsics)
                .expect("runs")
                .next;
        }
        s.regs.push(u128::from(pc));
        (s.regs, m.bytes)
    }

    fn bodies(fs: &[&Function]) -> BTreeMap<u64, Vec<Arc<Block>>> {
        fs.iter()
            .filter_map(|f| Some((f.entry, inline_body(f, 8)?)))
            .collect()
    }

    #[test]
    fn a_call_of_a_short_function_that_returns_runs_it_and_jumps_to_the_return_address() {
        let getter = func(vec![
            load(0x200),
            add(0x204, 3),
            to(0x208, LR, Flow::Return),
        ]);
        let caller = func(vec![
            add(0x100, 1),
            call(0x104, 0x200),
            add(0x108, 2),
            jump(0x10c, 0x900),
        ]);
        let inlined = inline_transfers(&caller, &bodies(&[&getter])).expect("inlined");
        let Insn::Lifted(at) = &inlined.insts[&0x104] else {
            panic!("lifted");
        };
        assert_eq!(at.exit.next, Next::Const(0x108));
        assert_eq!(at.exit.flow, Flow::Jump);
        assert!(at.verify().is_ok());
        assert_eq!(run(&[&inlined], 0x100), run(&[&caller, &getter], 0x100));
    }

    #[test]
    fn a_call_of_a_stub_that_jumps_on_becomes_a_call_of_where_it_jumps() {
        let stub = func(vec![load(0x200), to(0x204, R0, Flow::Jump)]);
        let caller = func(vec![call(0x100, 0x200), jump(0x104, 0x900)]);
        let inlined = inline_transfers(&caller, &bodies(&[&stub])).expect("inlined");
        let Insn::Lifted(at) = &inlined.insts[&0x100] else {
            panic!("lifted");
        };
        assert!(matches!(at.exit.next, Next::Dynamic(_)));
        assert_eq!(at.exit.flow, Flow::Call { ret: 0x104 });
        assert_eq!(run(&[&inlined], 0x100), run(&[&caller, &stub], 0x100));
    }

    #[test]
    fn a_jump_to_a_short_function_takes_its_exit_and_chains_run_in_place() {
        let last = func(vec![add(0x300, 9), to(0x304, LR, Flow::Return)]);
        let thunk = func(vec![add(0x200, 4), jump(0x204, 0x300)]);
        let caller = func(vec![add(0x100, 1), jump(0x104, 0x200)]);
        let inlined = inline_transfers(&caller, &bodies(&[&thunk, &last])).expect("inlined");
        let Insn::Lifted(at) = &inlined.insts[&0x104] else {
            panic!("lifted");
        };
        assert!(matches!(at.exit.next, Next::Dynamic(_)));
        assert_eq!(at.exit.flow, Flow::Return);
        assert_eq!(
            run(&[&inlined], 0x100),
            run(&[&caller, &thunk, &last], 0x100)
        );
    }

    #[test]
    fn a_return_to_an_address_that_is_not_the_call_s_stays_a_call() {
        let odd = func(vec![to(0x200, R0, Flow::Return)]);
        let caller = func(vec![call(0x100, 0x200), jump(0x104, 0x900)]);
        assert!(inline_transfers(&caller, &bodies(&[&odd])).is_none());
    }

    #[test]
    fn functions_that_branch_call_or_are_long_have_no_body() {
        let calls = func(vec![call(0x200, 0x300), to(0x204, LR, Flow::Return)]);
        assert!(inline_body(&calls, 8).is_none());
        let long = func(vec![
            add(0x200, 1),
            add(0x204, 1),
            to(0x208, LR, Flow::Return),
        ]);
        assert!(inline_body(&long, 2).is_none());
        assert!(inline_body(&long, 3).is_some());
        let back = func(vec![add(0x200, 1), jump(0x204, 0x200)]);
        assert!(inline_body(&back, 8).is_none());
    }
}
