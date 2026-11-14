//! The section 13 operations (tick, tick.if, I/O loads and stores of both
//! kinds, endians and odd sizes, sync points that take interrupts) on random
//! 16-bit blocks over a Game Boy-like map, one step at a time.

use relift_emit_wasm::{Options, ctx};
use relift_ir::interp::{self, FlatMemory, NoIntrinsics, State};
use relift_ir::{
    BinOp, Block, Builder, CmpOp, Endian, Exit, Flow, IoKind, Next, Op, Reg, RegDecl, RegFile,
    Value, Width,
};

use crate::ops::{module, step_options};
use crate::support::{
    Bytes, CTX, Ev, FIRST_SLOT, Irq, Model, Rig, Rng, WINDOW, ctx_u64, diff_bytes, engine, gb_map,
    get_reg, put_reg, scale, seed, set_clock,
};

static DECLS: [RegDecl; 6] = [
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
];

pub fn file() -> RegFile {
    RegFile::new("hw", &DECLS)
}

pub const IRQ: Irq = Irq {
    sp: Reg(3),
    ime: Reg(5),
    vector: 0x40,
};

const CONSTS_R: [u64; 10] = [
    0xff00, 0xff44, 0xff7e, 0xff80, 0xc000, 0x4000, 0x7fff, 0xfffe, 0xffff, 0x9000,
];
const CONSTS_W: [u64; 8] = [
    0xff01, 0xff40, 0xc123, 0x2000, 0xfffe, 0xffff, 0xa000, 0x7ffe,
];

/// Adds random section 13 operations to `b`; `vals8`, `addrs` and `bits`
/// are the 8-bit, 16-bit and 1-bit values so far.
pub fn ops(
    b: &mut Builder,
    r: &mut Rng,
    vals8: &mut Vec<Value>,
    addrs: &mut Vec<Value>,
    bits: &mut Vec<Value>,
    n: u64,
) {
    for _ in 0..n {
        match r.below(7) {
            0 => b.tick(4 * (1 + r.below(3) as u32)),
            1 => {
                let c = r.pick(bits);
                b.tick_if(c, 4);
            }
            2 | 3 => {
                let a = if r.chance(0.5) {
                    r.pick(addrs)
                } else {
                    b.konst(Width::W16, u128::from(r.pick(&CONSTS_R)))
                };
                let w = Width::new(8 * r.pick(&[1u32, 1, 2, 3, 4]));
                let kind = if r.chance(0.2) {
                    IoKind::Device
                } else {
                    IoKind::Any
                };
                let e = if r.chance(0.2) {
                    Endian::Big
                } else {
                    Endian::Little
                };
                let v = b.io_load(a, w, e, kind);
                if w == Width::W8 {
                    vals8.push(v);
                } else {
                    let lo = b.extract(v, 0, Width::W8);
                    vals8.push(lo);
                    let a = b.extract(v, 0, Width::W16);
                    addrs.push(a);
                }
            }
            4 => {
                let a = if r.chance(0.5) {
                    r.pick(addrs)
                } else {
                    b.konst(Width::W16, u128::from(r.pick(&CONSTS_W)))
                };
                let v = r.pick(vals8);
                let v = if r.chance(0.3) {
                    let h = r.pick(vals8);
                    b.concat(h, v)
                } else {
                    v
                };
                let kind = if r.chance(0.2) {
                    IoKind::Device
                } else {
                    IoKind::Any
                };
                let e = if r.chance(0.2) {
                    Endian::Big
                } else {
                    Endian::Little
                };
                b.io_store(a, v, e, kind);
            }
            5 => {
                let x = r.pick(vals8);
                let y = r.pick(vals8);
                let s = b.bin(r.pick(&[BinOp::Add, BinOp::Xor, BinOp::Sub]), x, y);
                vals8.push(s);
                let z = b.konst(Width::W8, 0);
                let c = b.cmp(CmpOp::Eq, s, z);
                bits.push(c);
            }
            _ => {
                let x = r.pick(addrs);
                let one = b.konst(Width::W16, 1);
                let y = b.bin(BinOp::Add, x, one);
                addrs.push(y);
            }
        }
    }
}

fn make_block(r: &mut Rng, pc: u64) -> Block {
    let mut b = Builder::new(file(), Width::W16, pc);
    let mut vals8 = vec![b.get(Reg(0)), b.get(Reg(1))];
    let mut addrs = vec![b.get(Reg(2))];
    let mut bits = vec![b.get(Reg(4))];
    let n = 3 + r.below(8);
    ops(&mut b, r, &mut vals8, &mut addrs, &mut bits, n);
    let a = r.pick(&vals8);
    b.set(Reg(0), a);
    let h = r.pick(&addrs);
    b.set(Reg(2), h);
    let c = r.pick(&bits);
    b.set(Reg(4), c);
    if r.chance(0.6) {
        b.sync();
    }
    let next = if r.chance(0.5) {
        Next::Const(pc + 1)
    } else {
        Next::Select {
            cond: c,
            taken: 0x200,
            not_taken: pc + 1,
        }
    };
    b.finish(
        1,
        Exit {
            next,
            flow: Flow::Jump,
        },
    )
}

/// Breaks one tick, else one I/O load's byte order; false when there is
/// neither.
fn mutate(b: &mut Block) -> bool {
    for i in &mut b.insts {
        match i.op {
            Op::Tick(n) => {
                i.op = Op::Tick(n + 4);
                return true;
            }
            Op::TickIf(c, n) => {
                i.op = Op::TickIf(c, n + 4);
                return true;
            }
            _ => {}
        }
    }
    for i in &mut b.insts {
        if let Op::IoLoad { addr, endian, kind } = i.op {
            i.op = Op::IoLoad {
                addr,
                endian: if endian == Endian::Big {
                    Endian::Little
                } else {
                    Endian::Big
                },
                kind,
            };
            return true;
        }
    }
    false
}

#[derive(Debug, PartialEq, Eq)]
struct Res {
    regs: Vec<u128>,
    mem: Bytes,
    pc: u64,
    time: u64,
    log: Vec<Ev>,
}

fn init(r: &mut Rng) -> (Vec<u128>, Vec<u8>) {
    let any = r.below(0x10000);
    let regs = vec![
        u128::from(r.below(256) as u8),
        u128::from(r.below(256) as u8),
        u128::from(r.pick(&[0xff40u64, 0xc000, 0xff7f, 0x8000, 0xfffe, 0x3fff, any])),
        u128::from(0xdff0 - 2 * r.below(8)),
        u128::from(r.below(2)),
        u128::from(r.below(2)),
    ];
    let mem = (0..WINDOW).map(|_| r.next() as u8).collect();
    (regs, mem)
}

fn interp_run(b: &Block, regs: &[u128], mem: &[u8], t0: u64) -> Res {
    let mut st = State::new(file().len());
    st.regs = regs.to_vec();
    let mut m = FlatMemory {
        base: 0,
        bytes: mem.to_vec(),
    };
    m.bytes.resize(WINDOW + 64, 0);
    let mut d = Model::new(gb_map(), t0, Some(IRQ));
    let o = interp::run_with(b, &mut st, &mut m, &mut NoIntrinsics, &mut d).expect("runs");
    m.bytes.truncate(WINDOW);
    Res {
        regs: st.regs,
        mem: Bytes(m.bytes),
        pc: o.next,
        time: d.time,
        log: d.log,
    }
}

fn wasm_run(
    rig: &mut Rig,
    f: &wasmtime::TypedFunc<(i32, i32), i32>,
    regs: &[u128],
    mem: &[u8],
    t0: u64,
) -> Res {
    let file = file();
    rig.store.data_mut().model = Some(Model::new(gb_map(), t0, Some(IRQ)));
    rig.store.data_mut().nested_irq = false;
    let m = rig.mem();
    m[CTX as usize..CTX as usize + ctx::size(&file) as usize].fill(0);
    for (i, v) in regs.iter().enumerate() {
        put_reg(m, CTX, Reg(i as u16), *v);
    }
    set_clock(m, CTX, t0);
    rig.guest()[..WINDOW].copy_from_slice(mem);
    rig.guest()[WINDOW..].fill(0);
    let rc = rig.call(f, CTX).expect("no trap");
    assert_eq!(rc, 0);
    let m = rig.mem();
    let time = ctx_u64(m, CTX, ctx::DEADLINE).wrapping_sub(ctx_u64(m, CTX, ctx::BUDGET));
    let pc = ctx_u64(m, CTX, ctx::PC);
    let regs = file
        .decls()
        .iter()
        .enumerate()
        .map(|(i, d)| get_reg(m, CTX, Reg(i as u16), d.width))
        .collect();
    let mem = Bytes(rig.guest()[..WINDOW].to_vec());
    let log = rig
        .store
        .data_mut()
        .model
        .take()
        .map(|m| m.log)
        .unwrap_or_default();
    Res {
        regs,
        mem,
        pc,
        time,
        log,
    }
}

fn options() -> Options {
    Options {
        io_map: gb_map(),
        ..step_options()
    }
}

#[test]
fn hardware_register_blocks_match_the_interpreter() {
    let n = scale(1500, 20_000);
    let batch = 100;
    let engine = engine(true);
    let mut r = Rng::new("hw", seed());
    let (mut ok, mut syncs, mut irqs, mut devr, mut devw) = (0, 0, 0, 0, 0);
    let (mut caught, mut seen) = (0, 0);
    let mut first = None;
    let mut done = 0;
    while done < n {
        let k = batch.min(n - done) as usize;
        let blocks: Vec<Block> = (0..k)
            .map(|i| make_block(&mut r, 0x150 + 2 * i as u64))
            .collect();
        let mut broken = blocks.clone();
        let changed: Vec<bool> = broken.iter_mut().map(mutate).collect();
        let out = module(&blocks, &options());
        assert!(out.errors.is_empty(), "emit errors: {:?}", out.errors);
        let bad = module(&broken, &options());
        let mut rig = Rig::new(&engine, FIRST_SLOT + k as u32, file(), &[]);
        let inst = rig.load(&out).expect("loads");
        let mut mrig = Rig::new(&engine, FIRST_SLOT + k as u32, file(), &[]);
        let minst = mrig.load(&bad).expect("loads");
        for i in 0..k {
            let (regs, mem) = init(&mut r);
            let t0 = r.below(1000);
            let want = interp_run(&blocks[i], &regs, &mem, t0);
            let f = rig.func(&inst, &format!("f{i}"));
            let got = wasm_run(&mut rig, &f, &regs, &mem, t0);
            for e in &want.log {
                match e {
                    Ev::Sync(..) => syncs += 1,
                    Ev::Read(..) => devr += 1,
                    Ev::Write(..) => devw += 1,
                }
            }
            irqs += u64::from(want.pc == IRQ.vector);
            if want == got {
                ok += 1;
            } else if first.is_none() {
                first = Some(format!(
                    "{}\nwant {want:x?}\ngot {got:x?}\nmemory {}",
                    blocks[i],
                    diff_bytes(&want.mem, &got.mem)
                ));
            }
            if changed[i] && interp_run(&broken[i], &regs, &mem, t0) != want {
                seen += 1;
                let mf = mrig.func(&minst, &format!("f{i}"));
                if wasm_run(&mut mrig, &mf, &regs, &mem, t0) != want {
                    caught += 1;
                }
            }
        }
        done += k as u64;
    }
    println!(
        "hw: {n} blocks, {ok} matched; device bytes read {devr} written {devw}, syncs {syncs}, interrupts {irqs}; mutation caught {caught}/{seen}"
    );
    assert!(
        first.is_none(),
        "first difference:\n{}",
        first.unwrap_or_default()
    );
    assert!(
        syncs > 0 && irqs > 0 && devr > 0 && devw > 0,
        "control: the sample misses an event kind"
    );
    assert!(
        seen > 0 && caught == seen,
        "mutation control: caught {caught}/{seen}"
    );
}
