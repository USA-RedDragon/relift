//! `lift`: every lifted SM83 opcode against the oracle, from random states.
//!
//! Each case lifts the instruction with `relift-cpu-sm83`, runs the block in
//! the IR interpreter with a device model, and runs the same bytes and state
//! in rustyboi (`run_step`). Compared: registers, flags, the program counter,
//! the T-cycle count and the RAM bytes written.
//!
//! The oracle cannot log device writes or time its reads, so device accesses
//! are checked in part: a device read returns the value rustyboi shows for
//! the register after a NOP from the same state, and the case is skipped as
//! volatile when that differs from the register after the instruction. A
//! Writes of the byte already there are left out on both sides, because the
//! oracle finds writes by diffing memory. A device write is compared with the oracle's read back of the register for
//! the registers that hold what is written. Writes to the cartridge range
//! (bank controller) are not compared: the oracle cartridge has none.

use std::collections::BTreeMap;

use relift_cpu_sm83::{AccessTarget, BASE, CB, Opcode, Sm83, decode, lift_instruction, regs as r};
use relift_guest::Frontend;
use relift_ir::interp::{self, Devices, Fault, FlatMemory, GuestMemory, NoIntrinsics, State};
use relift_ir::{AddrClass, BinOp, Block, IoMap, IoRange, Op};

use super::machine::flat_rom;
use super::step::StepResult;
use super::{Hardware, Regs, Segment, StepSpec, run_step};
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
}

pub struct Options {
    pub cases: u32,
    pub seed: u64,
    pub hardware: Vec<Hardware>,
    pub mutation: Mutation,
    pub only: Option<(bool, u8)>,
    pub verbose: bool,
}

struct Case {
    hardware: Hardware,
    regs: Regs,
    code: Vec<u8>,
    segments: Vec<Segment>,
    fill: u8,
}

impl Case {
    fn spec(&self, code: &[u8]) -> StepSpec {
        let mut image = vec![Segment {
            addr: PC,
            bytes: code.to_vec(),
        }];
        image.extend(self.segments.iter().cloned());
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

fn make_case(rng: &mut Rng, hw: Hardware, prefix: bool, code: u8) -> (Case, Opcode) {
    let op = if prefix {
        CB[usize::from(code)]
    } else {
        BASE[usize::from(code)]
    };
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
    if rng.chance(0.2) {
        regs.a = rng.pick(&[0x00, 0x01, 0x0F, 0x10, 0x7F, 0x80, 0xFF]);
        regs.b = rng.pick(&[0x00, 0x01, 0x0F, 0x10, 0x7F, 0x80, 0xFF]);
    }
    let mut bytes: Vec<u8> = if prefix { vec![0xCB, code] } else { vec![code] };
    let imm_at = bytes.len();
    for _ in imm_at..usize::from(op.length) {
        bytes.push(rng.next() as u8);
    }
    let mut windows: Vec<u16> = Vec::new();
    let imm16 = |rng: &mut Rng, bytes: &mut Vec<u8>, windows: &mut Vec<u16>, span| {
        let p = zone(rng, 0, span);
        bytes[imm_at] = p as u8;
        bytes[imm_at + 1] = (p >> 8) as u8;
        windows.push(p);
    };
    for a in op.accesses(false) {
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
                imm16(rng, &mut bytes, &mut windows, span);
            }
            AccessTarget::HighC
            | AccessTarget::HighImm8
            | AccessTarget::Imm16Plus1
            | AccessTarget::Pc => {}
        }
    }
    let fill = rng.next() as u8;
    let mut segments = Vec::new();
    for w in windows {
        for base in [w.wrapping_sub(2), w] {
            let wram = (0xC000..0xE000).contains(&base) && base + 4 < 0xE000;
            let hram = (0xFF80..0xFFFA).contains(&base);
            if wram || hram {
                let n = 4;
                segments.push(Segment {
                    addr: base,
                    bytes: (0..n).map(|_| rng.next() as u8).collect(),
                });
            }
        }
    }
    (
        Case {
            hardware: hw,
            regs,
            code: bytes,
            segments,
            fill,
        },
        op,
    )
}

struct Dev {
    map: IoMap,
    io: BTreeMap<u16, u8>,
    clock: u64,
    writes: Vec<(u16, u8)>,
    reads: Vec<u16>,
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
        _state: &mut State,
        _mem: &mut dyn GuestMemory,
        _next: u64,
    ) -> Result<Option<u64>, interp::Error> {
        Ok(None)
    }
}

struct Log {
    mem: FlatMemory,
    writes: BTreeMap<u16, u8>,
}

impl GuestMemory for Log {
    fn read(&mut self, addr: u64, buf: &mut [u8]) -> Result<(), Fault> {
        self.mem.read(addr, buf)
    }

    fn write(&mut self, addr: u64, data: &[u8]) -> Result<(), Fault> {
        for (i, b) in data.iter().enumerate() {
            self.writes.insert((addr + i as u64) as u16, *b);
        }
        self.mem.write(addr, data)
    }
}

struct Ir {
    regs: Regs,
    cycles: u64,
    ram: BTreeMap<u16, u8>,
    initial: Vec<u8>,
    device_writes: Vec<(u16, u8)>,
    device_reads: Vec<u16>,
}

fn is_ram(a: u16) -> bool {
    matches!(a, 0x8000..=0xDFFF | 0xFF80..=0xFFFE)
}

fn run_ir(block: &Block, case: &Case, pre_io: &[(u16, u8)]) -> Result<Ir, String> {
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
        writes: BTreeMap::new(),
    };
    let mut st = State::new(block.regs.len());
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
    let mut dev = Dev {
        map: gb_map(),
        io: pre_io.iter().copied().collect(),
        clock: 0,
        writes: Vec::new(),
        reads: Vec::new(),
    };
    let out = interp::run_with(block, &mut st, &mut mem, &mut NoIntrinsics, &mut dev)
        .map_err(|e| format!("interpreter: {e}"))?;
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
        pc: out.next as u16,
        ime: false,
    };
    let ram = mem
        .writes
        .iter()
        .filter(|(a, v)| is_ram(**a) && **v != initial[usize::from(**a)])
        .map(|(a, v)| (*a, *v))
        .collect();
    Ok(Ir {
        regs,
        cycles: dev.clock,
        ram,
        initial,
        device_writes: dev.writes,
        device_reads: dev.reads,
    })
}

enum Verdict {
    Pass,
    Skip,
    Fail(String),
}

fn compare(block: &Block, case: &Case) -> Result<Verdict, String> {
    let oracle: StepResult = run_step(&case.spec(&case.code)).map_err(|e| e.to_string())?;
    let nop = run_step(&case.spec(&[0x00])).map_err(|e| e.to_string())?;
    let ir = run_ir(block, case, &nop.io)?;
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
        ("cycles", ir.cycles as u32, oracle.cycles),
    ] {
        if a != b {
            diffs.push(format!("{name}: ir {a:#x} oracle {b:#x}"));
        }
    }
    let want: BTreeMap<u16, u8> = oracle
        .writes
        .iter()
        .filter(|w| w.value != ir.initial[usize::from(w.addr)])
        .map(|w| (w.addr, w.value))
        .collect();
    if want != ir.ram {
        diffs.push(format!("ram writes: ir {:x?} oracle {:x?}", ir.ram, want));
    }
    let io_after: BTreeMap<u16, u8> = oracle.io.iter().copied().collect();
    let io_before: BTreeMap<u16, u8> = nop.io.iter().copied().collect();
    let written: Vec<u16> = ir.device_writes.iter().map(|w| w.0).collect();
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
            return Ok(Verdict::Skip);
        }
    }
    Ok(if diffs.is_empty() {
        Verdict::Pass
    } else {
        Verdict::Fail(diffs.join("; "))
    })
}

fn mutate(block: &mut Block, mutation: Mutation) -> bool {
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
    fail: u32,
    first: Option<String>,
}

pub fn run_lift(opts: &Options) -> Result<bool, String> {
    let fe = Sm83::new(gb_map());
    let mut ok = true;
    let (mut lifted, mut passed, mut skipped, mut failed, mut caught, mut escaped, mut unmutable) =
        (0u32, 0u32, 0u32, 0u32, 0u32, 0u32, 0u32);
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
            let probe = {
                let mut b = if prefix { vec![0xCB, code] } else { vec![code] };
                b.resize(3, 0);
                b
            };
            if fe.lift(&probe, u64::from(PC)).is_err() {
                continue;
            }
            lifted += 1;
            let mut t = Tally {
                pass: 0,
                skip: 0,
                fail: 0,
                first: None,
            };
            let mut rng = Rng::new(&format!("gb-lift-{prefix}-{code}"), opts.seed);
            let mut detected = 0u32;
            let mut mutated = false;
            for i in 0..opts.cases {
                let hw = opts.hardware[i as usize % opts.hardware.len()];
                let (case, op) = make_case(&mut rng, hw, prefix, code);
                let ins = decode(&case.code).map_err(|e| e.to_string())?;
                debug_assert_eq!(ins.opcode.code, op.code);
                let mut block = lift_instruction(&ins, PC, &gb_map()).map_err(|e| e.to_string())?;
                if opts.mutation != Mutation::None {
                    mutated = mutate(&mut block, opts.mutation);
                    if !mutated {
                        break;
                    }
                }
                match compare(&block, &case)? {
                    Verdict::Pass => t.pass += 1,
                    Verdict::Skip => t.skip += 1,
                    Verdict::Fail(why) => {
                        t.fail += 1;
                        detected += 1;
                        t.first.get_or_insert_with(|| {
                            format!(
                                "hw {} regs {:?} code {:02x?}: {why}",
                                super::machine::hardware_name(case.hardware),
                                case.regs,
                                case.code
                            )
                        });
                    }
                }
            }
            let name = format!("{}{code:02x}", if prefix { "cb " } else { "   " });
            if opts.mutation == Mutation::None {
                passed += t.pass;
                skipped += t.skip;
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
                    println!("{name}: pass {} skip {}", t.pass, t.skip);
                }
            } else if !mutated {
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
        println!("opcodes lifted {lifted}; cases pass {passed} skip {skipped} fail {failed}");
    } else {
        println!(
            "mutation {:?}: opcodes {lifted}, caught {caught}, escaped {escaped}, nothing to mutate {unmutable}",
            opts.mutation
        );
    }
    Ok(ok)
}
