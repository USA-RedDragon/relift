//! `lift`: every lifted SM83 opcode against the oracle, from random states.
//!
//! Each case lifts the instruction with `relift-cpu-sm83`, runs the blocks in
//! the IR interpreter with a device model, and runs the same bytes and state
//! in rustyboi. Compared: registers, flags, the program counter, the interrupt
//! master enable, the T-cycle count and the RAM bytes written.
//!
//! * Control flow: jumps, calls, returns, `RST` and the conditional forms run
//!   from random flags; a conditional `CALL` or `RET` is two blocks, which the
//!   run follows.
//! * `EI` runs with a random instruction after it (the oracle takes two
//!   steps), `HALT` with and without a pending interrupt (the HALT bug runs
//!   the instruction after it from the synthetic block), `STOP` with a
//!   speed switch armed or not.
//! * Interrupt entry: after the instruction a random `IE` and `IF` are
//!   installed; when the master enable is set and a line is pending, the
//!   oracle takes the interrupt as its next step, and so does the sync point
//!   of the last block through the device model. A block with no sync point
//!   (the not-taken side of a conditional `CALL` or `RET`) takes it at the
//!   next sync point instead; those cases are compared without the entry and
//!   counted as deferred.
//!
//! The oracle cannot log device writes or time its reads, so device accesses
//! are checked in part: a device read returns the value rustyboi shows for
//! the register after a NOP from the same state, and the case is skipped as
//! volatile when that differs from the register after the instruction. A
//! device write is compared with the oracle's read back of the register for
//! the registers that hold what is written. A case that writes the HDMA
//! registers is skipped: it can start a copy into video memory that the lift
//! model does not run. Writes to the cartridge range (bank controller) are
//! not compared: the oracle cartridge has none.
//!
//! RAM writes: the oracle's are found by diffing memory around the step (see
//! `step.rs`), which misses a write of the byte already there. The
//! interpreter logs every store. Each store of the byte already there, to an
//! address the instruction did not read, is checked by running the oracle
//! again with the opposite byte at that address: the oracle must have
//! written it back.

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet};
use std::rc::Rc;

use relift_cpu_sm83::irq;
use relift_cpu_sm83::{
    AccessTarget, BASE, CB, Opcode, Options as LiftOptions, SyncPolicy, decode, lift_bytes,
    lift_bytes_sail, regs as r,
};
use relift_ir::interp::{self, Devices, Fault, FlatMemory, GuestMemory, Intrinsics, State};
use relift_ir::{AddrClass, BinOp, Block, IoMap, IoRange, Next, Op};

use super::machine::flat_rom;
use super::step::StepResult;
use super::{Hardware, Regs, Segment, Sequence, StepSpec, run_sequence};
use crate::states::Rng;

const PC: u16 = 0x0150;

/// Registers that read back exactly what was written.
const PLAIN_IO: [u16; 9] = [
    0xFF01, 0xFF06, 0xFF42, 0xFF43, 0xFF45, 0xFF47, 0xFF48, 0xFF49, 0xFF4B,
];

pub fn gb_map() -> IoMap {
    let dev = |first, last| IoRange {
        first,
        last,
        read: AddrClass::Device,
        write: AddrClass::Device,
    };
    IoMap {
        ranges: vec![
            IoRange {
                first: 0,
                last: 0x7FFF,
                read: AddrClass::Memory,
                write: AddrClass::Device,
            },
            dev(0xFF00, 0xFF7F),
            dev(0xFFFF, 0xFFFF),
        ],
        default: AddrClass::Memory,
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Mutation {
    None,
    Cycles,
    Ops,
    Exits,
}

pub struct Options {
    pub cases: u32,
    pub seed: u64,
    pub hardware: Vec<Hardware>,
    pub mutation: Mutation,
    pub only: Option<(bool, u8)>,
    pub verbose: bool,
    pub lifter: Lifter,
}

/// Which SM83 lifter the check runs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Lifter {
    /// The hand-written lifter.
    Hand,
    /// The templates generated from the Sail model.
    Sail,
}

#[derive(Clone)]
struct Case {
    hardware: Hardware,
    regs: Regs,
    code: Vec<u8>,
    segments: Vec<Segment>,
    fill: u8,
    seq: Sequence,
    early: Option<(u8, u8)>,
    armed: bool,
}

impl Case {
    fn spec(&self, code: &[u8]) -> StepSpec {
        let mut image = vec![Segment {
            addr: PC,
            bytes: code.to_vec(),
        }];
        image.extend(self.segments.iter().cloned());
        if let Some((ie, iff)) = self.early {
            image.push(Segment {
                addr: 0xFFFF,
                bytes: vec![ie],
            });
            image.push(Segment {
                addr: 0xFF0F,
                bytes: vec![iff],
            });
        }
        if self.armed {
            image.push(Segment {
                addr: 0xFF4D,
                bytes: vec![1],
            });
        }
        StepSpec {
            hardware: self.hardware,
            regs: self.regs,
            image,
            fill: self.fill,
        }
    }
}

fn zone(rng: &mut Rng, before: u16, after: u16) -> u16 {
    let pick = rng.below(100);
    let (lo, hi) = match pick {
        0..=54 => (0xC000u16, 0xDFFFu16),
        55..=79 => (0xFF80, 0xFFFE),
        80..=89 => (0xFF00, 0xFF7F),
        _ => (0x0200, 0x7FFF),
    };
    let (lo, hi) = (lo + before, hi - after);
    let edge = rng.below(8);
    if edge == 0 {
        lo
    } else if edge == 1 {
        hi
    } else {
        rng.range(i64::from(lo), i64::from(hi) + 1) as u16
    }
}

fn table_op(prefix: bool, code: u8) -> Opcode {
    if prefix {
        CB[usize::from(code)]
    } else {
        BASE[usize::from(code)]
    }
}

/// Appends the instruction `(prefix, code)` to `bytes` with random operands,
/// and sets up the registers and memory windows its accesses use.
fn append_op(
    rng: &mut Rng,
    regs: &mut Regs,
    bytes: &mut Vec<u8>,
    windows: &mut Vec<u16>,
    prefix: bool,
    code: u8,
) {
    let op = table_op(prefix, code);
    let at = bytes.len();
    if prefix {
        bytes.push(0xCB);
    }
    bytes.push(code);
    let imm_at = bytes.len();
    for _ in (imm_at - at)..usize::from(op.length) {
        bytes.push(rng.next() as u8);
    }
    let both: Vec<_> = op
        .accesses(false)
        .iter()
        .chain(op.accesses(true).iter())
        .copied()
        .collect();
    for a in both {
        match a.target {
            AccessTarget::Bc => {
                let p = zone(rng, 0, 0);
                regs.b = (p >> 8) as u8;
                regs.c = p as u8;
                windows.push(p);
            }
            AccessTarget::De => {
                let p = zone(rng, 0, 0);
                regs.d = (p >> 8) as u8;
                regs.e = p as u8;
                windows.push(p);
            }
            AccessTarget::Hl => {
                let p = zone(rng, 1, 1);
                regs.h = (p >> 8) as u8;
                regs.l = p as u8;
                windows.push(p);
            }
            AccessTarget::PopLow | AccessTarget::PopHigh => {
                regs.sp = zone(rng, 0, 1);
                windows.push(regs.sp);
            }
            AccessTarget::PushHigh | AccessTarget::PushLow => {
                regs.sp = zone(rng, 2, 0);
                windows.push(regs.sp - 2);
            }
            AccessTarget::Imm16 => {
                let span = u16::from(
                    op.accesses(false)
                        .iter()
                        .any(|x| x.target == AccessTarget::Imm16Plus1),
                );
                let p = zone(rng, 0, span);
                bytes[imm_at] = p as u8;
                bytes[imm_at + 1] = (p >> 8) as u8;
                windows.push(p);
            }
            AccessTarget::HighC
            | AccessTarget::HighImm8
            | AccessTarget::Imm16Plus1
            | AccessTarget::Pc => {}
        }
    }
    if matches!(
        op.mnemonic,
        relift_cpu_sm83::Mnemonic::Jp | relift_cpu_sm83::Mnemonic::Call
    ) && matches!(op.src, relift_cpu_sm83::Operand::Imm16)
    {
        let t = zone(rng, 0, 0);
        bytes[imm_at] = t as u8;
        bytes[imm_at + 1] = (t >> 8) as u8;
    }
}

/// Instructions that may follow `EI` or `HALT`.
const FOLLOWERS: &[(bool, u8)] = &[
    (false, 0x00),
    (false, 0xF3),
    (false, 0xFB),
    (false, 0x3C),
    (false, 0x3E),
    (false, 0x18),
    (false, 0x20),
    (false, 0xC3),
    (false, 0xC9),
    (false, 0xD9),
    (false, 0xC0),
    (false, 0xCD),
    (false, 0xC4),
    (false, 0xC5),
    (false, 0xF1),
    (false, 0x77),
    (false, 0xEA),
    (false, 0xE0),
    (false, 0xC7),
    (false, 0x76),
    (false, 0x10),
    (false, 0x27),
    (true, 0x46),
    (true, 0x86),
];

fn random_state(rng: &mut Rng, hw: Hardware) -> (Regs, u8) {
    let mut regs = Regs {
        a: rng.next() as u8,
        f: (rng.next() as u8) & 0xF0,
        b: rng.next() as u8,
        c: rng.next() as u8,
        d: rng.next() as u8,
        e: rng.next() as u8,
        h: rng.next() as u8,
        l: rng.next() as u8,
        sp: rng.next() as u16,
        pc: PC,
        ime: false,
    };
    let _ = hw;
    if rng.chance(0.2) {
        regs.a = rng.pick(&[0x00, 0x01, 0x0F, 0x10, 0x7F, 0x80, 0xFF]);
        regs.b = rng.pick(&[0x00, 0x01, 0x0F, 0x10, 0x7F, 0x80, 0xFF]);
    }
    (regs, rng.next() as u8)
}

fn irq_pair(rng: &mut Rng, pending: bool) -> (u8, u8) {
    loop {
        let ie = (rng.next() as u8) & 0x1F;
        let iff = (rng.next() as u8) & 0x1F;
        if (ie & iff != 0) == pending {
            return (ie, iff);
        }
    }
}

fn stack_zone(rng: &mut Rng) -> u16 {
    if rng.chance(0.7) {
        rng.range(0xC004, 0xDFF0) as u16
    } else {
        rng.range(0xFF84, 0xFFF0) as u16
    }
}

fn regs_stack_ok(sp: u16) -> bool {
    (0xC004..0xDFF0).contains(&sp) || (0xFF84..0xFFF0).contains(&sp)
}

fn irq_pair_p(rng: &mut Rng, p: f64) -> (u8, u8) {
    let pending = rng.chance(p);
    irq_pair(rng, pending)
}

fn make_case(rng: &mut Rng, hw: Hardware, prefix: bool, code: u8) -> Case {
    let (mut regs, fill) = random_state(rng, hw);
    let mut bytes = Vec::new();
    let mut windows = Vec::new();
    append_op(rng, &mut regs, &mut bytes, &mut windows, prefix, code);
    let m = table_op(prefix, code).mnemonic;
    let mut seq = Sequence::ONE;
    let mut early = None;
    let mut armed = false;
    match (prefix, m) {
        (false, relift_cpu_sm83::Mnemonic::Ei) => {
            let (p, c) = rng.pick(FOLLOWERS);
            append_op(rng, &mut regs, &mut bytes, &mut windows, p, c);
            seq.steps = 2;
            if rng.chance(0.5) {
                early = Some(irq_pair_p(rng, 0.6));
            }
            if rng.chance(0.5) {
                seq.late = Some(irq_pair_p(rng, 0.7));
            }
        }
        (false, relift_cpu_sm83::Mnemonic::Halt) => {
            let pending = rng.chance(0.5);
            early = Some(irq_pair(rng, pending));
            regs.ime = !pending && rng.chance(0.5);
            if pending {
                let (p, c) = rng.pick(FOLLOWERS);
                append_op(rng, &mut regs, &mut bytes, &mut windows, p, c);
                seq.steps = if (p, c) == (false, 0xFB) { 3 } else { 2 };
            }
        }
        (false, relift_cpu_sm83::Mnemonic::Stop) => {
            let pending = rng.chance(0.4);
            early = Some(irq_pair(rng, pending));
            regs.ime = !pending && rng.chance(0.5);
            armed = rng.chance(0.5);
            let pad = bytes[1];
            bytes.truncate(1);
            bytes.push(pad);
        }
        _ => {
            if rng.chance(0.4) {
                regs.ime = rng.chance(0.8);
                seq.late = Some(irq_pair_p(rng, 0.7));
            }
        }
    }
    if seq.late.is_some() || early.is_some() || seq.steps > 1 {
        if windows.is_empty() || !regs_stack_ok(regs.sp) {
            regs.sp = stack_zone(rng);
        }
        windows.push(regs.sp.wrapping_sub(2));
    }
    let mut segments = Vec::new();
    for w in windows {
        for base in [w.wrapping_sub(2), w] {
            let wram = (0xC000..0xE000).contains(&base) && base + 4 < 0xE000;
            let hram = (0xFF80..0xFFFA).contains(&base);
            if wram || hram {
                segments.push(Segment {
                    addr: base,
                    bytes: (0..4).map(|_| rng.next() as u8).collect(),
                });
            }
        }
    }
    armed &= super::machine::is_cgb(hw);
    Case {
        hardware: hw,
        regs,
        code: bytes,
        segments,
        fill,
        seq,
        early,
        armed,
    }
}

#[derive(Default)]
struct Machine {
    ime: bool,
    ie: u8,
    iff: u8,
    late: Option<(u8, u8)>,
    armed: bool,
    stopped: bool,
    halted: bool,
    switched: u32,
    dispatched: bool,
    early_dispatch: bool,
    skip: bool,
}

type Shared = Rc<RefCell<Machine>>;

struct Hooks(Shared);

impl Intrinsics for Hooks {
    fn call(&mut self, name: &'static str, _args: &[u128]) -> Result<u128, interp::Error> {
        let mut m = self.0.borrow_mut();
        match name {
            irq::EI | irq::RETI => m.ime = true,
            irq::DI => m.ime = false,
            irq::HALT => {
                let pending = m.ie & m.iff & 0x1F != 0;
                m.halted = !pending;
                return Ok(u128::from(pending));
            }
            irq::STOP => {
                if m.armed {
                    m.switched += 1;
                    return Ok(0);
                }
                m.stopped = true;
                return Ok(u128::from(m.ie & m.iff & 0x1F != 0));
            }
            _ => {
                return Err(interp::Error::UnknownIntrinsic(name));
            }
        }
        Ok(0)
    }
}

struct Dev {
    map: IoMap,
    io: BTreeMap<u16, u8>,
    clock: u64,
    writes: Vec<(u16, u8)>,
    reads: Vec<u16>,
    m: Shared,
}

impl Devices for Dev {
    fn tick(&mut self, cycles: u64) {
        self.clock += cycles;
    }

    fn map(&self) -> &IoMap {
        &self.map
    }

    fn io_read(
        &mut self,
        mem: &mut dyn GuestMemory,
        addr: u64,
        buf: &mut [u8],
    ) -> Result<(), Fault> {
        for (i, b) in buf.iter_mut().enumerate() {
            let a = (addr + i as u64) as u16;
            self.reads.push(a);
            match self.io.get(&a) {
                Some(v) => *b = *v,
                None => mem.read(u64::from(a), std::slice::from_mut(b))?,
            }
        }
        Ok(())
    }

    fn io_write(
        &mut self,
        _mem: &mut dyn GuestMemory,
        addr: u64,
        data: &[u8],
    ) -> Result<(), Fault> {
        for (i, b) in data.iter().enumerate() {
            self.writes.push(((addr + i as u64) as u16, *b));
        }
        Ok(())
    }

    fn sync(
        &mut self,
        state: &mut State,
        mem: &mut dyn GuestMemory,
        next: u64,
    ) -> Result<Option<u64>, interp::Error> {
        let mut m = self.m.borrow_mut();
        let last = next < 0x1_0000;
        let mut with_late = false;
        if last && let Some((ie, iff)) = m.late {
            m.ie = ie;
            m.iff = iff;
            m.late = None;
            with_late = true;
        }
        if !m.ime || m.stopped {
            return Ok(None);
        }
        let Some((line, vector)) = irq::take(m.ie & m.iff) else {
            return Ok(None);
        };
        let sp = state.regs[r::SP.index()] as u16;
        if !is_ram(sp.wrapping_sub(2))
            || !is_ram(sp.wrapping_sub(1))
            || (0xE000..0xFE00).contains(&sp.wrapping_sub(2))
        {
            m.skip = true;
            return Ok(None);
        }
        m.ime = false;
        m.iff &= !(1 << line);
        m.dispatched = true;
        m.early_dispatch = !with_late;
        let sp = (state.regs[r::SP.index()] as u16).wrapping_sub(2);
        state.regs[r::SP.index()] = u128::from(sp);
        let ret = next as u16;
        mem.write(u64::from(sp), &[ret as u8, (ret >> 8) as u8])
            .map_err(interp::Error::Fault)?;
        self.clock += u64::from(irq::ENTRY_CYCLES);
        Ok(Some(u64::from(vector)))
    }
}

fn fold_echo(addr: u64) -> u64 {
    if (0xE000..0xFE00).contains(&(addr & 0xFFFF)) {
        addr - 0x2000
    } else {
        addr
    }
}

struct Log {
    mem: FlatMemory,
    stores: Vec<(u16, u8)>,
    loads: BTreeSet<u16>,
}

impl GuestMemory for Log {
    fn read(&mut self, addr: u64, buf: &mut [u8]) -> Result<(), Fault> {
        for (i, b) in buf.iter_mut().enumerate() {
            let at = fold_echo(addr + i as u64);
            self.loads.insert(at as u16);
            self.mem.read(at, std::slice::from_mut(b))?;
        }
        Ok(())
    }

    fn write(&mut self, addr: u64, data: &[u8]) -> Result<(), Fault> {
        for (i, b) in data.iter().enumerate() {
            let at = fold_echo(addr + i as u64);
            self.stores.push((at as u16, *b));
            self.mem.write(at, &[*b])?;
        }
        Ok(())
    }
}

struct Ir {
    regs: Regs,
    cycles: u64,
    stores: Vec<(u16, u8)>,
    loads: BTreeSet<u16>,
    initial: Vec<u8>,
    device_writes: Vec<(u16, u8)>,
    device_reads: Vec<u16>,
    machine: Machine,
    synced: bool,
    blocks: usize,
}

fn is_ram(a: u16) -> bool {
    matches!(a, 0x8000..=0xDFFF | 0xFF80..=0xFFFE)
}

fn run_ir(
    case: &Case,
    pre_io: &[(u16, u8)],
    opts: &LiftOptions,
    mutation: Mutation,
    mutated: &mut bool,
    lifter: Lifter,
) -> Result<Ir, String> {
    let rom = flat_rom(case.hardware, &{
        let mut v = vec![0u8; usize::from(PC)];
        v.extend(&case.code);
        v
    });
    let mut bytes = vec![case.fill; 0x10000];
    bytes[..rom.len()].copy_from_slice(&rom);
    for s in &case.segments {
        for (i, b) in s.bytes.iter().enumerate() {
            let a = usize::from(s.addr) + i;
            bytes[a] = *b;
        }
    }
    let initial = bytes.clone();
    let mut mem = Log {
        mem: FlatMemory { base: 0, bytes },
        stores: Vec::new(),
        loads: BTreeSet::new(),
    };
    let mut st = State::new(r::regs().len());
    let g = &case.regs;
    for (reg, v) in [
        (r::A, g.a),
        (r::B, g.b),
        (r::C, g.c),
        (r::D, g.d),
        (r::E, g.e),
        (r::H, g.h),
        (r::L, g.l),
    ] {
        st.regs[reg.index()] = u128::from(v);
    }
    st.regs[r::SP.index()] = u128::from(g.sp);
    for (reg, bit) in [(r::FZ, 7), (r::FN, 6), (r::FH, 5), (r::FC, 4)] {
        st.regs[reg.index()] = u128::from((g.f >> bit) & 1);
    }
    let (ie, iff) = case.early.unwrap_or((0, 0));
    let shared: Shared = Rc::new(RefCell::new(Machine {
        ime: g.ime,
        ie,
        iff,
        late: case.seq.late,
        armed: case.armed,
        ..Machine::default()
    }));
    let mut hooks = Hooks(shared.clone());
    let mut dev = Dev {
        map: gb_map(),
        io: pre_io.iter().copied().collect(),
        clock: 0,
        writes: Vec::new(),
        reads: Vec::new(),
        m: shared.clone(),
    };
    let mut pc = u64::from(PC);
    let mut blocks = 0usize;
    let mut synced;
    let mut next_pc;
    loop {
        blocks += 1;
        if blocks > 6 {
            return Err(
                "skip: the HALT bug runs HALT again, as it does in the oracle until a pin goes low"
                    .into(),
            );
        }
        let low = (pc & 0xFFFF) as usize;
        let window = &mem.mem.bytes[low..(low + 4).min(0x10000)].to_vec();
        let lift = match lifter {
            Lifter::Hand => lift_bytes,
            Lifter::Sail => lift_bytes_sail,
        };
        let mut block = lift(window, pc, opts).map_err(|e| format!("lift {pc:#x}: {e}"))?;
        if mutation != Mutation::None && !*mutated {
            *mutated = mutate(&mut block, mutation);
        }
        synced = matches!(block.insts.last(), Some(i) if i.op == Op::Sync);
        let out = interp::run_with(&block, &mut st, &mut mem, &mut hooks, &mut dev)
            .map_err(|e| format!("interpreter: {e}"))?;
        next_pc = out.next;
        if out.resume.is_some() || out.next < 0x1_0000 {
            break;
        }
        pc = out.next;
    }
    let mut m = std::mem::take(&mut *shared.borrow_mut());
    if let Some((ie, iff)) = m.late.take() {
        m.ie = ie;
        m.iff = iff;
    }
    let get = |reg: relift_ir::Reg| st.regs[reg.index()] as u8;
    let flag = |reg: relift_ir::Reg, bit: u8| get(reg) << bit;
    let regs = Regs {
        a: get(r::A),
        f: flag(r::FZ, 7) | flag(r::FN, 6) | flag(r::FH, 5) | flag(r::FC, 4),
        b: get(r::B),
        c: get(r::C),
        d: get(r::D),
        e: get(r::E),
        h: get(r::H),
        l: get(r::L),
        sp: st.regs[r::SP.index()] as u16,
        pc: next_pc as u16,
        ime: m.ime,
    };
    Ok(Ir {
        regs,
        cycles: dev.clock,
        stores: mem.stores,
        loads: mem.loads,
        initial,
        device_writes: dev.writes,
        device_reads: dev.reads,
        machine: m,
        synced,
        blocks,
    })
}

enum Verdict {
    Pass,
    Skip,
    Deferred,
    Fail(String),
}

fn compare(
    case: &Case,
    opts: &LiftOptions,
    mutation: Mutation,
    lifter: Lifter,
) -> Result<(Verdict, bool), String> {
    let nop = run_sequence(&case.spec(&[0x00]), Sequence::ONE).map_err(|e| e.to_string())?;
    let mut mutated = false;
    let ir = match run_ir(case, &nop.io, opts, mutation, &mut mutated, lifter) {
        Err(e) if e.starts_with("skip:") || e.contains("is not lifted") => {
            return Ok((Verdict::Skip, mutated));
        }
        r => r?,
    };
    if ir.machine.skip {
        return Ok((Verdict::Skip, mutated));
    }
    let mut seq = case.seq;
    if ir.machine.stopped {
        seq.late = None;
    }
    let mut deferred = false;
    if ir.machine.dispatched && (seq.late.is_none() || ir.machine.early_dispatch) {
        seq.steps += 1;
    }
    if let Some((ie, iff)) = seq.late
        && !ir.synced
        && !ir.machine.dispatched
        && ir.machine.ime
        && ie & iff & 0x1F != 0
    {
        seq.late = None;
        deferred = true;
    }
    let oracle: StepResult =
        run_sequence(&case.spec(&case.code), seq).map_err(|e| e.to_string())?;
    let wake = if ir.machine.halted { oracle.wake } else { 0 };
    let ours = &ir.regs;
    let theirs = &oracle.regs;
    let mut diffs = Vec::new();
    for (name, a, b) in [
        ("a", u32::from(ours.a), u32::from(theirs.a)),
        ("f", u32::from(ours.f), u32::from(theirs.f & 0xF0)),
        ("b", u32::from(ours.b), u32::from(theirs.b)),
        ("c", u32::from(ours.c), u32::from(theirs.c)),
        ("d", u32::from(ours.d), u32::from(theirs.d)),
        ("e", u32::from(ours.e), u32::from(theirs.e)),
        ("h", u32::from(ours.h), u32::from(theirs.h)),
        ("l", u32::from(ours.l), u32::from(theirs.l)),
        ("sp", u32::from(ours.sp), u32::from(theirs.sp)),
        ("pc", u32::from(ours.pc), u32::from(theirs.pc)),
        ("ime", u32::from(ours.ime), u32::from(theirs.ime)),
        ("cycles", ir.cycles as u32, oracle.cycles - wake),
    ] {
        if a != b {
            diffs.push(format!("{name}: ir {a:#x} oracle {b:#x}"));
        }
    }
    let mut last: BTreeMap<u16, u8> = BTreeMap::new();
    for (a, v) in &ir.stores {
        if is_ram(*a) {
            last.insert(*a, *v);
        }
    }
    let ir_ram: BTreeMap<u16, u8> = last
        .iter()
        .filter(|(a, v)| **v != ir.initial[usize::from(**a)])
        .map(|(a, v)| (*a, *v))
        .collect();
    let want: BTreeMap<u16, u8> = oracle
        .writes
        .iter()
        .filter(|w| w.value != ir.initial[usize::from(w.addr)])
        .map(|w| (w.addr, w.value))
        .collect();
    if want != ir_ram {
        diffs.push(format!("ram writes: ir {ir_ram:x?} oracle {want:x?}"));
    }
    for (a, v) in &last {
        if *v != ir.initial[usize::from(*a)]
            || ir.loads.contains(a)
            || !(0xC000..0xE000).contains(a)
        {
            continue;
        }
        let mut spec = case.spec(&case.code);
        spec.image.push(Segment {
            addr: *a,
            bytes: vec![!*v],
        });
        let flipped = run_sequence(&spec, seq).map_err(|e| e.to_string())?;
        if !flipped.writes.iter().any(|w| w.addr == *a && w.value == *v) {
            diffs.push(format!(
                "ir stores {v:02x} at {a:04x}, the byte already there; the oracle does not"
            ));
        }
    }
    let io_after: BTreeMap<u16, u8> = oracle.io.iter().copied().collect();
    let io_before: BTreeMap<u16, u8> = nop.io.iter().copied().collect();
    if ir.machine.dispatched {
        let want_if = io_after.get(&0xFF0F).copied().unwrap_or(0) & 0x1F;
        if want_if != ir.machine.iff & 0x1F {
            diffs.push(format!(
                "IF: ir {:#x} oracle {:#x}",
                ir.machine.iff & 0x1F,
                want_if
            ));
        }
    }
    let written: Vec<u16> = ir.device_writes.iter().map(|w| w.0).collect();
    if written.iter().any(|a| (0xFF51..=0xFF55).contains(a)) {
        return Ok((Verdict::Skip, mutated));
    }
    for (a, v) in &ir.device_writes {
        if PLAIN_IO.contains(a) && io_after.get(a) != Some(v) {
            diffs.push(format!(
                "io write {a:04x}={v:02x}, oracle reads back {:02x?}",
                io_after.get(a)
            ));
        }
    }
    for a in &ir.device_reads {
        let moved = io_before.get(a) != io_after.get(a) || written.contains(a);
        if moved {
            return Ok((Verdict::Skip, mutated));
        }
    }
    let _ = ir.blocks;
    let v = if !diffs.is_empty() {
        Verdict::Fail(diffs.join("; "))
    } else if deferred {
        Verdict::Deferred
    } else {
        Verdict::Pass
    };
    Ok((v, mutated))
}

fn mutate(block: &mut Block, mutation: Mutation) -> bool {
    if mutation == Mutation::Exits {
        let to = match block.exit.next {
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
        block.exit.next = to;
        return true;
    }
    for inst in &mut block.insts {
        match (mutation, &inst.op) {
            (Mutation::Cycles, Op::Tick(n)) => {
                inst.op = Op::Tick(n + 4);
                return true;
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

struct Tally {
    pass: u32,
    skip: u32,
    deferred: u32,
    fail: u32,
    first: Option<String>,
}

pub fn run_lift(opts: &Options) -> Result<bool, String> {
    let lift_opts = LiftOptions {
        map: gb_map(),
        sync: SyncPolicy::Every,
        live_operands: false,
    };
    let mut ok = true;
    let (mut lifted, mut passed, mut skipped, mut deferred, mut failed) = (0u32, 0, 0, 0, 0);
    let (mut caught, mut escaped, mut unmutable) = (0u32, 0u32, 0u32);
    for prefix in [false, true] {
        for code in 0..=255u8 {
            if let Some(o) = opts.only
                && o != (prefix, code)
            {
                continue;
            }
            if !prefix && code == 0xCB {
                continue;
            }
            if table_op(prefix, code).is_illegal() {
                continue;
            }
            lifted += 1;
            let mut t = Tally {
                pass: 0,
                skip: 0,
                deferred: 0,
                fail: 0,
                first: None,
            };
            let mut rng = Rng::new(&format!("gb-lift-{prefix}-{code}"), opts.seed);
            let mut detected = 0u32;
            let mut mutated_any = false;
            for i in 0..opts.cases {
                let hw = opts.hardware[i as usize % opts.hardware.len()];
                let case = make_case(&mut rng, hw, prefix, code);
                decode(&case.code).map_err(|e| e.to_string())?;
                let (verdict, mutated) = compare(&case, &lift_opts, opts.mutation, opts.lifter)?;
                mutated_any |= mutated;
                if opts.mutation != Mutation::None && !mutated {
                    break;
                }
                match verdict {
                    Verdict::Pass => t.pass += 1,
                    Verdict::Skip => t.skip += 1,
                    Verdict::Deferred => t.deferred += 1,
                    Verdict::Fail(why) => {
                        t.fail += 1;
                        detected += 1;
                        t.first.get_or_insert_with(|| {
                            format!(
                                "hw {} regs {:?} code {:02x?} seq {:?} early {:?} armed {}: {why}",
                                super::machine::hardware_name(case.hardware),
                                case.regs,
                                case.code,
                                case.seq,
                                case.early,
                                case.armed
                            )
                        });
                    }
                }
            }
            let name = format!("{}{code:02x}", if prefix { "cb " } else { "   " });
            if opts.mutation == Mutation::None {
                passed += t.pass;
                skipped += t.skip;
                deferred += t.deferred;
                failed += t.fail;
                if t.fail > 0 {
                    ok = false;
                    println!(
                        "FAIL {name}: {}/{} failed; first: {}",
                        t.fail,
                        opts.cases,
                        t.first.unwrap_or_default()
                    );
                } else if opts.verbose {
                    println!(
                        "{name}: pass {} skip {} deferred {}",
                        t.pass, t.skip, t.deferred
                    );
                }
            } else if !mutated_any {
                unmutable += 1;
                if opts.verbose {
                    println!("{name}: nothing to mutate");
                }
            } else if detected > 0 {
                caught += 1;
            } else {
                escaped += 1;
                ok = false;
                println!(
                    "ESCAPED {name}: mutation {:?} not caught in {} cases",
                    opts.mutation, opts.cases
                );
            }
        }
    }
    if opts.mutation == Mutation::None {
        println!(
            "opcodes lifted {lifted}; cases pass {passed} skip {skipped} deferred {deferred} fail {failed}"
        );
    } else {
        println!(
            "mutation {:?}: opcodes {lifted}, caught {caught}, escaped {escaped}, nothing to mutate {unmutable}",
            opts.mutation
        );
    }
    Ok(ok)
}
