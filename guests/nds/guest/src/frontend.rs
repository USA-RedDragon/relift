//! The frontend of the two Nintendo DS CPUs.
//!
//! Lifting is `relift-cpu-arm32`'s, the ARM9 (ARM946E-S) with
//! [`Profile::V5te`] and the ARM7 (ARM7TDMI) with [`Profile::V4t`], reshaped
//! for the runtime's NDS layer (`runtime/guests/nds/run`):
//!
//! - every load and store goes to the machine's bus (`rl_nds_cpu_read`,
//!   `rl_nds_cpu_write`), which charges the access and, for I/O, shared work
//!   RAM, main RAM and video RAM, first waits until the other CPU is past it
//!   in the access order; the ARM9's TCMs, the ARM7's own work RAM and the
//!   BIOS regions are private and do not wait;
//! - an instruction's code fetch, ahead of its condition check, and a taken
//!   branch's refill cost what the runtime's table for the CPU and region
//!   says (`rl_arm32_fetch`, `rl_arm32_refill`), and its internal cycles
//!   are ticks of the block; code fetches never wait for the other CPU;
//! - MCR and MRC reach the ARM9's CP15 registers (`rl_nds_cpu_cp_write`,
//!   `rl_nds_cpu_cp_read`), which move the TCMs, set the vector base and
//!   wait for interrupts;
//! - SWI is a trap into the high-level BIOS (`rl_nds_swi_trap`), which goes
//!   on at the next instruction;
//! - the second half of a Thumb `BL` or `BLX` pair calls the constant its
//!   first half puts in LR;
//! - the first instruction of a function checks, before any effect, that
//!   the bytes the CPU fetches are the ones it was lifted from
//!   (`rl_nds_cpu_enter`, given the unit and the function); when they are
//!   not, the function leaves for the lifted function the runtime finds for
//!   those bytes (`rl_nds_cpu_variant`);
//! - a write of the I bit tells the runtime (`rl_nds_cpu_set_i`), which
//!   ends the budget when it unmasks a pending interrupt;
//! - every instruction ends in a sync point.

use std::collections::BTreeSet;

use relift_cpu_arm32::decode::Profile;
use relift_cpu_arm32::lift::{self, Bus, COND_FAIL, LiftError};
use relift_cpu_arm32::regs;
use relift_guest::{Frontend, Home, LiftFailure};
use relift_ir::{
    AddrClass, Block, CryptoOp, Exit, Flow, FpOp, Inst, IoKind, IoMap, Konst, Next, Op, Reg,
    RegFile, Value, Width,
};
use relift_nds_formats::Cpu;

/// Bit of a SWI trap code set for a SWI in Thumb state; bits 0 to 7 hold
/// the comment byte (`RL_NDS_SWI_THUMB`).
pub const SWI_THUMB: u32 = 0x100;

/// Set in the raw instruction unit of a Thumb instruction, above its
/// halfword, so the slow paths know the state.
pub const RAW_THUMB: u64 = 1 << 32;

/// The intrinsic of the fetch of the next ARM instruction: its address.
pub const FETCH_ARM: &str = "arm32.fetch_arm";
/// The intrinsic of the fetch of the next Thumb instruction.
pub const FETCH_THUMB: &str = "arm32.fetch_thumb";
/// The intrinsic of a function's entry check: unit and function index; 1
/// when the CPU fetches other bytes there.
pub const ENTER: &str = "nds.enter";
/// The guard code of a function whose bytes are not the ones it was
/// lifted from.
pub const CODE_CHANGED: u32 = 0x434f_4445;
/// The intrinsic of a branch's pipeline refill: target, Thumb, taken.
pub const REFILL: &str = "arm32.refill";
/// The intrinsic of a call's host frame: the return address.
pub const CALL: &str = "arm32.call";
/// The lifter's intrinsic of MCR: register key, value, condition.
pub const CP_WRITE: &str = "arm.cp_write";
/// The lifter's intrinsic of MRC: register key.
pub const CP_READ: &str = "arm.cp_read";
/// The intrinsic before the second word of the ARM9's LDRD, which the bus
/// takes as a new transfer (`RL_NDS_PAIR`).
pub const PAIR: &str = "nds.pair";
/// The intrinsic of a write of the I bit: its new value (an interrupt
/// pending under the mask is taken at the sync point once it clears).
pub const SET_I: &str = "nds.set_i";

/// The function a lifted entry instruction belongs to: the program's unit
/// index and the function's index in that unit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Entry {
    /// The unit.
    pub unit: u32,
    /// The function in the unit.
    pub function: u32,
}

/// The address map: every access reaches the machine's bus.
#[must_use]
pub fn device_map() -> IoMap {
    IoMap {
        ranges: Vec::new(),
        default: AddrClass::Device,
    }
}

/// The profile a CPU is lifted for.
#[must_use]
pub fn profile(cpu: Cpu) -> Profile {
    match cpu {
        Cpu::Arm9 => Profile::V5te,
        Cpu::Arm7 => Profile::V4t,
    }
}

/// The frontend of one CPU.
#[derive(Clone, Debug)]
pub struct Nds {
    cpu: Cpu,
    /// The computed jumps that are calls (LR holds the next instruction).
    calls: BTreeSet<u32>,
}

fn failure(word: u32, e: &LiftError) -> LiftFailure {
    LiftFailure {
        insn: u64::from(word),
        message: e.to_string(),
    }
}

/// The instruction unit at `pc` in `image`, which starts at `base`.
fn unit(image: &[u8], base: u32, pc: u32, thumb: bool) -> Option<u32> {
    let off = usize::try_from(pc.checked_sub(base)?).ok()?;
    let n = if thumb { 2 } else { 4 };
    let b = image.get(off..off + n)?;
    Some(if thumb {
        u32::from(u16::from_le_bytes([b[0], b[1]]))
    } else {
        u32::from_le_bytes([b[0], b[1], b[2], b[3]])
    })
}

/// The target of the Thumb pair whose second half is `lo` at `pc` and first
/// half `hi` at `pc - 2`, and whether it is in Thumb state: `BL` stays in
/// Thumb, ARMv5's `BLX` (with `v5`) goes to the word-aligned ARM target.
/// `None` when the halves are no pair.
#[must_use]
pub fn pair_target(hi: u32, lo: u32, pc: u32, v5: bool) -> Option<(u32, bool)> {
    if hi & 0xF800 != 0xF000 {
        return None;
    }
    let thumb = match lo & 0xF800 {
        0xF800 => true,
        0xE800 if v5 && lo & 1 == 0 => false,
        _ => return None,
    };
    let off = ((hi & 0x7FF) << 21) as i32 >> 9;
    let lr = pc.wrapping_add(2).wrapping_add(off as u32);
    let t = lr.wrapping_add((lo & 0x7FF) << 1);
    Some(if thumb {
        (t & !1, true)
    } else {
        (t & !3, false)
    })
}

fn push(b: &mut Block, op: Op, width: Option<Width>) -> Value {
    b.insts.push(Inst { op, width });
    Value((b.insts.len() - 1) as u32)
}

fn konst(b: &mut Block, w: Width, v: u64) -> Value {
    push(b, Op::Const(Konst::new(u128::from(v))), Some(w))
}

fn is_const(b: &Block, v: Value, want: u64) -> bool {
    matches!(b.insts[v.index()].op, Op::Const(k) if k.get() == u128::from(want))
}

fn last_set(b: &Block, r: Reg) -> Option<Value> {
    b.insts.iter().rev().find_map(|i| match i.op {
        Op::Set(x, v) if x == r => Some(v),
        _ => None,
    })
}

impl Nds {
    /// The frontend of `cpu`.
    #[must_use]
    pub fn new(cpu: Cpu) -> Nds {
        Nds::with_calls(cpu, BTreeSet::new())
    }

    /// The frontend of `cpu`, with the computed jumps at `calls` lifted as
    /// calls.
    #[must_use]
    pub fn with_calls(cpu: Cpu, calls: BTreeSet<u32>) -> Nds {
        Nds { cpu, calls }
    }

    /// The CPU.
    #[must_use]
    pub fn cpu(&self) -> Cpu {
        self.cpu
    }

    /// Lifts the instruction at `pc` in Thumb (`thumb`) or ARM state from
    /// `image`, loaded at `base`, for the NDS layer; `entry` names the
    /// function when the instruction is its entry, which then checks the
    /// function's bytes first.
    ///
    /// # Errors
    ///
    /// The address is outside the image, or the instruction does not lift
    /// on the CPU's profile.
    pub fn lift_at(
        &self,
        image: &[u8],
        base: u32,
        pc: u32,
        thumb: bool,
        entry: Option<Entry>,
    ) -> Result<Block, LiftFailure> {
        let word = unit(image, base, pc, thumb).ok_or_else(|| LiftFailure {
            insn: 0,
            message: format!("{pc:#010x} is outside the image"),
        })?;
        let profile = profile(self.cpu);
        let l = lift::lift_for(word, pc, thumb, profile).map_err(|e| failure(word, &e))?;
        let mut b = l.block;
        let len = if thumb { 2u64 } else { 4 };
        let fall = u64::from(pc) + len;
        if thumb
            && b.exit.flow == (Flow::Call { ret: fall })
            && let Next::Dynamic(_) = b.exit.next
            && let Some(hi) = pc.checked_sub(2).and_then(|p| unit(image, base, p, true))
            && let Some((t, _)) = pair_target(hi, word, pc, profile == Profile::V5te)
        {
            b.exit.next = Next::Const(u64::from(t));
        }
        if b.exit.flow == Flow::Return {
            b.exit.flow = Flow::Jump;
        }
        if b.exit.flow == Flow::Jump
            && matches!(b.exit.next, Next::Dynamic(_))
            && self.calls.contains(&pc)
        {
            b.exit.flow = Flow::Call { ret: fall };
        }
        let swi = b.exit.flow == (Flow::Call { ret: fall }) && b.exit.next == Next::Const(0x08);
        if swi {
            let number = if thumb {
                word & 0xFF
            } else {
                (word >> 16) & 0xFF
            };
            b.exit = Exit {
                next: Next::Const(fall),
                flow: Flow::Trap(number | if thumb { SWI_THUMB } else { 0 }),
            };
        }
        let mut internal: u32 = l
            .bus
            .iter()
            .map(|e| match e {
                Bus::I(n) => *n,
                _ => 0,
            })
            .sum();
        if self.cpu == Cpu::Arm9 {
            let falls = match b.exit.next {
                Next::Const(t) => t == fall,
                Next::Select {
                    taken, not_taken, ..
                } => taken == fall || not_taken == fall,
                Next::Dynamic(_) => false,
            } && matches!(b.exit.flow, Flow::Jump);
            let next = u32::try_from(fall)
                .ok()
                .filter(|_| falls)
                .and_then(|f| unit(image, base, f, thumb));
            internal = crate::cycles::arm9_internal(word, pc, thumb, next, internal);
        }
        let pair =
            self.cpu == Cpu::Arm9 && !thumb && word >> 28 != 15 && word & 0x0E10_00F0 == 0xD0;
        reshape(&mut b, &l.bus, internal, (pc, thumb), swi, pair, entry);
        Ok(b)
    }
}

/// The address before alignment, when instruction `k` aligns one: the
/// templates clear the low bits, and the bus is given the address the CPU
/// puts out, which the machine aligns.
fn unaligned(b: &Block, k: usize) -> Option<Value> {
    let Op::Concat(h, l) = b.insts[k].op else {
        return None;
    };
    let Op::Const(z) = b.insts[l.index()].op else {
        return None;
    };
    let Op::Extract(x, n) = b.insts[h.index()].op else {
        return None;
    };
    let lw = b.width_of(l)?.bits();
    (z.get() == 0 && u32::from(n) == lw && b.width_of(x) == Some(Width::W32)).then_some(x)
}

/// Puts the entry check and the fetch first, routes memory to the bus,
/// charges the `internal` cycles as one tick after the instruction's
/// accesses, adds the refill and closes the block with a sync point; with
/// `pair` (LDRD) marks the second load.
fn reshape(
    b: &mut Block,
    bus: &[Bus],
    internal: u32,
    (pc, thumb): (u32, bool),
    swi: bool,
    pair: bool,
    entry: Option<Entry>,
) {
    let unaligned: Vec<Option<Value>> = (0..b.insts.len()).map(|k| unaligned(b, k)).collect();
    let old = std::mem::take(&mut b.insts);
    if let Some(e) = entry {
        let u = konst(b, Width::W32, u64::from(e.unit));
        let f = konst(b, Width::W32, u64::from(e.function));
        let changed = push(b, Op::intrinsic(ENTER, vec![u, f]), Some(Width::W1));
        push(b, Op::Guard(changed, CODE_CHANGED), None);
    }
    let at = konst(b, Width::W32, u64::from(pc));
    let name = if thumb { FETCH_THUMB } else { FETCH_ARM };
    let fetch = push(b, Op::intrinsic(name, vec![at]), None);
    let mut map: Vec<Value> = Vec::with_capacity(old.len());
    let mut loads = 0;
    for inst in old {
        if matches!(inst.op, Op::Load { .. }) {
            loads += 1;
            if pair && loads == 2 {
                push(b, Op::intrinsic(PAIR, Vec::new()), None);
            }
        }
        let m = |v: &Value| map[v.index()];
        let addr_of = |a: Value| m(&unaligned[a.index()].unwrap_or(a));
        let op = match inst.op {
            Op::Load { addr, endian, .. } => Op::IoLoad {
                addr: addr_of(addr),
                endian,
                kind: IoKind::Device,
            },
            Op::Store {
                addr,
                value,
                endian,
                ..
            } => Op::IoStore {
                addr: addr_of(addr),
                value: m(&value),
                endian,
                kind: IoKind::Device,
            },
            Op::Tick(_) => {
                map.push(fetch);
                continue;
            }
            op => op.map_values(&m),
        };
        let v = push(b, op, inst.width);
        map.push(v);
    }
    b.exit.next = match b.exit.next {
        Next::Dynamic(v) => Next::Dynamic(map[v.index()]),
        Next::Select {
            cond,
            taken,
            not_taken,
        } => Next::Select {
            cond: map[cond.index()],
            taken,
            not_taken,
        },
        next => next,
    };
    if internal > 0 {
        push(b, Op::Tick(internal), None);
    }
    let refills = bus.iter().any(|e| matches!(e, Bus::Code(false, _)));
    if refills && !swi {
        let fall = u64::from(pc) + if thumb { 2 } else { 4 };
        let (target, taken) = match (b.exit.flow, b.exit.next) {
            (_, Next::Const(t)) => (konst(b, Width::W32, t), konst(b, Width::W1, 1)),
            (_, Next::Select { cond, taken: t, .. }) => (konst(b, Width::W32, t), cond),
            (_, Next::Dynamic(v)) => match b.insts[v.index()].op {
                Op::Select(g, _, f) if is_const(b, f, fall) => (v, g),
                _ => (v, konst(b, Width::W1, 1)),
            },
        };
        let t = match last_set(b, regs::T) {
            Some(v) => v,
            None => push(b, Op::Get(regs::T), Some(Width::W1)),
        };
        push(b, Op::intrinsic(REFILL, vec![target, t, taken]), None);
    }
    if let Flow::Call { ret } = b.exit.flow
        && !swi
    {
        let r = konst(b, Width::W32, ret);
        push(b, Op::intrinsic(CALL, vec![r]), None);
    }
    if let Some(v) = last_set(b, regs::I)
        && !swi
    {
        push(b, Op::intrinsic(SET_I, vec![v]), None);
    }
    push(b, Op::Sync, None);
}

fn field(r: Reg) -> String {
    let i = r.index();
    match i {
        0..=14 => format!("c->r[{i}]"),
        15 => "c->n".into(),
        16 => "c->z".into(),
        17 => "c->c".into(),
        18 => "c->v".into(),
        19 => "c->t".into(),
        20 => "c->m".into(),
        21 => "c->i".into(),
        22 => "c->f".into(),
        23 => "c->psr_rsvd".into(),
        24..=28 => format!("c->spsr[{}]", i - 24),
        29..=35 => format!("c->bank_usr[{}]", i - 29),
        36..=42 => format!("c->bank_fiq[{}]", i - 36),
        43 | 44 => format!("c->bank_irq[{}]", i - 43),
        45 | 46 => format!("c->bank_svc[{}]", i - 45),
        47 | 48 => format!("c->bank_abt[{}]", i - 47),
        _ => format!("c->bank_und[{}]", i - 49),
    }
}

impl Frontend for Nds {
    fn granule(&self) -> u64 {
        2
    }

    fn regs(&self) -> RegFile {
        regs::regs()
    }

    fn stack_pointer(&self) -> Option<Reg> {
        Some(regs::r(13))
    }

    fn preserved(&self) -> Vec<Reg> {
        (4..12).chain([13]).map(regs::r).collect()
    }

    /// Lifts in ARM state at a word-aligned `pc` and in Thumb state
    /// otherwise, with no entry check; the build lifts through
    /// [`Nds::lift_at`], which is told the state and the function.
    fn lift(&self, bytes: &[u8], pc: u64) -> Result<Block, LiftFailure> {
        let pc32 = pc as u32;
        self.lift_at(bytes, pc32, pc32, pc32 & 3 != 0, None)
    }

    fn abi_headers(&self) -> &'static [&'static str] {
        &["relift/arm32/abi.h", "relift/nds/run.h"]
    }

    fn home(&self, reg: Reg) -> Home {
        Home::Field(field(reg))
    }

    fn intrinsic(&self, name: &str, args: &[String]) -> Option<String> {
        Some(match (name, args) {
            (FETCH_ARM, [pc]) => format!("rl_arm32_fetch(c, (uint32_t)({pc}), 0u)"),
            (FETCH_THUMB, [pc]) => format!("rl_arm32_fetch(c, (uint32_t)({pc}), 1u)"),
            (ENTER, [u, f]) => format!("rl_nds_cpu_enter(c, (uint32_t)({u}), (uint32_t)({f}))"),
            (REFILL, [t, thumb, taken]) => format!(
                "rl_arm32_refill(c, (uint32_t)({t}), (unsigned)({thumb}), (unsigned)({taken}))"
            ),
            (CALL, [r]) => format!("rl_arm32_call(c, (uint32_t)({r}))"),
            (CP_WRITE, [k, v, g]) => {
                format!("rl_nds_cpu_cp_write(c, (uint32_t)({k}), (uint32_t)({v}), (unsigned)({g}))")
            }
            (CP_READ, [k]) => format!("rl_nds_cpu_cp_read(c, (uint32_t)({k}))"),
            (SET_I, [v]) => format!("rl_nds_cpu_set_i(c, (uint32_t)({v}))"),
            (PAIR, []) => "rl_nds_cpu_pair(c)".to_owned(),
            _ => return None,
        })
    }

    fn trap(&self, code: u32) -> String {
        format!("rl_nds_swi_trap(c, mem, {code:#x}u)")
    }

    fn undefined(&self, pc: u64, insn: u64) -> String {
        format!(
            "rl_undefined(c, mem, UINT64_C({pc:#x}), {:#x}u)",
            insn as u32
        )
    }

    fn monitor(&self) -> &'static str {
        "0"
    }

    fn fp(
        &self,
        _op: FpOp,
        _from: Width,
        _to: Width,
        _args: &[String],
        _reg: &dyn Fn(Reg) -> String,
    ) -> Option<String> {
        None
    }

    fn crypto(&self, _op: CryptoOp, _arg: &str) -> Option<String> {
        None
    }

    fn clock(&self) -> Option<&'static str> {
        Some("c->time")
    }

    fn io_calls(&self) -> (&'static str, &'static str) {
        ("rl_nds_cpu_read", "rl_nds_cpu_write")
    }

    fn fp_state(&self) -> Vec<Reg> {
        Vec::new()
    }

    fn guard_state(&self, _code: u32) -> Option<Reg> {
        None
    }

    fn guard_exit(&self, code: u32, pc: u64, insn: u64) -> Option<String> {
        (code == CODE_CHANGED).then(|| {
            format!(
                "rl_nds_cpu_variant(c, (uint32_t)UINT32_C({pc:#x}), {}u)",
                u32::from(insn & RAW_THUMB != 0)
            )
        })
    }

    fn guard_slow(&self, code: u32, pc: u64, insn: u64) -> Option<String> {
        (code == COND_FAIL).then(|| {
            format!(
                "rl_nds_cpu_cond_fail(c, mem, UINT64_C({:#x}))",
                pc + if insn & RAW_THUMB != 0 { 2 } else { 4 }
            )
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn arm(words: &[u32]) -> Vec<u8> {
        words.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    fn thumb(halves: &[u16]) -> Vec<u8> {
        halves.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    const BASE: u32 = 0x0200_0000;
    const ENTRY: Entry = Entry {
        unit: 3,
        function: 7,
    };

    fn intrinsics(b: &Block) -> Vec<&'static str> {
        b.insts
            .iter()
            .filter_map(|i| match &i.op {
                Op::Intrinsic(c) => Some(c.name),
                _ => None,
            })
            .collect()
    }

    fn lift9(img: &[u8], pc: u32, thumb: bool) -> Block {
        Nds::new(Cpu::Arm9)
            .lift_at(img, BASE, pc, thumb, None)
            .expect("lifts")
    }

    #[test]
    fn the_second_word_of_an_arm9_ldrd_is_marked_and_ldm_s_is_not() {
        let img = arm(&[0xE1C4_60D0, 0xE894_00C0]);
        let b = lift9(&img, BASE, false);
        let order: Vec<&str> = b
            .insts
            .iter()
            .filter_map(|i| match &i.op {
                Op::IoLoad { .. } => Some("load"),
                Op::Intrinsic(c) if c.name == PAIR => Some(PAIR),
                _ => None,
            })
            .collect();
        assert_eq!(order, ["load", PAIR, "load"]);
        assert!(!intrinsics(&lift9(&img, BASE + 4, false)).contains(&PAIR));
        let b7 = Nds::new(Cpu::Arm7).lift_at(&img, BASE, BASE + 4, false, None);
        assert!(b7.is_ok_and(|b| !intrinsics(&b).contains(&PAIR)));
    }

    #[test]
    fn loads_and_stores_go_to_the_bus_and_the_block_ends_in_a_sync_point() {
        let img = arm(&[0xE591_0004, 0xE581_0008]);
        for cpu in [Cpu::Arm9, Cpu::Arm7] {
            for pc in [BASE, BASE + 4] {
                let b = Nds::new(cpu)
                    .lift_at(&img, BASE, pc, false, None)
                    .expect("lifts");
                assert!(
                    b.insts
                        .iter()
                        .any(|i| matches!(i.op, Op::IoLoad { .. } | Op::IoStore { .. }))
                );
                assert!(
                    !b.insts
                        .iter()
                        .any(|i| matches!(i.op, Op::Load { .. } | Op::Store { .. }))
                );
                assert!(matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync)));
                assert_eq!(intrinsics(&b)[0], FETCH_ARM);
            }
        }
    }

    #[test]
    fn an_entry_checks_its_bytes_before_the_fetch_and_any_effect() {
        let img = arm(&[0xE581_0008]);
        let b = Nds::new(Cpu::Arm9)
            .lift_at(&img, BASE, BASE, false, Some(ENTRY))
            .expect("lifts");
        assert_eq!(intrinsics(&b)[..2], [ENTER, FETCH_ARM]);
        let Op::Intrinsic(call) = &b.insts[2].op else {
            panic!("the entry check comes first");
        };
        assert!(is_const(&b, call.args[0], 3) && is_const(&b, call.args[1], 7));
        let guard = b
            .insts
            .iter()
            .position(|i| matches!(i.op, Op::Guard(_, CODE_CHANGED)))
            .expect("a guard");
        let store = b
            .insts
            .iter()
            .position(|i| matches!(i.op, Op::IoStore { .. }))
            .expect("a store");
        assert!(guard < store);
        let fe = Nds::new(Cpu::Arm7);
        assert_eq!(
            fe.guard_exit(CODE_CHANGED, u64::from(BASE), RAW_THUMB | 0x4770)
                .as_deref(),
            Some("rl_nds_cpu_variant(c, (uint32_t)UINT32_C(0x2000000), 1u)")
        );
        assert_eq!(fe.guard_exit(COND_FAIL, 0, 0), None);
        let inner = lift9(&img, BASE, false);
        assert!(!intrinsics(&inner).contains(&ENTER));
    }

    #[test]
    fn only_internal_cycles_stay_ticks() {
        let ldr = lift9(&arm(&[0xE591_0004]), BASE, false);
        assert!(
            !ldr.insts.iter().any(|i| matches!(i.op, Op::Tick(_))),
            "an ARM9 load takes its own cycle and its access"
        );
        let mul = lift9(&arm(&[0xE006_0995]), BASE, false);
        assert!(mul.insts.iter().any(|i| matches!(i.op, Op::Tick(1))));
        let mov = lift9(&arm(&[0xE3A0_0001]), BASE, false);
        assert!(!mov.insts.iter().any(|i| matches!(i.op, Op::Tick(_))));
    }

    #[test]
    fn a_branch_refills_at_its_target() {
        let b = lift9(&arm(&[0xEA00_0002]), BASE, false);
        assert_eq!(b.exit.next, Next::Const(u64::from(BASE) + 16));
        assert_eq!(intrinsics(&b), [FETCH_ARM, REFILL]);
    }

    #[test]
    fn swi_is_a_trap_into_the_bios_on_both_cpus() {
        let b = lift9(&arm(&[0xEF03_0000]), BASE, false);
        assert_eq!(b.exit.flow, Flow::Trap(0x03));
        assert_eq!(b.exit.next, Next::Const(u64::from(BASE) + 4));
        let b = Nds::new(Cpu::Arm7)
            .lift_at(&thumb(&[0xDF06]), BASE, BASE, true, None)
            .expect("lifts");
        assert_eq!(b.exit.flow, Flow::Trap(0x06 | SWI_THUMB));
        assert_eq!(b.exit.next, Next::Const(u64::from(BASE) + 2));
        assert_eq!(
            Nds::new(Cpu::Arm7).trap(0x106),
            "rl_nds_swi_trap(c, mem, 0x106u)"
        );
    }

    #[test]
    fn mcr_and_mrc_reach_the_cp15_of_the_arm9() {
        let mcr = lift9(&arm(&[0xEE07_0F90]), BASE, false);
        assert!(intrinsics(&mcr).contains(&CP_WRITE));
        assert!(matches!(mcr.insts.last().map(|i| &i.op), Some(Op::Sync)));
        let mrc = lift9(&arm(&[0xEE11_0F10]), BASE, false);
        assert!(intrinsics(&mrc).contains(&CP_READ));
        let fe = Nds::new(Cpu::Arm9);
        let a = |s: &str| s.to_string();
        assert_eq!(
            fe.intrinsic(CP_WRITE, &[a("k"), a("v"), a("g")]).as_deref(),
            Some("rl_nds_cpu_cp_write(c, (uint32_t)(k), (uint32_t)(v), (unsigned)(g))")
        );
        assert_eq!(
            fe.intrinsic(CP_READ, &[a("k")]).as_deref(),
            Some("rl_nds_cpu_cp_read(c, (uint32_t)(k))")
        );
        assert!(
            Nds::new(Cpu::Arm7)
                .lift_at(&arm(&[0xEE07_0F90]), BASE, BASE, false, None)
                .is_err()
        );
    }

    #[test]
    fn a_write_of_the_i_bit_tells_the_runtime() {
        let msr = lift9(&arm(&[0xE121_F000]), BASE, false);
        assert!(intrinsics(&msr).contains(&SET_I), "{:?}", intrinsics(&msr));
        let mov = lift9(&arm(&[0xE3A0_0001]), BASE, false);
        assert!(!intrinsics(&mov).contains(&SET_I));
    }

    #[test]
    fn armv5te_code_lifts_on_the_arm9_only() {
        let clz = arm(&[0xE16F_0F11]);
        assert!(
            Nds::new(Cpu::Arm9)
                .lift_at(&clz, BASE, BASE, false, None)
                .is_ok()
        );
        assert!(
            Nds::new(Cpu::Arm7)
                .lift_at(&clz, BASE, BASE, false, None)
                .is_err()
        );
    }

    #[test]
    fn the_second_half_of_a_bl_or_blx_pair_calls_a_constant() {
        let img = thumb(&[0xF000, 0xF80E]);
        let b = lift9(&img, BASE + 2, true);
        assert_eq!(b.exit.next, Next::Const(u64::from(BASE) + 4 + 0x1C));
        assert_eq!(
            b.exit.flow,
            Flow::Call {
                ret: u64::from(BASE) + 4
            }
        );
        assert_eq!(intrinsics(&b), [FETCH_THUMB, REFILL, CALL]);
        let img = thumb(&[0xF000, 0xE80E]);
        let b = lift9(&img, BASE + 2, true);
        assert_eq!(b.exit.next, Next::Const(u64::from(BASE) + 4 + 0x1C));
        assert_eq!(pair_target(0xF000, 0xE80E, BASE + 2, false), None);
        assert_eq!(
            pair_target(0xF7FF, 0xFFFE, BASE + 2, true),
            Some((BASE + 4 - 0x1000 + 0xFFC, true))
        );
        assert_eq!(
            pair_target(0xF000, 0xE802, BASE + 2, true),
            Some((BASE + 8, false))
        );
    }

    #[test]
    fn every_slot_has_a_context_field() {
        let fe = Nds::new(Cpu::Arm9);
        let file = fe.regs();
        let names: Vec<String> = (0..file.len())
            .map(|i| match fe.home(Reg(i as u16)) {
                Home::Field(f) => f,
                Home::Flag { .. } => String::new(),
            })
            .collect();
        assert_eq!(names[13], "c->r[13]");
        assert_eq!(names[23], "c->psr_rsvd");
        assert_eq!(names[50], "c->bank_und[1]");
        assert_eq!(names.len(), 51);
    }
}
