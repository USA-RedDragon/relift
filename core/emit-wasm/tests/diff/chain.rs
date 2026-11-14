//! Random programs in the real chained mode: several guest functions in
//! two modules that share one memory and table, with branches and bounded
//! loops inside functions, direct and indirect calls and tail calls across
//! modules, jump tables, traps, unknown targets, and interrupts taken at
//! sync points and run as nested calls. Each program runs with tail calls
//! and with the fallback, and is compared with the interpreter stepping the
//! same blocks one at a time.

use std::collections::BTreeMap;

use relift_emit_wasm::{Exits, Function, Insn, Options, ctx, emit};
use relift_ir::interp::{self, FlatMemory, NoIntrinsics, State};
use relift_ir::{
    BinOp, Block, Builder, CmpOp, Endian, Exit, Flow, IoKind, Next, Order, Reg, RegDecl, RegFile,
    Value, Width,
};

use crate::mutate::mutate;
use crate::support::{
    Bytes, CTX, Ev, FIRST_SLOT, Irq, Model, Rig, Rng, WINDOW, ctx_u64, diff_bytes, engine, gb_map,
    get_reg, put_reg, scale, seed, set_clock,
};

const A: Reg = Reg(0);
const B: Reg = Reg(1);
const HL: Reg = Reg(2);
const SP: Reg = Reg(3);
const Z: Reg = Reg(4);
const IME: Reg = Reg(5);
const CNT: Reg = Reg(6);
const X: Reg = Reg(7);
const Q: Reg = Reg(8);

static DECLS: [RegDecl; 9] = [
    RegDecl {
        name: "a",
        width: Width::W8,
    },
    RegDecl {
        name: "b",
        width: Width::W8,
    },
    RegDecl {
        name: "hl",
        width: Width::W16,
    },
    RegDecl {
        name: "sp",
        width: Width::W16,
    },
    RegDecl {
        name: "z",
        width: Width::W1,
    },
    RegDecl {
        name: "ime",
        width: Width::W1,
    },
    RegDecl {
        name: "cnt",
        width: Width::W8,
    },
    RegDecl {
        name: "x",
        width: Width::W64,
    },
    RegDecl {
        name: "q",
        width: Width::W128,
    },
];

fn file() -> RegFile {
    RegFile::new("chain", &DECLS)
}

const ISR: u64 = 0x40;
const IRQ: Irq = Irq {
    sp: SP,
    ime: IME,
    vector: ISR,
};
const UNKNOWN: u64 = 0x0999;
const TRAP_CODE: u32 = 0x21;

fn entry(i: usize) -> u64 {
    0x1000 + 0x100 * i as u64
}

struct Gen<'a> {
    r: &'a mut Rng,
    nfns: usize,
    hw: bool,
}

impl Gen<'_> {
    fn body(&mut self, b: &mut Builder) {
        let n = self.r.below(5);
        for _ in 0..n {
            match self.r.below(if self.hw { 9 } else { 5 }) {
                0 => {
                    let (x, y) = (b.get(A), b.get(B));
                    let s = b.bin(self.r.pick(&[BinOp::Add, BinOp::Sub, BinOp::Xor]), x, y);
                    b.set(A, s);
                    let zero = b.konst(Width::W8, 0);
                    let z = b.cmp(CmpOp::Eq, s, zero);
                    b.set(Z, z);
                }
                1 => {
                    let x = b.get(X);
                    let k = b.konst(Width::W64, u128::from(self.r.next()));
                    let y = b.bin(self.r.pick(&[BinOp::Mul, BinOp::Add, BinOp::RotR]), x, k);
                    b.set(X, y);
                    let q = b.get(Q);
                    let yx = b.zext(y, Width::W128);
                    let s = b.bin(BinOp::Add, q, yx);
                    let one = b.konst(Width::W128, 3);
                    let s = b.bin(BinOp::RotR, s, one);
                    b.set(Q, s);
                }
                2 => {
                    let hl = b.get(HL);
                    let v = b.load(hl, Width::W16, Endian::Little, Order::Plain);
                    let lo = b.trunc(v, Width::W8);
                    b.set(B, lo);
                }
                3 => {
                    let hl = b.get(HL);
                    let m = b.konst(Width::W16, 0x0fff);
                    let off = b.and(hl, m);
                    let base = b.konst(Width::W16, 0xc000);
                    let a = b.or(off, base);
                    let v = b.get(X);
                    b.store(a, v, Endian::Little, Order::Plain);
                }
                4 => {
                    let hl = b.get(HL);
                    let k = b.konst(Width::W16, u128::from(self.r.below(0x1000)));
                    let s = b.add(hl, k);
                    b.set(HL, s);
                }
                5 => b.tick(4 * (1 + self.r.below(3) as u32)),
                6 => {
                    let z = b.get(Z);
                    b.tick_if(z, 4);
                }
                7 => {
                    let k = self
                        .r
                        .pick(&[0xff00u64, 0xff44, 0xc000, 0x7fff, 0xffff, 0xfffe]);
                    let a = b.konst(Width::W16, u128::from(k));
                    let w = Width::new(8 * self.r.pick(&[1u32, 2, 3]));
                    let kind = if self.r.chance(0.3) {
                        IoKind::Device
                    } else {
                        IoKind::Any
                    };
                    let v = b.io_load(a, w, Endian::Little, kind);
                    let lo = b.trunc(v, Width::W8);
                    b.set(A, lo);
                }
                _ => {
                    let k = self
                        .r
                        .pick(&[0xff01u64, 0xff40, 0xc123, 0x2000, 0xffff, 0x7ffe]);
                    let a = b.konst(Width::W16, u128::from(k));
                    let v = b.get(A);
                    let kind = if self.r.chance(0.3) {
                        IoKind::Device
                    } else {
                        IoKind::Any
                    };
                    b.io_store(a, v, Endian::Big, kind);
                }
            }
        }
    }

    fn sync(&mut self, b: &mut Builder) {
        if self.hw && self.r.chance(0.5) {
            b.sync();
        }
    }

    /// Pushes the 16-bit `ret` on the guest stack.
    fn push(b: &mut Builder, ret: u64) {
        let sp = b.get(SP);
        let two = b.konst(Width::W16, 2);
        let nsp = b.sub(sp, two);
        let r = b.konst(Width::W16, u128::from(ret));
        b.store(nsp, r, Endian::Little, Order::Plain);
        b.set(SP, nsp);
    }

    /// Pops the return address and returns through it.
    fn ret(b: &mut Builder) -> Value {
        let sp = b.get(SP);
        let r = b.load(sp, Width::W16, Endian::Little, Order::Plain);
        let two = b.konst(Width::W16, 2);
        let nsp = b.add(sp, two);
        b.set(SP, nsp);
        r
    }

    /// The blocks of function `i`.
    fn function(&mut self, i: usize) -> Vec<Block> {
        let e = entry(i);
        let m = 3 + self.r.below(10);
        let mut out = Vec::new();
        let mut j = 0;
        while j < m {
            let pc = e + j;
            let mut b = Builder::new(file(), Width::W16, pc);
            self.body(&mut b);
            let last = j + 1 == m;
            let callee = (i + 1 < self.nfns)
                .then(|| i + 1 + self.r.below((self.nfns - i - 1) as u64) as usize);
            let kind = if last {
                self.r.below(3) + 10
            } else {
                self.r.below(10)
            };
            let exit = match kind {
                0..=2 => {
                    self.sync(&mut b);
                    Exit {
                        next: Next::Const(pc + 1),
                        flow: Flow::Jump,
                    }
                }
                3 if j + 4 < m => {
                    let k = b.konst(Width::W8, 1 + u128::from(self.r.below(3)));
                    b.set(CNT, k);
                    out.push(b.finish(
                        1,
                        Exit {
                            next: Next::Const(pc + 1),
                            flow: Flow::Jump,
                        },
                    ));
                    let mut body = Builder::new(file(), Width::W16, pc + 1);
                    self.body(&mut body);
                    self.sync(&mut body);
                    out.push(body.jump(1, pc + 2));
                    let mut back = Builder::new(file(), Width::W16, pc + 2);
                    let c = back.get(CNT);
                    let one = back.konst(Width::W8, 1);
                    let c1 = back.sub(c, one);
                    back.set(CNT, c1);
                    let zero = back.konst(Width::W8, 0);
                    let more = back.cmp(CmpOp::Ne, c1, zero);
                    self.sync(&mut back);
                    out.push(back.finish(
                        1,
                        Exit {
                            next: Next::Select {
                                cond: more,
                                taken: pc + 1,
                                not_taken: pc + 3,
                            },
                            flow: Flow::Jump,
                        },
                    ));
                    j += 3;
                    continue;
                }
                4 => {
                    let z = b.get(Z);
                    let t = pc + 1 + self.r.below(m - j - 1);
                    self.sync(&mut b);
                    Exit {
                        next: Next::Select {
                            cond: z,
                            taken: t,
                            not_taken: pc + 1,
                        },
                        flow: Flow::Jump,
                    }
                }
                5 | 6 if callee.is_some() => {
                    let t = entry(callee.unwrap_or(0));
                    Self::push(&mut b, pc + 1);
                    let next = if kind == 5 {
                        Next::Const(t)
                    } else {
                        let v = b.konst(Width::W16, u128::from(t));
                        b.set(HL, v);
                        Next::Dynamic(v)
                    };
                    self.sync(&mut b);
                    Exit {
                        next,
                        flow: Flow::Call { ret: pc + 1 },
                    }
                }
                7 => Exit {
                    next: Next::Const(pc + 1),
                    flow: Flow::Trap(TRAP_CODE),
                },
                8 if j + 3 < m => {
                    let z = b.get(Z);
                    let t1 = b.konst(Width::W16, u128::from(pc + 1));
                    let t2 = b.konst(Width::W16, u128::from(pc + 2));
                    let v = b.select(z, t2, t1);
                    self.sync(&mut b);
                    Exit {
                        next: Next::Dynamic(v),
                        flow: Flow::Jump,
                    }
                }
                9 if self.r.chance(0.05) => {
                    Self::push(&mut b, pc + 1);
                    Exit {
                        next: Next::Const(UNKNOWN),
                        flow: Flow::Call { ret: pc + 1 },
                    }
                }
                10 | 11 if callee.is_some() => {
                    let t = entry(callee.unwrap_or(0));
                    let next = if kind == 10 {
                        Next::Const(t)
                    } else {
                        let v = b.konst(Width::W16, u128::from(t));
                        b.set(HL, v);
                        Next::Dynamic(v)
                    };
                    self.sync(&mut b);
                    Exit {
                        next,
                        flow: Flow::Jump,
                    }
                }
                _ if last => {
                    let r = Self::ret(&mut b);
                    if i != 0 {
                        self.sync(&mut b);
                    }
                    Exit {
                        next: Next::Dynamic(r),
                        flow: Flow::Return,
                    }
                }
                _ => Exit {
                    next: Next::Const(pc + 1),
                    flow: Flow::Jump,
                },
            };
            out.push(b.finish(1, exit));
            j += 1;
        }
        out
    }

    /// The interrupt handler: one odd tick, so it returns at an even cycle,
    /// then a return that sets `ime` again and is a sync point.
    fn isr(&mut self) -> Vec<Block> {
        let mut b0 = Builder::new(file(), Width::W16, ISR);
        b0.tick(3);
        let a = b0.get(A);
        let k = b0.konst(Width::W8, 0x11);
        let s = b0.add(a, k);
        b0.set(A, s);
        let b0 = b0.jump(1, ISR + 1);
        let mut b1 = Builder::new(file(), Width::W16, ISR + 1);
        let r = Self::ret(&mut b1);
        let one = b1.konst(Width::W1, 1);
        b1.set(IME, one);
        b1.sync();
        let b1 = b1.finish(
            1,
            Exit {
                next: Next::Dynamic(r),
                flow: Flow::Return,
            },
        );
        vec![b0, b1]
    }
}

#[derive(Debug, PartialEq, Eq)]
struct Res {
    rc: i32,
    regs: Vec<u128>,
    mem: Bytes,
    time: u64,
    log: Vec<Ev>,
    irqs: u64,
    traps: Vec<(u32, u64)>,
    misses: Vec<u64>,
}

struct Program {
    fns: Vec<(u64, Vec<Block>)>,
    tables: BTreeMap<u64, BTreeMap<u64, Vec<u64>>>,
    hw: bool,
}

fn program(r: &mut Rng, hw: bool) -> Program {
    let nfns = 2 + r.below(6) as usize;
    let mut g = Gen { r, nfns, hw };
    let mut fns: Vec<(u64, Vec<Block>)> = (0..nfns).map(|i| (entry(i), g.function(i))).collect();
    if hw {
        fns.push((ISR, g.isr()));
    }
    let mut tables = BTreeMap::new();
    for (e, blocks) in &fns {
        let mut t = BTreeMap::new();
        for b in blocks {
            if let (Flow::Jump, Next::Dynamic(_)) = (b.exit.flow, b.exit.next)
                && b.pc + 2 < e + blocks.len() as u64
            {
                t.insert(b.pc, vec![b.pc + 1, b.pc + 2]);
            }
        }
        tables.insert(*e, t);
    }
    Program { fns, tables, hw }
}

fn init(r: &mut Rng) -> (Vec<u128>, Vec<u8>, u64) {
    let regs = file()
        .decls()
        .iter()
        .map(|d| match d.name {
            "sp" => 0xdff0,
            "ime" => u128::from(r.below(2)),
            _ => r.value(d.width),
        })
        .collect();
    let mem = (0..WINDOW).map(|_| r.next() as u8).collect();
    (regs, mem, r.below(1000))
}

fn reference(p: &Program, regs: &[u128], mem: &[u8], t0: u64, trap_rc: i32) -> Res {
    let blocks: BTreeMap<u64, &Block> = p
        .fns
        .iter()
        .flat_map(|(_, bs)| bs.iter().map(|b| (b.pc, b)))
        .collect();
    let mut st = State::new(file().len());
    st.regs = regs.to_vec();
    let mut m = FlatMemory {
        base: 0,
        bytes: mem.to_vec(),
    };
    m.bytes.resize(WINDOW + 64, 0);
    let mut d = Model::new(gb_map(), t0, p.hw.then_some(IRQ));
    let (mut traps, mut misses) = (Vec::new(), Vec::new());
    let mut pc = entry(0);
    let mut depth: i64 = 0;
    let mut rc = 0;
    for _ in 0..1_000_000 {
        let Some(b) = blocks.get(&pc) else {
            misses.push(pc);
            rc = 1;
            break;
        };
        let o = interp::run_with(b, &mut st, &mut m, &mut NoIntrinsics, &mut d).expect("runs");
        match o.flow {
            Flow::Call { .. } => depth += 1,
            Flow::Return => depth -= 1,
            Flow::Trap(code) => traps.push((code, b.pc)),
            Flow::Jump => {}
        }
        if o.resume.is_some() {
            depth += 1;
        }
        if depth < 0 {
            break;
        }
        if matches!(o.flow, Flow::Trap(_)) && trap_rc != 0 {
            rc = trap_rc;
            break;
        }
        pc = o.next;
    }
    m.bytes.truncate(WINDOW);
    Res {
        rc,
        regs: st.regs,
        mem: Bytes(m.bytes),
        time: if p.hw { d.time } else { t0 },
        irqs: d.taken,
        log: d.log,
        traps,
        misses,
    }
}

fn functions<'a>(p: &'a Program, pick: impl Fn(usize) -> bool) -> Vec<Function<'a>> {
    p.fns
        .iter()
        .enumerate()
        .filter(|(i, _)| pick(*i))
        .map(|(_, (e, bs))| Function {
            entry: *e,
            bank: None,
            name: format!("f{e:x}"),
            insts: bs
                .iter()
                .map(|b| (b.pc, Insn::Lifted { block: b, raw: 0 }))
                .collect(),
            tables: p.tables[e].clone(),
        })
        .collect()
}

fn run(
    p: &Program,
    tail_calls: bool,
    split: bool,
    regs: &[u128],
    mem: &[u8],
    t0: u64,
    trap_rc: i32,
) -> Res {
    let opts = |base: u32| Options {
        exits: Exits::Chain,
        tail_calls,
        table_base: base,
        memory_pages: crate::support::PAGES,
        io_map: gb_map(),
        export: true,
        ..Options::default()
    };
    let (first, second) = if split {
        (functions(p, |i| i % 2 == 0), functions(p, |i| i % 2 == 1))
    } else {
        (functions(p, |_| true), Vec::new())
    };
    let o1 = emit(&first, &opts(FIRST_SLOT));
    assert!(o1.errors.is_empty(), "emit errors: {:?}", o1.errors);
    let base2 = FIRST_SLOT + first.len() as u32;
    let o2 = emit(&second, &opts(base2));
    assert!(o2.errors.is_empty(), "emit errors: {:?}", o2.errors);
    let engine = engine(tail_calls);
    let mut rig = Rig::new(&engine, base2, file(), &[]);
    let i1 = rig.load(&o1).expect("the first module loads");
    rig.grow_table(base2 + second.len() as u32);
    if !second.is_empty() {
        rig.load(&o2).expect("the second module loads");
    }
    rig.store.data_mut().model = Some(Model::new(gb_map(), t0, p.hw.then_some(IRQ)));
    rig.store.data_mut().nested_irq = true;
    rig.store.data_mut().trap_rc = trap_rc;
    let file = file();
    let m = rig.mem();
    m[CTX as usize..CTX as usize + ctx::size(&file) as usize].fill(0);
    for (i, v) in regs.iter().enumerate() {
        put_reg(m, CTX, Reg(i as u16), *v);
    }
    set_clock(m, CTX, t0);
    rig.guest()[..WINDOW].copy_from_slice(mem);
    rig.guest()[WINDOW..].fill(0);
    let f = rig.func(&i1, &format!("f{:x}", entry(0)));
    let rc = rig.call(&f, CTX).expect("no trap");
    let m = rig.mem();
    let time = ctx_u64(m, CTX, ctx::DEADLINE).wrapping_sub(ctx_u64(m, CTX, ctx::BUDGET));
    let regs = file
        .decls()
        .iter()
        .enumerate()
        .map(|(i, d)| get_reg(m, CTX, Reg(i as u16), d.width))
        .collect();
    let mem = Bytes(rig.guest()[..WINDOW].to_vec());
    let h = rig.store.data_mut();
    let model = h.model.take().expect("a device model");
    Res {
        rc,
        regs,
        mem,
        time,
        irqs: model.taken,
        log: model.log,
        traps: std::mem::take(&mut h.traps),
        misses: std::mem::take(&mut h.misses),
    }
}

#[test]
fn chained_programs_match_the_interpreter() {
    let n = scale(60, 2000);
    let mut r = Rng::new("chain", seed());
    let (mut ok, mut runs, mut syncs, mut irqs, mut pending, mut misses) = (0, 0, 0, 0, 0, 0);
    let (mut caught, mut seen) = (0, 0);
    let mut first = None;
    for k in 0..n {
        let hw = k % 2 == 1;
        let p = program(&mut r, hw);
        let (regs, mem, t0) = init(&mut r);
        let trap_rc = i32::from(r.chance(0.15));
        let want = reference(&p, &regs, &mem, t0, trap_rc);
        syncs += want
            .log
            .iter()
            .filter(|e| matches!(e, Ev::Sync(..)))
            .count() as u64;
        irqs += want.irqs;
        pending += u64::from(want.rc != 0);
        misses += want.misses.len() as u64;
        for (tail_calls, split) in [(true, false), (true, true), (false, true)] {
            let got = run(&p, tail_calls, split, &regs, &mem, t0, trap_rc);
            runs += 1;
            if got == want {
                ok += 1;
            } else if first.is_none() {
                let text: Vec<String> = p
                    .fns
                    .iter()
                    .flat_map(|(_, bs)| bs.iter().map(|b| b.to_string()))
                    .collect();
                first = Some(format!(
                    "program {k} tail_calls {tail_calls} split {split}\n{}\nwant {want:x?}\ngot {got:x?}\nmemory {}",
                    text.join("\n"),
                    diff_bytes(&want.mem, &got.mem)
                ));
            }
        }
        let mut broken = Program {
            fns: p.fns.clone(),
            tables: p.tables.clone(),
            hw: p.hw,
        };
        let f = r.below(broken.fns.len() as u64) as usize;
        let at = r.below(broken.fns[f].1.len() as u64) as usize;
        if !mutate(&mut broken.fns[f].1[at]).is_empty()
            && broken.fns[f].1[at].verify().is_ok()
            && reference(&broken, &regs, &mem, t0, trap_rc) != want
        {
            seen += 1;
            if run(&broken, true, true, &regs, &mem, t0, trap_rc) != want {
                caught += 1;
            }
        }
    }
    println!(
        "chain: {n} programs, {runs} runs, {ok} matched; syncs {syncs}, interrupts {irqs}, pending {pending}, misses {misses}; mutation caught {caught}/{seen}"
    );
    assert!(
        first.is_none(),
        "first difference:\n{}",
        first.unwrap_or_default()
    );
    assert!(
        syncs > 0 && irqs > 0 && pending > 0 && misses > 0,
        "control: the sample misses an event kind"
    );
    assert!(
        seen > 0 && caught == seen,
        "mutation control: caught {caught}/{seen}"
    );
}

#[test]
fn a_pending_trap_stores_the_registers_and_unwinds() {
    let mut b = Builder::new(file(), Width::W16, entry(0));
    let k = b.konst(Width::W8, 0x5a);
    b.set(A, k);
    let b0 = b.finish(
        1,
        Exit {
            next: Next::Const(entry(0) + 1),
            flow: Flow::Trap(TRAP_CODE),
        },
    );
    let mut b = Builder::new(file(), Width::W16, entry(0) + 1);
    let k = b.konst(Width::W8, 0x77);
    b.set(A, k);
    let r = Gen::ret(&mut b);
    let b1 = b.finish(
        1,
        Exit {
            next: Next::Dynamic(r),
            flow: Flow::Return,
        },
    );
    let p = Program {
        fns: vec![(entry(0), vec![b0, b1])],
        tables: [(entry(0), BTreeMap::new())].into_iter().collect(),
        hw: false,
    };
    let mut regs = vec![0u128; file().len()];
    regs[SP.index()] = 0xdff0;
    let mem = vec![0u8; WINDOW];
    for tail_calls in [true, false] {
        let got = run(&p, tail_calls, false, &regs, &mem, 0, 1);
        assert_eq!(got.rc, 1);
        assert_eq!(got.traps, vec![(TRAP_CODE, entry(0))]);
        assert_eq!(got.regs[A.index()], 0x5a);
        let got = run(&p, tail_calls, false, &regs, &mem, 0, 0);
        assert_eq!(got.rc, 0);
        assert_eq!(got.regs[A.index()], 0x77);
    }
}
