//! Whole functions: random sequences of A64 words with forward branches,
//! loops with a bounded trip count and calls to a stub are lifted as one
//! function, emitted (with the default budgets, and with budgets small enough
//! to split the function into pieces in many shards), compiled and run on
//! random states, and the final registers, flags, memory, monitor and exit
//! target must match the interpreter running the same blocks. A copy of the
//! run with the branches of the emitted blocks swapped must be caught.
//!
//! Environment: `RELIFT_EMIT_INTERP_SEED` sets the generator seed and
//! `RELIFT_EMIT_INTERP_FUNCS` the functions per budget (default 16).

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use relift_cpu_a64::decode::{Encoding, Features, decode};
use relift_cpu_a64::frontend::A64;
use relift_emit::{Function, Insn, Options, emit};
use relift_guest::{Frontend, Home};
use relift_ir::interp::{FlatMemory, NoIntrinsics, run};
use relift_ir::{Block, Flow, Next, Op, Reg};

use crate::common::{
    BASE, CTX, Expect, Harness, Rng, Slot, diffs, fill, fnv, left_for_runtime, pack, slots, state,
    value,
};

/// The offset of a function's stub from its entry.
const STUB: u64 = 0x1_0000;
/// The offset of the address a tail branch leaves for.
const OUTSIDE: u64 = 0x2_0000;
/// Random states per function.
const STATES: usize = 12;
/// Interpreter blocks run before a function counts as not ending.
const STEPS: usize = 100_000;

/// One slot of a function, before its word is chosen.
#[derive(Clone, Copy, Debug)]
enum Item {
    /// A random instruction that falls through.
    Plain,
    /// A conditional or unconditional forward branch to an index.
    Branch(Kind, usize),
    /// A call to the stub.
    Call,
    /// Starts a loop of this many trips.
    Count(u32),
    /// Decrements the loop counter.
    Dec,
    /// Branches back to an index while the counter is not zero.
    Back(usize),
    /// Ends the function: a return, or a branch out of it when true.
    End(bool),
}

#[derive(Clone, Copy, Debug)]
enum Kind {
    Cond(u32),
    Zero { wide: bool, not: bool, rt: u32 },
    Bit { not: bool, bit: u32, rt: u32 },
    Jump,
}

fn flat(r: &mut Rng, v: &mut Vec<Item>, len: usize, end: usize) {
    for _ in 0..len {
        let at = v.len();
        let target = at + 1 + r.below((end - at) as u64) as usize;
        let rt = r.below(31) as u32;
        v.push(match r.below(10) {
            0 | 1 => Item::Branch(Kind::Cond(r.below(15) as u32), target),
            2 => Item::Branch(
                Kind::Zero {
                    wide: r.below(2) == 0,
                    not: r.below(2) == 0,
                    rt,
                },
                target,
            ),
            3 => Item::Branch(
                Kind::Bit {
                    not: r.below(2) == 0,
                    bit: r.below(64) as u32,
                    rt,
                },
                target,
            ),
            4 if r.below(3) == 0 => Item::Branch(Kind::Jump, target),
            5 if r.below(2) == 0 => Item::Call,
            _ => Item::Plain,
        });
    }
}

/// The shape of a function: straight-line runs with forward branches inside
/// them and counted loops, ending in a return or a tail branch.
fn layout(r: &mut Rng) -> Vec<Item> {
    let mut v = Vec::new();
    for _ in 0..1 + r.below(4) {
        let len = 1 + r.below(7) as usize;
        if r.below(3) == 0 {
            let head = v.len();
            v.push(Item::Count(1 + r.below(4) as u32));
            let end = head + 1 + len;
            flat(r, &mut v, len, end);
            v.push(Item::Dec);
            v.push(Item::Back(head + 1));
        } else {
            let end = v.len() + len;
            flat(r, &mut v, len, end);
        }
    }
    v.push(Item::End(r.below(5) == 0));
    v
}

const COUNTER: u32 = 28;
const RET: u32 = 0xd65f_03c0;

fn offset(from: usize, to: usize) -> u32 {
    (to as i64 - from as i64) as u32
}

/// The branch word at index `at`.
fn branch(kind: Kind, at: usize, to: usize) -> u32 {
    let d = offset(at, to);
    match kind {
        Kind::Cond(c) => 0x5400_0000 | ((d & 0x7_ffff) << 5) | c,
        Kind::Zero { wide, not, rt } => {
            (u32::from(wide) << 31)
                | 0x3400_0000
                | (u32::from(not) << 24)
                | ((d & 0x7_ffff) << 5)
                | rt
        }
        Kind::Bit { not, bit, rt } => {
            ((bit >> 5) << 31)
                | 0x3600_0000
                | (u32::from(not) << 24)
                | ((bit & 31) << 19)
                | ((d & 0x3fff) << 5)
                | rt
        }
        Kind::Jump => 0x1400_0000 | (d & 0x03ff_ffff),
    }
}

/// The registers the random instructions leave alone: the loop counter and
/// the link register.
fn reserved(fe: &A64) -> Vec<Reg> {
    let regs = fe.regs();
    (0..regs.len())
        .map(|i| Reg(i as u16))
        .filter(|&r| matches!(fe.home(r), Home::Field(f) if f == "c->x[28]" || f == "c->x[30]"))
        .collect()
}

struct Gen<'a> {
    fe: &'a A64,
    encs: Vec<Encoding>,
    reserved: Vec<Reg>,
    r: Rng,
}

impl Gen<'_> {
    /// A random word at `pc` that falls through to the next instruction.
    fn plain(&mut self, pc: u64) -> (u32, Block) {
        loop {
            let enc = self.encs[self.r.below(self.encs.len() as u64) as usize];
            let w = enc.value() | (self.r.next() as u32 & !enc.mask());
            if decode(w).map(|i| i.encoding) != Some(enc) {
                continue;
            }
            let Ok(b) = self.fe.lift(&w.to_le_bytes(), pc) else {
                continue;
            };
            let special = b.insts.iter().any(|i| match &i.op {
                Op::Intrinsic(..) => true,
                Op::Set(r, _) => self.reserved.contains(r),
                _ => false,
            });
            let memory = b.insts.iter().any(|i| {
                matches!(
                    i.op,
                    Op::Load { .. }
                        | Op::Store { .. }
                        | Op::LoadExclusive { .. }
                        | Op::StoreExclusive { .. }
                )
            });
            if special
                || b.exit.next != Next::Const(pc + 4)
                || b.exit.flow != Flow::Jump
                || (memory && self.r.below(4) != 0)
            {
                continue;
            }
            return (w, b);
        }
    }

    fn lift(&self, w: u32, pc: u64) -> (u32, Block) {
        let b = self
            .fe
            .lift(&w.to_le_bytes(), pc)
            .unwrap_or_else(|e| panic!("{w:08x} lifts: {e:?}"));
        (w, b)
    }

    fn function(&mut self, k: usize) -> Case {
        let entry = 0x40_0000_0000 + (k as u64) * 0x1000_0000;
        let items = layout(&mut self.r);
        let mut main = BTreeMap::new();
        for (i, item) in items.iter().enumerate() {
            let pc = entry + 4 * i as u64;
            let (w, b) = match *item {
                Item::Plain => self.plain(pc),
                Item::Branch(kind, to) => self.lift(branch(kind, i, to), pc),
                Item::Call => self.lift(0x9400_0000 | (((STUB - 4 * i as u64) / 4) as u32), pc),
                Item::Count(n) => self.lift(0xd280_0000 | (n << 5) | COUNTER, pc),
                Item::Dec => self.lift(0xf100_0400 | (COUNTER << 5) | COUNTER, pc),
                Item::Back(to) => self.lift(branch(Kind::Cond(1), i, to), pc),
                Item::End(false) => self.lift(RET, pc),
                Item::End(true) => self.lift(
                    0x1400_0000 | (((OUTSIDE - 4 * i as u64) / 4) as u32 & 0x03ff_ffff),
                    pc,
                ),
            };
            main.insert(pc, (w, b));
        }
        let mut stub = BTreeMap::new();
        let n = 1 + self.r.below(3);
        for i in 0..=n {
            let pc = entry + STUB + 4 * i;
            let wb = if i == n {
                self.lift(RET, pc)
            } else {
                self.plain(pc)
            };
            stub.insert(pc, wb);
        }
        Case { entry, main, stub }
    }
}

/// A generated function and its stub.
#[derive(Clone)]
struct Case {
    entry: u64,
    main: BTreeMap<u64, (u32, Block)>,
    stub: BTreeMap<u64, (u32, Block)>,
}

impl Case {
    fn blocks(&self) -> BTreeMap<u64, &Block> {
        self.main
            .iter()
            .chain(&self.stub)
            .map(|(&pc, (_, b))| (pc, b))
            .collect()
    }

    fn listing(&self) -> String {
        self.main
            .iter()
            .map(|(pc, (w, _))| format!("  {pc:#x}: {w:08x}"))
            .collect::<Vec<_>>()
            .join("\n")
    }

    /// The same function with the two targets of every forward conditional
    /// exit swapped (a swapped loop branch would not end).
    fn mutated(&self) -> Case {
        let mut c = self.clone();
        for (_, b) in c.main.values_mut() {
            if let Next::Select {
                cond,
                taken,
                not_taken,
            } = b.exit.next
                && taken > b.pc
                && not_taken > b.pc
            {
                b.exit.next = Next::Select {
                    cond,
                    taken: not_taken,
                    not_taken: taken,
                };
            }
        }
        c
    }

    fn functions(&self, k: usize) -> [Function<'_>; 2] {
        [
            part(format!("F_{k}"), self.entry, &self.main),
            part(format!("S_{k}"), self.entry + STUB, &self.stub),
        ]
    }
}

fn part<'a>(name: String, entry: u64, map: &'a BTreeMap<u64, (u32, Block)>) -> Function<'a> {
    Function {
        entry,
        name,
        guest_name: None,
        span: 4 * map.len() as u64,
        insts: map
            .iter()
            .map(|(&pc, (w, b))| {
                (
                    pc,
                    Insn::Lifted {
                        block: b,
                        raw: u64::from(*w),
                    },
                )
            })
            .collect(),
        tables: BTreeMap::new(),
        pads: Default::default(),
    }
}

/// Runs the function in the interpreter. Gives the address of the first
/// block that is not part of it when it leaves, or none when it returns.
fn exec(
    blocks: &BTreeMap<u64, &Block>,
    entry: u64,
    s: &mut relift_ir::interp::State,
    mem: &mut FlatMemory,
) -> Option<Option<u64>> {
    let mut pc = entry;
    let mut stack = Vec::new();
    for _ in 0..STEPS {
        let Some(b) = blocks.get(&pc) else {
            return Some(Some(pc));
        };
        let o = run(b, s, mem, &mut NoIntrinsics).ok()?;
        match o.flow {
            Flow::Call { ret } => {
                stack.push(ret);
                pc = o.next;
            }
            Flow::Return => match stack.pop() {
                Some(ret) => pc = ret,
                None => return Some(None),
            },
            _ => pc = o.next,
        }
    }
    None
}

/// The input records and the interpreter's answers for `cases`.
fn expectations(cases: &[Case], slots: &[Slot], r: &mut Rng) -> (Vec<u64>, Vec<Expect>) {
    let mut input = Vec::new();
    let mut expect = Vec::new();
    for (k, c) in cases.iter().enumerate() {
        let blocks = c.blocks();
        for _ in 0..STATES {
            let mut s0 = state(r.next() as u32, slots, r);
            for (i, slot) in slots.iter().enumerate() {
                if matches!(slot, Slot::Word(n) if *n < 31) && r.below(4) == 0 {
                    s0.regs[i] = u128::from(BASE + 0x1000 + (r.below(0x2000) & !15));
                } else if matches!(slot, Slot::Word(n) if *n < 31) && r.below(4) == 0 {
                    s0.regs[i] = u128::from(value(r));
                }
            }
            let seed = r.next();
            let bytes = fill(seed);
            let mut mem = FlatMemory {
                base: BASE,
                bytes: bytes.clone(),
            };
            let mut s1 = s0.clone();
            let Some(next) = exec(&blocks, c.entry, &mut s1, &mut mem) else {
                continue;
            };
            input.push(k as u64);
            input.push(seed);
            pack(&s0, slots, &bytes, &mut input);
            expect.push(Expect {
                case: k,
                state: s1,
                mem: fnv(&mem.bytes),
                next,
            });
        }
    }
    (input, expect)
}

/// The budgets of one batch.
struct Budget {
    name: &'static str,
    piece: usize,
    shard: usize,
    functions: usize,
}

struct Tally {
    compared: usize,
    skipped: usize,
    bad: Vec<String>,
    pieces: usize,
    shards: usize,
}

fn check(
    fe: &A64,
    h: &Harness,
    dir: &Path,
    cases: &[Case],
    emitted: &[Case],
    budget: &Budget,
    r: &mut Rng,
) -> Tally {
    let slots = slots();
    let (input, expect) = expectations(cases, &slots, r);
    let fns: Vec<Function<'_>> = emitted
        .iter()
        .enumerate()
        .flat_map(|(k, c)| c.functions(k))
        .collect();
    let out = emit(
        fe,
        &fns,
        &[],
        &Options {
            shard_bytes: budget.shard,
            piece_bytes: budget.piece,
            ..Options::default()
        },
    );
    let pieces = out
        .files
        .iter()
        .map(|f| f.text.matches("\nRL_TEXT RL_LIFTED(F_").count())
        .sum::<usize>();
    let shards = out
        .files
        .iter()
        .filter(|f| f.name.starts_with("shard"))
        .count();
    let files: Vec<(String, String)> = out.files.into_iter().map(|f| (f.name, f.text)).collect();
    let names: Vec<String> = (0..emitted.len()).map(|k| format!("F_{k}")).collect();
    let got = h.run(dir, &files, &names, &input);
    assert_eq!(got.len(), expect.len() * (4 + CTX), "one record per state");
    let mut t = Tally {
        compared: 0,
        skipped: 0,
        bad: Vec::new(),
        pieces,
        shards,
    };
    for (e, rec) in expect.iter().zip(got.as_chunks::<{ 4 + CTX }>().0) {
        if left_for_runtime(rec) {
            t.skipped += 1;
            continue;
        }
        t.compared += 1;
        let d = diffs(fe, &slots, e, rec);
        if !d.is_empty() {
            t.bad.push(format!(
                "{} budget, function F_{}:\n{}\n  {}",
                budget.name,
                e.case,
                cases[e.case].listing(),
                d.join("; ")
            ));
        }
    }
    t
}

#[test]
fn emitted_functions_match_the_interpreter() {
    let Some(cc) = relift_build::find_tool("clang") else {
        eprintln!("skipped: no clang on PATH");
        return;
    };
    let seed = std::env::var("RELIFT_EMIT_INTERP_SEED")
        .ok()
        .and_then(|s| s.parse::<u64>().ok())
        .unwrap_or(0x5eed_f00d);
    let n = std::env::var("RELIFT_EMIT_INTERP_FUNCS")
        .ok()
        .and_then(|s| s.parse::<usize>().ok())
        .unwrap_or(16);
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let forms = std::fs::read_to_string(root.join("cpus/a64/gen/forms.txt"))
        .expect("forms.txt is readable");
    let by_name: BTreeMap<&str, Encoding> = Encoding::all().map(|e| (e.name(), e)).collect();
    let encs: Vec<Encoding> = forms
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .map(|n| *by_name.get(n).unwrap_or_else(|| panic!("form {n} decodes")))
        .collect();
    let fe = A64 {
        features: Features::ALL,
    };
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("emit-interp-seq");
    let h = Harness::new(cc, &root, &dir.join("rt"));
    let mut g = Gen {
        reserved: reserved(&fe),
        fe: &fe,
        encs,
        r: Rng(seed | 1),
    };
    let budgets = [
        Budget {
            name: "default",
            piece: Options::default().piece_bytes,
            shard: Options::default().shard_bytes,
            functions: n,
        },
        Budget {
            name: "small pieces",
            piece: 1500,
            shard: 6000,
            functions: n,
        },
        Budget {
            name: "one instruction per piece",
            piece: 1,
            shard: 1,
            functions: n.div_ceil(3),
        },
    ];
    let mut bad = Vec::new();
    let mut mutation_caught = 0;
    let mut mutation_runs = 0;
    for (i, b) in budgets.iter().enumerate() {
        let cases: Vec<Case> = (0..b.functions).map(|k| g.function(k)).collect();
        let mut r = Rng(g.r.next() | 1);
        let t = check(
            &fe,
            &h,
            &dir.join(format!("run{i}")),
            &cases,
            &cases,
            b,
            &mut r,
        );
        eprintln!(
            "{}: {} functions, {} C functions in {} shards, {} runs compared, {} left for the runtime",
            b.name, b.functions, t.pieces, t.shards, t.compared, t.skipped
        );
        assert!(
            t.compared * 2 >= (t.compared + t.skipped).max(1) && t.compared >= b.functions * 2,
            "{}: only {} of {} runs compared",
            b.name,
            t.compared,
            t.compared + t.skipped
        );
        if b.piece == 1 {
            assert!(t.pieces > b.functions * 3, "{}: not split", b.name);
        }
        bad.extend(t.bad);
        if i == 0 {
            let flipped: Vec<Case> = cases.iter().map(Case::mutated).collect();
            let mut r = Rng(g.r.next() | 1);
            let m = check(&fe, &h, &dir.join("mutated"), &cases, &flipped, b, &mut r);
            mutation_caught = m.bad.len();
            mutation_runs = m.compared;
        }
    }
    eprintln!("mutation: {mutation_caught} of {mutation_runs} runs caught");
    assert!(
        bad.is_empty(),
        "seed {seed:#x}, {} mismatches:\n{}",
        bad.len(),
        bad.join("\n")
    );
    assert!(
        mutation_caught > 0,
        "swapping the branches of the emitted blocks went unnoticed"
    );
}
