//! Compares the generated templates with the hand-written lifter, opcode by
//! opcode: the same IR text after `dce` and `simplify`, or else the same
//! behavior in the interpreter on random states (registers, flags, every
//! I/O access with its address, value and clock, the intrinsics, the exit
//! and the total clock).

use std::collections::BTreeMap;

use relift_ir::interp::{self, Devices, Error, Fault, GuestMemory, Intrinsics, State};
use relift_ir::{AddrClass, BinOp, Block, IoMap, Next, Op, Reg, dce, simplify};

use super::super::{BUG, HALF, Options, lift_bytes, lift_bytes_sail};
use crate::{BASE, CB, Mnemonic, Opcode, regs};

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
}

fn devices() -> IoMap {
    IoMap {
        ranges: Vec::new(),
        default: AddrClass::Device,
    }
}

#[derive(Debug, PartialEq, Eq)]
enum Event {
    Read(u64, u8),
    Write(u64, u8),
    Call(&'static str, u128),
}

struct Machine {
    seed: u64,
    clock: u64,
    events: Vec<(u64, Event)>,
    map: IoMap,
}

impl Machine {
    fn value(&self, salt: u64) -> u64 {
        let mut r = Rng(self.seed ^ salt.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1);
        r.next();
        r.next()
    }
}

struct NoMem;

impl GuestMemory for NoMem {
    fn read(&mut self, addr: u64, _buf: &mut [u8]) -> Result<(), Fault> {
        Err(Fault {
            addr,
            write: false,
            size: 1,
        })
    }

    fn write(&mut self, addr: u64, _data: &[u8]) -> Result<(), Fault> {
        Err(Fault {
            addr,
            write: true,
            size: 1,
        })
    }
}

impl Intrinsics for Machine {
    fn call(&mut self, name: &'static str, _args: &[u128]) -> Result<u128, Error> {
        let n = self.events.len() as u64;
        let v = u128::from(self.value(0x100 + n) & 1);
        self.events.push((self.clock, Event::Call(name, v)));
        Ok(v)
    }
}

impl Devices for Machine {
    fn tick(&mut self, cycles: u64) {
        self.clock += cycles;
    }

    fn map(&self) -> &IoMap {
        &self.map
    }

    fn io_read(
        &mut self,
        _mem: &mut dyn GuestMemory,
        addr: u64,
        buf: &mut [u8],
    ) -> Result<(), Fault> {
        let v = self.value(addr) as u8;
        buf[0] = v;
        self.events.push((self.clock, Event::Read(addr, v)));
        Ok(())
    }

    fn io_write(
        &mut self,
        _mem: &mut dyn GuestMemory,
        addr: u64,
        data: &[u8],
    ) -> Result<(), Fault> {
        self.events.push((self.clock, Event::Write(addr, data[0])));
        Ok(())
    }

    fn sync(
        &mut self,
        _state: &mut State,
        _mem: &mut dyn GuestMemory,
        _next: u64,
    ) -> Result<Option<u64>, Error> {
        Ok(None)
    }
}

/// Everything a run of a block shows.
#[derive(Debug, PartialEq, Eq)]
struct Run {
    regs: Vec<u128>,
    events: Vec<(u64, Event)>,
    clock: u64,
    exit: Result<(u64, relift_ir::Flow), String>,
}

fn run(block: &Block, start: &[u128], seed: u64) -> Run {
    let mut st = State::new(start.len());
    st.regs.copy_from_slice(start);
    let mut m = Machine {
        seed,
        clock: 0,
        events: Vec::new(),
        map: devices(),
    };
    let mut hooks = Machine {
        seed: seed ^ 0x55,
        clock: 0,
        events: Vec::new(),
        map: devices(),
    };
    let out = interp::run_with(block, &mut st, &mut NoMem, &mut hooks, &mut m);
    let mut events = m.events;
    events.extend(hooks.events.into_iter().map(|(_, e)| (u64::MAX, e)));
    Run {
        regs: st.regs,
        events,
        clock: m.clock,
        exit: out.map(|o| (o.next, o.flow)).map_err(|e| e.to_string()),
    }
}

fn random_state(rng: &mut Rng) -> Vec<u128> {
    let file = regs::regs();
    (0..file.len())
        .map(|i| {
            let w = file.decl(relift_ir::Reg(i as u16)).width.bits();
            u128::from(rng.next()) & ((1u128 << w) - 1)
        })
        .collect()
}

fn bytes_of(prefix: bool, code: u8, view: u64, rng: &mut Rng) -> Vec<u8> {
    let mut v = Vec::new();
    if view & BUG != 0 {
        v.push(0x76);
    }
    if prefix {
        v.push(0xCB);
    }
    v.push(code);
    for _ in 0..3 {
        v.push(rng.next() as u8);
    }
    v
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
enum Verdict {
    Identical,
    Equivalent,
}

fn is_conditional(op: &Opcode) -> bool {
    matches!(op.mnemonic, Mnemonic::Call | Mnemonic::Ret)
        && matches!(op.dst, crate::Operand::Cond(_))
}

/// Compares one opcode in every block view it has; the weakest verdict.
fn compare(prefix: bool, code: u8, rng: &mut Rng) -> Result<Verdict, String> {
    let op = if prefix { &CB } else { &BASE }[usize::from(code)];
    let opts = Options::new(devices());
    let mut views = vec![0, BUG];
    if is_conditional(&op) {
        views.extend([HALF, HALF | BUG]);
    }
    let mut verdict = Verdict::Identical;
    for view in views {
        for _ in 0..8 {
            let bytes = bytes_of(prefix, code, view, rng);
            let pc = 0x4150 | view;
            let hand = lift_bytes(&bytes, pc, &opts);
            let sail = lift_bytes_sail(&bytes, pc, &opts);
            let (hand, sail) = match (hand, sail) {
                (Ok(h), Ok(s)) => (h, s),
                (Err(h), Err(s)) if h == s => continue,
                (h, s) => return Err(format!("view {view:#x}: hand {h:?}, sail {s:?}")),
            };
            let (h, s) = (simplify(&dce(&hand)), simplify(&dce(&sail)));
            if h.to_string() == s.to_string() {
                continue;
            }
            verdict = Verdict::Equivalent;
            for _ in 0..64 {
                let start = random_state(rng);
                let seed = rng.next();
                let (a, b) = (run(&hand, &start, seed), run(&sail, &start, seed));
                if a != b {
                    return Err(format!(
                        "view {view:#x} bytes {bytes:02x?}:\nhand {a:?}\nsail {b:?}\n--- hand\n{h}\n--- sail\n{s}"
                    ));
                }
            }
        }
    }
    Ok(verdict)
}

/// A change the comparison must notice.
#[derive(Clone, Copy, Debug)]
enum Mutation {
    /// The first arithmetic or logic operation, as `--mutate ops` in the
    /// oracle check.
    Ops,
    /// The first tick, as `--mutate cycles`.
    Cycles,
    /// The first register write goes to another register of its width.
    Sets,
    /// The exit, as `--mutate exits`.
    Exits,
}

/// Changes `block` as `m` says; false when it has nothing to change.
fn mutate(block: &mut Block, m: Mutation) -> bool {
    if let Mutation::Exits = m {
        block.exit.next = match block.exit.next {
            Next::Const(t) => Next::Const(t ^ 1),
            Next::Select {
                cond,
                taken,
                not_taken,
            } => Next::Select {
                cond,
                taken: not_taken,
                not_taken: taken,
            },
            Next::Dynamic(_) => return false,
        };
        return true;
    }
    let file = regs::regs();
    for inst in &mut block.insts {
        match (m, &inst.op) {
            (Mutation::Cycles, Op::Tick(n)) => {
                inst.op = Op::Tick(n + 4);
                return true;
            }
            (Mutation::Sets, Op::Set(r, x)) => {
                let w = file.width(*r);
                let other = (0..file.len() as u16)
                    .map(Reg)
                    .find(|o| o != r && file.width(*o) == w);
                if let Some(o) = other {
                    inst.op = Op::Set(o, *x);
                    return true;
                }
            }
            (Mutation::Ops, Op::Bin(op, a, b))
                if a != b || !matches!(op, BinOp::And | BinOp::Or) =>
            {
                let to = match op {
                    BinOp::Add => BinOp::Sub,
                    BinOp::Sub => BinOp::Add,
                    BinOp::And | BinOp::Or => BinOp::Xor,
                    BinOp::Xor => BinOp::Or,
                    BinOp::Shl => BinOp::LShr,
                    BinOp::LShr => BinOp::Shl,
                    _ => continue,
                };
                inst.op = Op::Bin(to, *a, *b);
                return true;
            }
            _ => {}
        }
    }
    false
}

/// Whether the interpreter comparison tells the generated block of an
/// opcode (after `dce`) from a mutated copy: `None` when there is nothing
/// to mutate.
fn caught(prefix: bool, code: u8, m: Mutation, rng: &mut Rng) -> Option<bool> {
    let opts = Options::new(devices());
    let bytes = bytes_of(prefix, code, 0, rng);
    let sail = dce(&lift_bytes_sail(&bytes, 0x4150, &opts).ok()?);
    let mut bad = sail.clone();
    if !mutate(&mut bad, m) {
        return None;
    }
    Some((0..256).any(|_| {
        let start = random_state(rng);
        let seed = rng.next();
        run(&sail, &start, seed) != run(&bad, &start, seed)
    }))
}

#[test]
fn the_comparison_catches_mutated_templates() {
    let mut rng = Rng(0xC0FF_EE11);
    for m in [
        Mutation::Ops,
        Mutation::Cycles,
        Mutation::Sets,
        Mutation::Exits,
    ] {
        let (mut yes, mut no, mut none) = (0, Vec::new(), 0);
        for prefix in [false, true] {
            for code in 0..=255u8 {
                let op = if prefix { &CB } else { &BASE }[usize::from(code)];
                if matches!(op.mnemonic, Mnemonic::Illegal | Mnemonic::Prefix) {
                    continue;
                }
                match caught(prefix, code, m, &mut rng) {
                    Some(true) => yes += 1,
                    Some(false) => no.push(format!("{}{code:02x}", if prefix { "cb" } else { "" })),
                    None => none += 1,
                }
            }
        }
        println!(
            "mutate {m:?}: caught {yes}, escaped {}, nothing to mutate {none}",
            no.len()
        );
        assert!(no.is_empty(), "{m:?} escaped: {no:?}");
    }
}

fn group(m: Mnemonic) -> &'static str {
    use Mnemonic as M;
    match m {
        M::Ld => "loads",
        M::Push | M::Pop => "stack",
        M::Add | M::Adc | M::Sub | M::Sbc | M::And | M::Xor | M::Or | M::Cp => "alu",
        M::Inc | M::Dec => "inc/dec",
        M::Daa | M::Cpl | M::Scf | M::Ccf => "a/flags",
        M::Rlca | M::Rrca | M::Rla | M::Rra => "rotate a",
        M::Rlc | M::Rrc | M::Rl | M::Rr | M::Sla | M::Sra | M::Swap | M::Srl => "cb shift",
        M::Bit | M::Res | M::Set => "cb bit",
        M::Jr | M::Jp | M::Call | M::Ret | M::Reti | M::Rst => "control",
        _ => "system",
    }
}

#[test]
fn generated_templates_match_the_hand_lifter() {
    let mut rng = Rng(0x5EED_5EED);
    let mut table: BTreeMap<&str, [u32; 2]> = BTreeMap::new();
    let mut failures = Vec::new();
    let mut identical = Vec::new();
    for prefix in [false, true] {
        for code in 0..=255u8 {
            let op = if prefix { &CB } else { &BASE }[usize::from(code)];
            if matches!(op.mnemonic, Mnemonic::Illegal | Mnemonic::Prefix) {
                continue;
            }
            match compare(prefix, code, &mut rng) {
                Ok(v) => {
                    table.entry(group(op.mnemonic)).or_default()[v as usize] += 1;
                    if v == Verdict::Identical {
                        identical.push(code);
                    }
                }
                Err(e) => failures.push(format!(
                    "{}{code:02x} {:?}: {e}",
                    if prefix { "cb" } else { "" },
                    op.mnemonic
                )),
            }
        }
    }
    println!("group        identical  equivalent");
    let mut total = [0u32; 2];
    for (g, [i, e]) in &table {
        println!("{g:<12} {i:>9}  {e:>10}");
        total[0] += i;
        total[1] += e;
    }
    println!("{:<12} {:>9}  {:>10}", "total", total[0], total[1]);
    assert!(failures.is_empty(), "{}", failures.join("\n\n"));
    assert_eq!(total[0] + total[1], 500);
}
